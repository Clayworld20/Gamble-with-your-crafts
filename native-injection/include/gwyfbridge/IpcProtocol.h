// ============================================================================
//  IpcProtocol.h — ABI разделяемой памяти между игрой и Minecraft.
//
//  Транспорт выбран специально: разделяемая память (memory-mapped file) плюс
//  именованные события WinAPI. Почему не Named Pipes:
//    * запись в кольцевой буфер — это ~100 нс и ни одной системной операции,
//      тогда как WriteFile на пайп — это переход в ядро и планировщик;
//    * в хуке, который выполняется на игровом потоке, мы не имеем права
//      ждать. Здесь продюсер только пишет запись и делает SetEvent.
//
//  Модель обмена:
//    * два кольца фиксированных записей (64 байта) — события, команды, телеметрия;
//    * два байтовых кольца — «длинные» данные: отчёты, логи и массовая заливка
//      вокселей (bulk sync).
//    * Каждое направление имеет ровно одного продюсера и одного потребителя
//      (SPSC) — это позволяет обойтись без блокировок вообще.
//
//  ВАЖНО: структура выровнена по 64 байта (кэш-линия). Меняя ABI, обязательно
//  поднимайте kAbiVersion — обе стороны проверяют её при рукопожатии.
// ============================================================================
#pragma once

#include "Base.h"

#include <functional>

namespace gwyf::ipc {

/// 'GWMC' — сигнатура общего региона.
constexpr u32 kMagic = 0x434D5747u;

/// Версия ABI. Несовпадение = немедленный отказ от соединения с понятной ошибкой.
constexpr u32 kAbiVersion = 1;

/// Имя региона по умолчанию (Local\ — виден только в текущей сессии Windows,
/// поэтому другие пользователи машины доступа не получат).
inline constexpr const wchar_t* kDefaultRegionName = L"Local\\GWYF_MC_BRIDGE_ABI1";

/// Размер полезной нагрузки в записи кольца. 64 байта вычислены из фактической
/// раскладки: самая объёмная нагрузка (команда, ставка, стол) занимает 40 байт,
/// плюс запас на выравнивание — так мы НЕ полагаемся на угадывание размеров.
constexpr u32 kInlinePayload = 64;

/// Ёмкость кольца записей (степень двойки — берём маску вместо деления).
constexpr u32 kRecordRingCapacity = 512;
static_assert((kRecordRingCapacity & (kRecordRingCapacity - 1)) == 0, "capacity must be power of two");

/// Ёмкость байтовых колец (bulk-данные).
constexpr u32 kBulkRingCapacity = 256 * 1024;

// ── Типы записей ────────────────────────────────────────────────────────────

enum class RecordType : u32 {
    None = 0,
    Hello = 1,         // рукопожатие: кто, какая версия, какой процесс
    Bye = 2,           // вежливое завершение
    Heartbeat = 3,     // пульс продюсера (для определения его смерти)
    BetPlaced = 4,     // игрок сделал ставку (из памяти игры)
    BetResolved = 5,   // раунд завершён: выигрыш/проигрыш
    TableSpawned = 6,  // в игре создан игровой стол
    TableRemoved = 7,
    ChipState = 8,     // телеметрия: фишки/банк/ставки игроков
    LobbyState = 9,    // телеметрия: состав лобби
    VoxelEdit = 10,    // воксель поставлен/сломан (из Minecraft)
    BlockGrant = 11,   // выигрыш → выдать блоки в Minecraft
    Command = 12,      // команда между компонентами (dump/scan/status)
    CommandAck = 13,
    LogLine = 14,
    BulkSync = 15,     // начало массовой синхронизации вокселей (payload в bulk-кольце)
    CubeSpawned = 16,  // подтверждение: 3D-куб создан внутри игры
};

/// Кто именно отправил запись (для диагностики и логов).
enum class Producer : u32 { Unknown = 0, Game = 1, Minecraft = 2, Injector = 3 };

/// Код команды (RecordType::Command).
enum class CommandCode : u32 {
    None = 0,
    DumpClass = 1,      // выгрузить список методов/полей managed-класса
    ScanPattern = 2,    // AOB-скан по модулю игры
    Status = 3,         // сводка по мосту
    SpawnProbe = 4,     // создать пробный куб в игре (для настройки профиля)
    ReloadProfile = 5,  // перечитать профиль без перезапуска
    FlushWorld = 6,     // удалить все кубы, созданные мостом
};

enum class VoxelAction : u32 { Place = 1, Break = 2, BulkReplace = 3 };

enum class AckStatus : u32 { Ok = 0, Rejected = 1, NotFound = 2, BadRequest = 3, NotSupported = 4 };

struct Vec3i {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
};

// ── Полезные нагрузки (все POD, суммарно не больше kInlinePayload) ──────────

#pragma pack(push, 8)

/// Ставка. amount передаём как биты float, чтобы не зависеть от ABI float
/// при передаче через границу процессов.
struct PayloadBet {
    u32 amountBits = 0;   // float, приведённый к u32
    u32 chipType = 0;     // тип фишек (номинал/цвет)
    u32 betKind = 0;      // вид ставки: 0=неизвестно, 1=число, 2=цвет, 3=зона, 4=линия…
    u32 tableId = 0;
    u64 playerId = 0;     // SteamId/NetId игрока-игрока в лобби
    u32 seat = 0;
    u32 flags = 0;

