// ============================================================================
//  VoxelTo3DWorld.cpp — создание настоящих объектов внутри запущенной игры.
//
//  Это обратная половина моста: правка блока в Minecraft (поступившая по
//  разделяемой памяти) превращается в объект игрового мира, который видят все
//  участники лобби. Способы создания собраны в три адаптера, чтобы инструмент
//  работал и без реверса игры, и мог использовать «родные» фабрики, когда они
//  известны:
//
//    * UnityPrimitive — GameObject.CreatePrimitive(PrimitiveType.Cube) плюс
//      transform.set_position/set_localScale. Работает на любой Mono-сборке
//      Unity, ничего не требует от анализа игры. Режим по умолчанию.
//
//    * MonoFactory — вызов метода самой игры, который создаёт объект
//      (например, World.SpawnBlockObject). Даёт «родные» объекты с материалами
//      и логикой игры, но требует знания имени метода и его сигнатуры.
//
//    * NativeExport — вызов нативного экспорта (модуль + имя функции). Нужен,
//      когда объект создаётся в нативном слое; именно этот путь прогоняется
//      самотестом на синтетическом хосте.
//
//  ОБЯЗАТЕЛЬНЫЕ ПРАВИЛА БЕЗОПАСНОСТИ
//  ---------------------------------
//    1. Все вызовы Unity API — только из главного потока игры (см. хук
//       WH_GETMESSAGE в GWYF_HookEngine.cpp).
//    2. Размер структур проверяется до вызова: Vector3 = 12 байт, Color = 16.
//       Если runtime сообщает другой размер — адаптер отключается, а не
//       «пробует и падает».
//    3. Бюджет объектов на кадр ограничен (VoxelMirror), поэтому постройка
//       большого размера создаётся постепенно и не подвешивает игру.
// ============================================================================
#include "gwyfbridge/Common.h"
#include "gwyfbridge/MonoRuntime.h"
#include "gwyfbridge/PatternScan.h"
#include "gwyfbridge/VoxelWorld.h"

#include <cmath>
#include <string>
#include <vector>

namespace gwyf::voxel {

namespace {

// ── Проверка ABI структур ───────────────────────────────────────────────────

/// Vector3 в Unity: три float подряд.
struct UnityVector3 {
    float x;
    float y;
    float z;
};
static_assert(sizeof(UnityVector3) == 12, "Vector3 должен занимать 12 байт");

/// Color в Unity: четыре float (RGBA). Только для примера совместимости.
struct UnityColor {
    float r;
    float g;
    float b;
    float a;
};
static_assert(sizeof(UnityColor) == 16, "Color должен занимать 16 байт");

/// Значения UnityEngine.PrimitiveType.
enum class UnityPrimitive : i32 {
    Sphere = 0,
    Capsule = 1,
    Cylinder = 2,
    Cube = 3,
    Plane = 4,
    Quad = 5,
};

// ── Общая база: проверка размера структуры и логирование ────────────────────

class AdapterBase : public ISpawnAdapter {
public:
    bool Ready() const override { return ready_; }

protected:
    /// Проверить, что структура в runtime имеет ожидаемый размер.
    bool CheckStructSize(const char* className, void* klass, usize expected) {
        const i32 actual = mono::ClassInstanceSize(klass);
        if (actual < 0) {
            // Runtime не отдал размер — это не повод отказываться, но сказать надо.
            GWYF_DEBUG("%s: размер структуры неизвестен (нет mono_class_instance_size), полагаюсь на ABI", className);
            return true;
        }

        if (static_cast<usize>(actual) != expected) {
            GWYF_ERROR("%s занимает %d байт вместо ожидаемых %zu — вызовы с этой структурой запрещены",
                       className, actual, expected);
            return false;
        }
        return true;
    }

