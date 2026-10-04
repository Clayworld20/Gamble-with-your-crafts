// =============================================================================
//  VoxelTo3DWorld.cpp — схлопывание правок, применение в мир игры, RLE.
// =============================================================================
#include "VoxelTo3DWorld.h"

#include <algorithm>
#include <cstring>

namespace gwyc::world {

namespace {

/// Верхний предел разумного региона: защита от декомпрессионной бомбы в снимке.
constexpr size_t kMaxSnapshotCells = 512u * 512u * 512u;

} // namespace

VoxelWorldSync::VoxelWorldSync(VoxelCallbacks callbacks, size_t maxCubes)
    : m_callbacks(callbacks)
    , m_maxCubes(maxCubes == 0 ? 1 : maxCubes)
{
    m_pendingRemote.reserve(1024);
    m_mirror.reserve(1024);
    m_pendingLocal.reserve(256);
}

void VoxelWorldSync::SubmitRemoteEdit(const MsgVoxelEdit& edit)
{
    ++m_stats.remoteEditsReceived;

    if (!IsValidBlockId(edit.blockId)) {
        ++m_stats.overflowDropped;
        return;
    }

    VoxelDelta delta;
    delta.key = VoxelKey{ edit.x, edit.y, edit.z };
    delta.block = static_cast<BlockKind>(edit.blockId);
    delta.flags = edit.flags;
    delta.sourceId = edit.sourceId;
    delta.revision = ++m_revision;

    const auto existing = m_pendingRemote.find(delta.key);
    if (existing != m_pendingRemote.end()) {
        // Клетку уже правили в этом кадре: побеждает последняя правка.
        ++m_stats.remoteEditsCoalesced;
        existing->second = delta;
        return;
    }

    m_pendingRemote.emplace(delta.key, delta);
}

void VoxelWorldSync::SubmitRemoteBatch(const MsgVoxelEdit* edits, size_t count)
{
    if (edits == nullptr || count == 0) return;
    if (count > 100000) count = 100000;   // защита от мусорного заголовка пачки

    for (size_t i = 0; i < count; ++i) {
        SubmitRemoteEdit(edits[i]);
    }
}

size_t VoxelWorldSync::SubmitRemoteSnapshot(const MsgVoxelSnapshot& header, const uint8_t* encoded, size_t encodedSize)
{
    const size_t cells = static_cast<size_t>(header.sizeX) * header.sizeY * header.sizeZ;
    if (cells == 0 || cells > kMaxSnapshotCells || encoded == nullptr || encodedSize == 0) return 0;

    std::vector<uint8_t> blocks(cells, 0);
    if (!DecodeRle(encoded, encodedSize, blocks.data(), cells)) return 0;

    size_t applied = 0;
    size_t index = 0;

    // Порядок обхода совпадает с C#-движком: x быстрее всех, затем z, затем y.
    for (int y = 0; y < header.sizeY; ++y) {
        for (int z = 0; z < header.sizeZ; ++z) {
            for (int x = 0; x < header.sizeX; ++x, ++index) {
                MsgVoxelEdit edit{};
                edit.x = header.originX + x;
                edit.y = header.originY + y;
                edit.z = header.originZ + z;
                edit.blockId = blocks[index];
                edit.flags = 1;
                SubmitRemoteEdit(edit);
                ++applied;
            }
        }
    }

    return applied;
}

size_t VoxelWorldSync::Flush()
{
    if (m_pendingRemote.empty()) return 0;
    if (!m_callbacks.IsValid()) {
        // Материализовать кубы некому: чистим очередь, но не теряем статистику молча.
        m_stats.overflowDropped += m_pendingRemote.size();
        m_pendingRemote.clear();
        return 0;
    }

    size_t touched = 0;
    for (const auto& [key, delta] : m_pendingRemote) {
        const bool removing = (delta.block == BlockKind::Air);

        if (removing) {
            if (m_mirror.erase(key) == 0) continue;   // и не было — трогать нечего
            ++m_stats.cubesRemoved;
        }
        else {
            if (m_mirror.size() >= m_maxCubes && m_mirror.find(key) == m_mirror.end()) {
                ++m_stats.overflowDropped;
                continue;
            }
            m_mirror[key] = delta;
            ++m_stats.cubesSpawned;
        }

        m_callbacks.apply(key.x, key.y, key.z, static_cast<uint8_t>(delta.block), delta.sourceId);
        ++touched;
        ++m_stats.remoteEditsApplied;
    }

    m_pendingRemote.clear();
    return touched;
}

void VoxelWorldSync::SubmitLocalEdit(int32_t x, int32_t y, int32_t z, BlockKind block)
{
    const auto key = VoxelKey{ x, y, z };

    // Схлопываем и исходящий поток: игрок мог сто раз переставить один блок.
    for (MsgVoxelEdit& pending : m_pendingLocal) {
        if (pending.x == key.x && pending.y == key.y && pending.z == key.z) {
            pending.blockId = static_cast<uint8_t>(block);
            ++m_stats.localEditsQueued;
            return;
        }
    }

    MsgVoxelEdit edit{};
    edit.x = x;
    edit.y = y;
    edit.z = z;
    edit.blockId = static_cast<uint8_t>(block);
    edit.flags = 1;   // правка от локального игрока

    m_pendingLocal.push_back(edit);
    ++m_stats.localEditsQueued;
}

size_t VoxelWorldSync::DrainLocalEdits(std::vector<MsgVoxelEdit>& out, size_t maxCount)
{
    if (m_pendingLocal.empty() || maxCount == 0) return 0;

    const size_t take = std::min(maxCount, m_pendingLocal.size());
    out.insert(out.end(), m_pendingLocal.begin(), m_pendingLocal.begin() + static_cast<std::ptrdiff_t>(take));
    m_pendingLocal.erase(m_pendingLocal.begin(), m_pendingLocal.begin() + static_cast<std::ptrdiff_t>(take));
    return take;
}

VoxelSyncStats VoxelWorldSync::GetStats() const noexcept
{
    VoxelSyncStats stats = m_stats;
    stats.pendingRemote = m_pendingRemote.size();
    stats.pendingLocal = m_pendingLocal.size();
    return stats;
}

void VoxelWorldSync::Reset() noexcept
{
    m_pendingRemote.clear();
    m_mirror.clear();
    m_pendingLocal.clear();
    m_revision = 0;
    m_stats = VoxelSyncStats{};
}

// ─────────────────────────────────────────────────────────────────────────────
//  RLE
// ─────────────────────────────────────────────────────────────────────────────

void WriteVarUInt(std::vector<uint8_t>& out, uint32_t value)
{
    while (value >= 0x80u) {
        out.push_back(static_cast<uint8_t>(value | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

bool ReadVarUInt(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) noexcept
{
    uint32_t result = 0;
    int shift = 0;

    while (true) {
        if (offset >= size) return false;
        if (shift > 35) return false;   // слишком длинный varint = мусор

        const uint8_t byte = data[offset++];
        result |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
    }

    value = result;
    return true;
}

std::vector<uint8_t> EncodeRle(const uint8_t* blocks, size_t count)
{
    std::vector<uint8_t> encoded;
    if (blocks == nullptr || count == 0) return encoded;

    encoded.reserve(count / 4 + 16);

    size_t index = 0;
    while (index < count) {
        const uint8_t value = blocks[index];
        size_t run = 1;
        while (index + run < count && blocks[index + run] == value) ++run;

        WriteVarUInt(encoded, static_cast<uint32_t>(run));
        encoded.push_back(value);
        index += run;
    }

    return encoded;
}

bool DecodeRle(const uint8_t* encoded, size_t encodedSize, uint8_t* out, size_t count)
{
    if (encoded == nullptr || out == nullptr || count == 0) return false;

    size_t offset = 0;
    size_t written = 0;
    const size_t limit = count > kMaxSnapshotCells ? kMaxSnapshotCells : count;

    while (offset < encodedSize) {
        uint32_t run = 0;
        if (!ReadVarUInt(encoded, encodedSize, offset, run)) return false;
        if (run == 0) return false;                       // нулевая серия — битый поток
        if (offset >= encodedSize) return false;

        const uint8_t value = encoded[offset++];
        if (written + run > limit) return false;          // выход за границы региона

        std::memset(out + written, value, run);
        written += run;
    }

    return written == limit;
}

} // namespace gwyc::world
