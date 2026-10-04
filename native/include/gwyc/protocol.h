// =============================================================================
//  Gamble With Your Crafts — native bridge
//  protocol.h — единый формат обмена между процессами.
//
//  Все структуры ниже — POD, упакованы в 1 байт (#pragma pack(1)) и обязаны
//  совпадать байт-в-байт в C++, C# и Java. Любое изменение = новый layout_version.
//
//  Формат кадра в канале (shared memory / named pipe):
//
//      +--------+--------+--------+--------+---------------------+--------+
//      | magic  | layout |  type  | flags  | payload_size (u32)  | crc32  |
//      | u16    | u16    |  u8    |  u8    |                     | u32    |
//      +--------+--------+--------+--------+---------------------+--------+
//      |                        payload (payload_size байт)               |
//      +------------------------------------------------------------------+
//
//  crc32 считается по payload+заголовку(type/flags/size) — принимающая сторона
//  отбрасывает кадр при несовпадении (защита от рваного чтения и мусора).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_MSC_VER)
#   define GWYC_PACK_PUSH __pragma(pack(push, 1))
#   define GWYC_PACK_POP  __pragma(pack(pop))
#else
#   define GWYC_PACK_PUSH _Pragma("pack(push, 1)")
#   define GWYC_PACK_POP  _Pragma("pack(pop)")
#endif

#if defined(_WIN32)
#   define GWYC_EXPORT extern "C" __declspec(dllexport)
#   define GWYC_IMPORT extern "C" __declspec(dllimport)
#else
#   define GWYC_EXPORT extern "C" __attribute__((visibility("default")))
#   define GWYC_IMPORT extern "C"
#endif

namespace gwyc {

// ─────────────────────────────────────────────────────────────────────────────
//  Версии
// ─────────────────────────────────────────────────────────────────────────────

/// Версия бинарной раскладки. Меняется при любой правке структур ниже.
inline constexpr uint16_t kLayoutVersion = 3;

/// Версия логики протокола (набор сообщений и их смысл).
inline constexpr uint16_t kProtocolVersion = 1;

/// 'GW' — быстрый отсев мусора на входе канала.
inline constexpr uint16_t kMagic = 0x5747;

/// Максимальный payload одного кадра. Большие объёмы режутся на несколько кадров.
inline constexpr uint32_t kMaxPayloadSize = 64 * 1024;

/// Заголовок кадра: 12 байт.
GWYC_PACK_PUSH
struct FrameHeader {
    uint16_t magic;
    uint16_t layoutVersion;
    uint8_t  type;
    uint8_t  flags;
    uint32_t payloadSize;
    uint32_t crc32;
};
GWYC_PACK_POP

static_assert(sizeof(FrameHeader) == 14, "FrameHeader должен быть ровно 14 байт — он же формат канала");

/// Флаги кадра.
enum FrameFlags : uint8_t {
    kFlagNone      = 0,
    kFlagReliable  = 1u << 0,  ///< кадр нельзя терять (события ставок, награды)
    kFlagFragmented = 1u << 1, ///< часть многофрагментного сообщения
    kFlagCompressed = 1u << 2, ///< payload сжат RLE (пачки вокселей)
};

/// Типы сообщений.
enum class MsgType : uint8_t {
    None          = 0x00,

    // рукопожатие
    Hello         = 0x01,  ///< layout/proto/pid/тег сборки — обе стороны проверяют совместимость
    HelloAck      = 0x02,
    Ping          = 0x03,
    Pong          = 0x04,
    Log           = 0x05,

    // воксели: Minecraft → казино
    VoxelEdit     = 0x10,  ///< одна правка (поставить/убрать)
    VoxelBatch    = 0x11,  ///< пачка правок
    VoxelSnapshot = 0x12,  ///< полный снимок региона (RLE)