    bool ready_ = false;
    profile::VoxelSpawnSpec spec_{};
};

// ── 1. Unity-примитив ───────────────────────────────────────────────────────

class UnityPrimitiveAdapter final : public AdapterBase {
public:
    const char* Name() const override { return "unity_primitive (GameObject.CreatePrimitive)"; }

    bool Initialize(const profile::VoxelSpawnSpec& spec, const std::string& gameAssembly, std::string* error) override {

        spec_ = spec;
        (void)gameAssembly;

        if (!mono::RuntimeReady()) {
            if (error != nullptr) *error = "runtime Mono ещё не готов";
            return false;
        }

        gameObjectClass_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "GameObject");
        if (gameObjectClass_ == nullptr) {
            // Некоторые сборки держат модули иначе — ищем по основному образу.
            gameObjectClass_ = mono::FindClass("Assembly-CSharp", "UnityEngine", "GameObject");
        }

        transformClass_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Transform");
        vector3Class_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Vector3");
        objectClass_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Object");
        materialClass_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Material");
        rendererClass_ = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Renderer");

        if (gameObjectClass_ == nullptr || transformClass_ == nullptr) {
            if (error != nullptr) {
                *error = "не найдены классы UnityEngine.GameObject/Transform — сборка может быть на IL2CPP "
                         "или Unity ещё не инициализирован";
            }
            return false;
        }

        if (vector3Class_ != nullptr && !CheckStructSize("UnityEngine.Vector3", vector3Class_, sizeof(UnityVector3))) {
            if (error != nullptr) *error = "несовпадение размера Vector3 — создание объектов отключено";
            return false;
        }

        createPrimitive_ = mono::FindMethod(gameObjectClass_, "CreatePrimitive", 1);
        getTransform_ = mono::FindMethod(gameObjectClass_, "get_transform", 0);
        setPosition_ = mono::FindMethodInHierarchy(transformClass_, "set_position", 1);
        setLocalScale_ = mono::FindMethodInHierarchy(transformClass_, "set_localScale", 1);
        getRenderer_ = mono::FindMethod(gameObjectClass_, "GetComponent", 1);
        getMaterial_ = mono::FindMethodInHierarchy(rendererClass_, "get_material", 0);
        setColor_ = mono::FindMethodInHierarchy(materialClass_, "set_color", 1);
        destroy_ = mono::FindMethodInHierarchy(objectClass_, "Destroy", 1);

        if (createPrimitive_ == nullptr || getTransform_ == nullptr || setPosition_ == nullptr) {
            if (error != nullptr) {
                *error = "не найдены методы GameObject.CreatePrimitive/get_transform/set_position";
            }
            return false;
        }

        ready_ = true;
        GWYF_INFO("Адаптер объектов готов: %s; цвет по типу блока: %s", Name(),
                  setColor_ != nullptr ? "да" : "нет (материал по умолчанию)");
        return true;
    }

    bool Spawn(const Vec3i& position, u32 block, void** outHandle, std::string* error) override {
        if (!ready_) {
            if (error != nullptr) *error = "адаптер не инициализирован";
            return false;
        }

        // 1) Примитив-куб.
        i32 primitive = static_cast<i32>(UnityPrimitive::Cube);
        void* arguments[1] = {&primitive};
        std::string exception;
        void* gameObject = mono::Invoke(createPrimitive_, nullptr, arguments, &exception);
        if (gameObject == nullptr) {
            if (error != nullptr) *error = "CreatePrimitive вернул null: " + exception;
            return false;
        }

        // 2) Позиция и масштаб.
        const float gamePosition[3] = {
            spec_.origin[0] + static_cast<float>(position.x) * spec_.scale,
            spec_.origin[1] + (spec_.flattenY ? 0.0f : static_cast<float>(position.y) * spec_.scale),
            spec_.origin[2] + static_cast<float>(position.z) * spec_.scale,
        };

        void* transform = mono::Invoke(getTransform_, gameObject, nullptr);
        if (transform == nullptr) {
            if (error != nullptr) *error = "get_transform вернул null";
            return false;
        }

        UnityVector3 translation{gamePosition[0], gamePosition[1], gamePosition[2]};
        void* positionArguments[1] = {&translation};
        mono::Invoke(setPosition_, transform, positionArguments, nullptr);

        if (setLocalScale_ != nullptr) {
            const float size = spec_.scale;
            UnityVector3 scale{size, size, size};
            void* scaleArguments[1] = {&scale};
            mono::Invoke(setLocalScale_, transform, scaleArguments, nullptr);
        }

        // 3) Цвет по типу блока (необязательная часть: если материала нет,
        //    куб останется стандартного цвета — это не ошибка).
        ApplyColor(gameObject, block);

        if (outHandle != nullptr) *outHandle = gameObject;
        return true;
    }

