// =============================================================================
//  plugin_abi.h — контракт между игрой и нативным мостом (DLL).
//
//  ЭТО И ЕСТЬ ГРАНИЦА «ВМЕСТО ИНЖЕКЦИИ». Вместо чтения чужой памяти игра сама
//  отдаёт мосту события через экспортируемые функции, а мост возвращает события
//  мира через колбэки. Никаких поисков структур по смещениям в чужом бинарнике:
//  раскладку знает только владелец процесса, он же её и публикует.
//
//  Правила границы (иначе DLL уронит хост-процесс):
//    1. Только POD-структуры с фиксированной шириной полей и #pragma pack(1).
//    2. Никаких C++-типов (std::string, std::vector, исключений) в сигнатурах.
//    3. Строки — UTF-8, фиксированной длины, с гарантированным нулём.
//    4. Каждая структура начинается с поля structSize: принимающая сторона
//       проверяет его до чтения остальных полей (защита от рассинхрона версий).
//    5. Все точки входа возвращают GwycStatus; исключения не пересекают границу.
//    6. Колбэки вызываются ТОЛЬКО из главного потока игры (см. GwycBridge_Tick).
// =============================================================================
#pragma once

#include <stdint.h>

#if defined(_WIN32)
#   define GWYC_ABI_CALL __cdecl
#   ifdef GWYC_BRIDGE_EXPORTS
#       define GWYC_BRIDGE_API __declspec(dllexport)
#   else
#       define GWYC_BRIDGE_API __declspec(dllimport)
#   endif
#else
#   define GWYC_ABI_CALL
#   define GWYC_BRIDGE_API
#endif

#if defined(__cplusplus)
#   define GWYC_ABI_EXTERN extern "C"
#else
#   define GWYC_ABI_EXTERN
#endif

/// Версия ABI. Игра и DLL обязаны сойтись, иначе Initialize вернёт ошибку.
#define GWYC_ABI_VERSION 3

/// Коды результата. Ноль = успех, всё остальное — отказ (никогда не бросаем исключения).
typedef enum GwycStatus {
    GwycStatus_Ok = 0,
    GwycStatus_NotInitialized = 1,
    GwycStatus_AlreadyInitialized = 2,
    GwycStatus_AbiMismatch = 3,
    GwycStatus_InvalidArgument = 4,
    GwycStatus_ChannelOpenFailed = 5,
    GwycStatus_NoPeer = 6,
    GwycStatus_SendFailed = 7,
    GwycStatus_InternalError = 8,
    GwycStatus_NotSupported = 9,
} GwycStatus;

/// Транспорт, выбранный игрой.
typedef enum GwycTransport {
    GwycTransport_SharedMemory = 0,   ///< файловый канал (кроссплатформенно, основной путь)
    GwycTransport_NamedPipe = 1,      ///< именованный канал (Windows message-mode / POSIX FIFO)
} GwycTransport;

/// Роль стороны в канале.
typedef enum GwycRole {
    GwycRole_Game = 0,        ///< консольная игра-казино
    GwycRole_Minecraft = 1,   ///< процесс Minecraft
} GwycRole;

/// Уровни лога, которые мост отдаёт игре.
typedef enum GwycLogLevel {
    GwycLogLevel_Info = 0,
    GwycLogLevel_Warn = 1,
    GwycLogLevel_Error = 2,
} GwycLogLevel;

/// Типы блоков: те же значения, что в C#-игре и в Minecraft-моде мода.
typedef enum GwycBlockKind {
    GwycBlock_Air = 0,
    GwycBlock_Dirt = 1,
    GwycBlock_Grass = 2,
    GwycBlock_Stone = 3,
    GwycBlock_Wood = 4,
    GwycBlock_Gold = 5,
    GwycBlock_Diamond = 6,
    GwycBlock_Table = 7,
    GwycBlock_Lamp = 8,
} GwycBlockKind;

#pragma pack(push, 1)