    static float Amount(const PayloadBet& value) {
        float result = 0.0f;
        std::memcpy(&result, &value.amountBits, sizeof(result));
        return result;
    }
    static void SetAmount(PayloadBet& value, float amount) {
        std::memcpy(&value.amountBits, &amount, sizeof(amount));
    }
};

struct PayloadBetResult {
    i64 payout = 0;       // сколько выплачено (в фишках)
    i64 chipsAfter = 0;   // баланс игрока после раунда
    u32 tableId = 0;
    u32 seat = 0;
    u8 won = 0;
    u8 wheelPocket = 0;   // выпавшее число рулетки, если применимо
    u8 diceA = 0;
    u8 diceB = 0;
    u64 playerId = 0;
};

struct PayloadTable {
    u32 tableId = 0;
    u32 lobbyId = 0;
    u32 seats = 0;
    u32 phase = 0;
    u32 gameKind = 0;
    i32 posX = 0;
    i32 posY = 0;
    i32 posZ = 0;
    i64 minBet = 0;
};

struct PayloadChipState {
    i64 chips = 0;
    i64 pendingBet = 0;
    i64 bank = 0;
    u32 playerCount = 0;
    u32 roundIndex = 0;
    u64 playerId = 0;
};

struct PayloadVoxel {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u32 block = 0;      // тип блока (числовой id, соответствие настраивается в профиле)
    u32 action = 0;     // VoxelAction
    u64 authorId = 0;
};

struct PayloadGrant {
    u32 block = 0;
    u32 count = 0;
    u64 playerId = 0;
    u32 reason = 0;     // 1=выигрыш в казино, 2=крафт, 3=ручная выдача
    u32 reserved = 0;
};

struct PayloadCommand {
    u32 code = 0;       // CommandCode
    u32 arg0 = 0;
    u32 arg1 = 0;
    u32 textLength = 0; // длина строки в bulk-кольце (0 = нет текста)
    u32 textOffset = 0; // смещение строки в bulk-кольце
    u32 status = 0;     // AckStatus для CommandAck
    u32 reserved = 0;
};

/// Общий union — ровно kInlinePayload байт. static_assert ниже ловит
/// неаккуратное расширение структур на этапе компиляции.
union Payload {
    u8 raw[kInlinePayload];
    PayloadBet bet;
    PayloadBetResult betResult;
    PayloadTable table;
    PayloadChipState chips;
    PayloadVoxel voxel;
    PayloadGrant grant;
    PayloadCommand command;
};
static_assert(sizeof(Payload) == kInlinePayload, "IPC payload must stay 32 bytes");
static_assert(sizeof(PayloadBet) <= kInlinePayload, "PayloadBet too big");
static_assert(sizeof(PayloadBetResult) <= kInlinePayload, "PayloadBetResult too big");
static_assert(sizeof(PayloadTable) <= kInlinePayload, "PayloadTable too big");
static_assert(sizeof(PayloadChipState) <= kInlinePayload, "PayloadChipState too big");
static_assert(sizeof(PayloadVoxel) <= kInlinePayload, "PayloadVoxel too big");
static_assert(sizeof(PayloadGrant) <= kInlinePayload, "PayloadGrant too big");
static_assert(sizeof(PayloadCommand) <= kInlinePayload, "PayloadCommand too big");

/// Одна запись кольца: 128 байт = две кэш-линии ровно, без «рваных» границ
/// между записями (иначе два ядра дрались бы за одну линию).
/// Первые 48 байт — шапка, дальше — полезная нагрузка, затем запас на вырост.
struct Record {
    u32 type = 0;        // RecordType
    u32 producer = 0;    // Producer
    u64 sequence = 0;    // монотонный номер (диагностика потерь)
    u64 timestampMs = 0; // Unix-время в мс
    u64 sourceId = 0;    // SteamId/NetId отправителя, если применимо
    u64 aux = 0;         // произвольная метка (pid, id раунда…)
    u32 flags = 0;       // зарезервировано под признаки (например, «срочно»)
    u32 reserved = 0;
    Payload payload{};
    u8 tail[16]{};       // добивка до 128 байт
};
static_assert(sizeof(Record) == 128, "IPC record must stay 128 bytes (two cache lines)");

/// Кольцо фиксированных записей. head — куда писать, tail — откуда читать.
/// Заполнено: (head - tail) == capacity.
struct RecordRing {
    /// Индексы монотонно растут и никогда не сбрасываются; маска берётся по capacity.
    volatile u64 head = 0;   // пишет только продюсер
    volatile u64 tail = 0;   // пишет только потребитель
    u32 capacity = 0;
    u32 dropped = 0;         // сколько записей потеряно из-за переполнения
    u64 pushed = 0;
    u64 popped = 0;
    u8 pad[16] = {};
    Record records[kRecordRingCapacity]{};
};
static_assert(sizeof(RecordRing) < 128 * 1024, "record ring must fit into the region");

/// Байтовое кольцо для длинных данных: [u32 длина][данные]. Если кадр не
/// помещается до конца буфера — пишем «пустой» кадр-заполнитель и переносим
/// запись в начало (потребитель такой кадр просто пропускает).
struct BulkRing {
    volatile u64 head = 0;
    volatile u64 tail = 0;
    u32 capacity = 0;
    u32 dropped = 0;
    u64 pushed = 0;
    u64 popped = 0;
    u8 pad[16] = {};
    u8 data[kBulkRingCapacity]{};
};

/// Заголовок региона. Содержит оба кольца в каждую сторону и телеметрию.
/// Описание общего региона: заголовок И сами кольца (655 680 байт целиком).
///
/// Размер не случаен и не «шапка»: регион — это два кольца записей по 64 КБ
/// (512 × 128 байт) и два байтовых кольца по 256 КБ, плюс счётчики. Держите
/// объект этого типа в разделяемой памяти или в куче: на стеке главного потока
/// Windows (1 МБ) он не помещается — самотест ловил из-за этого 0xC00000FD.
struct SharedHeader {
    u32 magic = kMagic;
    u32 abiVersion = kAbiVersion;
    u32 headerSize = 0;      // проверяется обеими сторонами при рукопожатии
    u32 regionSize = 0;

