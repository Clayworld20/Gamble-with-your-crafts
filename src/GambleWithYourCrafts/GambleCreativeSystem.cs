using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace GambleWithYourCrafts;

/// <summary>
/// Креатив-инвентарь: типы блоков и их количество.
/// Ключ — строковый идентификатор блока ("dirt", "gold", "diamond", ...),
/// ровно как просит дизайн: Dictionary&lt;string, int&gt;.
/// </summary>
public sealed class Inventory
{
    private readonly Dictionary<string, int> _items = new(StringComparer.OrdinalIgnoreCase);

    public IReadOnlyDictionary<string, int> Items => _items;

    public int Kinds => _items.Count;

    public long TotalBlocks
    {
        get
        {
            long total = 0;
            foreach (var pair in _items) total += pair.Value;
            return total;
        }
    }

    /// <summary>Оценка "богатства" в блоках-единицах (по ценности каждого типа).</summary>
    public long Wealth
    {
        get
        {
            long total = 0;
            foreach (var pair in _items)
            {
                if (Blocks.TryParse(pair.Key, out BlockId id)) total += (long)Blocks.Info(id).Value * pair.Value;
            }
            return total;
        }
    }

    public int Get(string key)
    {
        if (string.IsNullOrWhiteSpace(key)) return 0;
        return _items.TryGetValue(key, out int amount) ? amount : 0;
    }

    public bool Has(string key, int amount) => amount <= 0 || Get(key) >= amount;

    /// <summary>Начислить блоки (выигрыш в казино тоже приходит сюда).</summary>
    public void Add(string key, int amount)
    {
        if (amount <= 0) return;
        string normalized = Normalize(key);
        _items.TryGetValue(normalized, out int current);
        _items[normalized] = current + amount;
    }

    /// <summary>Списать блоки. false — не хватает (инвентарь не уходит в минус никогда).</summary>
    public bool TrySpend(string key, int amount)
    {
        if (amount <= 0) return true;
        string normalized = Normalize(key);
        if (!_items.TryGetValue(normalized, out int current) || current < amount) return false;

        int left = current - amount;
        if (left == 0) _items.Remove(normalized);
        else _items[normalized] = left;
        return true;
    }

    public void Set(string key, int amount)
    {
        string normalized = Normalize(key);
        if (amount <= 0) _items.Remove(normalized);
        else _items[normalized] = amount;
    }

    public void Clear() => _items.Clear();

    /// <summary>Снимок для сети: только непустые позиции, отсортированы от дорогих к дешёвым.</summary>
    public List<KeyValuePair<string, int>> Snapshot()
    {
        var list = new List<KeyValuePair<string, int>>(_items.Count);
        foreach (var pair in _items)
        {
            if (pair.Value > 0) list.Add(new KeyValuePair<string, int>(pair.Key, pair.Value));
        }

        list.Sort((a, b) =>
        {
            int valueA = Blocks.TryParse(a.Key, out BlockId idA) ? Blocks.Info(idA).Value : 0;
            int valueB = Blocks.TryParse(b.Key, out BlockId idB) ? Blocks.Info(idB).Value : 0;
            int byValue = valueB.CompareTo(valueA);
            return byValue != 0 ? byValue : string.CompareOrdinal(a.Key, b.Key);
        });
        return list;
    }

    public void Load(IEnumerable<KeyValuePair<string, int>> items)
    {
        ArgumentNullException.ThrowIfNull(items);
        _items.Clear();
        foreach (var pair in items)
        {
            if (pair.Value > 0) Add(pair.Key, pair.Value);
        }
    }

    public Inventory Clone()
    {
        var copy = new Inventory();
        foreach (var pair in _items) copy.Add(pair.Key, pair.Value);
        return copy;
    }

    /// <summary>Человекочитаемая строка вида "Земля x64, Золото x8".</summary>
    public string Describe()
    {
        var ordered = Snapshot();
        if (ordered.Count == 0) return "пусто";

        var sb = new StringBuilder();
        for (int i = 0; i < ordered.Count; i++)
        {
            if (i > 0) sb.Append(", ");
            var pair = ordered[i];
            string title = Blocks.TryParse(pair.Key, out BlockId id) ? Blocks.Info(id).Title : pair.Key;
            sb.Append(title).Append(" x").Append(pair.Value.ToString(CultureInfo.InvariantCulture));
        }
        return sb.ToString();
    }

    private static string Normalize(string key)
    {
        if (Blocks.TryParse(key, out BlockId id)) return Blocks.Info(id).Key;
        return key.Trim().ToLowerInvariant();
    }
}

/// <summary>Игра на столе.</summary>
public enum CasinoGame : byte
{
    Dice = 1,
    Roulette = 2,
}

/// <summary>Результат раунда: кто, на что ставил, сколько выиграл.</summary>
public sealed class CasinoOutcome
{
    public CasinoGame Game { get; init; }
    public string Target { get; init; } = string.Empty;
    public int Stake { get; init; }
    public bool Won { get; init; }
    public int Multiplier { get; init; }
    public int Payout { get; init; }
    public int[] Rolls { get; init; } = Array.Empty<int>();
    public string RollText { get; init; } = string.Empty;

    public string Summary()
    {
        string payout = Won ? $"+{Payout}" : $"-{Stake}";
        return Won
            ? $"ВЫИГРЫШ {payout} (x{Multiplier})"
            : $"проигрыш {-Stake}";
    }
}

/// <summary>Источник случайности. Вынесен в интерфейс, чтобы тесты были детерминированными.</summary>
public interface IRandomSource
{
    /// <summary>Случайное число в диапазоне [0, maxExclusive).</summary>
    int NextInt(int maxExclusive);
}

/// <summary>Криптослучайный источник — для честных бросков на хосте.</summary>
public sealed class CryptoRandomSource : IRandomSource
{
    public static readonly CryptoRandomSource Shared = new();

    public int NextInt(int maxExclusive)
    {
        if (maxExclusive <= 1) return 0;
        return RandomNumberGenerator.GetInt32(maxExclusive);
    }
}

/// <summary>
/// Правила стола: коэффициенты и условия выигрыша для костей и европейской рулетки.
/// </summary>
public static class CasinoRules
{
    /// <summary>Рулетка: 37 лунок (0..36), 0 — зеро.</summary>
    public const int RoulettePockets = 37;
    public const int MaxStake = 512;

    private static readonly HashSet<int> RedPockets = new()
    {
        1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36,
    };

    public enum PocketColor
    {
        Green = 0,
        Red = 1,
        Black = 2,
    }

    public static PocketColor ColorOf(int pocket)
    {
        if (pocket <= 0 || pocket >= RoulettePockets) return PocketColor.Green;
        return RedPockets.Contains(pocket) ? PocketColor.Red : PocketColor.Black;
    }

    /// <summary>Разбор цели ставки, введённой игроком. Возвращает нормализованное имя.</summary>
    public static bool TryParseTarget(CasinoGame game, string text, out string normalized, out string error)
    {
        normalized = string.Empty;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(text))
        {
            error = "Не указана цель ставки.";
            return false;
        }

