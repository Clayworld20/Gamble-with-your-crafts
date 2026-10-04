// ============================================================================
//  McWireFormat.cpp — раскладка байтов, хеш раскладки и упаковка правок.
//
//  Файл переносимый: он проверяется самотестом (в том числе на CI), потому
//  что именно здесь ломается интеграция «нативная библиотека ↔ Java-мод».
// ============================================================================
#include "gwyfbridge/McWireFormat.h"

#include <cstdio>
#include <cstring>

namespace gwyf::mcwire {

namespace {

/// Смещения внутри структур полезной нагрузки. Выведены из объявлений
/// IpcProtocol.h (POD, #pragma pack(8), фиксированные типы) и продублированы
/// в Java-классе BridgeRecords — рассинхрон ловится хешем раскладки.
constexpr u32 kBetAmountBits = 0;
constexpr u32 kBetChipType = 4;
constexpr u32 kBetBetKind = 8;
constexpr u32 kBetTableId = 12;
constexpr u32 kBetPlayerId = 16;
constexpr u32 kBetSeat = 24;
constexpr u32 kBetFlags = 28;

constexpr u32 kBetResPayout = 0;
constexpr u32 kBetResChipsAfter = 8;
constexpr u32 kBetResTableId = 16;
constexpr u32 kBetResSeat = 20;
constexpr u32 kBetResWon = 24;
constexpr u32 kBetResPocket = 25;
constexpr u32 kBetResDiceA = 26;
constexpr u32 kBetResDiceB = 27;
constexpr u32 kBetResPlayerId = 32;

constexpr u32 kTableId = 0;
constexpr u32 kTableLobbyId = 4;
constexpr u32 kTableSeats = 8;
constexpr u32 kTablePhase = 12;
constexpr u32 kTableGameKind = 16;
constexpr u32 kTablePosX = 20;
constexpr u32 kTablePosY = 24;
constexpr u32 kTablePosZ = 28;
constexpr u32 kTableMinBet = 32;

constexpr u32 kChipsChips = 0;
constexpr u32 kChipsPending = 8;
constexpr u32 kChipsBank = 16;
constexpr u32 kChipsPlayers = 24;
constexpr u32 kChipsRound = 28;
constexpr u32 kChipsPlayerId = 32;

constexpr u32 kVoxelX = 0;
constexpr u32 kVoxelY = 4;
constexpr u32 kVoxelZ = 8;
constexpr u32 kVoxelBlock = 12;
constexpr u32 kVoxelAction = 16;
constexpr u32 kVoxelAuthor = 24;

constexpr u32 kGrantBlock = 0;
constexpr u32 kGrantCount = 4;
constexpr u32 kGrantPlayerId = 8;
constexpr u32 kGrantReason = 16;
constexpr u32 kGrantReserved = 20;

constexpr u32 kCommandCode = 0;
constexpr u32 kCommandArg0 = 4;
constexpr u32 kCommandArg1 = 8;
constexpr u32 kCommandTextLength = 12;
constexpr u32 kCommandTextOffset = 16;

template <typename T>
void Append(u32 offset, const char* name, std::string& out) {
    char line[128]{};
    std::snprintf(line, sizeof(line), "%s=%u:%u;", name, offset, static_cast<u32>(sizeof(T)));
    out += line;
}

u32 Fnv1a(const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    u32 hash = 2166136261u;
    for (usize index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

std::string RecordLayoutText() {
    std::string out;
    out.reserve(1024);

    // Запись целиком.
    Append<u32>(kOffsetType, "rec.type", out);
    Append<u32>(kOffsetFlags, "rec.flags", out);
    Append<u64>(kOffsetSourceId, "rec.sourceId", out);
    Append<u64>(kOffsetAux, "rec.aux", out);
    Append<u8[ipc::kInlinePayload]>(kOffsetPayload, "rec.payload", out);

    // Полезные нагрузки по типам событий.
    Append<u32>(kBetAmountBits, "bet.amountBits", out);
    Append<u32>(kBetChipType, "bet.chipType", out);
    Append<u32>(kBetBetKind, "bet.betKind", out);
    Append<u32>(kBetTableId, "bet.tableId", out);
    Append<u64>(kBetPlayerId, "bet.playerId", out);
    Append<u32>(kBetSeat, "bet.seat", out);
    Append<u32>(kBetFlags, "bet.flags", out);

    Append<i64>(kBetResPayout, "result.payout", out);
    Append<i64>(kBetResChipsAfter, "result.chipsAfter", out);
    Append<u32>(kBetResTableId, "result.tableId", out);
    Append<u32>(kBetResSeat, "result.seat", out);
    Append<u8>(kBetResWon, "result.won", out);
    Append<u8>(kBetResPocket, "result.wheelPocket", out);
    Append<u8>(kBetResDiceA, "result.diceA", out);
    Append<u8>(kBetResDiceB, "result.diceB", out);
    Append<u64>(kBetResPlayerId, "result.playerId", out);

    Append<u32>(kTableId, "table.tableId", out);
    Append<u32>(kTableLobbyId, "table.lobbyId", out);
    Append<u32>(kTableSeats, "table.seats", out);
    Append<u32>(kTablePhase, "table.phase", out);
    Append<u32>(kTableGameKind, "table.gameKind", out);
    Append<i32>(kTablePosX, "table.posX", out);
    Append<i32>(kTablePosY, "table.posY", out);
    Append<i32>(kTablePosZ, "table.posZ", out);
    Append<i64>(kTableMinBet, "table.minBet", out);

    Append<i64>(kChipsChips, "chips.chips", out);
    Append<i64>(kChipsPending, "chips.pendingBet", out);
    Append<i64>(kChipsBank, "chips.bank", out);
    Append<u32>(kChipsPlayers, "chips.playerCount", out);
    Append<u32>(kChipsRound, "chips.roundIndex", out);
    Append<u64>(kChipsPlayerId, "chips.playerId", out);

    Append<i32>(kVoxelX, "voxel.x", out);
    Append<i32>(kVoxelY, "voxel.y", out);
    Append<i32>(kVoxelZ, "voxel.z", out);
    Append<u32>(kVoxelBlock, "voxel.block", out);
    Append<u32>(kVoxelAction, "voxel.action", out);
    Append<u64>(kVoxelAuthor, "voxel.authorId", out);

    Append<u32>(kGrantBlock, "grant.block", out);
    Append<u32>(kGrantCount, "grant.count", out);
    Append<u64>(kGrantPlayerId, "grant.playerId", out);
    Append<u32>(kGrantReason, "grant.reason", out);
    Append<u32>(kGrantReserved, "grant.reserved", out);

    Append<u32>(kCommandCode, "command.code", out);
    Append<u32>(kCommandArg0, "command.arg0", out);
    Append<u32>(kCommandArg1, "command.arg1", out);
    Append<u32>(kCommandTextLength, "command.textLength", out);
    Append<u32>(kCommandTextOffset, "command.textOffset", out);

    char stride[64]{};
    std::snprintf(stride, sizeof(stride), "stride=%u;bulk=%u;", kRecordStride, kBulkEditStride);
    out += stride;
    return out;
}

u32 LayoutHash() {
    const std::string text = RecordLayoutText();
    return Fnv1a(text.data(), text.size());
}

bool WriteRecord(u8* out, u32 capacity, const ipc::Record& record, u32* written) {
    if (written != nullptr) *written = 0;
    if (out == nullptr || capacity < kRecordStride) return false;

    std::memcpy(out + kOffsetType, &record.type, sizeof(record.type));
    const u32 flags = record.flags;
    std::memcpy(out + kOffsetFlags, &flags, sizeof(flags));
    std::memcpy(out + kOffsetSourceId, &record.sourceId, sizeof(record.sourceId));
    std::memcpy(out + kOffsetAux, &record.aux, sizeof(record.aux));
    std::memcpy(out + kOffsetPayload, record.payload.raw, ipc::kInlinePayload);

    if (written != nullptr) *written = kRecordStride;
    return true;
}

u32 PackBulkEdits(const ipc::PayloadVoxel* edits, u32 count, u8* out, u32 capacity) {
    if (edits == nullptr || out == nullptr) return 0;

    const u32 limit = capacity / kBulkEditStride;
    if (count > limit) count = limit;

    for (u32 index = 0; index < count; ++index) {
        u8* entry = out + static_cast<usize>(index) * kBulkEditStride;
        const ipc::PayloadVoxel& edit = edits[index];

        std::memcpy(entry + 0, &edit.x, sizeof(edit.x));
        std::memcpy(entry + 4, &edit.y, sizeof(edit.y));
        std::memcpy(entry + 8, &edit.z, sizeof(edit.z));
        std::memcpy(entry + 12, &edit.block, sizeof(edit.block));
        std::memcpy(entry + 16, &edit.action, sizeof(edit.action));
    }

    return count;
}

u32 UnpackBulkEdits(const u8* data, u32 size, const std::function<void(const ipc::PayloadVoxel&)>& sink) {
    if (data == nullptr || sink == nullptr) return 0;

    const u32 count = size / kBulkEditStride;
    for (u32 index = 0; index < count; ++index) {
        const u8* entry = data + static_cast<usize>(index) * kBulkEditStride;

        ipc::PayloadVoxel edit{};
        std::memcpy(&edit.x, entry + 0, sizeof(edit.x));
        std::memcpy(&edit.y, entry + 4, sizeof(edit.y));
        std::memcpy(&edit.z, entry + 8, sizeof(edit.z));
        std::memcpy(&edit.block, entry + 12, sizeof(edit.block));
        std::memcpy(&edit.action, entry + 16, sizeof(edit.action));
        edit.authorId = 0;

        sink(edit);
    }

    return count;
}

std::string Describe() {
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Формат моста с JVM: запись %u Б (payload на смещении %u), "
                  "пакет вокселей %u Б на элемент, раскладка #%08X",
                  kRecordStride, kOffsetPayload, kBulkEditStride, LayoutHash());
    return buffer;
}

}  // namespace gwyf::mcwire
