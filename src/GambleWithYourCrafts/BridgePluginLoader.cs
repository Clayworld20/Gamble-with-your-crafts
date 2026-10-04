using System.Runtime.InteropServices;

namespace GambleWithYourCrafts;

/// <summary>
/// Загрузчик нативного моста (GWYFCraftsBridge.dll).
///
/// <para>
/// Это и есть «граница вместо инъекции»: наша игра сама загружает плагин и сама
/// отдаёт ему события через экспортированные функции. Никаких поисков структур
/// в чужом процессе, никакой правки чужой памяти — раскладку знает владелец
/// процесса (мы), он же её и публикует. См. native/include/gwyc/plugin_abi.h.
/// </para>
///
/// <para>
/// Направления обмена живут в DLL: она держит канал (файл в памяти или пайп),
/// кодирует кадры и общается с модом Minecraft. Игре остаётся дёргать
/// <see cref="BridgePlugin.Tick"/> раз в кадр и звать Publish*-методы.
/// </para>
/// </summary>
public static class BridgePluginLoader
{
    /// <summary>Версия ABI: обязана совпадать с GWYC_ABI_VERSION в plugin_abi.h.</summary>
    public const uint AbiVersion = 3;

    // ── Структуры ABI (pack = 1, как в C++) ──────────────────────────────────

    public enum GwycStatus
    {
        Ok = 0,
        NotInitialized = 1,
        AlreadyInitialized = 2,
        AbiMismatch = 3,
        InvalidArgument = 4,
        ChannelOpenFailed = 5,
        NoPeer = 6,
        SendFailed = 7,
        InternalError = 8,
        NotSupported = 9,
    }