/// Настройки моста. Передаётся в GwycBridge_Initialize.
typedef struct GwycBridgeConfig {
    uint32_t structSize;            ///< = sizeof(GwycBridgeConfig)
    uint32_t abiVersion;            ///< = GWYC_ABI_VERSION
    uint32_t transport;             ///< GwycTransport
    uint32_t role;                  ///< GwycRole
    uint32_t ringCapacity;          ///< ёмкость кольца канала, байт
    uint32_t pollIntervalMs;        ///< период опроса канала рабочим потоком
    uint32_t peerWaitMs;            ///< сколько ждать вторую сторону при старте
    uint32_t maxCubes;              ///< предел одновременных кубов в мире игры
    const char* channelPath;        ///< путь к файлу канала или имя пайпа (UTF-8)
    const char* gameTag;            ///< метка сборки игры для логов и рукопожатия
} GwycBridgeConfig;

/// Правка вокселя: Minecraft → мир казино.
typedef struct GwycVoxelEdit {
    uint32_t structSize;
    int32_t x;
    int32_t y;
    int32_t z;
    uint8_t blockKind;              ///< GwycBlockKind; GwycBlock_Air = убрать
    uint8_t fromLocalPlayer;        ///< 1 = правку сделал локальный игрок
    uint16_t sourceId;              ///< индекс игрока Minecraft (0 = мир)
} GwycVoxelEdit;

/// Награда: казино → Minecraft (выдать блоки).
typedef struct GwycReward {
    uint32_t structSize;
    uint32_t playerId;
    uint8_t blockKind;              ///< GwycBlockKind
    uint8_t reason;                 ///< 0 = выигрыш, 1 = обмен, 2 = отладка
    uint16_t reserved;
    int32_t count;
} GwycReward;

/// Ставка принята (для зеркалирования события в Minecraft).
typedef struct GwycBetPlaced {
    uint32_t structSize;
    uint32_t playerId;
    int64_t stake;
    uint8_t game;                   ///< 1 = кости, 2 = рулетка
    uint8_t blockKind;              ///< чем ставили
    uint16_t reserved;
    char target[16];                ///< "red", "high", "17", ...
    char playerName[24];
} GwycBetPlaced;

/// Раунд завершён. Minecraft показывает титры/частицы и выдаёт блоки по payout.
typedef struct GwycBetResolved {
    uint32_t structSize;
    uint32_t playerId;
    int64_t stake;
    int64_t payout;
    int32_t multiplier;
    uint8_t won;                    ///< 1 = выигрыш
    uint8_t game;                   ///< 1 = кости, 2 = рулетка
    uint8_t blockKind;
    uint8_t reserved;
    char target[16];
    char playerName[24];
} GwycBetResolved;

/// Стол появился/изменился (для спавна визуализации в Minecraft).
typedef struct GwycTableEvent {
    uint32_t structSize;
    uint32_t tableId;
    int32_t x;
    int32_t y;
    int32_t z;
    int64_t minBet;
    int64_t maxBet;
    uint8_t seats;
    uint8_t state;                  ///< 0 = создан, 1 = активен, 2 = закрыт
    uint16_t reserved;
    char ownerName[24];
} GwycTableEvent;

/// Состояние игрока казино.
typedef struct GwycPlayerState {
    uint32_t structSize;
    uint32_t playerId;
    int64_t chips;
    int64_t netWorth;
    uint8_t lobbyState;             ///< 0 = один, 1 = в лобби, 2 = хост
    uint8_t reserved[3];
    char playerName[24];
} GwycPlayerState;

/// Строка чата в любую сторону.
typedef struct GwycChatLine {
    uint32_t structSize;
    uint8_t channel;                ///< 0 = сессия, 1 = системное
    uint8_t reserved[3];
    char author[24];
    char text[160];
} GwycChatLine;