    u32 gamePid = 0;
    u32 mcPid = 0;

    u64 gameHeartbeatMs = 0;
    u64 mcHeartbeatMs = 0;
    u64 gameStartedMs = 0;
    u64 mcStartedMs = 0;

    /// Игра → Minecraft: результаты раундов, столы, телеметрия.
    RecordRing gameToMc{};
    BulkRing gameToMcBulk{};

    /// Minecraft → Игра: правки вокселей, команды.
    RecordRing mcToGame{};
    BulkRing mcToGameBulk{};

    /// Информация о сборке моста (заполняется при Hello).
    u32 bridgeVersion = 0;
    char targetTag[32] = {};
};
#pragma pack(pop)

static_assert(sizeof(SharedHeader) < 1024 * 1024, "shared region sanity check");

/// Полный размер региона (используется и создателем, и подключающимся).
constexpr usize kRegionSize = sizeof(SharedHeader);

/// Имена объектов синхронизации. Потребитель ждёт своё событие, продюсер
/// делает SetEvent после записи в кольцо (авто-сброс — не накапливаем сигналы).
inline constexpr const wchar_t* kEventGameToMc = L"Local\\GWYF_MC_BRIDGE_G2M";
inline constexpr const wchar_t* kEventMcToGame = L"Local\\GWYF_MC_BRIDGE_M2G";
inline constexpr const wchar_t* kEventStop = L"Local\\GWYF_MC_BRIDGE_STOP";

/// Сколько миллисекунд без пульса считаем «партнёр умер».
constexpr u64 kPeerTimeoutMs = 5000;

/// Порог предупреждения о переполнении кольца.
constexpr u32 kBackpressureWarnThreshold = 16;

// Виды кадров в bulk-кольце: текст отчёта и массив вокселей.
constexpr u32 kBulkKindText = 1;
constexpr u32 kBulkKindVoxel = 2;
/// Заголовок кадра: u32 длина, u32 вид.
constexpr u32 kBulkFrameHeader = 8;
/// Больше четверти кольца писать нельзя: иначе кадр перестанет влезать
/// в свободное место и продюсер «заклинит» сам себя.
constexpr u32 kBulkMaxFrame = kBulkRingCapacity / 4;

// ── Кольца SPSC ─────────────────────────────────────────────────────────────
//
//  Классы ниже работают с «сырыми» структурами RecordRing/BulkRing, поэтому
//  одинаково применимы и к разделяемой памяти между процессами, и к обычной
//  памяти внутри процесса (так самотест проверяет ABI без Windows).

/// Продюсер кольца записей: единственный, кто меняет head.
class RecordProducer {
public:
    RecordProducer() = default;
    RecordProducer(RecordRing* ring, BulkRing* bulk, WakeHandle wakeEvent, Producer who, u64* heartbeatSlot)
        : ring_(ring), bulk_(bulk), wakeEvent_(wakeEvent), who_(who), heartbeat_(heartbeatSlot) {}

