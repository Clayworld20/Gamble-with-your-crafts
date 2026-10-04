using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using Steamworks;
using Steamworks.Data;

namespace GambleWithYourCrafts;

/// <summary>Видимость лобби — как в Steam: закрытое, только для друзей, публичное.</summary>
public enum LobbyVisibility
{
    /// <summary>Только по приглашению (Steam-лобби типа private).</summary>
    Private = 0,

    /// <summary>Друзья участников видят и могут подключиться.</summary>
    FriendsOnly = 1,

    /// <summary>Публичный список лобби (фильтруем по тегу игры и протоколу).</summary>
    Public = 2,
}

/// <summary>Краткая информация о лобби для консоли/списка.</summary>
public readonly record struct LobbyInfo(ulong Id, string Name, int Members, int MaxMembers, string Version);

/// <summary>
/// Сетевой менеджер: лобби Steam, приглашение друзей через оверлей и
/// P2P-пакеты напрямую между машинами (без сторонних серверов).
///
/// Роль хоста = владелец лобби (Steam сам передаёт владение, если хост вышел),
/// поэтому <see cref="IsHost"/> всегда вычисляется из актуального состояния лобби.
/// </summary>
public sealed class P2PLobbyManager : INetTransport, IDisposable
{
    /// <summary>Тестовый AppID Spacewar — официально разрешён Valve для отладки Steamworks.</summary>
    public const uint SpacewarAppId = 480;

    /// <summary>Ключ лобби, по которому ищем свои лобби в публичном списке.</summary>
    public const string GameTag = "gwyc";

    private const string KeyGame = "game";
    private const string KeyName = "name";
    private const string KeyProto = "proto";
    private const string KeyVersion = "ver";

    /// <summary>Канал надёжных сообщений (правки мира, ставки, инвентари).</summary>
    private const int ControlChannel = 0;

    /// <summary>Канал быстрых сообщений (пинги, «пульс» сессии).</summary>
    private const int StateChannel = 1;

    private const int MaxPacketsPerChannelPerPump = 256;

    private readonly ConcurrentQueue<Action> _mainThreadQueue = new();
    private readonly List<ulong> _peers = new();
    private readonly List<LobbyInfo> _browseResults = new();

    private Lobby? _lobby;
    private bool _hooked;
    private bool _inLobby;
    private long _lastLobbyRefreshMs;
    private ulong _hostId;
    private bool _isHost;
    private int _packetsReceived;
    private int _packetsSent;
    private int _packetsDropped;

    public P2PLobbyManager() => Hook();

    /// <summary>Диагностика/логи для консоли.</summary>
    public event Action<string>? Log;

    /// <summary>Входящий пакет. Поднимается на главном потоке во время <see cref="Pump"/>.</summary>
    public event Action<ulong, byte[]>? PacketReceived;

    /// <summary>Состав лобби изменился (кто-то зашёл/вышел).</summary>
    public event Action? RosterChanged;

    /// <summary>Сообщение из чата лобби Steam (друзья могут писать прямо из оверлея).</summary>
    public event Action<ulong, string, string>? LobbyChatReceived;

    public bool InLobby => _inLobby;

    public Lobby? CurrentLobby => _lobby;

    public string LobbyName { get; private set; } = string.Empty;

    public int MemberCount { get; private set; }

    public int MaxMembers { get; private set; }

    public int PacketsSent => _packetsSent;

    public int PacketsReceived => _packetsReceived;

    public int PacketsDropped => _packetsDropped;

    // ── INetTransport ────────────────────────────────────────────────────────

    public bool IsHost => _inLobby && _isHost;

    public ulong LocalId => SteamClient.SteamId.Value;

    public ulong HostId => _hostId;

    public IReadOnlyCollection<ulong> Peers => _peers;

    // ── Лобби ────────────────────────────────────────────────────────────────

