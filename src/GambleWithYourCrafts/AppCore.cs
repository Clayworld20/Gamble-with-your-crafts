using System.Globalization;
using System.Reflection;
using Steamworks;

namespace GambleWithYourCrafts;

/// <summary>Сборка и протокол: используется в логах, лобби-данных и проверке совместимости.</summary>
public static class BuildInfo
{
    /// <summary>Человекочитаемая версия приложения.</summary>
    public static string Version { get; } =
        Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.1.0";

    /// <summary>Полная метка сборки: версия + версия протокола P2P.</summary>
    public static string Tag { get; } = $"{Version}+p{NetMessage.ProtocolVersion}";

    /// <summary>Короткое представление SteamID для логов: аккаунт + полный id.</summary>
    public static string ShortId(ulong steamId)
    {
        if (steamId == 0) return "локально";
        uint account = (uint)(steamId & 0xFFFFFFFFul);
        return $"акк.{account.ToString(CultureInfo.InvariantCulture)}";
    }

    public static string Product => "Gamble With Your Crafts";
}

/// <summary>
/// Простейший лог с цветами. Терминал подписывается на <see cref="Sink"/>,
/// поэтому ядро не зависит от консольного интерфейса и легко тестируется.
/// </summary>
public static class Log
{
    /// <summary>Куда писать: (текст, цвет). Если никто не подписан — пишем в стандартный вывод.</summary>
    public static Action<string, ConsoleColor>? Sink { get; set; }

    public static bool Verbose { get; set; }

    public static void Info(string message) => Write(message, ConsoleColor.Gray);

    public static void Ok(string message) => Write(message, ConsoleColor.Green);

    public static void Warn(string message) => Write(message, ConsoleColor.Yellow);

    public static void Error(string message) => Write(message, ConsoleColor.Red);

    public static void Steam(string message) => Write(message, ConsoleColor.Cyan);

    public static void Debug(string message)
    {
        if (Verbose) Write(message, ConsoleColor.DarkGray);
    }

    private static void Write(string message, ConsoleColor color)
    {
        if (Sink is not null)
        {
            Sink(message, color);
            return;
        }

        ConsoleColor previous = Console.ForegroundColor;
        try
        {
            Console.ForegroundColor = color;
            Console.WriteLine(message);
        }
        finally
        {
            Console.ForegroundColor = previous;
        }
    }
}

/// <summary>Разобранные параметры запуска (включая аргумент +connect_lobby, который подставляет Steam).</summary>
public sealed class LaunchOptions
{
    public uint AppId { get; private set; } = AppCore.SpacewarAppId;

    public string PlayerName { get; private set; } = string.Empty;

    public bool HostOnStart { get; private set; }

    public LobbyVisibility Visibility { get; private set; } = LobbyVisibility.FriendsOnly;

    public int MaxMembers { get; private set; } = 8;

    public ulong? JoinLobbyId { get; private set; }

    public bool BrowseOnStart { get; private set; }

    public bool Sandbox { get; private set; }

    public bool NoColor { get; private set; }

    public bool Verbose { get; private set; }

    public bool SelfTest { get; private set; }

    public bool ShowHelp { get; private set; }

    public int WorldX { get; private set; } = 48;

    public int WorldY { get; private set; } = 28;

    public int WorldZ { get; private set; } = 48;

    public int Seed { get; private set; } = Environment.TickCount;

    /// <summary>Путь к нативной библиотеке моста (GWYFCraftsBridge.dll). Пусто — мост выключен.</summary>
    public string BridgePath { get; private set; } = string.Empty;

    /// <summary>Файл канала обмена с Minecraft. Должен совпадать с настройкой мода.</summary>
    public string BridgeChannel { get; private set; } = "gwyc_bridge.channel";

    public string Error { get; private set; } = string.Empty;

    public static LaunchOptions Parse(string[] args)
    {
        var options = new LaunchOptions();

        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i].Trim();
            string Next(string fallback = "")
            {
                if (i + 1 < args.Length && !args[i + 1].StartsWith('-')) return args[++i];
                return fallback;
            }