    void Despawn(void* handle) override {
        if (handle == nullptr || destroy_ == nullptr) return;
        void* arguments[1] = {handle};
        mono::Invoke(destroy_, nullptr, arguments, nullptr);
    }

private:
    void ApplyColor(void* gameObject, u32 block) {
        if (setColor_ == nullptr || getRenderer_ == nullptr || getMaterial_ == nullptr || rendererClass_ == nullptr) {
            return;
        }

        void* arguments[1] = {rendererClass_};
        void* renderer = mono::Invoke(getRenderer_, gameObject, arguments, nullptr);
        if (renderer == nullptr) return;

        void* material = mono::Invoke(getMaterial_, renderer, nullptr);
        if (material == nullptr) return;

        const UnityColor color = BlockColor(block);
        void* colorArguments[1] = {const_cast<UnityColor*>(&color)};
        mono::Invoke(setColor_, material, colorArguments, nullptr);
    }

    /// Палитра блоков. Совпадает с [blocks] в профиле и с BlockGranter.java,
    /// чтобы постройка «там» и «здесь» читалась одинаково.
    static UnityColor BlockColor(u32 block) {
        switch (block) {
            case 1: return {0.55f, 0.40f, 0.22f, 1.0f};  // земля
            case 2: return {0.30f, 0.70f, 0.25f, 1.0f};  // трава
            case 3: return {0.55f, 0.55f, 0.58f, 1.0f};  // камень
            case 4: return {0.45f, 0.30f, 0.15f, 1.0f};  // дерево
            case 5: return {0.95f, 0.80f, 0.20f, 1.0f};  // золото
            case 6: return {0.30f, 0.90f, 0.95f, 1.0f};  // алмаз
            case 7: return {0.85f, 0.15f, 0.15f, 1.0f};  // стол казино
            case 8: return {0.95f, 0.95f, 0.75f, 1.0f};  // светильник
            case 9: return {0.15f, 0.85f, 0.40f, 1.0f};  // изумруд
            case 10: return {0.25f, 0.25f, 0.28f, 1.0f}; // незерит
            default: return {0.75f, 0.75f, 0.75f, 1.0f};
        }
    }

    void* gameObjectClass_ = nullptr;
    void* transformClass_ = nullptr;
    void* vector3Class_ = nullptr;
    void* objectClass_ = nullptr;
    void* rendererClass_ = nullptr;
    void* materialClass_ = nullptr;

    void* createPrimitive_ = nullptr;
    void* getTransform_ = nullptr;
    void* setPosition_ = nullptr;
    void* setLocalScale_ = nullptr;
    void* getRenderer_ = nullptr;
    void* getMaterial_ = nullptr;
    void* setColor_ = nullptr;
    void* destroy_ = nullptr;
};

// ── 2. Фабрика самой игры ───────────────────────────────────────────────────

class MonoFactoryAdapter final : public AdapterBase {
public:
    const char* Name() const override { return "mono_factory (фабрика игры)"; }