    /// <summary>
    /// Создать лобби. Facepunch создаёт его невидимым — видимость выставляем
    /// после создания методами SetPublic/SetFriendsOnly/SetPrivate.
    /// </summary>
    public async Task<LobbyInfo?> HostLobbyAsync(LobbyVisibility visibility, int maxMembers, string? lobbyName = null)
    {
        if (!SteamClient.IsValid)
        {
            Log?.Invoke("Steam не инициализирован — лобби создать нельзя.");
            return null;
        }

        maxMembers = Math.Clamp(maxMembers, 2, 32);
        string name = string.IsNullOrWhiteSpace(lobbyName)
            ? $"{SteamClient.Name}: {GameTag}"
            : lobbyName.Trim();

        try
        {
            Lobby? lobby = await SteamMatchmaking.CreateLobbyAsync(maxMembers).ConfigureAwait(false);
            if (lobby is null)
            {
                Log?.Invoke("Steam не отдал лобби (CreateLobbyAsync вернул null).");
                return null;
            }

            Lobby value = lobby.Value;
            value.SetData(KeyGame, GameTag);
            value.SetData(KeyName, name);
            value.SetData(KeyProto, NetMessage.ProtocolVersion.ToString(CultureInfo.InvariantCulture));
            value.SetData(KeyVersion, BuildInfo.Tag);
            value.SetJoinable(true);

            bool visibilityApplied = visibility switch
            {
                LobbyVisibility.Public => value.SetPublic(),
                LobbyVisibility.FriendsOnly => value.SetFriendsOnly(),
                _ => value.SetPrivate(),
            };

            LobbyInfo info = Describe(value);
            Post(() =>
            {
                _lobby = value;
                _inLobby = true;
                LobbyName = name;
                RefreshRoster(value);
                Log?.Invoke(visibilityApplied
                    ? $"Лобби создано ({VisibilityName(visibility)}): {info.Name}"
                    : $"Лобби создано, но видимость «{VisibilityName(visibility)}» не применилась (Steam вернул ошибку).");
                Log?.Invoke($"ID лобби: {info.Id} — приглашайте друзей командой invite (оверлей Steam) или отправьте им этот ID.");
                RosterChanged?.Invoke();
            });

            return info;
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Ошибка создания лобби: {ex.Message}");
            return null;
        }
    }

    /// <summary>Подключиться к лобби по SteamID (он же используется в ссылке-приглашении).</summary>
    public async Task<LobbyInfo?> JoinLobbyAsync(ulong lobbyId)
    {
        if (!SteamClient.IsValid)
        {
            Log?.Invoke("Steam не инициализирован — подключиться нельзя.");
            return null;
        }

        if (_inLobby)
        {
            Log?.Invoke("Сначала выйдите из текущего лобби (leave).");
            return null;
        }

        try
        {
            Lobby? lobby = await SteamMatchmaking.JoinLobbyAsync(lobbyId).ConfigureAwait(false);
            if (lobby is null)
            {
                Log?.Invoke($"Не удалось войти в лобби {lobbyId} (нет такого / нет доступа / лобби заполнено).");
                return null;
            }

            Lobby value = lobby.Value;
            LobbyInfo info = Describe(value);
            Post(() =>
            {
                _lobby = value;
                _inLobby = true;
                LobbyName = info.Name;
                RefreshRoster(value);
                Log?.Invoke($"Подключились к лобби {info.Name} ({info.Members}/{info.MaxMembers}), версия {info.Version}.");
                RosterChanged?.Invoke();
            });
            return info;
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Ошибка подключения к лобби: {ex.Message}");
            return null;
        }
    }

    /// <summary>Список публичных лобби нашей игры (для команды browse).</summary>
    public async Task<IReadOnlyList<LobbyInfo>> BrowseLobbiesAsync(int max = 30)
    {
        if (!SteamClient.IsValid) return Array.Empty<LobbyInfo>();

        try
        {
            Lobby[] found = await SteamMatchmaking.LobbyList
                .FilterDistanceWorldwide()
                .WithKeyValue(KeyGame, GameTag)
                .WithSlotsAvailable(1)
                .WithMaxResults(Math.Clamp(max, 1, 100))
                .RequestAsync()
                .ConfigureAwait(false);

            var list = new List<LobbyInfo>();
            foreach (Lobby lobby in found)
            {
                LobbyInfo info = Describe(lobby);
                // Отсекаем лобби с другой версией протокола — иначе рукопожатие всё равно провалится.
                if (!string.Equals(info.Version, BuildInfo.Tag, StringComparison.Ordinal)) continue;
                list.Add(info);
            }

            Post(() =>
            {
                _browseResults.Clear();
                _browseResults.AddRange(list);
                Log?.Invoke(list.Count == 0
                    ? "Публичных лобби не найдено. Создайте своё (host public) или попросите друга пригласить вас."
                    : $"Найдено лобби: {list.Count}.");
                foreach (LobbyInfo info in list)
                {
                    Log?.Invoke($"  • {info.Name} — {info.Members}/{info.MaxMembers}, id {info.Id}");
                }
            });

            return list;
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Ошибка поиска лобби: {ex.Message}");
            return Array.Empty<LobbyInfo>();
        }
    }