            try
            {
                switch (arg.ToLowerInvariant())
                {
                    case "--appid":
                        options.AppId = uint.Parse(Next("480"), CultureInfo.InvariantCulture);
                        break;
                    case "--name":
                        options.PlayerName = Next();
                        break;
                    case "--host":
                        options.HostOnStart = true;
                        break;
                    case "--private":
                        options.HostOnStart = true;
                        options.Visibility = LobbyVisibility.Private;
                        break;
                    case "--friends":
                        options.HostOnStart = true;
                        options.Visibility = LobbyVisibility.FriendsOnly;
                        break;
                    case "--public":
                        options.HostOnStart = true;
                        options.Visibility = LobbyVisibility.Public;
                        break;
                    case "--max":
                        options.MaxMembers = Math.Clamp(int.Parse(Next("8"), CultureInfo.InvariantCulture), 2, 32);
                        break;
                    case "--join":
                        options.JoinLobbyId = ulong.Parse(Next("0"), CultureInfo.InvariantCulture);
                        break;
                    case "+connect_lobby":
                        options.JoinLobbyId = ulong.Parse(Next("0"), CultureInfo.InvariantCulture);
                        break;
                    case "--browse":
                        options.BrowseOnStart = true;
                        break;
                    case "--sandbox":
                        options.Sandbox = true;
                        break;
                    case "--no-color":
                        options.NoColor = true;
                        break;
                    case "--verbose":
                        options.Verbose = true;
                        break;
                    case "--selftest":
                    case "--self-test":
                        options.SelfTest = true;
                        break;
                    case "--world":
                    {
                        string[] parts = Next("48,28,48").Split(new[] { 'x', ',', ':' }, StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length != 3) throw new FormatException("--world ожидает WxHxD, например --world 64x32x64");
                        options.WorldX = Math.Clamp(int.Parse(parts[0], CultureInfo.InvariantCulture), 8, VoxelGrid.MaxDimension);
                        options.WorldY = Math.Clamp(int.Parse(parts[1], CultureInfo.InvariantCulture), 4, VoxelGrid.MaxDimension);
                        options.WorldZ = Math.Clamp(int.Parse(parts[2], CultureInfo.InvariantCulture), 8, VoxelGrid.MaxDimension);
                        break;
                    }
                    case "--seed":
                        options.Seed = int.Parse(Next("1"), CultureInfo.InvariantCulture);
                        break;
                    case "--bridge":
                        options.BridgePath = Next();
                        break;
                    case "--bridge-channel":
                        options.BridgeChannel = Next("gwyc_bridge.channel");
                        break;
                    case "--help":
                    case "-h":
                    case "/?":
                        options.ShowHelp = true;
                        break;
                    default:
                        Log.Warn($"Неизвестный параметр «{arg}» — пропускаю (--help покажет список).");
                        break;
                }
            }
            catch (Exception ex) when (ex is FormatException or OverflowException)
            {
                options.Error = $"Не удалось разобрать параметр «{arg}»: {ex.Message}";
                return options;
            }
        }

        return options;
    }

    public static string HelpText() =>
        $"""
        {BuildInfo.Product} v{BuildInfo.Version} — казино и воксельная стройка в одном exe, сеть через Steam P2P.

        Запуск:
          dotnet run --project src/GambleWithYourCrafts              # одиночная игра (проверить механику)
          dotnet run --project src/GambleWithYourCrafts -- --host    # создать лобби (для друзей)
          dotnet run --project src/GambleWithYourCrafts -- --join <lobbyId>

        Параметры:
          --appid <id>        тестовый AppID (по умолчанию {AppCore.SpacewarAppId} — Spacewar, бесплатный для отладки)
          --host / --public / --friends / --private   сразу создать лобби нужной видимости
          --max <n>           лимит участников лобби (2..32, по умолчанию 8)
          --join <lobbyId>    подключиться к лобби по ID (Steam также передаёт +connect_lobby)
          --browse            показать публичные лобби на старте
          --world <WxHxD>     размер воксельного мира (по умолчанию 48x28x48, максимум {VoxelGrid.MaxDimension})
          --seed <n>          детерминированный пресет арены
          --bridge <файл>     подключить нативный мост к Minecraft (GWYFCraftsBridge.dll/.so)
          --bridge-channel <файл>  файл канала обмена (по умолчанию gwyc_bridge.channel)
          --sandbox           бесконечные блоки на стройку (казино всё равно играет на инвентарь)
          --name <ник>        переопределить ник (по умолчанию — Steam-ник)
          --no-color          без ANSI-цветов (для перенаправленного вывода)
          --verbose           подробный лог пакетов и callback'ов
          --selftest          прогнать встроенные тесты логики без Steam и выйти
          --help              эта справка

        Игровые команды: help, status, invite, say, place, break, fill, craft, bet, rules, map, iso, roster, sync, leave, quit.
        """;
}