        string raw = text.Trim().ToLowerInvariant();
        if (game == CasinoGame.Dice)
        {
            switch (raw)
            {
                case "high" or "больше" or "больш": normalized = "high"; return true;
                case "low" or "меньше" or "меньш": normalized = "low"; return true;
                case "seven" or "семь" or "7": normalized = "seven"; return true;
                case "double" or "дубль" or "пара": normalized = "double"; return true;
                case "snakeeyes" or "змеиные" or "2": normalized = "snakeeyes"; return true;
                case "boxcars" or "12": normalized = "boxcars"; return true;
                default:
                    error = $"Для костей доступно: {string.Join(", ", DiceTargets)}.";
                    return false;
            }
        }

        switch (raw)
        {
            case "red" or "красное" or "красный": normalized = "red"; return true;
            case "black" or "чёрное" or "черное" or "черный": normalized = "black"; return true;
            case "even" or "чёт" or "чет": normalized = "even"; return true;
            case "odd" or "нечет" or "нечёт": normalized = "odd"; return true;
            case "low" or "малые" or "1-18": normalized = "low"; return true;
            case "high" or "большие" or "19-36": normalized = "high"; return true;
            case "dozen1" or "1-12" or "первая": normalized = "dozen1"; return true;
            case "dozen2" or "13-24" or "вторая": normalized = "dozen2"; return true;
            case "dozen3" or "25-36" or "третья": normalized = "dozen3"; return true;
        }

        if (int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out int pocket) && pocket is >= 0 and < RoulettePockets)
        {
            normalized = pocket.ToString(CultureInfo.InvariantCulture);
            return true;
        }

        error = $"Для рулетки доступно: red, black, even, odd, low, high, dozen1..dozen3 или число 0..36.";
        return false;
    }

    public static readonly string[] DiceTargets = { "high", "low", "seven", "double", "snakeeyes", "boxcars" };

    public static readonly string[] RouletteTargets =
    {
        "red", "black", "even", "odd", "low", "high", "dozen1", "dozen2", "dozen3", "0..36",
    };

    /// <summary>Во сколько раз ставка возвращается целиком (x2 = вернули ставку и столько же сверху).</summary>
    public static int Multiplier(CasinoGame game, string target)
    {
        if (game == CasinoGame.Dice)
        {
            return target switch
            {
                "high" or "low" => 2,
                "seven" => 5,
                "double" => 6,
                "snakeeyes" or "boxcars" => 31,
                _ => 0,
            };
        }

        return target switch
        {
            "red" or "black" or "even" or "odd" or "low" or "high" => 2,
            "dozen1" or "dozen2" or "dozen3" => 3,
            _ => int.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out int pocket) && pocket is >= 0 and < RoulettePockets ? 36 : 0,
        };
    }

    public static bool Wins(CasinoGame game, string target, IReadOnlyList<int> rolls)
    {
        if (rolls.Count == 0) return false;

        if (game == CasinoGame.Dice)
        {
            if (rolls.Count < 2) return false;
            int a = rolls[0], b = rolls[1];
            int sum = a + b;
            return target switch
            {
                "high" => sum >= 8,
                "low" => sum <= 6,
                "seven" => sum == 7,
                "double" => a == b,
                "snakeeyes" => sum == 2,
                "boxcars" => sum == 12,
                _ => false,
            };
        }

        int pocket = rolls[0];
        return target switch
        {
            "red" => ColorOf(pocket) == PocketColor.Red,
            "black" => ColorOf(pocket) == PocketColor.Black,
            "even" => pocket != 0 && pocket % 2 == 0,
            "odd" => pocket % 2 == 1,
            "low" => pocket is >= 1 and <= 18,
            "high" => pocket is >= 19 and <= 36,
            "dozen1" => pocket is >= 1 and <= 12,
            "dozen2" => pocket is >= 13 and <= 24,
            "dozen3" => pocket is >= 25 and <= 36,
            _ => int.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out int wanted) && wanted == pocket,
        };
    }

    public static string RollText(CasinoGame game, IReadOnlyList<int> rolls)
    {
        if (game == CasinoGame.Dice)
        {
            if (rolls.Count < 2) return "?";
            return $"{rolls[0]}+{rolls[1]} = {rolls[0] + rolls[1]}";
        }

        if (rolls.Count == 0) return "?";
        int pocket = rolls[0];
        string color = ColorOf(pocket) switch
        {
            PocketColor.Red => "красное",
            PocketColor.Black => "чёрное",
            _ => "зеро",
        };
        return $"{pocket} ({color})";
    }

    public static string RulesText()
    {
        var sb = new StringBuilder();
        sb.AppendLine("КОСТИ (2d6):");
        sb.AppendLine("  high      — сумма >= 8, возврат x2      low       — сумма <= 6, возврат x2");
        sb.AppendLine("  seven     — ровно 7, возврат x5         double    — дубль, возврат x6");
        sb.AppendLine("  snakeeyes — 1+1, возврат x31            boxcars   — 6+6, возврат x31");
        sb.AppendLine("РУЛЕТКА (европейская, 0..36, одно зеро):");
        sb.AppendLine("  red/black/even/odd/low/high — x2        dozen1/dozen2/dozen3 — x3");
        sb.AppendLine("  точное число 0..36          — x36");
        sb.Append("Ставка списывается сразу, выигрыш начисляет хост новыми блоками того же типа.");
        return sb.ToString();
    }
}

/// <summary>Крупье: бросает кости/крутит рулетку и считает выплату.</summary>
public sealed class CasinoDealer
{
    private readonly IRandomSource _random;

    public CasinoDealer(IRandomSource? random = null) => _random = random ?? CryptoRandomSource.Shared;

    public int[] RollDice() => new[] { _random.NextInt(6) + 1, _random.NextInt(6) + 1 };

    public int[] RollRoulette() => new[] { _random.NextInt(CasinoRules.RoulettePockets) };

    /// <summary>Провести раунд. Ставка уже списана вызывающей стороной.</summary>
    public CasinoOutcome Spin(CasinoGame game, string target, int stake)
    {
        int[] rolls = game == CasinoGame.Dice ? RollDice() : RollRoulette();
        int multiplier = CasinoRules.Multiplier(game, target);
        bool won = multiplier > 0 && CasinoRules.Wins(game, target, rolls);

        return new CasinoOutcome
        {
            Game = game,
            Target = target,
            Stake = stake,
            Won = won,
            Multiplier = won ? multiplier : 0,
            Payout = won ? stake * multiplier : 0,
            Rolls = rolls,
            RollText = CasinoRules.RollText(game, rolls),
        };
    }
}

/// <summary>Что мы знаем об участнике сессии.</summary>
public sealed class PlayerRecord
{
    public PlayerRecord(ulong id, string name, Inventory inventory)
    {
        Id = id;
        Name = name;
        Inventory = inventory;
    }

    public ulong Id { get; }
    public string Name { get; set; }
    public Inventory Inventory { get; }
    public bool IsHost { get; set; }
    public bool Greeted { get; set; }