    /// <summary>Последний результат поиска лобби (для команды join по номеру).</summary>
    public IReadOnlyList<LobbyInfo> LastBrowseResults => _browseResults;

    /// <summary>
    /// Официальный диалог приглашения друзей в оверлее Steam
    /// (ISteamFriends::ActivateGameOverlayInviteDialog).
    /// </summary>
    public bool OpenInviteOverlay()
    {
        if (!_inLobby || _lobby is null)
        {
            Log?.Invoke("Вы не в лобби — приглашать некого.");
            return false;
        }

        try
        {
            SteamFriends.OpenGameInviteOverlay(_lobby.Value.Id);
            Log?.Invoke("Открыт оверлей Steam с приглашением друзей. Если оверлей выключен — включите его в свойствах игры в Steam.");
            return true;
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Не удалось открыть оверлей: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Пригласить конкретного друга по имени/ID (резервный путь, если оверлей недоступен).
    /// </summary>
    public bool InviteFriend(string friendNameOrId, out string message)
    {
        message = string.Empty;
        if (!_inLobby || _lobby is null)
        {
            message = "Сначала создайте или войдите в лобби.";
            return false;
        }

        if (string.IsNullOrWhiteSpace(friendNameOrId))
        {
            message = "Укажите имя друга или его SteamID.";
            return false;
        }

        if (ulong.TryParse(friendNameOrId.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong rawId))
        {
            bool ok = _lobby.Value.InviteFriend(rawId);
            message = ok ? $"Приглашение отправлено SteamID {rawId}." : $"Steam отказал в приглашении {rawId}.";
            return ok;
        }

        string query = friendNameOrId.Trim();
        var matches = new List<Friend>();
        foreach (Friend friend in SteamFriends.GetFriends())
        {
            if (friend.Name.Contains(query, StringComparison.OrdinalIgnoreCase)) matches.Add(friend);
        }

        if (matches.Count == 0)
        {
            message = $"Друг «{query}» не найден в списке друзей Steam.";
            return false;
        }

        if (matches.Count > 1)
        {
            var sb = new StringBuilder($"Найдено несколько друзей по «{query}»: ");
            foreach (Friend friend in matches.Take(6)) sb.Append(friend.Name).Append(" (").Append(friend.Id.Value).Append(") ");
            message = sb.ToString().Trim();
            return false;
        }

        Friend target = matches[0];
        bool invited = _lobby.Value.InviteFriend(target.Id);
        message = invited
            ? $"Приглашение отправлено: {target.Name}."
            : $"Steam отказал в приглашении для {target.Name}.";
        return invited;
    }

    /// <summary>Выйти из лобби и закрыть P2P-сессии.</summary>
    public void LeaveLobby()
    {
        if (!_inLobby || _lobby is null)
        {
            Log?.Invoke("Вы и так не в лобби.");
            return;
        }

        foreach (ulong peer in _peers)
        {
            CloseSession(peer);
        }

        try
        {
            _lobby.Value.Leave();
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Ошибка выхода из лобби: {ex.Message}");
        }

        _lobby = null;
        _inLobby = false;
        _isHost = false;
        _hostId = 0;
        LobbyName = string.Empty;
        MemberCount = 0;
        MaxMembers = 0;
        _peers.Clear();
        Log?.Invoke("Вы вышли из лобби.");
        RosterChanged?.Invoke();
    }

    // ── P2P-пакеты ───────────────────────────────────────────────────────────

    public bool SendTo(ulong peer, byte[] payload, bool reliable)
    {
        if (!_inLobby)
        {
            Log?.Invoke("Отправка невозможна: вы не в лобби.");
            return false;
        }

        if (payload.Length > NetMessage.MaxPacketBytes)
        {
            Log?.Invoke($"Пакет {payload.Length} байт больше предела {NetMessage.MaxPacketBytes} — отправка отменена.");
            return false;
        }

        var target = (SteamId)peer;

        // Сессию можно открыть со своей стороны: вторая сторона получит OnP2PSessionRequest.
        SteamNetworking.AcceptP2PSessionWithUser(target);

        bool sent = SteamNetworking.SendP2PPacket(
            target,
            payload,
            payload.Length,
            reliable ? ControlChannel : StateChannel,
            reliable ? P2PSend.Reliable : P2PSend.UnreliableNoDelay);

        if (sent)
        {
            _packetsSent++;
            return true;
        }

        string kind = payload.Length > 3 ? NetMessage.TypeName((MessageType)payload[3]) : "?";
        Log?.Invoke($"P2P-пакет {kind} для {BuildInfo.ShortId(peer)} не ушёл.");
        return false;
    }

    public void Broadcast(byte[] payload, bool reliable)
    {
        foreach (ulong peer in _peers)
        {
            SendTo(peer, payload, reliable);
        }
    }

    private void CloseSession(ulong peer)
    {
        try
        {
            SteamNetworking.CloseP2PSessionWithUser((SteamId)peer);
        }
        catch (Exception ex)
        {
            Log?.Invoke($"Не удалось закрыть P2P-сессию с {BuildInfo.ShortId(peer)}: {ex.Message}");
        }
    }

    /// <summary>
    /// Главный тик транспорта: выполняет отложенные действия, обновляет состав лобби
    /// и вычитывает входящие P2P-пакеты. Вызывать раз в кадр на главном потоке.
    /// </summary>
    public void Pump()
    {
        while (_mainThreadQueue.TryDequeue(out Action? action))
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                Log?.Invoke($"Ошибка в отложенном действии лобби: {ex.Message}");
            }
        }

        if (!_inLobby || _lobby is null) return;

        long now = Environment.TickCount64;
        if (now - _lastLobbyRefreshMs > 3000)
        {
            _lastLobbyRefreshMs = now;
            try
            {
                bool alive = _lobby.Value.Refresh();
                if (!alive)
                {
                    Log?.Invoke("Лобби больше не существует (Steam вернул ошибку) — выходим из сессии.");
                    LeaveLobby();
                    return;
                }
                RefreshRoster(_lobby.Value);
            }
            catch (Exception ex)
            {
                Log?.Invoke($"Не удалось обновить данные лобби: {ex.Message}");
            }
        }

        ReadChannel(ControlChannel);
        ReadChannel(StateChannel);
    }