/// <summary>
/// Инициализация приложения: Steamworks на тестовом AppID 480, ник игрока,
/// прокачка callback'ов и корректное завершение.
/// </summary>
public static class AppCore
{
    /// <summary>
    /// Тестовый AppID Spacewar. Valve официально держит его для отладки интеграций:
    /// Steam-сеть (лобби, P2P, оверлей) работает без публикации своей игры.
    /// </summary>
    public const uint SpacewarAppId = 480;

    /// <summary>Успешно ли поднялся Steamworks. Без него игра уходит в одиночный режим.</summary>
    public static bool SteamReady { get; private set; }

    /// <summary>Причина, по которой Steam не поднялся (для консоли).</summary>
    public static string SteamError { get; private set; } = string.Empty;

    /// <summary>Ник текущего пользователя Steam (или локальное имя в одиночном режиме).</summary>
    public static string PlayerName { get; private set; } = "Игрок";

    public static ulong PlayerId { get; private set; }

    public static uint AppId => SteamReady ? SteamClient.AppId.Value : SpacewarAppId;

    /// <summary>
    /// Подключённый нативный мост к Minecraft (null — мост не подключён).
    ///
    /// <para>
    /// Мост — это наш собственный плагин: игра сама загружает его и сама отдаёт
    /// события. Мы не читаем память чужого процесса и не правим чужие бинарники —
    /// см. native/include/gwyc/plugin_abi.h и README, раздел про мост.
    /// </para>
    /// </summary>
    public static BridgePluginLoader.BridgePlugin? Bridge { get; private set; }

    /// <summary>
    /// Загрузить мост и отдать ему события игры. Возвращает false, если библиотеки
    /// нет или она несовместима: игра в этом случае просто работает без Minecraft.
    /// </summary>
    public static bool AttachBridge(string libraryPath, string channelPath, BridgePluginLoader.BridgeEvents events)
    {
        if (Bridge is not null) return true;
        ArgumentNullException.ThrowIfNull(events);

        Bridge = BridgePluginLoader.Load(
            libraryPath: libraryPath,
            channelPath: channelPath,
            transport: BridgePluginLoader.GwycTransport.SharedMemory,
            ringCapacity: 128 * 1024,
            maxCubes: 8192,
            gameTag: BuildInfo.Tag);

        if (Bridge is null) return false;

        BridgePluginLoader.GwycStatus status = Bridge.Start(events);
        if (status != BridgePluginLoader.GwycStatus.Ok)
        {
            Log.Error($"Мост не запустился: {status}. Мир Minecraft не подключён.");
            Bridge.Dispose();
            Bridge = null;
            return false;
        }

        Log.Ok("Мост запущен: мир Minecraft подключён (критерий — мод gwyc_bridge на сервере).");
        return true;
    }

    /// <summary>Отправить в Minecraft правку вокселя (если мост подключён).</summary>
    public static void NotifyVoxelEdit(int x, int y, int z, BlockId block)
    {
        Bridge?.PublishVoxelEdit(x, y, z, (byte)block, fromLocalPlayer: true);
    }