    /// <summary>Инвентарь уже подтверждён хостом: дальше хост — единственный источник истины.</summary>
    public bool InventoryReported { get; set; }
    public int RttMs { get; set; } = -1;
    public long LastSeenMs { get; set; }
    public long LastPingMs { get; set; }
    public int Wins { get; set; }
    public int Losses { get; set; }
    public long NetWorthDelta { get; set; }

    public double WinRate => Wins + Losses == 0 ? 0d : (double)Wins / (Wins + Losses);
}

/// <summary>
/// Слияние механик: креативный инвентарь + воксельный мир + азартный стол,
/// которые живут в одной сессии и синхронизируются Steam P2P-пакетами.
///
/// РОЛИ. Хост (владелец лобби) авторитетен: он проверяет ресурсы, бросает кости,
/// применяет правки мира и рассылает результат. Клиенты отправляют запросы и
/// применяют то, что пришло от хоста — расхождений не бывает даже если кто-то
/// подправит свою копию игры.
/// </summary>
public sealed class GambleCreativeSystem : IDisposable
{
    private readonly Dictionary<ulong, PlayerRecord> _players = new();
    private readonly FragmentAssembler _worldAssembler = new();
    private readonly FragmentAssembler _batchAssembler = new();
    private readonly TransferIdSource _transferIds = new();
    private readonly CasinoDealer _dealer;
    private readonly HashSet<ulong> _knownPeers = new();
    private readonly Action<ulong, byte[]> _packetHandler;

    private bool _wasHost;
    private bool _stateSentForCurrentHost;

    public GambleCreativeSystem(INetTransport transport, string localName, VoxelGrid world, Inventory? startingInventory = null, bool sandbox = false, IRandomSource? random = null)
    {
        Net = transport ?? throw new ArgumentNullException(nameof(transport));
        World = world ?? throw new ArgumentNullException(nameof(world));
        Sandbox = sandbox;
        LocalPlayerName = string.IsNullOrWhiteSpace(localName) ? "Игрок" : localName.Trim();
        _dealer = new CasinoDealer(random);

        _players[Net.LocalId] = new PlayerRecord(Net.LocalId, LocalPlayerName, startingInventory ?? CreateStartingInventory())
        {
            IsHost = Net.IsHost,
        };

        _wasHost = Net.IsHost;
        _packetHandler = OnRawPacket;
        Net.PacketReceived += _packetHandler;
    }

    public INetTransport Net { get; }

    public VoxelGrid World { get; }

    public bool Sandbox { get; }

    public string LocalPlayerName { get; }

    public IReadOnlyDictionary<ulong, PlayerRecord> Players => _players;

    /// <summary>Мой креатив-инвентарь (Dictionary&lt;string,int&gt; внутри).</summary>
    public Inventory LocalInventory => _players[Net.LocalId].Inventory;

    /// <summary>Строки статуса для консоли.</summary>
    public event Action<string>? Status;

    /// <summary>Раунд казино завершён — консоль показывает анимацию и результат.</summary>
    public event Action<CasinoResultMessage>? CasinoResolved;

    /// <summary>Правка мира применена локально (кем угодно) — рендер может обновиться.</summary>
    public event Action<VoxelEdit>? WorldEditApplied;

    /// <summary>Инвентарь какого-то игрока изменился (для HUD).</summary>
    public event Action<PlayerRecord>? InventoryChanged;

    public static Inventory CreateStartingInventory()
    {
        var inventory = new Inventory();
        inventory.Add(Blocks.DirtKey, 64);
        inventory.Add(Blocks.GrassKey, 32);
        inventory.Add(Blocks.StoneKey, 32);
        inventory.Add(Blocks.WoodKey, 24);
        inventory.Add(Blocks.GoldKey, 8);
        inventory.Add(Blocks.DiamondKey, 3);
        return inventory;
    }

    /// <summary>Цена крафта стола казино.</summary>
    public static readonly IReadOnlyDictionary<string, int> CasinoTableRecipe = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
    {
        [Blocks.DirtKey] = 16,
        [Blocks.WoodKey] = 8,
    };

    public void Dispose()
    {
        Net.PacketReceived -= _packetHandler;
    }

    // ────────────────────────────────────────────────────────────────────────
    #region Приём пакетов

    private void OnRawPacket(ulong from, byte[] payload)
    {
        NetMessage message;
        try
        {
            message = NetMessage.Deserialize(payload);
        }
        catch (ProtocolException ex)
        {
            Status?.Invoke($"⚠ Пакет от {ShortId(from)} отброшен: {ex.Message}");
            return;
        }

        try
        {
            HandleMessage(from, message);
        }
        catch (ProtocolException ex)
        {
            Status?.Invoke($"⚠ Содержимое пакета от {ShortId(from)} некорректно: {ex.Message}");
        }
    }

    private void HandleMessage(ulong from, NetMessage message)
    {
        PlayerRecord? sender = EnsurePlayer(from, null);

        switch (message)
        {
            case HelloMessage hello:
                OnHello(from, hello);
                break;

            case ChatMessage chat:
                OnChat(from, chat);
                break;

            case PingMessage ping:
                Net.SendTo(from, new PongMessage { Stamp = ping.Stamp, PeerStamp = NowMs() }.Serialize(), reliable: false);
                break;

            case PongMessage pong:
                if (sender is not null)
                {
                    sender.RttMs = (int)Math.Max(0, NowMs() - pong.Stamp);
                    sender.LastSeenMs = NowMs();
                }
                break;

            case SyncRequestMessage:
                if (Net.IsHost) SendFullStateTo(from);
                else RequestFullSync();
                break;

            case PresenceMessage presence:
                OnPresence(from, presence);
                break;

            case BlockRequestMessage block:
                if (Net.IsHost) HostHandleBlockRequest(from, block);
                break;

            case FillRequestMessage fill:
                if (Net.IsHost) HostHandleFillRequest(from, fill);
                break;

            case CraftRequestMessage craft:
                if (Net.IsHost) HostHandleCraftRequest(from, craft);
                break;

            case CasinoBetRequestMessage bet:
                if (Net.IsHost) HostHandleBetRequest(from, bet);
                break;

            case VoxelEditMessage edit:
                if (IsFromHost(from)) ApplyRemoteEdit(edit, announce: true);
                break;

            case VoxelBatchMessage batch:
                if (IsFromHost(from)) ApplyBatchFragment(from, batch);
                break;

            case VoxelSnapshotMessage snapshot:
                if (IsFromHost(from)) ApplySnapshotFragment(snapshot);
                break;

            case InventorySyncMessage inventory:
                ApplyInventorySync(from, inventory);
                break;

            case CasinoResultMessage result:
                if (IsFromHost(from) || result.PlayerId == Net.LocalId) ApplyCasinoResult(result);
                break;

            case ByeMessage bye:
                Status?.Invoke($"👋 {sender?.Name ?? ShortId(from)} вышел из сессии ({bye.Reason}).");
                _players.Remove(from);
                _knownPeers.Remove(from);
                break;
        }
    }

