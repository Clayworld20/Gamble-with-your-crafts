using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace GambleWithYourCrafts;

/// <summary>Точка входа: разбор параметров, запуск сессии, консольный цикл.</summary>
public static class Program
{
    public static int Main(string[] args)
    {
        LaunchOptions options = LaunchOptions.Parse(args);

        if (options.ShowHelp)
        {
            Console.WriteLine(LaunchOptions.HelpText());
            return 0;
        }

        if (!string.IsNullOrEmpty(options.Error))
        {
            Console.Error.WriteLine(options.Error);
            return 2;
        }

        // Тесты логики не требуют Steam и клиента — можно гонять в CI.
        if (options.SelfTest) return SelfTest.RunAll();

        Terminal.Initialize(options.NoColor, options.Verbose);
        AppCore.Initialize(options.AppId, string.IsNullOrWhiteSpace(options.PlayerName) ? null : options.PlayerName);

        Terminal.Banner(AppCore.SteamReady ? "сетевая сессия готова" : "одиночный режим");

        var app = new GameApp(options, AppCore.SteamReady);
        try
        {
            app.Run();
            return 0;
        }
        catch (Exception ex)
        {
            Log.Error($"Критическая ошибка: {ex}");
            return 1;
        }
        finally
        {
            app.Dispose();
        }
    }
}

/// <summary>
/// Игровая оболочка: держит мир, сессию, лобби и консольный цикл.
/// Все игровые события выполняются на главном потоке (Steam-callback'и качаются здесь же),
/// ввод читается отдельным потоком и только складывается в очередь команд.
/// </summary>
internal sealed class GameApp : IDisposable
{
    private readonly ConcurrentQueue<string> _commands = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly LaunchOptions _options;
    private readonly P2PLobbyManager? _lobby;
    private readonly INetTransport _transport;
    private readonly GambleCreativeSystem _session;
    private readonly VoxelGrid _world;
    private readonly bool _steamReady;

    private VoxelPos? _cursor;
    private int _mapHeight;
    private bool _running = true;
    private bool _stdinClosed;

    public GameApp(LaunchOptions options, bool steamReady)
    {
        _options = options;
        _steamReady = steamReady;

        _world = new VoxelGrid(options.WorldX, options.WorldY, options.WorldZ);
        List<VoxelEdit> preset = VoxelWorldPresets.BuildStarterArena(_world);

        if (steamReady)
        {
            _lobby = new P2PLobbyManager();
            _lobby.Log += message => Log.Info(message);
            _lobby.LobbyChatReceived += (id, name, text) => Log.Info($"💬 (чат лобби Steam) {name}: {text}");
            _transport = _lobby;
        }
        else
        {
            _transport = new OfflineTransport();
        }

        _session = new GambleCreativeSystem(_transport, AppCore.PlayerName, _world, sandbox: options.Sandbox);
        _session.Status += message => Log.Info(message);
        _session.CasinoResolved += OnCasinoResolved;

        _mapHeight = Math.Max(1, Math.Min(_world.SizeY - 1, 2));

        Log.Ok($"Мир готов: {_world.Describe()}, стартовая арена из {preset.Count} вокселей.");
        Log.Info($"Креатив-инвентарь: {_session.LocalInventory.Describe()}");
        Log.Info($"Команда help — список команд. Быстрый старт: craft table → place <x> <y> <z> table → bet.");
    }

    public void Run()
    {
        InstallCancellationHandlers();

        var input = new Thread(ReadInputLoop) { IsBackground = true, Name = "console-input" };
        input.Start();

        Task? boot = BootstrapLobbyAsync();

        const int ticksPerSecond = 50;
        const int tickMs = 1000 / ticksPerSecond;

        while (_running)
        {
            long now = Environment.TickCount64;

            // 1. Прокачиваем callback'и Steam (события лобби и P2P-сессии приходят на этом потоке).
            AppCore.Tick();

            // 2. Транспорт: отложенные действия, состав лобби, чтение P2P-пакетов.
            _lobby?.Pump();

            // 3. Логика сессии: рукопожатия, пинги, синхронизация.
            _session.Tick(now);

            // 4. Команды игрока.
            while (_commands.TryDequeue(out string? line))
            {
                HandleCommand(line);
            }

            // 5. Сон до следующего тика (не жжём CPU).
            long elapsed = Environment.TickCount64 - now;
            int sleep = (int)Math.Max(1, tickMs - elapsed);
            Thread.Sleep(sleep);
        }

        Log.Info("Завершение: рассылаю прощание и выхожу из лобби...");
        try
        {
            _session.Leave("игра закрыта");
        }
        catch (Exception ex)
        {
            Log.Debug($"Не удалось разослать прощание: {ex.Message}");
        }

        try
        {
            boot?.Wait(TimeSpan.FromSeconds(2));
        }
        catch (Exception ex)
        {
            Log.Debug($"Ошибка запуска лобби: {ex.Message}");
        }
    }

