// ============================================================================
//  VoxelWorld.h — зеркало воксельного мира Minecraft внутри игры.
//
//  Задача: превратить поток правок «друг поставил блок в креативе» в поток
//  вызовов «создать объект-куб в игре». Здесь живёт вся механика:
//    * очередь входящих правок (SPSC-кольцо без блокировок): продюсер —
//      поток IPC, потребитель — главный поток игры;
//    * зеркало «координата → объект игры», чтобы обновлять и удалять кубы;
//    * бюджет на кадр: Unity не переживёт создание тысяч GameObject за кадр,
//      поэтому за один кадр создаём не больше N объектов;
//    * статистика и диагностика (сколько создано, что не получилось).
//
//  ПОТОКИ: PushIncoming — из любого потока (обычно IPC-воркер),
//  ProcessOnMainThread — ТОЛЬКО из главного потока игры (Unity API).
// ============================================================================
#pragma once

#include "Common.h"
#include "IpcProtocol.h"
#include "TargetProfile.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gwyf::voxel {

using gwyf::ipc::PayloadVoxel;
using gwyf::ipc::Vec3i;
using gwyf::profile::VoxelSpawnSpec;

/// Ключ координаты: три 21-битных компонента упакованы в u64.
/// Этого хватает на мир ±1 048 575 по каждой оси.
struct VoxelKey {
    static u64 Pack(i32 x, i32 y, i32 z) {
        const u64 ux = static_cast<u64>(static_cast<u32>(x) & 0x1FFFFFu);
        const u64 uy = static_cast<u64>(static_cast<u32>(y) & 0x1FFFFFu);
        const u64 uz = static_cast<u64>(static_cast<u32>(z) & 0x1FFFFFu);
        return ux | (uy << 21) | (uz << 42);
    }

    static Vec3i Unpack(u64 key) {
        Vec3i position;
        auto signExtend = [](u32 value) -> i32 {
            return (value & 0x100000u) ? static_cast<i32>(value | 0xFFE00000u) : static_cast<i32>(value);
        };
        position.x = signExtend(static_cast<u32>(key & 0x1FFFFFu));
        position.y = signExtend(static_cast<u32>((key >> 21) & 0x1FFFFFu));
        position.z = signExtend(static_cast<u32>((key >> 42) & 0x1FFFFFu));
        return position;
    }
};

/// Состояние одного вокселя в игре.
struct VoxelInstance {
    u32 block = 0;      ///< тип блока (по профилю [blocks])
    void* handle = nullptr;  ///< указатель на объект игры (GameObject/Transform)
    bool spawned = false;
    bool pendingSpawn = false;
    bool pendingDespawn = false;
    u64 revision = 0;    ///< растёт при каждой правке — защищает от гонок
};

/// Адаптер «как создавать объекты в конкретной игре».
/// Реализации — в VoxelTo3DWorld.cpp (Unity-примитив, фабрика игры, нативный экспорт).
class ISpawnAdapter {
public:
    virtual ~ISpawnAdapter() = default;

    /// Человекочитаемое имя режима (для логов).
    virtual const char* Name() const = 0;

    /// Подготовка: поиск классов/методов/экспортов. Вызывается на спокойном
    /// этапе, когда можно позволить себе медленные операции (перебор классов).
    virtual bool Initialize(const VoxelSpawnSpec& spec, const std::string& gameAssembly, std::string* error) = 0;

    /// Создать объект-куб. ВЫЗЫВАЕТСЯ ТОЛЬКО НА ГЛАВНОМ ПОТОКЕ.
    virtual bool Spawn(const Vec3i& position, u32 block, void** outHandle, std::string* error) = 0;

    /// Удалить ранее созданный объект. Тоже главный поток.
    virtual void Despawn(void* handle) = 0;

    /// Готов ли адаптер к работе.
    virtual bool Ready() const = 0;
};

/// Преобразование координат Minecraft → координаты игры (origin/scale/flattenY).
/// Одна функция на все адаптеры: иначе разные пути спавна разъезжались бы.
void ToGameSpace(const VoxelSpawnSpec& spec, const Vec3i& minecraftPosition, float out[3]);

/// Создать адаптер спавна по настройкам профиля. Владелец — вызывающий код.
/// Реализации в VoxelTo3DWorld.cpp:
///   * UnityPrimitive — GameObject.CreatePrimitive(Cube) (работает без реверса);
///   * MonoFactory    — вызов метода самой игры, создающего объект;
///   * NativeExport   — вызов нативной функции по имени экспорта (тесты).
std::unique_ptr<ISpawnAdapter> CreateAdapter(const VoxelSpawnSpec& spec, const std::string& gameAssembly, std::string* error);

/// Статистика моста вокселей (для команды status).
struct VoxelStats {
    u64 received = 0;      ///< получено правок из IPC
    u64 placed = 0;        ///< поставлено блоков
    u64 removed = 0;       ///< сломано блоков
    u64 spawned = 0;       ///< успешно создано объектов в игре
    u64 despawned = 0;
    u64 failed = 0;        ///< не удалось создать объект
    u64 dropped = 0;       ///< потеряно из-за переполнения очереди
    u32 liveObjects = 0;   ///< объектов сейчас в игре
    u32 pending = 0;       ///< ждут обработки
};

/// Зеркало мира: очередь правок + таблица объектов.
class VoxelMirror {
public:
    VoxelMirror();
    ~VoxelMirror();

    /// Применить настройки профиля.
    void Configure(const VoxelSpawnSpec& spec);

    /// Поставить правку в очередь. Поток: воркер IPC.
    bool PushIncoming(const PayloadVoxel& edit);

    /// Обработать очередь на главном потоке. Возвращает число обработанных
    /// правок. budget — максимум созданных объектов за вызов.
    u32 ProcessOnMainThread(ISpawnAdapter& adapter, u32 budget, std::string* error);

    /// Удалить все объекты, созданные мостом (команда flush).
    u32 DespawnAll(ISpawnAdapter& adapter);

    /// Полная очистка состояния (при выходе).
    void Reset();

    VoxelStats Stats() const;

    /// Преобразование координат Minecraft → координаты игры.
    void ToGameSpace(const Vec3i& minecraftPosition, float out[3]) const;

    /// Человекочитаемый отчёт (топ чанков по плотности изменений).
    std::string Describe() const;

    /// Заглянуть в очередь, не забирая (для диагностики).
    bool PeekIncoming(PayloadVoxel& out) const;

private:
    /// Очередь входящих правок: SPSC-кольцо фиксированной ёмкости.
    struct InboundQueue {
        static constexpr u32 Capacity = 8192;
        PayloadVoxel slots[Capacity]{};
        u64 head = 0;   ///< пишет продюсер
        u64 tail = 0;   ///< пишет потребитель
        u64 dropped = 0;
    };

    InboundQueue queue_{};
    std::unordered_map<u64, VoxelInstance> instances_;
    VoxelSpawnSpec spec_{};

    // Статистика (пишется/читается разными потоками — атомики обязательны).
    std::atomic<u64> received_{0};
    std::atomic<u64> placed_{0};
    std::atomic<u64> removed_{0};
    std::atomic<u64> spawned_{0};
    std::atomic<u64> despawned_{0};
    std::atomic<u64> failed_{0};

    /// Плотность изменений по чанкам 16×16 (для диагностики и отчётов).
    std::unordered_map<u64, u32> chunkDensity_;

    /// Счётчик ревизий: каждая правка получает новый номер.
    u64 revisionCounter_ = 0;
};

}  // namespace gwyf::voxel