    // события казино: игра → Minecraft
    PlayerState   = 0x20,  ///< фишки игрока, баланс, статус
    BetPlaced     = 0x21,  ///< ставка принята (только чтение, ничего не меняет)
    BetResolved   = 0x22,  ///< раунд завершён: выплата, множитель, выигрыш/проигрыш
    TableEvent    = 0x30,  ///< появление/изменение стола: лимиты, места, состояние
    GrantReward   = 0x40,  ///< выдать игроку блоки в Minecraft
    ConsoleToMc   = 0x41,  ///< строка из чата сессии в Minecraft
    ConsoleToGame = 0x42,  ///< строка из Minecraft в консоль игры

    // служебные
    Shutdown      = 0x7F,
};

const char* MsgTypeName(MsgType type) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
//  Payload'ы
// ─────────────────────────────────────────────────────────────────────────────

/// Максимум символов в строковых полях (без терминирующего нуля).
inline constexpr size_t kTagLength  = 32;
inline constexpr size_t kNameLength = 24;
inline constexpr size_t kTextLength = 160;

GWYC_PACK_PUSH
struct MsgHello {
    uint16_t layoutVersion;
    uint16_t protocolVersion;
    uint32_t pid;
    uint32_t role;              ///< 0 = игра (казино), 1 = Minecraft
    char     buildTag[kTagLength];  ///< UTF-8, может быть без нуля — читать через CopyFixed()
};
GWYC_PACK_POP

GWYC_PACK_PUSH
struct MsgPingPong {
    uint64_t seq;
    uint64_t sendTimeMs;   ///< utc ms, для замера RTT
};
GWYC_PACK_POP

GWYC_PACK_PUSH
struct MsgLog {
    uint8_t  level;        ///< 0=info 1=warn 2=error
    char     text[kTextLength];
};
GWYC_PACK_POP

/// Одна правка вокселя. Координаты — в мировых блоках Minecraft.
GWYC_PACK_PUSH
struct MsgVoxelEdit {
    int32_t x;
    int32_t y;
    int32_t z;
    uint8_t blockId;       ///< 0 = убрать блок, иначе — id из таблицы соответствия
    uint8_t flags;         ///< 1 = правка от локального игрока, 2 = пришла от другого
    uint16_t sourceId;     ///< кто поставил (индекс в таблице игроков Minecraft, 0 = мир)
};
GWYC_PACK_POP

/// Заголовок пачки правок: следом идут MsgVoxelEdit подряд.
GWYC_PACK_PUSH
struct MsgVoxelBatch {
    uint32_t count;
    uint32_t firstIndex;   ///< для фрагментации: индекс первой правки в пачке
};
GWYC_PACK_POP

/// Снимок региона: RLE-поток пар [varint run][uint8 block], как в C#-части проекта.
GWYC_PACK_PUSH
struct MsgVoxelSnapshot {
    int32_t originX;
    int32_t originY;
    int32_t originZ;
    uint16_t sizeX;
    uint16_t sizeY;
    uint16_t sizeZ;
    uint32_t encodedSize;
};
GWYC_PACK_POP

/// Состояние игрока казино (фишки — игровая валюта, не деньги).
GWYC_PACK_PUSH
struct MsgPlayerState {
    uint32_t playerId;
    int64_t  chips;        ///< текущий баланс фишек
    int64_t  netWorth;     ///< чистое изменение за сессию
    uint8_t  lobbyState;   ///< 0=не в лобби 1=в лобби 2=хост
    uint8_t  reserved[3];
    char     name[kNameLength];
};
GWYC_PACK_POP

/// Ставка принята. Только для отображения — менять состояние по нему нельзя.
GWYC_PACK_PUSH
struct MsgBetPlaced {
    uint32_t playerId;
    int64_t  stake;
    uint8_t  game;         ///< 1 = кости, 2 = рулетка
    char     target[16];
    char     name[kNameLength];
};
GWYC_PACK_POP

/// Раунд завершён.
GWYC_PACK_PUSH
struct MsgBetResolved {
    uint32_t playerId;
    int64_t  stake;
    int64_t  payout;
    int32_t  multiplier;
    uint8_t  won;          ///< 1 = выигрыш
    uint8_t  game;         ///< 1 = кости, 2 = рулетка
    uint8_t  blockType;    ///< какой блок был на кону (см. BlockKind)
    uint8_t  reserved;
    char     target[16];
    char     name[kNameLength];
};
GWYC_PACK_POP

/// Стол появился/изменился.
GWYC_PACK_PUSH
struct MsgTableEvent {
    uint32_t tableId;      ///< стабильный id стола в мире казино
    int32_t  x, y, z;      ///< мировые координаты в казино (для отладки/визуализации)
    int64_t  minBet;
    int64_t  maxBet;
    uint8_t  seats;
    uint8_t  state;        ///< 0=создан 1=активен 2=закрыт
    char     owner[kNameLength];
};
GWYC_PACK_POP

/// Награда: выдать блоки в Minecraft.
GWYC_PACK_PUSH
struct MsgGrantReward {
    uint32_t playerId;
    uint16_t blockKind;    ///< тип блока
    int32_t  count;
    uint8_t  reason;       ///< 0=выигрыш в казино 1=обмен фишек 2=отладка
    uint8_t  reserved;
};
GWYC_PACK_POP

/// Строка чата в любую сторону.
GWYC_PACK_PUSH
struct MsgChatLine {
    uint8_t channel;       ///< 0=сессия 1=система
    char    author[kNameLength];
    char    text[kTextLength];
};
GWYC_PACK_POP

/// Соответствие типов блоков C++ ↔ C# ↔ Minecraft.
/// Значения общие для всех трёх сторон — не менять без bump'а layout_version.
enum class BlockKind : uint8_t {
    Air     = 0,
    Dirt    = 1,
    Grass   = 2,
    Stone   = 3,
    Wood    = 4,
    Gold    = 5,
    Diamond = 6,
    Table   = 7,
    Lamp    = 8,
};

/// Проверка, что байт из сети описывает известный тип блока.
/// Всё, что пришло извне, проверяется до использования — иначе мусорный байт
/// превратится в выход за пределы таблицы блоков.
[[nodiscard]] inline bool IsValidBlockId(uint8_t id) noexcept
{
    return id <= static_cast<uint8_t>(BlockKind::Lamp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Утилиты
// ─────────────────────────────────────────────────────────────────────────────

/// Потоковый CRC32 (IEEE, bitwise). Малый и предсказуемый; в профиле не участвует.
///
/// Нужен именно потоковый вариант: кадр состоит из двух несмежных кусков памяти
/// (поля заголовка и payload), и склеивать их ради контрольной суммы — лишний memcpy.
class Crc32Stream {
public:
    void Update(const void* data, size_t size) noexcept
    {
        static constexpr uint32_t kPoly = 0xEDB88320u;
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint32_t crc = m_state;

        for (size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) {
                const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
                crc = (crc >> 1) ^ (kPoly & mask);
            }
        }

        m_state = crc;
    }

    [[nodiscard]] uint32_t Final() const noexcept { return ~m_state; }

private:
    uint32_t m_state = 0xFFFFFFFFu;
};

/// CRC32 одним вызовом (для тестов и мелких сообщений).
[[nodiscard]] inline uint32_t Crc32(const void* data, size_t size) noexcept
{
    Crc32Stream stream;
    stream.Update(data, size);
    return stream.Final();
}

/// Скопировать строку фиксированной длины в std::string (поле может быть без '\0').
inline void CopyFixed(const char* field, size_t capacity, char* out, size_t outSize) noexcept
{
    if (outSize == 0) return;
    size_t length = 0;
    while (length < capacity && field[length] != '\0') ++length;
    if (length >= outSize) length = outSize - 1;
    std::memcpy(out, field, length);
    out[length] = '\0';
}

/// Записать строку в фиксированное поле с гарантированным нулём в конце.
inline void WriteFixed(char* field, size_t capacity, const char* text) noexcept
{
    if (capacity == 0) return;
    size_t length = 0;
    while (length + 1 < capacity && text[length] != '\0') ++length;
    std::memcpy(field, text, length);
    field[length] = '\0';
}

/// Проверка размеров на этапе компиляции: правки структур ломают сборку, а не рантайм.
static_assert(sizeof(MsgHello) == 2 + 2 + 4 + 4 + kTagLength, "MsgHello: неожиданный размер");
static_assert(sizeof(MsgVoxelEdit) == 4 + 4 + 4 + 1 + 1 + 2, "MsgVoxelEdit: неожиданный размер");
static_assert(sizeof(MsgBetResolved) == 4 + 8 + 8 + 4 + 1 + 1 + 1 + 1 + 16 + kNameLength, "MsgBetResolved: неожиданный размер");
static_assert(sizeof(MsgGrantReward) == 4 + 2 + 4 + 1 + 1, "MsgGrantReward: неожиданный размер");
static_assert(sizeof(MsgTableEvent) == 4 + 4 + 4 + 4 + 8 + 8 + 1 + 1 + kNameLength, "MsgTableEvent: неожиданный размер");

/// Размеры payload'ов: те же числа продублированы в Java (Payloads.java) и C#.
/// Любое расхождение здесь = сломанный кадр на той стороне, поэтому размеры
/// проверяются на этапе компиляции.
static_assert(sizeof(MsgHello) == 44, "MsgHello: 2+2+4+4+32");
static_assert(sizeof(MsgPingPong) == 16, "MsgPingPong: 8+8");
static_assert(sizeof(MsgLog) == 161, "MsgLog: 1+160");
static_assert(sizeof(MsgVoxelEdit) == 16, "MsgVoxelEdit: 4+4+4+1+1+2");
static_assert(sizeof(MsgVoxelBatch) == 8, "MsgVoxelBatch: 4+4");
static_assert(sizeof(MsgVoxelSnapshot) == 22, "MsgVoxelSnapshot: 12+6+4");
static_assert(sizeof(MsgPlayerState) == 48, "MsgPlayerState: 4+8+8+1+3+24");
static_assert(sizeof(MsgBetPlaced) == 53, "MsgBetPlaced: 4+8+1+16+24");
static_assert(sizeof(MsgBetResolved) == 68, "MsgBetResolved: 4+8+8+4+4+16+24");
static_assert(sizeof(MsgTableEvent) == 58, "MsgTableEvent: 4+12+16+2+24");
static_assert(sizeof(MsgGrantReward) == 12, "MsgGrantReward: 4+2+4+1+1");
// Важно: в протокольной структуре НЕТ выравнивающих байтов (флаг pack(1)),
// поэтому 1 + 24 + 160 = 185. Три байта reserved есть только в ABI-структуре
// GwycChatLine (plugin_abi.h) и на провод не попадают.
static_assert(sizeof(MsgChatLine) == 185, "MsgChatLine: 1+24+160");

/// Проверки смещений: раскладка обязана совпадать в MSVC/GCC/Clang и в Java.
static_assert(offsetof(FrameHeader, magic) == 0, "FrameHeader.magic");
static_assert(offsetof(FrameHeader, layoutVersion) == 2, "FrameHeader.layoutVersion");
static_assert(offsetof(FrameHeader, type) == 4, "FrameHeader.type");
static_assert(offsetof(FrameHeader, flags) == 5, "FrameHeader.flags");
static_assert(offsetof(FrameHeader, payloadSize) == 6, "FrameHeader.payloadSize");
static_assert(offsetof(FrameHeader, crc32) == 10, "FrameHeader.crc32");
static_assert(offsetof(MsgVoxelEdit, blockId) == 12, "MsgVoxelEdit.blockId");
// playerId(4) + stake(8) + payout(8) + multiplier(4) + 4 u8-поля = 28
static_assert(offsetof(MsgBetResolved, target) == 28, "MsgBetResolved.target");

/// Проверка «сырое окно байт → POD» без нарушения strict aliasing.
template <typename T>
[[nodiscard]] inline bool ReadPod(const uint8_t* data, size_t size, T& out) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "ReadPod работает только с POD");
    if (data == nullptr || size < sizeof(T)) return false;
    std::memcpy(&out, data, sizeof(T));
    return true;
}

} // namespace gwyc