    private void OnHello(ulong from, HelloMessage hello)
    {
        PlayerRecord record = EnsurePlayer(from, hello.PlayerName) ?? throw new ProtocolException("Не удалось создать запись игрока.");
        record.Name = string.IsNullOrWhiteSpace(hello.PlayerName) ? ShortId(from) : hello.PlayerName.Trim();
        record.Greeted = true;
        record.LastSeenMs = NowMs();

        Status?.Invoke($"🤝 {record.Name} на связи ({ShortId(from)}), сборка «{hello.ClientTag}».");

        // Отвечаем приветствием, чтобы обе стороны знали имена.
        Net.SendTo(from, new HelloMessage
        {
            PlayerName = LocalPlayerName,
            ClientTag = BuildInfo.Tag,
            WantsFullState = false,
        }.Serialize(), reliable: true);

        if (Net.IsHost)
        {
            // Хост сразу отдаёт новичку мир и все инвентари.
            SendFullStateTo(from);
            BroadcastPresence(from, PresenceKind.Joined, record.Name);
        }
    }

    private void OnChat(ulong from, ChatMessage chat)
    {
        string text = chat.Text.Length > 512 ? chat.Text[..512] : chat.Text;
        if (Net.IsHost)
        {
            string name = _players.TryGetValue(from, out var record) ? record.Name : chat.SenderName;
            ChatMessage outgoing = new() { SenderName = name, Text = text };
            Broadcast(outgoing.Serialize(), reliable: true);
            Status?.Invoke($"💬 {name}: {text}");
        }
        else
        {
            Status?.Invoke($"💬 {chat.SenderName}: {text}");
        }
    }

    private void OnPresence(ulong from, PresenceMessage presence)
    {
        PlayerRecord? record = EnsurePlayer(presence.PlayerId, presence.PlayerName);
        if (record is not null)
        {
            record.Name = string.IsNullOrWhiteSpace(presence.PlayerName) ? ShortId(presence.PlayerId) : presence.PlayerName;
            record.IsHost = presence.IsHost;
            record.LastSeenMs = NowMs();
        }

        switch (presence.Kind)
        {
            case PresenceKind.Joined:
                Status?.Invoke($"➕ {presence.PlayerName} в лобби. {presence.Note}");
                break;
            case PresenceKind.Left:
                Status?.Invoke($"➖ {presence.PlayerName} покинул лобби. {presence.Note}");
                _players.Remove(presence.PlayerId);
                _knownPeers.Remove(presence.PlayerId);
                break;
            case PresenceKind.Denied:
                Status?.Invoke($"⛔ {presence.PlayerName}: {presence.Note}");
                break;
        }
    }

    private void ApplyInventorySync(ulong from, InventorySyncMessage message)
    {
        // Свои инвентарём распоряжается только хост (кроме первого self-report'а при входе).
        bool isAboutMe = message.PlayerId == Net.LocalId;
        if (isAboutMe && !message.IsSelfReport && !IsFromHost(from) && !Net.IsHost) return;
        if (!isAboutMe && !IsFromHost(from) && !Net.IsHost) return;

        PlayerRecord? record = EnsurePlayer(message.PlayerId, message.PlayerName);
        if (record is null) return;

        // Клиент может заявить о своём инвентаре только один раз — при входе в лобби.
        // Дальше количеством блоков распоряжается исключительно хост.
        if (Net.IsHost && message.IsSelfReport && record.InventoryReported) return;

        if (!string.IsNullOrWhiteSpace(message.PlayerName)) record.Name = message.PlayerName;
        record.Inventory.Load(message.Items);
        if (message.IsSelfReport) record.InventoryReported = true;
        record.LastSeenMs = NowMs();
        InventoryChanged?.Invoke(record);
    }

    private void ApplyCasinoResult(CasinoResultMessage result)
    {
        CasinoResolved?.Invoke(result);

        if (_players.TryGetValue(result.PlayerId, out var record))
        {
            if (result.Won)
            {
                record.Wins++;
                record.NetWorthDelta += result.Payout - result.Stake;
            }
            else
            {
                record.Losses++;
                record.NetWorthDelta -= result.Stake;
            }
        }

        if (result.PlayerId == Net.LocalId)
        {
            Status?.Invoke(result.Won
                ? $"🎲 {result.RollText} — {result.SummaryText()} Забрано {result.Payout} x {KeyOf(result.BlockKey)}."
                : $"🎲 {result.RollText} — {result.SummaryText()} Потеряно {result.Stake} x {KeyOf(result.BlockKey)}.");
        }
    }

    private void ApplyRemoteEdit(VoxelEditMessage edit, bool announce)
    {
        if (!Blocks.IsDefined(edit.Block)) throw new ProtocolException($"Неизвестный блок в правке: {edit.Block}.");
        var pos = new VoxelPos(edit.X, edit.Y, edit.Z);

        if (!World.Set(pos, (BlockId)edit.Block)) return; // вне мира или уже так — молча игнорируем

        WorldEditApplied?.Invoke(new VoxelEdit(pos, (BlockId)edit.Block, edit.Author, edit.AuthorName));
        if (announce && edit.Author != Net.LocalId)
        {
            string block = Blocks.Info((BlockId)edit.Block).Title;
            string what = edit.Block == (byte)BlockId.Air ? "сломал" : $"поставил {block}";
            Status?.Invoke($"🏗 {edit.AuthorName} {what} в {pos}.");
        }
    }

    /// <summary>Применить пачку правок к локальному миру (после сборки фрагментов).</summary>
    private int ApplyEdits(List<VoxelEdit> edits)
    {
        int applied = 0;
        foreach (VoxelEdit edit in edits)
        {
            if (!World.Set(edit.Pos, edit.Block)) continue;
            applied++;
            WorldEditApplied?.Invoke(edit);
        }

        return applied;
    }

    private void ApplyBatchFragment(ulong from, VoxelBatchMessage message)
    {
        byte[]? assembled = _batchAssembler.Add(message.BatchId, message.Index, message.Count, message.Payload, NowMs());
        if (assembled is null) return;

        List<VoxelEdit> edits = EditBatchCodec.Decode(assembled, message.Author, message.AuthorName);
        int applied = ApplyEdits(edits);
        if (applied > 0)
        {
            Status?.Invoke($"🏗 {message.AuthorName} изменил {applied} вокселей массовой командой.");
        }
    }

    private void ApplySnapshotFragment(VoxelSnapshotMessage message)
    {
        byte[]? assembled = _worldAssembler.Add(message.SyncId, message.Index, message.Count, message.Payload, NowMs());
        if (assembled is null) return;

        int cells = message.SizeX * message.SizeY * message.SizeZ;
        byte[] raw = VoxelCodec.Decode(assembled, cells);

        if (World.SizeX != message.SizeX || World.SizeY != message.SizeY || World.SizeZ != message.SizeZ)
        {
            World.Resize(message.SizeX, message.SizeY, message.SizeZ);
            Status?.Invoke($"🌍 Мир другого размера — перестроил сетку под {message.SizeX}x{message.SizeY}x{message.SizeZ}.");
        }

        World.FromBytes(raw);
        Status?.Invoke($"🌍 Получен снимок мира: {World.Describe()}, версия {World.Version}.");
    }

    #endregion

    // ────────────────────────────────────────────────────────────────────────
    #region Локальные действия игрока

