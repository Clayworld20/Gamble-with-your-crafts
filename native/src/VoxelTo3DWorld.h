// =============================================================================
//  VoxelTo3DWorld.h — синхронизация вокселей между Minecraft и миром казино.
//
//  Два встречных потока:
//
//   1) Minecraft → казино (основной сценарий задачи).
//      Друг поставил/убрал блок → мод отправляет VoxelEdit → мост складывает
//      правки в карту схлопывания (последняя правка клетки побеждает) → раз в
//      кадр Flush() отдаёт их владельцу процесса, и тот спавнит/убирает
//      «физический» куб в мире игры. Схлопывание принципиально: если игрок
//      тащит мышью линию блоков, в игру прилетает не 500 вызовов, а итоговый
//      набор изменённых клеток за кадр.
//
//   2) казино → Minecraft (обратный сценарий).
//      Правки своего мира (постройки в казино) уходят в мод, который зеркалит
//      их блоками Minecraft; выигрыш в казино конвертируется в GrantReward.
//
//  Формат RLE-потока (общий для C++, C# и Java — менять только с bump'ом
//  kLayoutVersion): пары [varint runLength][uint8 blockId], где 0 = воздух.
//  Порядок обхода: x (быстрее всех), затем z, затем y — как в C#-движке мира.
// =============================================================================
#pragma once

#include "gwyc/protocol.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gwyc::world {

/// Ключ клетки. Хэш — 64-битная свёртка координат (координаты миров Minecraft
/// выходят за 16 бит, поэтому пакуем с переполнением, но без коллизий на
/// диапазоне ±1M блоков, что с запасом покрывает арену).
struct VoxelKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    [[nodiscard]] bool operator==(const VoxelKey& other) const noexcept
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VoxelKeyHash {
    [[nodiscard]] size_t operator()(const VoxelKey& key) const noexcept
    {
        uint64_t hash = 1469598103934665603ull;   // FNV-1a
        const auto mix = [&hash](uint32_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        mix(static_cast<uint32_t>(key.x));
        mix(static_cast<uint32_t>(key.y));
        mix(static_cast<uint32_t>(key.z));
        return static_cast<size_t>(hash);
    }
};

/// Одна правка мира в терминах игры-казино.
struct VoxelDelta {
    VoxelKey key;
    BlockKind block = BlockKind::Air;
    uint8_t flags = 0;
    uint16_t sourceId = 0;
    uint64_t revision = 0;   ///< порядковый номер: нужен, чтобы «последняя правка побеждала»
};

/// Колбэки владельца процесса: он знает, как материализовать куб в своём мире.
struct VoxelCallbacks {
    /// Поставить/обновить куб. block == Air означает убрать.
    void (*apply)(int32_t x, int32_t y, int32_t z, uint8_t block, uint16_t sourceId) = nullptr;

    /// Статистика/диагностика в лог игры.
    void (*trace)(const char* message) = nullptr;

    [[nodiscard]] bool IsValid() const noexcept { return apply != nullptr; }
};

/// Счётчики для HUD/логов.
struct VoxelSyncStats {
    uint64_t remoteEditsReceived = 0;
    uint64_t remoteEditsApplied = 0;
    uint64_t remoteEditsCoalesced = 0;   ///< сколько правок «съел» схлопыватель
    uint64_t localEditsQueued = 0;
    uint64_t cubesSpawned = 0;
    uint64_t cubesRemoved = 0;
    uint64_t overflowDropped = 0;        ///< правок пришло больше лимита мира
    size_t pendingRemote = 0;
    size_t pendingLocal = 0;
};

class VoxelWorldSync {
public:
    /// maxCubes — сколько кубов игра согласна держать одновременно (защита от
    /// «изрисуем весь Minecraft, а казино упадёт по памяти»).
    explicit VoxelWorldSync(VoxelCallbacks callbacks = {}, size_t maxCubes = 8192);

    // ── Minecraft → казино ──────────────────────────────────────────────────

    /// Принять правку от мода. Дубликаты схлопываются: в карте остаётся
    /// последняя правка каждой клетки.
    void SubmitRemoteEdit(const MsgVoxelEdit& edit);

    /// Принять пачку правок (payload MsgVoxelBatch).
    void SubmitRemoteBatch(const MsgVoxelEdit* edits, size_t count);

    /// Разобрать payload снимка региона (RLE) и применить его целиком.
    /// Возвращает количество разобранных клеток или 0, если формат битый.
    size_t SubmitRemoteSnapshot(const MsgVoxelSnapshot& header, const uint8_t* encoded, size_t encodedSize);

    /// Применить накопленные правки в мир игры. Вызывать раз в кадр.
    /// Возвращает, сколько кубов реально тронули.
    size_t Flush();

    // ── казино → Minecraft ──────────────────────────────────────────────────

    /// Игрок казино поставил/убрал блок — отправить это в Minecraft.
    void SubmitLocalEdit(int32_t x, int32_t y, int32_t z, BlockKind block);

    /// Забрать накопленные локальные правки пачкой (для отправки в мод).
    size_t DrainLocalEdits(std::vector<MsgVoxelEdit>& out, size_t maxCount);

    // ── сервис ──────────────────────────────────────────────────────────────

    [[nodiscard]] VoxelSyncStats GetStats() const noexcept;
    [[nodiscard]] size_t MirrorCubeCount() const noexcept { return m_mirror.size(); }
    [[nodiscard]] bool IsOverCapacity() const noexcept { return m_mirror.size() >= m_maxCubes; }

    void Reset() noexcept;

private:
    void ApplyDelta(const VoxelDelta& delta);

    VoxelCallbacks m_callbacks;
    size_t m_maxCubes;
    uint64_t m_revision = 0;

    std::unordered_map<VoxelKey, VoxelDelta, VoxelKeyHash> m_pendingRemote;   // схлопывание
    std::unordered_map<VoxelKey, VoxelDelta, VoxelKeyHash> m_mirror;          // что уже стоит в мире игры
    std::vector<MsgVoxelEdit> m_pendingLocal;
    VoxelSyncStats m_stats{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  RLE — общий формат для трёх языков (C++, C#, Java).
// ─────────────────────────────────────────────────────────────────────────────

/// Сжать массив блоков в поток [varint run][uint8 block]. Длина входа = sizeX*sizeY*sizeZ.
[[nodiscard]] std::vector<uint8_t> EncodeRle(const uint8_t* blocks, size_t count);

/// Развернуть RLE-поток в массив блоков. false — поток битый (нулевая серия,
/// выход за границы, неполные данные) — такой снимок применять нельзя.
[[nodiscard]] bool DecodeRle(const uint8_t* encoded, size_t encodedSize, uint8_t* out, size_t count);

/// Записать varint (LEB128) — используется и RLE, и полями протокола.
void WriteVarUInt(std::vector<uint8_t>& out, uint32_t value);

/// Прочитать varint. Возвращает false при чтении за границей буфера.
[[nodiscard]] bool ReadVarUInt(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) noexcept;

} // namespace gwyc::world