    /// <summary>Отправить в Minecraft результат раунда казино (если мост подключён).</summary>
    public static void NotifyCasinoResult(ulong playerId, string playerName, byte game, string target,
                                          byte blockKind, int stake, int multiplier, int payout, bool won)
    {
        Bridge?.PublishBetResolved(
            playerId: (uint)(playerId & 0xFFFF_FFFF),
            stake: stake,
            payout: payout,
            multiplier: multiplier,
            won: won,
            game: game == 0 ? (byte)1 : game,
            blockKind: blockKind,
            target: target,
            playerName: playerName);
    }

    /// <summary>Строка чата сессии → в чат Minecraft.</summary>
    public static void NotifyChat(string author, string text)
    {
        Bridge?.PublishChat(channel: 0, author: author, text: text);
    }

    /// <summary>
    /// Поднять Steamworks. Callback'и качаем вручную (asyncCallbacks: false),
    /// чтобы вся логика сессии жила на главном потоке без гонок.
    /// </summary>
    public static bool Initialize(uint appId, string? nameOverride = null)
    {
        try
        {
            SteamClient.Init(appId, asyncCallbacks: false);

            // Подробности ошибок внутри callback'ов — иначе они теряются молча.
            Dispatch.OnException += exception => Log.Warn($"Исключение в Steam-callback: {exception.Message}");

            // Разрешаем релей Steam: если прямой NAT-пробив не удался, пакеты пойдут через Steam.
            SteamNetworking.AllowP2PPacketRelay(true);

            SteamReady = SteamClient.IsValid;
            PlayerId = SteamClient.SteamId.Value;
            PlayerName = string.IsNullOrWhiteSpace(nameOverride) ? SteamClient.Name : nameOverride.Trim();

            Log.Steam($"Steam поднят: AppID {SteamClient.AppId.Value}, пользователь «{SteamClient.Name}» ({BuildInfo.ShortId(PlayerId)}), залогинен: {SteamClient.IsLoggedOn}.");
            if (PlayerId == 0) Log.Warn("SteamID нулевой — похоже, клиент Steam ещё не готов.");
            return true;
        }
        catch (Exception ex)
        {
            SteamReady = false;
            SteamError = ex.Message;
            PlayerName = string.IsNullOrWhiteSpace(nameOverride) ? "Игрок" : nameOverride.Trim();
            PlayerId = 0;

            Log.Error($"Steam не инициализирован: {ex.Message}");
            Log.Warn("Проверьте: 1) клиент Steam запущен и вы вошли в аккаунт; 2) игра запущена из папки сборки (рядом должен лежать steam_api64.dll);");
            Log.Warn($"3) AppID {appId} доступен вашему аккаунту (Spacewar = 480 добавляется в библиотеку бесплатно).");
            Log.Warn("Игра продолжит работать в одиночном режиме: мир, инвентарь и казино локально, сетевые команды отключены.");
            return false;
        }
    }

    /// <summary>Прокачать очередь callback'ов Steam (лобби, P2P-session, оверлей). Один раз в кадр.</summary>
    public static void Tick()
    {
        if (SteamReady)
        {
            try
            {
                SteamClient.RunCallbacks();
            }
            catch (Exception ex)
            {
                Log.Error($"Ошибка прокачки Steam callback'ов: {ex.Message}");
            }
        }

        // Мост тикает всегда, когда подключён: он разбирает входящие кадры и зовёт
        // наши колбэки. Делать это нужно на главном потоке — игровые системы
        // не потокобезопасны.
        if (Bridge is not null)
        {
            BridgePluginLoader.GwycStatus status = Bridge.Tick();
            if (status != BridgePluginLoader.GwycStatus.Ok)
            {
                Log.Warn($"Мост вернул {status} — обмен с Minecraft приостановлен.");
            }
        }
    }

    public static void Shutdown()
    {
        if (Bridge is not null)
        {
            Bridge.Dispose();
            Bridge = null;
            Log.Info("Мост остановлен.");
        }

        if (!SteamReady) return;

        try
        {
            SteamClient.Shutdown();
            Log.Info("Steamworks остановлен.");
        }
        catch (Exception ex)
        {
            Log.Warn($"Ошибка остановки Steamworks: {ex.Message}");
        }
        finally
        {
            SteamReady = false;
        }
    }
}