    /// <summary>Поставить блок. На хосте применяется сразу, у клиента уходит запросом.</summary>
    public bool Place(int x, int y, int z, BlockId block, out string message)
    {
        message = string.Empty;
        if (block == BlockId.Air)
        {
            message = "Поставить воздух нельзя — для сноса есть команда break.";
            return false;
        }

        var pos = new VoxelPos(x, y, z);
        if (!World.InBounds(pos))
        {
            message = $"Вне мира. Допустимо: x 0..{World.SizeX - 1}, y 0..{World.SizeY - 1}, z 0..{World.SizeZ - 1}.";
            return false;
        }

        if (World.Get(pos) != BlockId.Air)
        {
            message = $"Клетка {pos} занята ({Blocks.Info(World.Get(pos)).Title}).";
            return false;
        }

        string key = Blocks.Info(block).Key;
        if (!Sandbox && !LocalInventory.Has(key, 1))
        {
            message = $"Нет блоков «{Blocks.Info(block).Title}» в креатив-инвентаре (есть {LocalInventory.Get(key)}).";
            return false;
        }

        if (Net.IsHost)
        {
            HostApplyBlock(Net.LocalId, pos, block);
            message = $"Поставлено: {Blocks.Info(block).Title} → {pos}.";
        }
        else
        {
            Net.SendTo(Net.HostId, new BlockRequestMessage { X = x, Y = y, Z = z, Block = (byte)block }.Serialize(), reliable: true);
            message = $"Запрос на установку {Blocks.Info(block).Title} → {pos} отправлен хосту.";
        }
        return true;
    }

    /// <summary>Сломать блок (вернуть его в инвентарь).</summary>
    public bool Break(int x, int y, int z, out string message)
    {
        message = string.Empty;
        var pos = new VoxelPos(x, y, z);
        if (!World.InBounds(pos))
        {
            message = "Вне мира.";
            return false;
        }

        BlockId current = World.Get(pos);
        if (current == BlockId.Air)
        {
            message = $"В {pos} и так пусто.";
            return false;
        }

        if (Net.IsHost)
        {
            HostApplyBlock(Net.LocalId, pos, BlockId.Air);
            message = $"Сломано: {Blocks.Info(current).Title} в {pos} (возврат +1).";
        }
        else
        {
            Net.SendTo(Net.HostId, new BlockRequestMessage { X = x, Y = y, Z = z, Block = (byte)BlockId.Air }.Serialize(), reliable: true);
            message = $"Запрос на снос {pos} отправлен хосту.";
        }
        return true;
    }

    /// <summary>Заполнить прямоугольный объём блоком (до 8192 вокселей). Стоит по одному блоку за воксель.</summary>
    public bool Fill(VoxelPos a, VoxelPos b, BlockId block, out string message)
    {
        message = string.Empty;
        if (block == BlockId.Air)
        {
            message = "Заполнить воздухом нельзя — для сноса есть break по конкретным клеткам.";
            return false;
        }

        int minX = Math.Max(0, Math.Min(a.X, b.X)), maxX = Math.Min(World.SizeX - 1, Math.Max(a.X, b.X));
        int minY = Math.Max(0, Math.Min(a.Y, b.Y)), maxY = Math.Min(World.SizeY - 1, Math.Max(a.Y, b.Y));
        int minZ = Math.Max(0, Math.Min(a.Z, b.Z)), maxZ = Math.Min(World.SizeZ - 1, Math.Max(a.Z, b.Z));
        long volume = (long)(maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1);

        if (volume <= 0)
        {
            message = "Объём за пределами мира.";
            return false;
        }

        if (volume > 8192)
        {
            message = $"Слишком большой объём: {volume} вокселей (максимум 8192 за раз).";
            return false;
        }

        var targets = new List<VoxelEdit>((int)volume);
        for (int y = minY; y <= maxY; y++)
        {
            for (int z = minZ; z <= maxZ; z++)
            {
                for (int x = minX; x <= maxX; x++)
                {
                    if (World.Get(x, y, z) != BlockId.Air) continue;
                    targets.Add(new VoxelEdit(new VoxelPos(x, y, z), block, Net.LocalId, LocalPlayerName));
                }
            }
        }

        if (targets.Count == 0)
        {
            message = "Заполнять нечего — все клетки уже заняты.";
            return false;
        }

        string key = Blocks.Info(block).Key;
        if (!Sandbox && !LocalInventory.Has(key, targets.Count))
        {
            message = $"Нужно {targets.Count} блоков «{Blocks.Info(block).Title}», а есть {LocalInventory.Get(key)}.";
            return false;
        }

        if (Net.IsHost)
        {
            HostApplyBatch(Net.LocalId, targets);
            message = $"Заполнено {targets.Count} вокселей блоком {Blocks.Info(block).Title}.";
        }
        else
        {
            Net.SendTo(Net.HostId, new FillRequestMessage
            {
                Block = (byte)block,
                MinX = minX, MinY = minY, MinZ = minZ,
                MaxX = maxX, MaxY = maxY, MaxZ = maxZ,
            }.Serialize(), reliable: true);
            message = $"Запрос на заливку {volume} клеток отправлен хосту.";
        }
        return true;
    }

    /// <summary>Скрафтить стол казино из земли и дерева (рецепт проверяет хост).</summary>
    public bool CraftTable(int count, out string message)
    {
        message = string.Empty;
        if (count is < 1 or > 16)
        {
            message = "Можно скрафтить от 1 до 16 столов за раз.";
            return false;
        }

        if (!CanAffordRecipe(LocalInventory, count, out string costText, out string missing))
        {
            message = $"Не хватает ресурсов: {missing}";
            return false;
        }

        if (Net.IsHost)
        {
            if (!HostCraft(Net.LocalId, count, out string error))
            {
                message = error;
                return false;
            }
            message = $"Скрафчено {count} x Стол казино (потрачено {costText}).";
        }
        else
        {
            Net.SendTo(Net.HostId, new CraftRequestMessage { Count = count }.Serialize(), reliable: true);
            message = $"Запрос на крафт {count} x Стол казино ({costText}) отправлен хосту.";
        }
        return true;
    }

    private static bool CanAffordRecipe(Inventory inventory, int count, out string costText, out string missing)
    {
        var parts = new List<string>();
        foreach (var pair in CasinoTableRecipe)
        {
            int need = pair.Value * count;
            string title = Blocks.TryParse(pair.Key, out BlockId id) ? Blocks.Info(id).Title : pair.Key;
            parts.Add($"{need} x {title}");
            if (inventory.Get(pair.Key) < need)
            {
                costText = string.Join(", ", parts);
                missing = $"нужно {need} x {title}, а есть {inventory.Get(pair.Key)}";
                return false;
            }
        }

        costText = string.Join(", ", parts);
        missing = string.Empty;
        return true;
    }