    /// <summary>
    /// Вычитать всё, что накопилось в канале. Facepunch отдаёт готовый P2Packet
    /// (данные + отправитель), поэтому и размер, и автора получаем сразу.
    /// </summary>
    private void ReadChannel(int channel)
    {
        int processed = 0;
        while (processed < MaxPacketsPerChannelPerPump)
        {
            P2Packet? packet = SteamNetworking.ReadP2PPacket(channel);
            if (packet is null) return;

            processed++;
            _packetsReceived++;

            P2Packet value = packet.Value;
            if (value.Data.Length == 0) continue;

            if (value.Data.Length > NetMessage.MaxPacketBytes)
            {
                _packetsDropped++;
                Log?.Invoke($"Пакет от {BuildInfo.ShortId(value.SteamId.Value)} отброшен: {value.Data.Length} байт (предел {NetMessage.MaxPacketBytes}).");
                continue;
            }

            try
            {
                PacketReceived?.Invoke(value.SteamId.Value, value.Data);
            }
            catch (Exception ex)
            {
                Log?.Invoke($"Обработчик пакета упал: {ex.Message}");
            }
        }
    }

    // ── Steam-события ────────────────────────────────────────────────────────

    private void Hook()
    {
        if (_hooked) return;
        _hooked = true;

        SteamMatchmaking.OnLobbyCreated += OnLobbyCreated;
        SteamMatchmaking.OnLobbyEntered += OnLobbyEntered;
        SteamMatchmaking.OnLobbyMemberJoined += OnMemberJoined;
        SteamMatchmaking.OnLobbyMemberLeave += OnMemberLeave;
        SteamMatchmaking.OnLobbyMemberDisconnected += OnMemberDisconnected;
        SteamMatchmaking.OnChatMessage += OnLobbyChat;
        SteamFriends.OnGameLobbyJoinRequested += OnJoinRequested;
        SteamNetworking.OnP2PSessionRequest += OnP2PSessionRequest;
        SteamNetworking.OnP2PConnectionFailed += OnP2PConnectionFailed;
    }