    private void InstallCancellationHandlers()
    {
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            Log.Warn("Ctrl+C — выходим.");
            _running = false;
        };

        AppDomain.CurrentDomain.ProcessExit += (_, _) =>
        {
            _running = false;
        };
    }

    private async Task BootstrapLobbyAsync()
    {
        if (!_steamReady)
        {
            Log.Warn("Steam недоступен: доступны стройка, инвентарь и казино, но сетевые команды отключены.");
            return;
        }

        if (_options.BrowseOnStart && _lobby is not null) await _lobby.BrowseLobbiesAsync();

        if (_options.JoinLobbyId is ulong lobbyId && lobbyId != 0)
        {
            Log.Steam($"Подключаемся к лобби {lobbyId} (параметр --join/+connect_lobby).");
            if (_lobby is not null) await _lobby.JoinLobbyAsync(lobbyId);
            return;
        }

        if (_options.HostOnStart && _lobby is not null)
        {
            await _lobby.HostLobbyAsync(_options.Visibility, _options.MaxMembers);
            if (_lobby.InLobby) PrintInviteHint();
        }
    }

    private void ReadInputLoop()
    {
        while (_running)
        {
            string? line;
            try
            {
                line = Console.ReadLine();
            }
            catch (IOException)
            {
                break;
            }

            if (line is null)
            {
                _stdinClosed = true;
                return;
            }

            _commands.Enqueue(line);
        }
    }

    // ── Команды ──────────────────────────────────────────────────────────────

    private void HandleCommand(string rawLine)
    {
        List<string> parts = CommandLine.Split(rawLine);
        if (parts.Count == 0) return;

        string command = parts[0].ToLowerInvariant();
        try
        {
            switch (command)
            {
                case "help" or "?" or "помощь":
                    PrintHelp();
                    break;

                case "status" or "статус":
                    PrintStatus();
                    break;

                case "inv" or "inventory" or "инвентарь":
                    Log.Ok($"Инвентарь: {_session.LocalInventory.Describe()}");
                    Log.Info($"Блоков: {_session.LocalInventory.TotalBlocks}, богатство: {_session.LocalInventory.Wealth} (в единицах блоков).");
                    if (_session.Sandbox) Log.Warn("Включён режим --sandbox: блоки на стройку не тратятся.");
                    break;

                case "blocks" or "блоки":
                    Log.Info("Каталог блоков:\n" + Blocks.CatalogText());
                    break;

                case "host" or "создать":
                    HostLobby(parts);
                    break;

                case "join" or "подключиться":
                    JoinLobby(parts);
                    break;

                case "browse" or "список":
                    BrowseLobbies();
                    break;

                case "invite" or "пригласить":
                    InviteFriend(parts);
                    break;

                case "leave" or "выйти":
                    LeaveLobby();
                    break;

                case "roster" or "состав":
                    Log.Info(_session.RosterText());
                    if (_lobby is not null) Log.Info(_lobby.StatusText());
                    break;

                case "say" or "чат":
                    Say(parts);
                    break;

                case "place" or "поставить":
                    PlaceBlock(parts);
                    break;

                case "break" or "сломать":
                    BreakBlock(parts);
                    break;

                case "fill" or "залить":
                    FillBlocks(parts);
                    break;

                case "craft" or "крафт":
                    CraftTable(parts);
                    break;

                case "bet" or "ставка":
                    Bet(parts);
                    break;

                case "rules" or "правила":
                    Log.Info(CasinoRules.RulesText());
                    break;

                case "map" or "карта":
                    ShowMap(parts);
                    break;

                case "iso" or "изометрия":
                    ShowIso(parts);
                    break;

                case "look" or "навести":
                    Look(parts);
                    break;

                case "find" or "найти":
                    FindBlock(parts);
                    break;

                case "top" or "рейтинг":
                    PrintLeaderboard();
                    break;

                case "sync" or "синхронизация":
                    _session.RequestFullSync();
                    break;

                case "leave-lobby" or "покинуть":
                    LeaveLobby();
                    break;

                case "quit" or "exit" or "выход":
                    _running = false;
                    break;

                default:
                    Log.Warn($"Неизвестная команда «{command}». Наберите help.");
                    break;
            }
        }
        catch (Exception ex)
        {
            Log.Error($"Команда «{command}» упала: {ex.Message}");
        }
    }

    private void PrintHelp()
    {
        var sb = new StringBuilder();
        sb.AppendLine("СЕССИЯ");
        sb.AppendLine("  host [public|friends|private]  создать лобби (по умолчанию friends)");
        sb.AppendLine("  join <lobbyId> | browse        подключиться по ID / посмотреть публичные лобби");
        sb.AppendLine("  invite [имя или SteamID]       оверлей Steam со списком друзей (или пригласить конкретного)");
        sb.AppendLine("  roster | status                кто в лобби, роли, пинг, инвентари");
        sb.AppendLine("  say <текст> | leave | sync     чат сессии / выйти из лобби / пересинхронизация");
        sb.AppendLine("СТРОЙКА (креатив)");
        sb.AppendLine("  place <x> <y> <z> <блок>       поставить блок из инвентаря");
        sb.AppendLine("  break <x> <y> <z>              сломать блок (вернётся в инвентарь)");
        sb.AppendLine("  fill <блок> <x1 y1 z1> <x2 y2 z2>   залить объём (до 8192 вокселей)");
        sb.AppendLine("  inv | blocks                    инвентарь / каталог блоков");
        sb.AppendLine("  map [y] [радиус] | iso [радиус] | look <x> <z> | find <блок>");
        sb.AppendLine("КАЗИНО");
        sb.AppendLine("  craft [n]                      скрафтить n столов казино (16 земли + 8 дерева за стол)");
        sb.AppendLine("  bet <блок> <количество> <цель> [dice|roulette]");
        sb.AppendLine("      примеры: bet gold 4 red   |   bet dirt 10 high dice   |   bet diamond 1 17");
        sb.AppendLine("  rules | top                    правила и таблица выплат / рейтинг игроков");
        sb.AppendLine("ПРОЧЕЕ: help, quit");
        sb.Append("Подсказка: цель по умолчанию — рулетка; допишите «dice» в конце для игры в кости.");
        Log.Info(sb.ToString());
    }

    private void PrintStatus()
    {
        Log.Info($"Игрок: {AppCore.PlayerName} ({BuildInfo.ShortId(AppCore.PlayerId)}), сборка {BuildInfo.Tag}");
        Log.Info(_steamReady
            ? $"Steam: AppID {AppCore.AppId}, оверлей и P2P доступны."
            : "Steam недоступен — одиночный режим.");
        if (_lobby is not null) Log.Info(_lobby.StatusText());
        Log.Info(_world.Describe() + $", столов казино: {_world.Count(BlockId.CasinoTable)}.");
        Log.Info($"Инвентарь: {_session.LocalInventory.Describe()}");
        Log.Info($"Ставок сыграно: {_session.Players.Values.Sum(p => p.Wins + p.Losses)}, побед: {_session.Players.Values.Sum(p => p.Wins)}.");
    }

    private void PrintLeaderboard()
    {
        var players = _session.Players.Values
            .Where(p => p.Wins + p.Losses > 0)
            .OrderByDescending(p => p.NetWorthDelta)
            .ThenByDescending(p => p.Wins)
            .ToList();

        if (players.Count == 0)
        {
            Log.Info("Ещё никто не играл — сделайте первую ставку (bet).");
            return;
        }

        var sb = new StringBuilder("ТАБЛИЦА ИГРОКОВ (по чистому выигрышу в блоках):\n");
        int place = 1;
        foreach (var player in players)
        {
            string sign = player.NetWorthDelta >= 0 ? "+" : string.Empty;
            sb.Append("  ").Append(place++).Append(". ").Append(player.Name.PadRight(18))
              .Append(" баланс ").Append(sign).Append(player.NetWorthDelta.ToString(CultureInfo.InvariantCulture))
              .Append(", ставок ").Append((player.Wins + player.Losses).ToString(CultureInfo.InvariantCulture))
              .Append(", побед ").Append(player.Wins.ToString(CultureInfo.InvariantCulture))
              .Append(", винрейт ").Append(Fmt.Pct(player.WinRate))
              .AppendLine();
        }
        Log.Info(sb.ToString());
    }

    // ── Лобби ────────────────────────────────────────────────────────────────

    private void HostLobby(List<string> parts)
    {
        if (_lobby is null)
        {
            Log.Warn("Steam недоступен — лобби не создать.");
            return;
        }

        LobbyVisibility visibility = _options.Visibility;
        if (parts.Count > 1)
        {
            visibility = parts[1].ToLowerInvariant() switch
            {
                "public" or "открытое" => LobbyVisibility.Public,
                "friends" or "друзья" => LobbyVisibility.FriendsOnly,
                "private" or "приватное" => LobbyVisibility.Private,
                _ => visibility,
            };
        }

        _ = HostLobbyInternalAsync(visibility);
    }

    private async Task HostLobbyInternalAsync(LobbyVisibility visibility)
    {
        if (_lobby is null) return;

        await _lobby.HostLobbyAsync(visibility, _options.MaxMembers);
        if (_lobby.InLobby) PrintInviteHint();
        _session.RequestFullSync();
    }

    private void PrintInviteHint()
    {
        Log.Ok("Лобби готово. Пригласите друзей:");
        Log.Info("  1) команда invite — откроется официальный оверлей Steam со списком друзей;");
        Log.Info("  2) либо отправьте другу ID лобби (он введёт join <id>);");
        Log.Info("  3) либо включите видимость public и попросите друга выполнить browse.");
        Log.Info($"Ваш SteamID для отладки: {AppCore.PlayerId} ({BuildInfo.ShortId(AppCore.PlayerId)}).");
    }

    private void JoinLobby(List<string> parts)
    {
        if (_lobby is null)
        {
            Log.Warn("Steam недоступен — подключение к лобби невозможно.");
            return;
        }

        if (parts.Count < 2)
        {
            Log.Warn("Укажите ID лобби: join 109775242899999999");
            return;
        }

        if (!ulong.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong lobbyId))
        {
            Log.Warn("ID лобби — это число (SteamID лобби). Его печатает хост после команды host.");
            return;
        }

        _ = _lobby.JoinLobbyAsync(lobbyId);
    }

    private void BrowseLobbies()
    {
        if (_lobby is null)
        {
            Log.Warn("Steam недоступен — список лобби недоступен.");
            return;
        }

        _ = _lobby.BrowseLobbiesAsync();
    }

    private void InviteFriend(List<string> parts)
    {
        if (_lobby is null || !_lobby.InLobby)
        {
            Log.Warn("Сначала создайте лобби (host) или войдите в него (join).");
            return;
        }

        if (parts.Count == 1)
        {
            _lobby.OpenInviteOverlay();
            return;
        }

        string query = string.Join(' ', parts.Skip(1));
        if (_lobby.InviteFriend(query, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private void LeaveLobby()
    {
        if (_lobby is null || !_lobby.InLobby)
        {
            Log.Warn("Вы не в лобби.");
            return;
        }

        _session.Leave("выход из лобби");
        _lobby.LeaveLobby();
        _session.ResetSessionState();
    }

    // ── Стройка ──────────────────────────────────────────────────────────────

    private void PlaceBlock(List<string> parts)
    {
        if (parts.Count < 5)
        {
            Log.Warn("Формат: place <x> <y> <z> <блок>, например place 24 3 24 gold");
            return;
        }

        if (!TryParseCoords(parts, 1, out VoxelPos pos)) return;

        if (!Blocks.TryParse(parts[4], out BlockId block) || block == BlockId.Air)
        {
            Log.Warn($"Неизвестный блок «{parts[4]}». Доступно: {string.Join(", ", Blocks.Placeable.Select(b => b.Key))}.");
            return;
        }

        if (_session.Place(pos.X, pos.Y, pos.Z, block, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private void BreakBlock(List<string> parts)
    {
        if (parts.Count < 4)
        {
            Log.Warn("Формат: break <x> <y> <z>");
            return;
        }

        if (!TryParseCoords(parts, 1, out VoxelPos pos)) return;

        if (_session.Break(pos.X, pos.Y, pos.Z, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private void FillBlocks(List<string> parts)
    {
        if (parts.Count < 9)
        {
            Log.Warn("Формат: fill <блок> <x1> <y1> <z1> <x2> <y2> <z2>");
            return;
        }

        if (!Blocks.TryParse(parts[1], out BlockId block))
        {
            Log.Warn($"Неизвестный блок «{parts[1]}».");
            return;
        }

        if (!TryParseCoords(parts, 2, out VoxelPos a) || !TryParseCoords(parts, 5, out VoxelPos b)) return;

        if (_session.Fill(a, b, block, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private void CraftTable(List<string> parts)
    {
        int count = 1;
        if (parts.Count > 1 && !int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out count))
        {
            Log.Warn("Формат: craft [количество]");
            return;
        }

        if (_session.CraftTable(count, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private bool TryParseCoords(List<string> parts, int startIndex, out VoxelPos pos)
    {
        pos = default;
        if (parts.Count < startIndex + 3)
        {
            Log.Warn("Нужны три координаты: x y z.");
            return false;
        }

        if (!int.TryParse(parts[startIndex], NumberStyles.Integer, CultureInfo.InvariantCulture, out int x) ||
            !int.TryParse(parts[startIndex + 1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int y) ||
            !int.TryParse(parts[startIndex + 2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int z))
        {
            Log.Warn($"Координаты должны быть целыми числами: «{parts[startIndex]} {parts[startIndex + 1]} {parts[startIndex + 2]}».");
            return false;
        }

        pos = new VoxelPos(x, y, z);
        return true;
    }

    // ── Казино ───────────────────────────────────────────────────────────────

    private void Bet(List<string> parts)
    {
        if (parts.Count < 4)
        {
            Log.Warn("Формат: bet <блок> <количество> <цель> [dice|roulette], например bet gold 4 red");
            Log.Info("Цели рулетки: red, black, even, odd, low, high, dozen1..dozen3, 0..36.");
            Log.Info("Цели костей: high (>=8), low (<=6), seven, double, snakeeyes, boxcars (добавьте «dice»).");
            return;
        }

        string blockKey = parts[1];
        if (!int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int amount))
        {
            Log.Warn($"Количество должно быть числом, а не «{parts[2]}».");
            return;
        }

        CasinoGame game = CasinoGame.Roulette;
        string target = parts[3];
        if (parts.Count > 4 && TryParseGame(parts[4], out CasinoGame explicitGame)) game = explicitGame;

        // «bet gold 4 dice high» — угадаем порядок аргументов, если игрок написал игру перед целью.
        if (parts.Count > 4 && !TryParseGame(parts[4], out _) && TryParseGame(target, out CasinoGame swapped))
        {
            game = swapped;
            target = parts[4];
        }

        if (_session.Bet(game, target, blockKey, amount, out string message)) Log.Ok(message);
        else Log.Warn(message);
    }

    private static bool TryParseGame(string text, out CasinoGame game)
    {
        switch (text.Trim().ToLowerInvariant())
        {
            case "dice" or "кости" or "кубики":
                game = CasinoGame.Dice;
                return true;
            case "roulette" or "рулетка":
                game = CasinoGame.Roulette;
                return true;
            default:
                game = CasinoGame.Roulette;
                return false;
        }
    }

    /// <summary>Короткая анимация для того, кто ставил: саспенс перед оглашением результата.</summary>
    private void OnCasinoResolved(CasinoResultMessage result)
    {
        bool isMine = result.PlayerId == AppCore.PlayerId || (!_steamReady && result.PlayerId == 0);
        if (!isMine) return;

        if (result.Game == (byte)CasinoGame.Dice) AnimateDice(result);
        else AnimateRoulette(result);
    }

    private void AnimateDice(CasinoResultMessage result)
    {
        if (result.Rolls.Length >= 2 && result.Stake > 0 && !Console.IsOutputRedirected)
        {
            var random = new Random();
            for (int frame = 0; frame < 3; frame++)
            {
                int a = random.Next(6) + 1;
                int b = random.Next(6) + 1;
                Terminal.Write($"\r  🎲 бросаем: [{a}] [{b}] …   ");
                Thread.Sleep(110);
            }
            Terminal.Write("\r" + new string(' ', 32) + "\r");
        }

        string target = DescribeTarget(CasinoGame.Dice, result.Target);
        string mood = result.Won ? $"ВЫИГРЫШ: +{result.Payout} (x{result.Multiplier})" : $"ставка сгорела: -{result.Stake}";
        ConsoleColor color = result.Stake == 0 ? ConsoleColor.Yellow : (result.Won ? ConsoleColor.Green : ConsoleColor.DarkRed);
        Terminal.WriteLine($"  🎲 Кости: {result.RollText} на «{target}» → {mood}.", color);
    }

    private void AnimateRoulette(CasinoResultMessage result)
    {
        if (result.Stake > 0 && !Console.IsOutputRedirected)
        {
            var random = new Random();
            for (int frame = 0; frame < 3; frame++)
            {
                Terminal.Write($"\r  🎡 шарик по кругу… ({random.Next(37):00})");
                Thread.Sleep(110);
            }
            Terminal.Write("\r" + new string(' ', 32) + "\r");
        }

        string target = DescribeTarget(CasinoGame.Roulette, result.Target);
        string mood = result.Stake == 0
            ? "ставка не принята"
            : (result.Won ? $"ВЫИГРЫШ: +{result.Payout} (x{result.Multiplier})" : $"ставка сгорела: -{result.Stake}");
        ConsoleColor color = result.Stake == 0 ? ConsoleColor.Yellow : (result.Won ? ConsoleColor.Green : ConsoleColor.DarkRed);
        Terminal.WriteLine($"  🎡 Рулетка: выпало {result.RollText} на «{target}» → {mood}.", color);
    }

    private static string DescribeTarget(CasinoGame game, string target)
    {
        if (game == CasinoGame.Dice) return target;
        if (int.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out int pocket))
        {
            string color = CasinoRules.ColorOf(pocket) switch
            {
                CasinoRules.PocketColor.Red => "красное",
                CasinoRules.PocketColor.Black => "чёрное",
                _ => "зеро",
            };
            return $"число {pocket} ({color})";
        }
        return target;
    }

    // ── Просмотр мира ────────────────────────────────────────────────────────

    private void ShowMap(List<string> parts)
    {
        int height = _mapHeight;
        if (parts.Count > 1 && int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed)) height = parsed;

        int radius = 0;
        if (parts.Count > 2 && int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsedRadius)) radius = parsedRadius;

        VoxelPos center = _cursor ?? _world.Center;
        Terminal.Write(Renderer.Slice(_world, height, radius, center.X, center.Z));
        Terminal.WriteLine();
    }

    private void ShowIso(List<string> parts)
    {
        int radius = 12;
        if (parts.Count > 1 && int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed)) radius = parsed;

        VoxelPos center = _cursor ?? _world.Center;
        Terminal.Write(Renderer.Isometric(_world, center, radius));
        Terminal.WriteLine();
    }

    private void Look(List<string> parts)
    {
        if (parts.Count < 3 || !TryParseCoords(new List<string> { parts[0], parts[1], parts[2], "0" }, 1, out VoxelPos pos))
        {
            Log.Warn("Формат: look <x> <z> — точка, вокруг которой рисуются map/iso.");
            return;
        }

        _cursor = new VoxelPos(pos.X, _mapHeight, pos.Z);
        Log.Info($"Курсор обзора: {_cursor.Value}. map/iso теперь показывают окрестности этой точки.");
    }

    private void FindBlock(List<string> parts)
    {
        if (parts.Count < 2)
        {
            Log.Warn("Формат: find <блок>, например find table");
            return;
        }

        if (!Blocks.TryParse(parts[1], out BlockId block))
        {
            Log.Warn($"Неизвестный блок «{parts[1]}».");
            return;
        }

        var found = new List<VoxelPos>();
        foreach (VoxelEdit edit in _world.NonAir())
        {
            if (edit.Block != block) continue;
            found.Add(edit.Pos);
            if (found.Count >= 12) break;
        }

        if (found.Count == 0)
        {
            Log.Warn($"Блоков «{Blocks.Info(block).Title}» в мире нет.");
            return;
        }

        Log.Ok($"Найдено {_world.Count(block)} x {Blocks.Info(block).Title}. Первые: {string.Join("; ", found)}");
    }

    private void Say(List<string> parts)
    {
        if (parts.Count < 2)
        {
            Log.Warn("Формат: say <текст>");
            return;
        }

        string text = string.Join(' ', parts.Skip(1));
        _session.Chat(text);
        if (!_steamReady || _lobby is null || !_lobby.InLobby)
        {
            Log.Info($"💬 {AppCore.PlayerName}: {text}");
        }
    }

    public void Dispose()
    {
        _session.Dispose();
        _lobby?.Dispose();
        AppCore.Shutdown();
        GC.SuppressFinalize(this);
    }
}
