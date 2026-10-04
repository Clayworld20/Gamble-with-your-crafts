// ============================================================================
//  McWireFormat.h — формат данных, которые нативная библиотека отдаёт JVM.
//
//  Зачем отдельный файл: Java и C++ должны договориться о байтах. Любая
//  правка раскладки структуры, сделанная только с одной стороны, — это
//  классический источник «тихо неправильных чисел», который невозможно
//  заметить глазами. Поэтому:
//
//    1. раскладка описана здесь один раз и отдаётся в Java текстом
//       (RecordLayoutText) для логов;
//    2. от текста считается хеш FNV-1a (LayoutHash); Java считает тот же хеш
//       из своих констант и сравнивает при инициализации. Расхождение = мод
//       пишет громкую ошибку в лог вместо того, чтобы рисовать ерунду.
//
//  Формат кадра записи (по 88 байт на запись), записывается в прямой
//  ByteBuffer строго в порядке нативного little-endian:
//      [0]  u32  type          — RecordType
//      [4]  u32  flags         — зарезервировано
//      [8]  u64  sourceId      — SteamId / NetId автора
//      [16] u64  aux           — произвольная метка (индекс раунда и т.п.)
//      [24] 64 B payload       — сырые байты ipc::Payload
// ============================================================================
#pragma once

#include "Base.h"
#include "IpcProtocol.h"

#include <functional>
#include <string>
#include <vector>

namespace gwyf::mcwire {

/// Размер одной записи в буфере обмена с JVM.
constexpr u32 kRecordStride = 88;

/// Смещения полей внутри записи.
constexpr u32 kOffsetType = 0;
constexpr u32 kOffsetFlags = 4;
constexpr u32 kOffsetSourceId = 8;
constexpr u32 kOffsetAux = 16;
constexpr u32 kOffsetPayload = 24;

/// Размер записи массовой заливки вокселей (упакованный VoxelEdit).
constexpr u32 kBulkEditStride = 20;
constexpr u32 kBulkEditMaxPerFrame = ipc::kBulkMaxFrame / kBulkEditStride;

/// Текстовая раскладка структур полезной нагрузки (для логов и хеша).
std::string RecordLayoutText();

/// Хеш FNV-1a от RecordLayoutText(). Сравнивается с константой на стороне Java.
u32 LayoutHash();

/// Записать одну запись в буфер. false = буфер мал.
bool WriteRecord(u8* out, u32 capacity, const ipc::Record& record, u32* written);

/// Упаковать правки вокселей в кадр массовой заливки.
/// out должен вмещать count * kBulkEditStride байт.
u32 PackBulkEdits(const ipc::PayloadVoxel* edits, u32 count, u8* out, u32 capacity);

/// Разобрать кадр массовой заливки. Возвращает число разобранных записей.
u32 UnpackBulkEdits(const u8* data, u32 size, const std::function<void(const ipc::PayloadVoxel&)>& sink);

/// Человекочитаемое описание формата для команды `status` в игре и логов мода.
std::string Describe();

}  // namespace gwyf::mcwire