    private void Unhook()
    {
        if (!_hooked) return;
        _hooked = false;

        SteamMatchmaking.OnLobbyCreated -= OnLobbyCreated;
        SteamMatchmaking.OnLobbyEntered -= OnLobbyEntered;
        SteamMatchmaking.OnLobbyMemberJoined -= OnMemberJoined;
        SteamMatchmaking.OnLobbyMemberLeave -= OnMemberLeave;
        SteamMatchmaking.OnLobbyMemberDisconnected -= OnMemberDisconnected;
        SteamMatchmaking.OnChatMessage -= OnLobbyChat;
        SteamFriends.OnGameLobbyJoinRequested -= OnJoinRequested;
        SteamNetworking.OnP2PSessionRequest -= OnP2PSessionRequest;
        SteamNetworking.OnP2PConnectionFailed -= OnP2PConnectionFailed;
    }

    private void OnLobbyCreated(Result result, Lobby lobby)
    {
        if (result != Result.OK)
        {
            Post(() => Log?.Invoke($"Steam сообщил, что лобби не создано: {result}."));
        }
    }

    private void OnLobbyEntered(Lobby lobby)
    {
        LobbyInfo info = Describe(lobby);
        Post(() =>
        {
            _lobby = lobby;
            _inLobby = true;
            LobbyName = info.Name;
            RefreshRoster(lobby);
            Log?.Invoke($"Вошли в лобби: {info.Name} ({info.Members}/{info.MaxMembers}).");
            RosterChanged?.Invoke();
        });
    }

    private void OnMemberJoined(Lobby lobby, Friend friend)
    {
        string text = $"{friend.Name} подключился к лобби.";
        Post(() =>
        {
            RefreshRoster(lobby);
            Log?.Invoke(text);
            RosterChanged?.Invoke();
        });
    }

    private void OnMemberLeave(Lobby lobby, Friend friend)
    {
        string text = $"{friend.Name} покинул лобби.";
        Post(() =>
        {
            RefreshRoster(lobby);
            Log?.Invoke(text);
            RosterChanged?.Invoke();
        });
    }

    private void OnMemberDisconnected(Lobby lobby, Friend friend)
    {
        string text = $"{friend.Name} потерял связь с лобби.";
        Post(() =>
        {
            RefreshRoster(lobby);
            Log?.Invoke(text);
            RosterChanged?.Invoke();
        });
    }

    private void OnLobbyChat(Lobby lobby, Friend friend, string message)
    {
        Post(() => LobbyChatReceived?.Invoke(friend.Id.Value, friend.Name, message));
    }

    /// <summary>Игрок принял приглашение в оверлее (или нажал "Присоединиться" в Steam) — входим в лобби.</summary>
    private void OnJoinRequested(Lobby lobby, SteamId inviter)
    {
        string text = $"{inviter.Value} пригласил вас в лобби {lobby.Id.Value} — подключаемся.";
        Post(() =>
        {
            Log?.Invoke(text);
            if (_inLobby)
            {
                Log?.Invoke("Вы уже в другом лобби — сначала выполните leave.");
                return;
            }

            _ = JoinLobbyAsync(lobby.Id.Value);
        });
    }