    bool Initialize(const profile::VoxelSpawnSpec& spec, const std::string& gameAssembly, std::string* error) override {
        spec_ = spec;

        if (spec.factoryClass.empty() || spec.factoryMethod.empty()) {
            if (error != nullptr) {
                *error = "не заданы factory_class/factory_method — этот режим без них не работает";
            }
            return false;
        }

        // Имя класса может быть как «World.Builder», так и «Game.World.Builder» —
        // пространство имён отделяем сами, чтобы профиль был читаемым.
        std::string nameSpace;
        std::string className = spec.factoryClass;
        const usize separator = spec.factoryClass.find_last_of('.');
        if (separator != std::string::npos) {
            nameSpace = spec.factoryClass.substr(0, separator);
            className = spec.factoryClass.substr(separator + 1);
        }

        void* klass = mono::FindClass(gameAssembly.c_str(), nameSpace.empty() ? nullptr : nameSpace.c_str(),
                                      className.c_str());
        if (klass == nullptr) {
            if (error != nullptr) *error = "класс фабрики не найден: " + spec.factoryClass;
            return false;
        }

        factoryMethod_ = mono::FindMethodInHierarchy(klass, spec.factoryMethod.c_str(),
                                                    static_cast<int>(spec.factoryArgTypes.size()));
        if (factoryMethod_ == nullptr) {
            if (error != nullptr) {
                *error = "метод фабрики не найден: " + spec.factoryClass + "." + spec.factoryMethod + "/" +
                         std::to_string(spec.factoryArgTypes.size());
            }
            return false;
        }

        // Проверяем, что раскладка из профиля совпадает с реальной сигнатурой:
        // иначе вызов с неверными аргументами повредит стек.
        const std::vector<mono::ArgKind> kinds = mono::ClassifyArgs(factoryMethod_);
        if (!kinds.empty() && kinds.size() != spec.factoryArgTypes.size()) {
            if (error != nullptr) {
                *error = "у метода фабрики " + std::to_string(kinds.size()) + " аргументов, а в профиле " +
                         std::to_string(spec.factoryArgTypes.size());
            }
            return false;
        }

        ready_ = true;
        GWYF_INFO("Адаптер объектов готов: %s → %s.%s", Name(), spec.factoryClass.c_str(), spec.factoryMethod.c_str());
        return true;
    }

    bool Spawn(const Vec3i& position, u32 block, void** outHandle, std::string* error) override {
        if (!ready_) {
            if (error != nullptr) *error = "адаптер не инициализирован";
            return false;
        }

        const float gamePosition[3] = {
            spec_.origin[0] + static_cast<float>(position.x) * spec_.scale,
            spec_.origin[1] + (spec_.flattenY ? 0.0f : static_cast<float>(position.y) * spec_.scale),
            spec_.origin[2] + static_cast<float>(position.z) * spec_.scale,
        };

        // Аргументы строим строго по описанию из профиля: i32/x/y/z/block,
        // f32_x/f32_y/f32_z, fx/fy/fz или vector3.
        std::vector<i32> integers(spec_.factoryArgTypes.size(), 0);
        std::vector<float> floats(spec_.factoryArgTypes.size(), 0.0f);
        std::vector<UnityVector3> vectors(spec_.factoryArgTypes.size());
        std::vector<void*> arguments(spec_.factoryArgTypes.size(), nullptr);

        for (usize index = 0; index < spec_.factoryArgTypes.size(); ++index) {
            const std::string& kind = spec_.factoryArgTypes[index];

            if (kind == "x") { integers[index] = static_cast<i32>(gamePosition[0]); arguments[index] = &integers[index]; }
            else if (kind == "y") { integers[index] = static_cast<i32>(std::round(gamePosition[1])); arguments[index] = &integers[index]; }
            else if (kind == "z") { integers[index] = static_cast<i32>(gamePosition[2]); arguments[index] = &integers[index]; }
            else if (kind == "block") { integers[index] = static_cast<i32>(block); arguments[index] = &integers[index]; }
            else if (kind == "i32") { integers[index] = static_cast<i32>(block); arguments[index] = &integers[index]; }
            else if (kind == "fx") { floats[index] = gamePosition[0]; arguments[index] = &floats[index]; }
            else if (kind == "fy") { floats[index] = gamePosition[1]; arguments[index] = &floats[index]; }
            else if (kind == "fz") { floats[index] = gamePosition[2]; arguments[index] = &floats[index]; }
            else if (kind == "vector3") {
                vectors[index] = UnityVector3{gamePosition[0], gamePosition[1], gamePosition[2]};
                arguments[index] = &vectors[index];
            } else if (kind == "scale") {
                floats[index] = spec_.scale;
                arguments[index] = &floats[index];
            } else {
                if (error != nullptr) *error = "неизвестный тип аргумента фабрики: " + kind;
                return false;
            }
        }

        std::string exception;
        void* handle = mono::Invoke(factoryMethod_, nullptr, arguments.data(), &exception);
        if (handle == nullptr && !exception.empty()) {
            if (error != nullptr) *error = "фабрика игры выбросила исключение: " + exception;
            return false;
        }

        if (outHandle != nullptr) *outHandle = handle;
        return true;
    }

