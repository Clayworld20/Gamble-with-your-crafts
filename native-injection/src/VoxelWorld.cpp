// ============================================================================
//  VoxelWorld.cpp — очередь правок, зеркало объектов и бюджет спавна.
// ============================================================================
#include "gwyfbridge/VoxelWorld.h"

#include <algorithm>
#include <cstdio>

namespace gwyf::voxel {

namespace {

#if defined(_MSC_VER)
#define GWYF_BARRIER() _ReadWriteBarrier()
#else
#define GWYF_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

u64 LoadAcquire(u64* slot) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 value = *reinterpret_cast<volatile u64*>(slot);
    GWYF_BARRIER();
    return value;
}

void StoreRelease(u64* slot, u64 value) {
    GWYF_BARRIER();
    *reinterpret_cast<volatile u64*>(slot) = value;
    std::atomic_thread_fence(std::memory_order_release);
}

/// Ключ чанка 16×16 (для диагностики плотности).
u64 ChunkKey(i32 x, i32 z) {
    const u64 chunkX = static_cast<u64>(static_cast<u32>(x / 16) & 0xFFFFFFFFull);
    const u64 chunkZ = static_cast<u64>(static_cast<u32>(z / 16) & 0xFFFFFFFFull);
    return chunkX | (chunkZ << 32);
}

}  // namespace

VoxelMirror::VoxelMirror() = default;
VoxelMirror::~VoxelMirror() = default;

void VoxelMirror::Configure(const VoxelSpawnSpec& spec) {
    spec_ = spec;
    GWYF_INFO("Зеркало вокселей настроено: масштаб %.2f, бюджет %u объектов/кадр, предел %u объектов, режим %d",
              spec.scale, spec.budgetPerFrame, spec.poolLimit, static_cast<int>(spec.mode));
}