    /// <summary>Сделать ставку. Ставки — только блоками из инвентаря, цели считает хост.</summary>
    public bool Bet(CasinoGame game, string target, string blockKey, int amount, out string message)
    {
        message = string.Empty;

        if (amount is < 1 or > CasinoRules.MaxStake)
        {
            message = $"Ставка должна быть от 1 до {CasinoRules.MaxStake} блоков.";
            return false;
        }

        if (!Blocks.TryParse(blockKey, out BlockId block) || !Blocks.Info(block).IsCurrency)
        {
            message = $"Ставить можно только на ценные блоки ({string.Join(", ", Blocks.Currencies.Select(c => c.Key))}).";
            return false;
        }

        if (!CasinoRules.TryParseTarget(game, target, out string normalized, out string error))
        {
            message = error;
            return false;
        }

        if (World.Count(BlockId.CasinoTable) == 0)
        {
            message = "В мире нет стола казино. Скрафтите (craft table) и поставьте блок table.";
            return false;
        }

        if (!LocalInventory.Has(Blocks.Info(block).Key, amount))
        {
            message = $"Не хватает блоков: нужно {amount} x {Blocks.Info(block).Title}, есть {LocalInventory.Get(Blocks.Info(block).Key)}.";
            return false;
        }

        if (Net.IsHost)
        {
            CasinoResultMessage result = HostResolveBet(Net.LocalId, game, normalized, Blocks.Info(block).Key, amount);
            message = $"{result.RollText} — {(result.Won ? "выигрыш" : "проигрыш")}";
        }
        else
        {
            Net.SendTo(Net.HostId, new CasinoBetRequestMessage
            {
                Game = (byte)game,
                Target = normalized,
                BlockKey = Blocks.Info(block).Key,
                Amount = amount,
            }.Serialize(), reliable: true);
            message = $"Ставка отправлена хосту: {amount} x {Blocks.Info(block).Title} на «{normalized}» в {GameName(game)}.";
        }
        return true;
    }

    /// <summary>Чат сессии: у клиента уходит хосту, хост рассылает всем (включая себя).</summary>
    public void Chat(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return;
        string trimmed = text.Trim();
        if (trimmed.Length > 512) trimmed = trimmed[..512];

        if (Net.IsHost)
        {
            var message = new ChatMessage { SenderName = LocalPlayerName, Text = trimmed };
            Broadcast(message.Serialize(), reliable: true);
            Status?.Invoke($"💬 {LocalPlayerName}: {trimmed}");
        }
        else
        {
            Net.SendTo(Net.HostId, new ChatMessage { SenderName = LocalPlayerName, Text = trimmed }.Serialize(), reliable: true);
        }
    }

    /// <summary>Попросить у хоста полный снимок мира и инвентарей.</summary>
    public void RequestFullSync()
    {
        if (Net.IsHost)
        {
            BroadcastFullState();
            return;
        }
        Net.SendTo(Net.HostId, new SyncRequestMessage().Serialize(), reliable: true);
        Status?.Invoke("🔄 Запросил у хоста полную синхронизацию.");
    }

    #endregion

    // ────────────────────────────────────────────────────────────────────────
    #region Хост-логика (авторитет)

    private void HostHandleBlockRequest(ulong from, BlockRequestMessage request)
    {
        var pos = new VoxelPos(request.X, request.Y, request.Z);
        if (!Blocks.IsDefined(request.Block))
        {
            Status?.Invoke($"⚠ {ShortId(from)} запросил неизвестный блок {request.Block}.");
            return;
        }

        if (!World.InBounds(pos))
        {
            RejectTo(from, $"позиция {pos} вне мира");
            return;
        }

        var block = (BlockId)request.Block;
        if (block != BlockId.Air && World.Get(pos) != BlockId.Air)
        {
            RejectTo(from, $"клетка {pos} уже занята");
            return;
        }

        if (block == BlockId.Air && World.Get(pos) == BlockId.Air)
        {
            RejectTo(from, $"в {pos} уже пусто");
            return;
        }

        if (!Sandbox && block != BlockId.Air && !InventoryOf(from).Has(Blocks.Info(block).Key, 1))
        {
            RejectTo(from, $"нет блоков {Blocks.Info(block).Key}");
            return;
        }

        HostApplyBlock(from, pos, block);
    }

    private void HostHandleBetRequest(ulong from, CasinoBetRequestMessage request)
    {
        var game = (CasinoGame)request.Game;
        if (game is not (CasinoGame.Dice or CasinoGame.Roulette))
        {
            RejectTo(from, $"неизвестная игра {request.Game}");
            return;
        }

        if (request.Amount is < 1 or > CasinoRules.MaxStake)
        {
            RejectTo(from, $"некорректная ставка {request.Amount}");
            return;
        }

        if (!CasinoRules.TryParseTarget(game, request.Target, out string normalized, out string error))
        {
            RejectTo(from, error);
            return;
        }

        if (!Blocks.TryParse(request.BlockKey, out BlockId block) || !Blocks.Info(block).IsCurrency)
        {
            RejectTo(from, $"нельзя ставить блоком {request.BlockKey}");
            return;
        }

        if (World.Count(BlockId.CasinoTable) == 0)
        {
            RejectTo(from, "в мире нет стола казино");
            return;
        }

        string key = Blocks.Info(block).Key;
        if (!InventoryOf(from).Has(key, request.Amount))
        {
            RejectTo(from, $"у него только {InventoryOf(from).Get(key)} x {key}, а ставка {request.Amount}");
            return;
        }

        HostResolveBet(from, game, normalized, key, request.Amount);
    }

    private void HostHandleCraftRequest(ulong from, CraftRequestMessage request)
    {
        if (!HostCraft(from, request.Count, out string error)) RejectTo(from, error);
    }

    /// <summary>Хост валидирует рецепт, тратит ресурсы и начисляет столы казино.</summary>
    private bool HostCraft(ulong playerId, int count, out string error)
    {
        error = string.Empty;
        Inventory inventory = InventoryOf(playerId);

        if (count is < 1 or > 16)
        {
            error = $"некорректное количество столов: {count}";
            return false;
        }

        if (!CanAffordRecipe(inventory, count, out _, out string missing))
        {
            error = missing;
            return false;
        }

        foreach (var pair in CasinoTableRecipe) inventory.TrySpend(pair.Key, pair.Value * count);
        inventory.Add(Blocks.TableKey, count);
        HostBroadcastInventory(playerId);
        return true;
    }

    private void HostHandleFillRequest(ulong from, FillRequestMessage request)
    {
        if (!Blocks.IsDefined(request.Block) || request.Block == (byte)BlockId.Air)
        {
            RejectTo(from, "некорректный блок для заливки");
            return;
        }

        int minX = Math.Max(0, Math.Min(request.MinX, request.MaxX));
        int maxX = Math.Min(World.SizeX - 1, Math.Max(request.MinX, request.MaxX));
        int minY = Math.Max(0, Math.Min(request.MinY, request.MaxY));
        int maxY = Math.Min(World.SizeY - 1, Math.Max(request.MinY, request.MaxY));
        int minZ = Math.Max(0, Math.Min(request.MinZ, request.MaxZ));
        int maxZ = Math.Min(World.SizeZ - 1, Math.Max(request.MinZ, request.MaxZ));

        long volume = (long)(maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1);
        if (volume is <= 0 or > 8192)
        {
            RejectTo(from, $"некорректный объём заливки ({volume})");
            return;
        }

        var targets = new List<VoxelEdit>();
        for (int y = minY; y <= maxY; y++)
        {
            for (int z = minZ; z <= maxZ; z++)
            {
                for (int x = minX; x <= maxX; x++)
                {
                    if (World.Get(x, y, z) != BlockId.Air) continue;
                    targets.Add(new VoxelEdit(new VoxelPos(x, y, z), (BlockId)request.Block, from, string.Empty));
                }
            }
        }

        if (targets.Count == 0)
        {
            RejectTo(from, "заливать нечего — все клетки заняты");
            return;
        }

        string key = Blocks.Info((BlockId)request.Block).Key;
        if (!Sandbox && !InventoryOf(from).Has(key, targets.Count))
        {
            RejectTo(from, $"нужно {targets.Count} x {key}, а есть {InventoryOf(from).Get(key)}");
            return;
        }

        HostApplyBatch(from, targets);
    }