    void Despawn(void* handle) override {
        if (handle == nullptr) return;

        // Удаляем тем же Unity-методом: фабрика возвращает GameObject.
        void* objectClass = mono::FindClass("UnityEngine.CoreModule", "UnityEngine", "Object");
        if (objectClass == nullptr) return;

        void* destroy = mono::FindMethodInHierarchy(objectClass, "Destroy", 1);
        if (destroy == nullptr) return;

        void* arguments[1] = {handle};
        mono::Invoke(destroy, nullptr, arguments, nullptr);
    }

private:
    void* factoryMethod_ = nullptr;
};

// ── 3. Нативный экспорт ─────────────────────────────────────────────────────

class NativeExportAdapter final : public AdapterBase {
public:
    using SpawnFn = bool (*)(i32 x, i32 y, i32 z, u32 block, void** outHandle);
    using DestroyFn = void (*)(void* handle);

    NativeExportAdapter(SpawnFn spawn, DestroyFn destroy, const std::string& source)
        : spawn_(spawn), destroy_(destroy) {
        ready_ = spawn_ != nullptr;
        GWYF_INFO("Адаптер объектов готов: native_export (%s), уничтожение: %s", source.c_str(),
                  destroy_ != nullptr ? "есть" : "нет");
    }

    const char* Name() const override { return "native_export"; }

    bool Initialize(const profile::VoxelSpawnSpec& spec, const std::string& gameAssembly,
                    std::string* error) override {
        (void)gameAssembly;
        (void)error;
        spec_ = spec;
        return ready_;
    }

    bool Spawn(const Vec3i& position, u32 block, void** outHandle, std::string* error) override {
        if (!ready_ || spawn_ == nullptr) {
            if (error != nullptr) *error = "нативная фабрика недоступна";
            return false;
        }

        // Нативный слой принимает уже готовые координаты игры (он наш, значит
        // договорённость о единицах нам известна): масштаб применяем здесь.
        const i32 x = static_cast<i32>(std::lround(spec_.origin[0] + position.x * spec_.scale));
        const i32 y = static_cast<i32>(std::lround(spec_.origin[1] + (spec_.flattenY ? 0.0f : position.y * spec_.scale)));
        const i32 z = static_cast<i32>(std::lround(spec_.origin[2] + position.z * spec_.scale));

        if (!spawn_(x, y, z, block, outHandle)) {
            if (error != nullptr) *error = "нативная функция создания объекта вернула false";
            return false;
        }
        return true;
    }