    /// <summary>Друг стучится в P2P. Впускаем только участников нашего лобби.</summary>
    private void OnP2PSessionRequest(SteamId requester)
    {
        bool allowed = false;
        if (_inLobby && _lobby is not null)
        {
            foreach (Friend member in _lobby.Value.Members)
            {
                if (member.Id.Value != requester.Value) continue;
                allowed = true;
                break;
            }
        }

        if (allowed)
        {
            SteamNetworking.AcceptP2PSessionWithUser(requester);
            Post(() => Log?.Invoke($"P2P-сессия открыта с {BuildInfo.ShortId(requester.Value)}."));
        }
        else
        {
            SteamNetworking.CloseP2PSessionWithUser(requester);
            Post(() => Log?.Invoke($"Отклонён P2P-запрос от {BuildInfo.ShortId(requester.Value)}: не участник лобби."));
        }
    }

    private void OnP2PConnectionFailed(SteamId peer, P2PSessionError error)
    {
        Post(() => Log?.Invoke($"Не удалось пробить P2P-соединение с {BuildInfo.ShortId(peer.Value)}: {error}. Попробуем снова через релей Steam."));
    }

    // ── Утилиты ──────────────────────────────────────────────────────────────

    /// <summary>Отложить работу, результат которой придёт из пула потоков (await) — выполнится в Pump.</summary>
    private void Post(Action action) => _mainThreadQueue.Enqueue(action);

    private static LobbyInfo Describe(Lobby lobby)
    {
        string name = lobby.GetData(KeyName);
        string version = lobby.GetData(KeyVersion);
        return new LobbyInfo(
            lobby.Id.Value,
            string.IsNullOrWhiteSpace(name) ? "Лобби без названия" : name,
            lobby.MemberCount,
            lobby.MaxMembers,
            string.IsNullOrWhiteSpace(version) ? "?" : version);
    }

    private void RefreshRoster(Lobby lobby)
    {
        _peers.Clear();
        ulong local = SteamClient.SteamId.Value;
        foreach (Friend member in lobby.Members)
        {
            if (member.Id.Value == local) continue;
            _peers.Add(member.Id.Value);
        }

        MemberCount = lobby.MemberCount;
        MaxMembers = lobby.MaxMembers;
        _hostId = lobby.Owner.Id.Value;
        _isHost = _hostId == local;
    }

    public IReadOnlyList<(ulong Id, string Name, bool IsHost, bool IsFriend)> Roster()
    {
        var list = new List<(ulong, string, bool, bool)>();
        if (!_inLobby || _lobby is null) return list;

        foreach (Friend member in _lobby.Value.Members)
        {
            list.Add((member.Id.Value, member.Name, member.Id.Value == _hostId, member.IsFriend));
        }
        return list;
    }

    public static string VisibilityName(LobbyVisibility visibility) => visibility switch
    {
        LobbyVisibility.Public => "публичное",
        LobbyVisibility.FriendsOnly => "только для друзей",
        _ => "приватное (по приглашению)",
    };

    /// <summary>Готовые строки для команды status.</summary>
    public string StatusText()
    {
        if (!_inLobby || _lobby is null) return "Сессия: не в лобби.";

        var sb = new StringBuilder();
        sb.Append("Лобби: ").Append(LobbyName)
          .Append(" — ").Append(MemberCount).Append('/').Append(MaxMembers)
          .Append(", id ").Append(_lobby.Value.Id.Value).AppendLine();
        sb.Append("Роль: ").Append(_isHost ? "хост (владелец лобби)" : "клиент").AppendLine();
        sb.Append("Пакеты: отправлено ").Append(_packetsSent)
          .Append(", получено ").Append(_packetsReceived)
          .Append(", отброшено ").Append(_packetsDropped).AppendLine();
        sb.Append("Участники:");
        foreach (var member in Roster())
        {
            sb.Append(' ').Append(member.Name)
              .Append(member.IsHost ? "(хост)" : string.Empty)
              .Append(member.IsFriend ? "" : "[не друг]");
        }
        return sb.ToString();
    }

    public void Dispose()
    {
        Unhook();
        if (_inLobby)
        {
            try
            {
                _lobby?.Leave();
            }
            catch (Exception)
            {
                // Выходим молча: приложение уже закрывается.
            }
        }
        _inLobby = false;
        _lobby = null;
    }
}