    /// <summary>Применить правку блока на хосте: списать/вернуть ресурс и разослать всем.</summary>
    private void HostApplyBlock(ulong author, VoxelPos pos, BlockId block)
    {
        BlockId previous = World.Get(pos);
        if (!World.Set(pos, block)) return;

        string authorName = NameOf(author);

        if (!Sandbox)
        {
            if (block != BlockId.Air)
            {
                InventoryOf(author).TrySpend(Blocks.Info(block).Key, 1);
            }
            else if (previous != BlockId.Air)
            {
                InventoryOf(author).Add(Blocks.Info(previous).Key, 1);
            }
            HostBroadcastInventory(author);
        }

        var edit = new VoxelEdit(pos, block, author, authorName);
        Broadcast(new VoxelEditMessage
        {
            X = pos.X,
            Y = pos.Y,
            Z = pos.Z,
            Block = (byte)block,
            Author = author,
            AuthorName = authorName,
        }.Serialize(), reliable: true);

        WorldEditApplied?.Invoke(edit);
    }

    private void HostApplyBatch(ulong author, List<VoxelEdit> edits)
    {
        string authorName = NameOf(author);
        var stamped = new List<VoxelEdit>(edits.Count);
        foreach (var edit in edits)
        {
            if (World.Set(edit.Pos, edit.Block)) stamped.Add(new VoxelEdit(edit.Pos, edit.Block, author, authorName));
        }

        if (stamped.Count == 0) return;

        if (!Sandbox)
        {
            string key = Blocks.Info(stamped[0].Block).Key;
            InventoryOf(author).TrySpend(key, stamped.Count);
            HostBroadcastInventory(author);
        }

        byte[] packed = EditBatchCodec.Encode(stamped);
        int count = Fragmenter.CountFor(packed.Length);
        int batchId = _transferIds.Next();
        for (int index = 0; index < count; index++)
        {
            var fragment = new VoxelBatchMessage
            {
                BatchId = batchId,
                Index = index,
                Count = count,
                Author = author,
                AuthorName = authorName,
                Payload = Fragmenter.Slice(packed, index),
            };
            Broadcast(fragment.Serialize(), reliable: true);
        }

        foreach (var edit in stamped) WorldEditApplied?.Invoke(edit);
    }

    /// <summary>Раунд казино: списать ставку, бросить, начислить выигрыш, разослать результат.</summary>
    private CasinoResultMessage HostResolveBet(ulong playerId, CasinoGame game, string target, string blockKey, int amount)
    {
        Inventory inventory = InventoryOf(playerId);
        string playerName = NameOf(playerId);

        if (!inventory.TrySpend(blockKey, amount))
        {
            // Хост — источник истины: если ставка не проходит, игрок узнаёт об этом и получает актуальный инвентарь.
            HostBroadcastInventory(playerId);
            return new CasinoResultMessage
            {
                PlayerId = playerId,
                PlayerName = playerName,
                Game = (byte)game,
                Target = target,
                BlockKey = blockKey,
                Stake = 0,
                Payout = 0,
                Won = false,
                Rolls = Array.Empty<byte>(),
                RollText = "ставка не принята (недостаточно блоков)",
            };
        }

        CasinoOutcome outcome = _dealer.Spin(game, target, amount);
        if (outcome.Won) inventory.Add(blockKey, outcome.Payout);

        var message = new CasinoResultMessage
        {
            PlayerId = playerId,
            PlayerName = playerName,
            Game = (byte)game,
            Target = target,
            BlockKey = blockKey,
            Stake = amount,
            Multiplier = outcome.Multiplier,
            Payout = outcome.Payout,
            Won = outcome.Won,
            Rolls = outcome.Rolls.Select(r => (byte)r).ToArray(),
            RollText = outcome.RollText,
        };

        ApplyCasinoResult(message);
        Broadcast(message.Serialize(), reliable: true);
        HostBroadcastInventory(playerId);
        return message;
    }

    /// <summary>Отказ клиенту: уходит адресно, чтобы игрок видел причину.</summary>
    private void RejectTo(ulong peer, string reason)
    {
        var presence = new PresenceMessage
        {
            Kind = PresenceKind.Denied,
            PlayerId = peer,
            PlayerName = NameOf(peer),
            IsHost = false,
            Note = $"хост отклонил: {reason}",
        };
        Net.SendTo(peer, presence.Serialize(), reliable: true);
    }

    #endregion

    // ────────────────────────────────────────────────────────────────────────
    #region Синхронизация состояния

    /// <summary>Отправить новичку (или по запросу) мир целиком и все инвентари.</summary>
    public void SendFullStateTo(ulong peer)
    {
        if (!Net.IsHost) return;

        byte[] packed = VoxelCodec.Encode(World.ToBytes());
        int count = Fragmenter.CountFor(packed.Length);
        int syncId = _transferIds.Next();

        for (int index = 0; index < count; index++)
        {
            var fragment = new VoxelSnapshotMessage
            {
                SyncId = syncId,
                Index = index,
                Count = count,
                Payload = Fragmenter.Slice(packed, index),
                SizeX = (ushort)World.SizeX,
                SizeY = (ushort)World.SizeY,
                SizeZ = (ushort)World.SizeZ,
            };
            Net.SendTo(peer, fragment.Serialize(), reliable: true);
        }

        foreach (var pair in _players)
        {
            var sync = BuildInventorySync(pair.Value, selfReport: false);
            Net.SendTo(peer, sync.Serialize(), reliable: true);
        }

        Status?.Invoke($"📤 Отправил {ShortId(peer)} мир ({count} фрагм., {packed.Length} байт) и {_players.Count} инвентарей.");
    }

    private void BroadcastFullState()
    {
        foreach (ulong peer in Net.Peers) SendFullStateTo(peer);
    }

    private InventorySyncMessage BuildInventorySync(bool selfReport)
    {
        PlayerRecord me = _players[Net.LocalId];
        return BuildInventorySync(me, selfReport);
    }

    private static InventorySyncMessage BuildInventorySync(PlayerRecord record, bool selfReport) => new()
    {
        PlayerId = record.Id,
        PlayerName = record.Name,
        IsSelfReport = selfReport,
        Items = record.Inventory.Snapshot(),
    };