/// Счётчики моста.
typedef struct GwycBridgeStats {
    uint32_t structSize;
    uint32_t peerReady;             ///< 1 — вторая сторона подключена
    uint32_t transport;             ///< GwycTransport
    uint32_t overruns;              ///< сколько кадров не влезло в кольцо
    uint32_t crcFailures;           ///< сколько кадров отброшено как битые
    uint32_t resyncs;               ///< сколько раз поток пересинхронизировали
    uint64_t messagesSent;
    uint64_t messagesReceived;
    uint64_t rttMs;                 ///< последний измеренный RTT до второй стороны
    uint64_t cubesSpawned;
    uint64_t cubesRemoved;
    uint64_t editsDropped;          ///< правок не влезло в лимит мира
    uint64_t uptimeMs;
} GwycBridgeStats;

#pragma pack(pop)

/// Колбэки, которые игра отдаёт мосту. Вызываются только из GwycBridge_Tick.
typedef struct GwycGameCallbacks {
    uint32_t structSize;

    /// Minecraft поставил/убрал блок → игра материализует/убирает куб в своём мире.
    void (GWYC_ABI_CALL* onVoxelApply)(int32_t x, int32_t y, int32_t z, uint8_t blockKind, uint16_t sourceId);

    /// Minecraft просит выдать блоки (например, за обмен фишек).
    void (GWYC_ABI_CALL* onRewardRequest)(const GwycReward* reward);

    /// Пришла строка чата из Minecraft.
    void (GWYC_ABI_CALL* onChatFromMinecraft)(const GwycChatLine* line);

    /// Вторая сторона подключилась (1) или отключилась (0).
    void (GWYC_ABI_CALL* onPeerState)(uint32_t connected);

    /// Диагностика моста для лога игры.
    void (GWYC_ABI_CALL* onLog)(uint32_t level, const char* message);
} GwycGameCallbacks;

// ─────────────────────────────────────────────────────────────────────────────
//  Экспорт DLL
// ─────────────────────────────────────────────────────────────────────────────

/// Версия ABI, которую реализует DLL (игра сверяет до Initialize).
GWYC_BRIDGE_API uint32_t GWYC_ABI_CALL GwycBridge_GetAbiVersion(void);

/// Текстовая версия сборки моста (для логов).
GWYC_BRIDGE_API const char* GWYC_ABI_CALL GwycBridge_GetBuildTag(void);

/// Инициализация: открывает канал, запускает рабочий поток, шлёт Hello.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_Initialize(const GwycBridgeConfig* config, const GwycGameCallbacks* callbacks);

/// Остановка: прощание, закрытие канала, остановка потока.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_Shutdown(void);

/// Тик моста на главном потоке игры: разбирает входящие сообщения и ВЫЗЫВАЕТ колбэки.
/// Обязателен к вызову раз в кадр, пока мост активен.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_Tick(void);

// ── игра → мост (публикация событий) ────────────────────────────────────────

GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishVoxelEdit(const GwycVoxelEdit* edit);
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishBetPlaced(const GwycBetPlaced* bet);
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishBetResolved(const GwycBetResolved* result);
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishTableEvent(const GwycTableEvent* table);
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishPlayerState(const GwycPlayerState* state);
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_PublishChat(const GwycChatLine* line);

/// Попросить у Minecraft полный снимок региона (мост отправит запрос и применит ответ).
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_RequestRegionSnapshot(int32_t x, int32_t y, int32_t z, uint16_t sizeX, uint16_t sizeY, uint16_t sizeZ);

/// Статистика моста для HUD.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_GetStats(GwycBridgeStats* outStats);

/// Поставить хуки на события казино (ставки/столы) по адресам функций игры.
/// Адреса игра берёт у себя: экспорт, отладочные символы или AOB-сканер.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_InstallCasinoHooks(void* betSubmitTarget, void* tableCreateTarget);

/// Снять хуки казино.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_RemoveCasinoHooks(void);

/// Сколько ставок и столов перехвачено хуками.
GWYC_BRIDGE_API int32_t GWYC_ABI_CALL GwycBridge_GetHookCounters(uint64_t* bets, uint64_t* tables);