    public enum GwycTransport : uint
    {
        SharedMemory = 0,
        NamedPipe = 1,
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycBridgeConfig
    {
        public uint StructSize;
        public uint AbiVersion;
        public uint Transport;
        public uint Role;
        public uint RingCapacity;
        public uint PollIntervalMs;
        public uint PeerWaitMs;
        public uint MaxCubes;
        public IntPtr ChannelPath;   // const char* (UTF-8)
        public IntPtr GameTag;       // const char* (UTF-8)
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycGameCallbacks
    {
        public uint StructSize;
        public IntPtr OnVoxelApply;
        public IntPtr OnRewardRequest;
        public IntPtr OnChatFromMinecraft;
        public IntPtr OnPeerState;
        public IntPtr OnLog;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycVoxelEdit
    {
        public uint StructSize;
        public int X;
        public int Y;
        public int Z;
        public byte BlockKind;
        public byte FromLocalPlayer;
        public ushort SourceId;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycReward
    {
        public uint StructSize;
        public uint PlayerId;
        public byte BlockKind;
        public byte Reason;
        public ushort Reserved;
        public int Count;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycBetPlaced
    {
        public uint StructSize;
        public uint PlayerId;
        public long Stake;
        public byte Game;
        public byte BlockKind;
        public ushort Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
        public string Target;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]
        public string PlayerName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycBetResolved
    {
        public uint StructSize;
        public uint PlayerId;
        public long Stake;
        public long Payout;
        public int Multiplier;
        public byte Won;
        public byte Game;
        public byte BlockKind;
        public byte Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
        public string Target;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]
        public string PlayerName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycTableEvent
    {
        public uint StructSize;
        public uint TableId;
        public int X;
        public int Y;
        public int Z;
        public long MinBet;
        public long MaxBet;
        public byte Seats;
        public byte State;
        public ushort Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]
        public string OwnerName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycPlayerState
    {
        public uint StructSize;
        public uint PlayerId;
        public long Chips;
        public long NetWorth;
        public byte LobbyState;
        public byte Reserved0;
        public byte Reserved1;
        public byte Reserved2;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]
        public string PlayerName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct GwycChatLine
    {
        public uint StructSize;
        public byte Channel;
        public byte Reserved0;
        public byte Reserved1;
        public byte Reserved2;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]
        public string Author;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 160)]
        public string Text;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct GwycBridgeStats
    {
        public uint StructSize;
        public uint PeerReady;
        public uint Transport;
        public uint Overruns;
        public uint CrcFailures;
        public uint Resyncs;
        public ulong MessagesSent;
        public ulong MessagesReceived;
        public ulong RttMs;
        public ulong CubesSpawned;
        public ulong CubesRemoved;
        public ulong EditsDropped;
        public ulong UptimeMs;
    }

    // ── Делегаты колбэков (cdecl, как в C++) ─────────────────────────────────

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void VoxelApplyCallback(int x, int y, int z, byte blockKind, ushort sourceId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void RewardRequestCallback(IntPtr reward);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ChatCallback(IntPtr line);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void PeerStateCallback(uint connected);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LogCallback(uint level, IntPtr message);

    /// <summary>То, что игра хочет знать о мире Minecraft. Заполняется хост-кодом.</summary>
    public sealed class BridgeEvents
    {
        /// <summary>Minecraft поставил/убрал блок: материализовать куб в мире казино.</summary>
        public Action<int, int, int, byte, ushort>? VoxelApply { get; set; }

        /// <summary>Minecraft просит выдать блоки (обмен фишек и т. п.).</summary>
        public Action<GwycReward>? RewardRequest { get; set; }

        /// <summary>Строка чата из Minecraft.</summary>
        public Action<string, string>? ChatFromMinecraft { get; set; }

        /// <summary>Вторая сторона подключилась (true) или отключилась (false).</summary>
        public Action<bool>? PeerState { get; set; }

        /// <summary>Диагностика моста.</summary>
        public Action<uint, string>? Log { get; set; }
    }

    /// <summary>Загруженный мост. Создавать через <see cref="Load"/>.</summary>
    public sealed class BridgePlugin : IDisposable
    {
        private readonly IntPtr _library;
        private readonly GwycBridgeConfig _config;

        private readonly Func<uint> _getAbiVersion;
        private readonly Func<string> _getBuildTag;
        private readonly Func<IntPtr, IntPtr, GwycStatus> _initialize;
        private readonly Func<GwycStatus> _shutdown;
        private readonly Func<GwycStatus> _tick;
        private readonly Func<IntPtr, GwycStatus> _publishVoxelEdit;
        private readonly Func<IntPtr, GwycStatus> _publishBetPlaced;
        private readonly Func<IntPtr, GwycStatus> _publishBetResolved;
        private readonly Func<IntPtr, GwycStatus> _publishTableEvent;
        private readonly Func<IntPtr, GwycStatus> _publishPlayerState;
        private readonly Func<IntPtr, GwycStatus> _publishChat;
        private readonly Func<IntPtr, GwycStatus> _getStats;
        private readonly Func<IntPtr, IntPtr, GwycStatus> _installHooks;
        private readonly Func<GwycStatus> _removeHooks;
        private readonly Func<IntPtr, IntPtr, GwycStatus> _getHookCounters;
        private readonly Func<int, int, int, ushort, ushort, ushort, GwycStatus> _requestSnapshot;

        // Держим делегаты живыми: сборщик мусора обязан их не тронуть,
        // пока DLL может их вызвать.
        private readonly List<Delegate> _callbackKeepAlive = new();

        private IntPtr _callbacksBlock;
        private bool _initialized;

        /// <summary>Создаётся только из <see cref="BridgePluginLoader.Load"/>.</summary>
        internal BridgePlugin(IntPtr library, GwycBridgeConfig config)
        {
            _library = library;
            _config = config;

            _getAbiVersion = Bind<Func<uint>>("GwycBridge_GetAbiVersion");
            _getBuildTag = BindString("GwycBridge_GetBuildTag");
            _initialize = Bind<Func<IntPtr, IntPtr, GwycStatus>>("GwycBridge_Initialize");
            _shutdown = Bind<Func<GwycStatus>>("GwycBridge_Shutdown");
            _tick = Bind<Func<GwycStatus>>("GwycBridge_Tick");
            _publishVoxelEdit = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishVoxelEdit");
            _publishBetPlaced = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishBetPlaced");
            _publishBetResolved = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishBetResolved");
            _publishTableEvent = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishTableEvent");
            _publishPlayerState = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishPlayerState");
            _publishChat = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_PublishChat");
            _getStats = Bind<Func<IntPtr, GwycStatus>>("GwycBridge_GetStats");
            _installHooks = Bind<Func<IntPtr, IntPtr, GwycStatus>>("GwycBridge_InstallCasinoHooks");
            _removeHooks = Bind<Func<GwycStatus>>("GwycBridge_RemoveCasinoHooks");
            _getHookCounters = Bind<Func<IntPtr, IntPtr, GwycStatus>>("GwycBridge_GetHookCounters");
            _requestSnapshot = Bind<Func<int, int, int, ushort, ushort, ushort, GwycStatus>>("GwycBridge_RequestRegionSnapshot");
        }

        public string BuildTag => _getBuildTag();

        public bool IsInitialized => _initialized;

        public uint AbiVersion => _getAbiVersion();

        private T Bind<T>(string name) where T : Delegate
        {
            if (!NativeLibrary.TryGetExport(_library, name, out IntPtr address))
                throw new EntryPointNotFoundException($"В библиотеке моста нет функции {name}.");

            return Marshal.GetDelegateForFunctionPointer<T>(address);
        }

        private Func<string> BindString(string name)
        {
            if (!NativeLibrary.TryGetExport(_library, name, out IntPtr address))
                throw new EntryPointNotFoundException($"В библиотеке моста нет функции {name}.");

            var pointer = Marshal.GetDelegateForFunctionPointer<IntPtrFn>(address);
            return () => Marshal.PtrToStringUTF8(pointer()) ?? string.Empty;
        }

        private delegate IntPtr IntPtrFn();

        /// <summary>
        /// Слить колбэки игры в неуправляемый блок и запустить мост.
        /// </summary>
        public GwycStatus Start(BridgeEvents events)
        {
            if (_initialized) return GwycStatus.AlreadyInitialized;

            IntPtr block = IntPtr.Zero;
            try
            {
                block = BuildCallbacks(events);

                GwycBridgeConfig config = _config;
                IntPtr configBlock = Marshal.AllocHGlobal(Marshal.SizeOf<GwycBridgeConfig>());
                Marshal.StructureToPtr(config, configBlock, false);

                try
                {
                    GwycStatus status = _initialize(configBlock, block);
                    if (status == GwycStatus.Ok) _initialized = true;
                    return status;
                }
                finally
                {
                    Marshal.FreeHGlobal(configBlock);
                }
            }
            catch
            {
                if (block != IntPtr.Zero) Marshal.FreeHGlobal(block);
                throw;
            }
        }

        private IntPtr BuildCallbacks(BridgeEvents events)
        {
            var block = new GwycGameCallbacks
            {
                StructSize = (uint)Marshal.SizeOf<GwycGameCallbacks>(),
            };

            VoxelApplyCallback voxel = (x, y, z, blockKind, sourceId) => events.VoxelApply?.Invoke(x, y, z, blockKind, sourceId);
            RewardRequestCallback reward = pointer =>
            {
                if (pointer == IntPtr.Zero) return;
                events.RewardRequest?.Invoke(Marshal.PtrToStructure<GwycReward>(pointer));
            };
            ChatCallback chat = pointer =>
            {
                if (pointer == IntPtr.Zero) return;
                GwycChatLine line = Marshal.PtrToStructure<GwycChatLine>(pointer);
                events.ChatFromMinecraft?.Invoke(line.Author ?? string.Empty, line.Text ?? string.Empty);
            };
            PeerStateCallback peer = connected => events.PeerState?.Invoke(connected != 0);
            LogCallback log = (level, message) =>
            {
                string text = Marshal.PtrToStringUTF8(message) ?? string.Empty;
                events.Log?.Invoke(level, text);
            };

            _callbackKeepAlive.AddRange(new Delegate[] { voxel, reward, chat, peer, log });
            block.OnVoxelApply = Marshal.GetFunctionPointerForDelegate(voxel);
            block.OnRewardRequest = Marshal.GetFunctionPointerForDelegate(reward);
            block.OnChatFromMinecraft = Marshal.GetFunctionPointerForDelegate(chat);
            block.OnPeerState = Marshal.GetFunctionPointerForDelegate(peer);
            block.OnLog = Marshal.GetFunctionPointerForDelegate(log);

            _callbacksBlock = Marshal.AllocHGlobal(Marshal.SizeOf<GwycGameCallbacks>());
            Marshal.StructureToPtr(block, _callbacksBlock, false);
            return _callbacksBlock;
        }

        public GwycStatus Tick() => _initialized ? _tick() : GwycStatus.NotInitialized;

        public GwycStatus PublishVoxelEdit(int x, int y, int z, byte blockKind, bool fromLocalPlayer = true, ushort sourceId = 0)
        {
            var edit = new GwycVoxelEdit
            {
                StructSize = (uint)Marshal.SizeOf<GwycVoxelEdit>(),
                X = x,
                Y = y,
                Z = z,
                BlockKind = blockKind,
                FromLocalPlayer = (byte)(fromLocalPlayer ? 1 : 0),
                SourceId = sourceId,
            };
            return SendStruct(edit, _publishVoxelEdit);
        }

        public GwycStatus PublishBetPlaced(uint playerId, long stake, byte game, byte blockKind, string target, string playerName)
        {
            var bet = new GwycBetPlaced
            {
                StructSize = (uint)Marshal.SizeOf<GwycBetPlaced>(),
                PlayerId = playerId,
                Stake = stake,
                Game = game,
                BlockKind = blockKind,
                Target = Clamp(target, 15),
                PlayerName = Clamp(playerName, 23),
            };
            return SendStruct(bet, _publishBetPlaced);
        }

        public GwycStatus PublishBetResolved(uint playerId, long stake, long payout, int multiplier,
                                             bool won, byte game, byte blockKind, string target, string playerName)
        {
            var result = new GwycBetResolved
            {
                StructSize = (uint)Marshal.SizeOf<GwycBetResolved>(),
                PlayerId = playerId,
                Stake = stake,
                Payout = payout,
                Multiplier = multiplier,
                Won = (byte)(won ? 1 : 0),
                Game = game,
                BlockKind = blockKind,
                Target = Clamp(target, 15),
                PlayerName = Clamp(playerName, 23),
            };
            return SendStruct(result, _publishBetResolved);
        }

        public GwycStatus PublishTableEvent(uint tableId, int x, int y, int z, long minBet, long maxBet, byte seats, byte state, string owner)
        {
            var table = new GwycTableEvent
            {
                StructSize = (uint)Marshal.SizeOf<GwycTableEvent>(),
                TableId = tableId,
                X = x,
                Y = y,
                Z = z,
                MinBet = minBet,
                MaxBet = maxBet,
                Seats = seats,
                State = state,
                OwnerName = Clamp(owner, 23),
            };
            return SendStruct(table, _publishTableEvent);
        }

        public GwycStatus PublishPlayerState(uint playerId, long chips, long netWorth, byte lobbyState, string playerName)
        {
            var state = new GwycPlayerState
            {
                StructSize = (uint)Marshal.SizeOf<GwycPlayerState>(),
                PlayerId = playerId,
                Chips = chips,
                NetWorth = netWorth,
                LobbyState = lobbyState,
                PlayerName = Clamp(playerName, 23),
            };
            return SendStruct(state, _publishPlayerState);
        }

        public GwycStatus PublishChat(byte channel, string author, string text)
        {
            var line = new GwycChatLine
            {
                StructSize = (uint)Marshal.SizeOf<GwycChatLine>(),
                Channel = channel,
                Author = Clamp(author, 23),
                Text = Clamp(text, 159),
            };
            return SendStruct(line, _publishChat);
        }

        /// <summary>Попросить у Minecraft снимок региона (мод ответит данными).</summary>
        public GwycStatus RequestRegionSnapshot(int x, int y, int z, ushort sizeX, ushort sizeY, ushort sizeZ)
            => _initialized ? _requestSnapshot(x, y, z, sizeX, sizeY, sizeZ) : GwycStatus.NotInitialized;

        /// <summary>Поставить хуки на события казино по адресам функций нашей игры.</summary>
        public GwycStatus InstallCasinoHooks(IntPtr betSubmitTarget, IntPtr tableCreateTarget)
            => _initialized ? _installHooks(betSubmitTarget, tableCreateTarget) : GwycStatus.NotInitialized;

        public GwycStatus RemoveCasinoHooks() => _initialized ? _removeHooks() : GwycStatus.NotInitialized;

        public (ulong Bets, ulong Tables) GetHookCounters()
        {
            IntPtr bets = Marshal.AllocHGlobal(sizeof(ulong));
            IntPtr tables = Marshal.AllocHGlobal(sizeof(ulong));
            try
            {
                if (_getHookCounters(bets, tables) != GwycStatus.Ok) return (0, 0);
                return ((ulong)Marshal.ReadInt64(bets), (ulong)Marshal.ReadInt64(tables));
            }
            finally
            {
                Marshal.FreeHGlobal(bets);
                Marshal.FreeHGlobal(tables);
            }
        }

        public bool TryGetStats(out GwycBridgeStats stats)
        {
            stats = default;
            if (!_initialized) return false;

            IntPtr block = Marshal.AllocHGlobal(Marshal.SizeOf<GwycBridgeStats>());
            try
            {
                var seed = new GwycBridgeStats { StructSize = (uint)Marshal.SizeOf<GwycBridgeStats>() };
                Marshal.StructureToPtr(seed, block, false);

                if (_getStats(block) != GwycStatus.Ok) return false;

                stats = Marshal.PtrToStructure<GwycBridgeStats>(block);
                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(block);
            }
        }

        private GwycStatus SendStruct<T>(T value, Func<IntPtr, GwycStatus> publisher) where T : struct
        {
            if (!_initialized) return GwycStatus.NotInitialized;

            IntPtr block = Marshal.AllocHGlobal(Marshal.SizeOf<T>());
            try
            {
                Marshal.StructureToPtr(value, block, false);
                return publisher(block);
            }
            finally
            {
                Marshal.FreeHGlobal(block);
            }
        }

        private static string Clamp(string? text, int maxLength)
        {
            // В C# строки в ABI живут как TStr: ограничение по длине задаёт сама
            // структура (SizeConst), поэтому длину подрезаем здесь.
            if (string.IsNullOrEmpty(text)) return string.Empty;
            return text.Length <= maxLength ? text : text[..maxLength];
        }

        public void Dispose()
        {
            if (_initialized)
            {
                _shutdown();
                _initialized = false;
            }

            if (_callbacksBlock != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_callbacksBlock);
                _callbacksBlock = IntPtr.Zero;
            }

            _callbackKeepAlive.Clear();

            // Библиотеку не выгружаем: мост мог завести хук-трамплины и рабочий
            // поток, а FreeLibrary в этот момент валит процесс.
        }
    }

    /// <summary>
    /// Загрузить мост. Возвращает null и пишет причину в лог, если библиотеки нет
    /// или она собрана под другую версию ABI — игра при этом продолжает работать.
    /// </summary>
    public static BridgePlugin? Load(string libraryPath, string channelPath, GwycTransport transport,
                                     uint ringCapacity, uint maxCubes, string gameTag)
    {
        try
        {
            if (!File.Exists(libraryPath))
            {
                Log.Warn($"Мост не найден: {libraryPath}. Игра работает локально, мир Minecraft не подключён.");
                return null;
            }

            IntPtr library = NativeLibrary.Load(libraryPath);
            IntPtr versionPointer = IntPtr.Zero;

            if (NativeLibrary.TryGetExport(library, "GwycBridge_GetAbiVersion", out versionPointer))
            {
                var getVersion = Marshal.GetDelegateForFunctionPointer<Func<uint>>(versionPointer);
                uint version = getVersion();

                if (version != AbiVersion)
                {
                    Log.Error($"Мост собран под ABI {version}, игра ждёт {AbiVersion}. Пересоберите native-часть.");
                    return null;
                }
            }
            else
            {
                Log.Error("В библиотеке нет GwycBridge_GetAbiVersion — это не мост GWYC.");
                return null;
            }

            var config = new GwycBridgeConfig
            {
                StructSize = (uint)Marshal.SizeOf<GwycBridgeConfig>(),
                AbiVersion = AbiVersion,
                Transport = (uint)transport,
                Role = 0,   // игра
                RingCapacity = ringCapacity == 0 ? 128u * 1024u : ringCapacity,
                PollIntervalMs = 2,
                PeerWaitMs = 0,
                MaxCubes = maxCubes == 0 ? 8192u : maxCubes,
                ChannelPath = Marshal.StringToCoTaskMemUTF8(channelPath),
                GameTag = Marshal.StringToCoTaskMemUTF8(gameTag),
            };

            var plugin = new BridgePlugin(library, config);
            Log.Ok($"Мост загружен: {Path.GetFileName(libraryPath)} ({plugin.BuildTag}).");
            return plugin;
        }
        catch (Exception ex)
        {
            Log.Error($"Не удалось загрузить мост: {ex.Message}");
            return null;
        }
    }
}