    void Despawn(void* handle) override {
        if (destroy_ != nullptr && handle != nullptr) destroy_(handle);
    }

private:
    SpawnFn spawn_ = nullptr;
    DestroyFn destroy_ = nullptr;
};

// ── Фабрика адаптеров ───────────────────────────────────────────────────────

/// Имена экспортов, которые принимаются как «нативная фабрика объектов».
struct NativeCandidate {
    const char* module;
    const char* spawn;
    const char* destroy;
};

constexpr NativeCandidate kNativeCandidates[] = {
    // Свои имена: используются, если объект создаётся нативным слоем игры.
    {"UnityPlayer.dll", "GWYF_SpawnBlockObject", "GWYF_DestroyBlockObject"},
    {"GameAssembly.dll", "GWYF_SpawnBlockObject", "GWYF_DestroyBlockObject"},
    // Синтетический хост самотеста: проверяет этот путь целиком.
    {"fake_mono_host.dll", "FakeUnity_SpawnCube", "FakeUnity_DestroyObject"},
};

}  // namespace

std::unique_ptr<ISpawnAdapter> CreateAdapter(const profile::VoxelSpawnSpec& spec, const std::string& gameAssembly,
                                             std::string* error) {
    switch (spec.mode) {
        case profile::VoxelSpawnSpec::Mode::UnityPrimitive: {
            auto adapter = std::make_unique<UnityPrimitiveAdapter>();
            if (!adapter->Initialize(spec, gameAssembly, error)) return nullptr;
            return adapter;
        }

        case profile::VoxelSpawnSpec::Mode::MonoFactory: {
            auto adapter = std::make_unique<MonoFactoryAdapter>();
            if (!adapter->Initialize(spec, gameAssembly, error)) return nullptr;
            return adapter;
        }

        case profile::VoxelSpawnSpec::Mode::NativeExport: {
            std::string moduleName = spec.moduleName;
            std::string symbol = spec.symbol;
            std::string destroySymbol = spec.nativeArgTypes.size() > 1 ? spec.nativeArgTypes[1] : "";

            if (moduleName.empty() || symbol.empty()) {
                // Автопоиск: перебираем известные модули и имена экспортов.
                for (const NativeCandidate& candidate : kNativeCandidates) {
                    ModuleInfo module{};
                    if (!FindModule(candidate.module, module)) continue;

                    void* spawn = GetExport(module.handle, candidate.spawn);
                    if (spawn == nullptr) continue;

                    moduleName = candidate.module;
                    symbol = candidate.spawn;
                    destroySymbol = candidate.destroy;
                    break;
                }
            }

            if (moduleName.empty() || symbol.empty()) {
                if (error != nullptr) {
                    *error = "не найден модуль с экспортом создания объектов "
                             "(ожидались GWYF_SpawnBlockObject или FakeUnity_SpawnCube)";
                }
                return nullptr;
            }

            ModuleInfo module{};
            if (!FindModule(moduleName.c_str(), module)) {
                if (error != nullptr) *error = "модуль не загружен: " + moduleName;
                return nullptr;
            }

            auto spawn = reinterpret_cast<NativeExportAdapter::SpawnFn>(GetExport(module.handle, symbol.c_str()));
            if (spawn == nullptr) {
                if (error != nullptr) *error = "экспорт не найден: " + moduleName + "!" + symbol;
                return nullptr;
            }

            auto destroy = destroySymbol.empty()
                               ? nullptr
                               : reinterpret_cast<NativeExportAdapter::DestroyFn>(
                                     GetExport(module.handle, destroySymbol.c_str()));

            auto adapter = std::make_unique<NativeExportAdapter>(spawn, destroy, moduleName + "!" + symbol);
            if (!adapter->Initialize(spec, gameAssembly, error)) return nullptr;
            return adapter;
        }

        case profile::VoxelSpawnSpec::Mode::Disabled:
        default:
            if (error != nullptr) *error = "создание объектов отключено в профиле ([voxel] mode=disabled)";
            return nullptr;
    }
}

}  // namespace gwyf::voxel