    [[nodiscard]] bool Valid() const { return ring_ != nullptr; }
    [[nodiscard]] u64 Pushed() const { return sequence_; }
    [[nodiscard]] u32 Dropped() const { return dropped_; }

    /// Записать запись. false = переполнение (запись теряется, счётчик растёт).
    /// Код, вызываемый из хуков, НИКОГДА не ждёт: блокировок здесь нет.
    bool Push(RecordType type, const Payload& payload, u64 sourceId = 0, u64 aux = 0);

    bool PushSimple(RecordType type);

    /// Записать длинные данные (текст отчёта, массив вокселей) в bulk-кольцо.
    bool PushBulk(const void* data, u32 size, u32 kind = kBulkKindVoxel);

    /// Отметить пульс: партнёр по нему понимает, что мы живы.
    void Heartbeat();

private:
    RecordRing* ring_ = nullptr;
    BulkRing* bulk_ = nullptr;
    WakeHandle wakeEvent_ = nullptr;
    Producer who_ = Producer::Unknown;
    u64* heartbeat_ = nullptr;
    u64 sequence_ = 0;
    u32 dropped_ = 0;

    friend class Bridge;
    void BindBulk(BulkRing* bulk) { bulk_ = bulk; }
};

/// Потребитель кольца записей: единственный, кто меняет tail.
class RecordConsumer {
public:
    RecordConsumer() = default;
    RecordConsumer(RecordRing* ring, BulkRing* bulk, WakeHandle waitEvent)
        : ring_(ring), bulk_(bulk), waitEvent_(waitEvent) {}

    [[nodiscard]] bool Valid() const { return ring_ != nullptr; }

    /// Забрать одну запись, если она есть.
    bool TryPop(Record& out);

    /// Забрать всё, что накопилось. Обработчик вызывается на нашем потоке.
    u32 Drain(const std::function<void(const Record&)>& handler, u32 maxRecords = 1024);

    /// Дождаться пробуждения. false = таймаут.
    bool Wait(u32 timeoutMs);

    /// Прочитать кадр из bulk-кольца (команды, отчёты, массовая заливка).
    bool TryReadBulk(std::string& out, u32& frameLength);

    [[nodiscard]] u64 Head() const;
    [[nodiscard]] u64 Tail() const;
    [[nodiscard]] u32 Dropped() const;
    [[nodiscard]] u64 PendingRecords() const;

private:
    RecordRing* ring_ = nullptr;
    BulkRing* bulk_ = nullptr;
    WakeHandle waitEvent_ = nullptr;
};

/// Записать float в битовое поле (для передачи через границу процессов).
inline u32 FloatToBits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float BitsToFloat(u32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

const char* ToString(RecordType type);
const char* ToString(CommandCode code);

/// Сконструировать запись с заполненными служебными полями.
Record MakeRecord(RecordType type, Producer producer, u64 sequence, u64 sourceId = 0, u64 aux = 0);

/// Проверить, что заголовок корректен (магия, версия, размеры).
bool ValidateHeader(const SharedHeader& header, std::string* error);

}  // namespace gwyf::ipc