    private void HostBroadcastInventory(ulong playerId)
    {
        if (!Net.IsHost) return;
        if (!_players.TryGetValue(playerId, out var record)) return;

        Broadcast(BuildInventorySync(record, selfReport: false).Serialize(), reliable: true);
        InventoryChanged?.Invoke(record);
    }

    #endregion

    // ────────────────────────────────────────────────────────────────────────
    #region Тик сессии

    /// <summary>
    /// Обслуживание сессии: состав лобби, рукопожатия, пинги, уборка зависших передач.
    /// Вызывать раз в кадр из главного цикла.
    /// </summary>
    public void Tick(long nowMs)
    {
        _worldAssembler.Prune(nowMs);
        _batchAssembler.Prune(nowMs);

        // Смена хоста (Steam передал владение лобби) — новый хост раздаёт состояние.
        bool isHost = Net.IsHost;
        if (isHost != _wasHost)
        {
            _wasHost = isHost;
            Status?.Invoke(isHost
                ? "👑 Вы стали хостом сессии (владение лобби перешло к вам)."
                : "🙋 Хост сессии — другой игрок.");
            _stateSentForCurrentHost = false;
        }

        if (isHost && !_stateSentForCurrentHost)
        {
            _stateSentForCurrentHost = true;
            BroadcastFullState();
        }

        if (!isHost && !_stateSentForCurrentHost && Net.Peers.Count > 0)
        {
            _stateSentForCurrentHost = true;
            Net.SendTo(Net.HostId, new HelloMessage
            {
                PlayerName = LocalPlayerName,
                ClientTag = BuildInfo.Tag,
                WantsFullState = true,
            }.Serialize(), reliable: true);
        }

        // Новые пиры: приветствие + self-report инвентаря (хост сразу узнаёт стартовый набор).
        foreach (ulong peer in Net.Peers)
        {
            if (!_knownPeers.Add(peer)) continue;

            Net.SendTo(peer, new HelloMessage
            {
                PlayerName = LocalPlayerName,
                ClientTag = BuildInfo.Tag,
                WantsFullState = Net.IsHost,
            }.Serialize(), reliable: true);

            if (!Net.IsHost) Net.SendTo(Net.HostId, BuildInventorySync(selfReport: true).Serialize(), reliable: true);
        }

        // Пропавшие пиры: чистим записи (Steam уже не держит сессию).
        var departed = _knownPeers.Where(p => !Net.Peers.Contains(p)).ToList();
        foreach (ulong peer in departed)
        {
            _knownPeers.Remove(peer);
            if (_players.TryGetValue(peer, out var record))
            {
                Status?.Invoke($"➖ {record.Name} пропал из лобби.");
                _players.Remove(peer);
            }
        }

        // Пинги раз в 2 секунды — HUD показывает задержку до друзей.
        foreach (ulong peer in Net.Peers)
        {
            PlayerRecord? record = EnsurePlayer(peer, null);
            if (record is null) continue;
            if (nowMs - record.LastPingMs < 2000) continue;

            record.LastPingMs = nowMs;
            Net.SendTo(peer, new PingMessage { Stamp = nowMs }.Serialize(), reliable: false);
        }
    }

    public void Leave(string reason)
    {
        var bye = new ByeMessage { Reason = reason };
        Broadcast(bye.Serialize(), reliable: true);
    }

    public void BroadcastPresence(ulong playerId, PresenceKind kind, string note)
    {
        if (!Net.IsHost) return;
        string name = _players.TryGetValue(playerId, out var record) ? record.Name : ShortId(playerId);
        Broadcast(new PresenceMessage
        {
            Kind = kind,
            PlayerId = playerId,
            PlayerName = name,
            IsHost = playerId == Net.LocalId,
            Note = note,
        }.Serialize(), reliable: true);
    }

    #endregion

    // ────────────────────────────────────────────────────────────────────────
    #region Утилиты

    private Inventory InventoryOf(ulong playerId) => EnsurePlayer(playerId, null) is { } record
        ? record.Inventory
        : throw new ProtocolException($"Нет записи игрока {playerId}.");

    private string NameOf(ulong playerId) => _players.TryGetValue(playerId, out var record) ? record.Name : ShortId(playerId);

    private bool IsFromHost(ulong from) => from == Net.HostId || from == Net.LocalId && Net.IsHost;

    private PlayerRecord? EnsurePlayer(ulong id, string? name)
    {
        if (_players.TryGetValue(id, out var existing))
        {
            if (!string.IsNullOrWhiteSpace(name)) existing.Name = name.Trim();
            existing.IsHost = id == Net.HostId;
            return existing;
        }

        if (id == 0) return null;

        var created = new PlayerRecord(id, string.IsNullOrWhiteSpace(name) ? ShortId(id) : name.Trim(), new Inventory())
        {
            IsHost = id == Net.HostId,
            LastSeenMs = NowMs(),
        };
        _players[id] = created;
        return created;
    }

    private static string ShortId(ulong id) => BuildInfo.ShortId(id);

    private static string KeyOf(string blockKey) => Blocks.TryParse(blockKey, out BlockId id) ? Blocks.Info(id).Title : blockKey;

    private static string GameName(CasinoGame game) => game == CasinoGame.Dice ? "костях" : "рулетке";

    private static long NowMs() => Environment.TickCount64;

    /// <summary>
    /// Рассылка всем участникам лобби. Net.Peers не содержит нас самих,
    /// поэтому локальное применение делается отдельно — эха не требуется.
    /// </summary>
    private void Broadcast(byte[] payload, bool reliable)
    {
        foreach (ulong peer in Net.Peers)
        {
            Net.SendTo(peer, payload, reliable);
        }
    }

    /// <summary>Хост должен чистить состояние, когда сессия закрыта.</summary>
    public void ResetSessionState()
    {
        _knownPeers.Clear();
        _stateSentForCurrentHost = false;
        _worldAssembler.Reset();
        _batchAssembler.Reset();
    }

    /// <summary>Текст для команды status/roster.</summary>
    public string RosterText()
    {
        var sb = new StringBuilder();
        sb.AppendLine("УЧАСТНИКИ СЕССИИ:");
        foreach (var record in _players.Values.OrderByDescending(p => p.IsHost).ThenBy(p => p.Name, StringComparer.OrdinalIgnoreCase))
        {
            string role = record.IsHost ? "хост" : "клиент";
            string rtt = record.RttMs >= 0 ? $"{record.RttMs} мс" : "—";
            string mark = record.Id == Net.LocalId ? " (вы)" : string.Empty;
            sb.Append("  • ").Append(record.Name).Append(mark)
              .Append(" — ").Append(role)
              .Append(", SteamID ").Append(record.Id.ToString(CultureInfo.InvariantCulture))
              .Append(", пинг ").Append(rtt)
              .Append(", ставок: ").Append((record.Wins + record.Losses).ToString(CultureInfo.InvariantCulture))
              .Append(" (побед ").Append(record.Wins.ToString(CultureInfo.InvariantCulture)).Append(')')
              .AppendLine();
            sb.Append("     инвентарь: ").AppendLine(record.Inventory.Describe());
        }
        return sb.ToString();
    }

    #endregion
}