bool VoxelMirror::PushIncoming(const PayloadVoxel& edit) {
    const u64 head = queue_.head;
    const u64 tail = LoadAcquire(&queue_.tail);

    if (head - tail >= InboundQueue::Capacity) {
        queue_.dropped++;
        return false;
    }

    queue_.slots[head % InboundQueue::Capacity] = edit;
    StoreRelease(&queue_.head, head + 1);

    if (edit.action == static_cast<u32>(ipc::VoxelAction::Break)) {
        removed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        placed_.fetch_add(1, std::memory_order_relaxed);
    }
    received_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VoxelMirror::PeekIncoming(PayloadVoxel& out) const {
    auto* self = const_cast<VoxelMirror*>(this);
    const u64 tail = self->queue_.tail;
    const u64 head = LoadAcquire(&self->queue_.head);
    if (tail == head) return false;

    out = self->queue_.slots[tail % InboundQueue::Capacity];
    return true;
}

void ToGameSpace(const VoxelSpawnSpec& spec, const Vec3i& minecraftPosition, float out[3]) {
    // Масштаб: один блок Minecraft = scale условных единиц игры.
    // Смещение origin задаёт, где именно в мире игры появится постройка.
    out[0] = spec.origin[0] + static_cast<float>(minecraftPosition.x) * spec.scale;
    out[1] = spec.origin[1] + (spec.flattenY ? 0.0f : static_cast<float>(minecraftPosition.y) * spec.scale);
    out[2] = spec.origin[2] + static_cast<float>(minecraftPosition.z) * spec.scale;
}

void VoxelMirror::ToGameSpace(const Vec3i& minecraftPosition, float out[3]) const {
    gwyf::voxel::ToGameSpace(spec_, minecraftPosition, out);
}

u32 VoxelMirror::ProcessOnMainThread(ISpawnAdapter& adapter, u32 budget, std::string* error) {
    if (!adapter.Ready()) {
        if (error != nullptr) *error = std::string("адаптер спавна не готов: ") + adapter.Name();
        return 0;
    }

    u32 processed = 0;
    u32 spawnedThisFrame = 0;

    // Забираем всё, что накопилось в очереди: удаления обрабатываем всегда
    // (они дешёвые), создания — по бюджету.
    while (processed < 4096) {
        const u64 tail = queue_.tail;
        const u64 head = LoadAcquire(&queue_.head);
        if (tail == head) break;

        PayloadVoxel edit = queue_.slots[tail % InboundQueue::Capacity];
        StoreRelease(&queue_.tail, tail + 1);

        const u64 key = VoxelKey::Pack(edit.x, edit.y, edit.z);
        chunkDensity_[ChunkKey(edit.x, edit.z)]++;

        if (edit.action == static_cast<u32>(ipc::VoxelAction::Break)) {
            auto it = instances_.find(key);
            if (it != instances_.end()) {
                if (it->second.spawned && it->second.handle != nullptr && spec_.despawnOnBreak) {
                    adapter.Despawn(it->second.handle);
                    despawned_.fetch_add(1, std::memory_order_relaxed);
                }
                instances_.erase(it);
            }
            ++processed;
            continue;
        }

        // Создание объекта: проверяем бюджет кадра.
        if (spawnedThisFrame >= budget) {
            // Бюджет исчерпан — правку не теряем: возвращаем в очередь через
            // «нулевую» запись невозможно, поэтому просто создаём отложенную
            // запись в зеркале и обработаем её в следующем кадре.
            VoxelInstance& pending = instances_[key];
            pending.block = edit.block;
            pending.pendingSpawn = true;
            pending.revision = ++revisionCounter_;
            ++processed;
            break;
        }

        auto it = instances_.find(key);
        if (it != instances_.end() && it->second.spawned && it->second.block == edit.block) {
            // Такой куб уже есть — ничего не делаем (идемпотентность важна:
            // Minecraft может прислать повторную установку того же блока).
            ++processed;
            continue;
        }

        if (it != instances_.end() && it->second.spawned && it->second.handle != nullptr) {
            adapter.Despawn(it->second.handle);
            despawned_.fetch_add(1, std::memory_order_relaxed);
            it->second.handle = nullptr;
            it->second.spawned = false;
        }

        if (instances_.size() >= spec_.poolLimit) {
            failed_.fetch_add(1, std::memory_order_relaxed);
            GWYF_WARN("Достигнут предел объектов (%u) — новые кубы не создаются. Увеличьте pool_limit или очистите мир (flush).",
                      spec_.poolLimit);
            ++processed;
            break;
        }

        VoxelInstance instance{};
        instance.block = edit.block;
        instance.pendingSpawn = true;
        instance.revision = ++revisionCounter_;

        void* handle = nullptr;
        std::string spawnError;
        if (adapter.Spawn(Vec3i{edit.x, edit.y, edit.z}, edit.block, &handle, &spawnError)) {
            instance.handle = handle;
            instance.spawned = true;
            instance.pendingSpawn = false;
            spawned_.fetch_add(1, std::memory_order_relaxed);
            ++spawnedThisFrame;
        } else {
            instance.spawned = false;
            instance.pendingSpawn = false;
            failed_.fetch_add(1, std::memory_order_relaxed);
            if (error != nullptr && error->empty()) {
                *error = spawnError;
            }

            static std::atomic<u32> warnCount{0};
            if (warnCount.fetch_add(1) < 5) {
                GWYF_WARN("Не удалось создать куб %d,%d,%d: %s", edit.x, edit.y, edit.z, spawnError.c_str());
            }
        }

        instances_[key] = instance;
        ++processed;
    }

    // Доспавниваем то, что не влезло в бюджет в прошлые кадры.
    if (spawnedThisFrame < budget) {
        for (auto& pair : instances_) {
            if (spawnedThisFrame >= budget) break;
            VoxelInstance& instance = pair.second;
            if (!instance.pendingSpawn) continue;

            void* handle = nullptr;
            std::string spawnError;
            const Vec3i position = VoxelKey::Unpack(pair.first);

            if (adapter.Spawn(position, instance.block, &handle, &spawnError)) {
                instance.handle = handle;
                instance.spawned = true;
                instance.pendingSpawn = false;
                spawned_.fetch_add(1, std::memory_order_relaxed);
                ++spawnedThisFrame;
            } else {
                instance.pendingSpawn = false;
                failed_.fetch_add(1, std::memory_order_relaxed);
                if (error != nullptr && error->empty()) *error = spawnError;
            }
        }
    }

    return processed;
}

u32 VoxelMirror::DespawnAll(ISpawnAdapter& adapter) {
    u32 count = 0;
    for (auto& pair : instances_) {
        if (pair.second.spawned && pair.second.handle != nullptr) {
            adapter.Despawn(pair.second.handle);
            ++count;
            despawned_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    instances_.clear();
    chunkDensity_.clear();
    GWYF_INFO("Мир внутри игры очищен: удалено объектов %u", count);
    return count;
}

void VoxelMirror::Reset() {
    instances_.clear();
    chunkDensity_.clear();
    queue_.head = 0;
    queue_.tail = 0;
    queue_.dropped = 0;
}

VoxelStats VoxelMirror::Stats() const {
    VoxelStats stats{};
    stats.received = received_.load(std::memory_order_relaxed);
    stats.placed = placed_.load(std::memory_order_relaxed);
    stats.removed = removed_.load(std::memory_order_relaxed);
    stats.spawned = spawned_.load(std::memory_order_relaxed);
    stats.despawned = despawned_.load(std::memory_order_relaxed);
    stats.failed = failed_.load(std::memory_order_relaxed);
    stats.dropped = queue_.dropped;
    stats.liveObjects = static_cast<u32>(instances_.size());

    // В «ожидающих» обязаны попасть обе категории: правки, что ещё лежат в
    // очереди, и объекты, отложенные из-за бюджета кадра. Иначе внешний код
    // (и самотест) решил бы, что работа закончена, когда она ещё идёт.
    u32 deferred = 0;
    for (const auto& pair : instances_) {
        if (pair.second.pendingSpawn) ++deferred;
    }
    stats.pending = static_cast<u32>(queue_.head - queue_.tail) + deferred;
    return stats;
}

std::string VoxelMirror::Describe() const {
    const VoxelStats stats = Stats();

    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Воксели: получено %llu (поставлено %llu / сломано %llu), создано объектов %llu, удалено %llu, "
                  "ошибок %llu, потеряно %llu, живых объектов %u, в очереди %u",
                  static_cast<unsigned long long>(stats.received),
                  static_cast<unsigned long long>(stats.placed),
                  static_cast<unsigned long long>(stats.removed),
                  static_cast<unsigned long long>(stats.spawned),
                  static_cast<unsigned long long>(stats.despawned),
                  static_cast<unsigned long long>(stats.failed),
                  static_cast<unsigned long long>(stats.dropped),
                  stats.liveObjects, stats.pending);

    std::string text = buffer;

    if (!chunkDensity_.empty()) {
        std::vector<std::pair<u64, u32>> sorted(chunkDensity_.begin(), chunkDensity_.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
            return left.second > right.second;
        });

        text += "\n  самые активные чанки (x,z): ";
        for (usize index = 0; index < sorted.size() && index < 5; ++index) {
            const u32 chunkX = static_cast<u32>(sorted[index].first & 0xFFFFFFFFull);
            const u32 chunkZ = static_cast<u32>(sorted[index].first >> 32);
            char chunk[64]{};
            std::snprintf(chunk, sizeof(chunk), "(%d,%d)=%u ", static_cast<i32>(chunkX) * 16, static_cast<i32>(chunkZ) * 16, sorted[index].second);
            text += chunk;
        }
    }

    return text;
}

}  // namespace gwyf::voxel
