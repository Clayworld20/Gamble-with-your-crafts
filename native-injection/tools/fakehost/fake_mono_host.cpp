// ============================================================================
//  fake_mono_host.cpp — синтетический хост «игры»: поддельный Mono + поддельная
//  фабрика объектов Unity.
//
//  ЗАЧЕМ ЭТО НУЖНО
//  ---------------
//  Половину кода моста нельзя проверить на живом GWYF (его нет в сборочной
//  среде), но можно проверить на подставном runtime с тем же ABI: движок
//  ищет классы по именам через mono_* и перехватывает mono_runtime_invoke —
//  если подставить DLL с такими же экспортами, движок отработает ровно тот же
//  путь: разрешение целей → установка хука → публикация события в кольцо.
//
//  МОДЕЛЬ ДАННЫХ (умышленно крошечная, но не «заглушка-заглушка»)
//  -------------------------------------------------------------
//  * один образ:            Assembly-CSharp
//  * классы:                Game.BetManager, Game.TableManager, Game.GameManager
//  * метод места ставки:     Game.BetManager.PlaceBet(float amount, int chipType)
//  * метод создания стола:   Game.TableManager.SpawnTable(int tableId, int seats)
//  * статическое поле:       Game.GameManager.chips (int64)
//  * статическое поле:       Game.GameManager.instance (ссылка на экземпляр)
//  У методов и полей настоящие CIL-типы (R4/I4/I8/OBJECT), поэтому проверка
//  раскладки хуков в движке работает по-настоящему, а не «на веру».
//
//  ПРОВЕРЯЕМЫЙ СЦЕНАРИЙ (см. gwyfbridge_selftest.exe --integration)
//  ---------------------------------------------------------------
//    1. самотест ставит переменную окружения GWYF_MONO_MODULE на эту DLL;
//    2. загружает GWYF_HookEngine.dll — движок находит «Mono», «сборку»
//       и цели из профиля, ставит хук на mono_runtime_invoke;
//    3. самотест вызывает FakeMono_InvokeByName("Game.BetManager", ...,
//       "PlaceBet") — как это сделала бы игра;
//    4. движок публикует BetPlaced в общую память, самотест читает событие.
// ============================================================================
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// ── Коды типов CIL (те же, что в mono/metadata/blob.h) ──────────────────────
constexpr u32 kTypeI4 = 0x08;
constexpr u32 kTypeI8 = 0x0a;
constexpr u32 kTypeR4 = 0x0c;
constexpr u32 kTypeObject = 0x1c;

struct FakeType {
    u32 code = kTypeI4;
    const char* name = "int";
    u32 size = 4;
};

struct FakeMethod;
struct FakeClass;

struct FakeField {
    const char* name = nullptr;
    FakeType* type = nullptr;
    u32 offset = 0;       ///< смещение в экземпляре
    bool isStatic = false;
    i64 staticSlot = 0;   ///< хранилище статического значения
    FakeClass* klass = nullptr;
};

struct FakeMethod {
    const char* name = nullptr;
    FakeClass* klass = nullptr;
    FakeType* returnType = nullptr;
    FakeType* params[8]{};
    int paramCount = 0;
    int flags = 0;
    void* compiled = nullptr;   ///< «JIT-адрес» (для mode=method)
};

struct FakeClass {
    const char* nameSpace = nullptr;
    const char* name = nullptr;
    FakeClass* parent = nullptr;
    FakeField* fields = nullptr;
    int fieldCount = 0;
    FakeMethod* methods = nullptr;
    int methodCount = 0;
    FakeClass** interfaces = nullptr;
    int interfaceCount = 0;
    u32 typeToken = 0x02000001;
};

struct FakeImage {
    const char* name = nullptr;
    FakeClass* classes = nullptr;
    int classCount = 0;
};

struct FakeVTable {
    FakeClass* klass = nullptr;
};

struct FakeThread {
    u64 id = 0;
};

struct FakeDomain {
    int unused = 0;
};

// ── Таблицы типов ───────────────────────────────────────────────────────────

FakeType g_typeInt{kTypeI4, "int", 4};
FakeType g_typeLong{kTypeI8, "long", 8};
FakeType g_typeFloat{kTypeR4, "float", 4};
FakeType g_typeObject{kTypeObject, "object", 8};

// ── Методы и поля ───────────────────────────────────────────────────────────

FakeMethod g_betManagerMethods[2];
FakeField g_betManagerFields[1];
FakeClass g_betManager{};

FakeMethod g_tableManagerMethods[1];
FakeField g_tableManagerFields[1];
FakeClass g_tableManager{};

FakeMethod g_gameManagerMethods[2];
FakeField g_gameManagerFields[2];
FakeClass g_gameManager{};

FakeClass* g_classes[3] = {&g_betManager, &g_tableManager, &g_gameManager};
FakeImage g_image{"Assembly-CSharp", *g_classes, 3};

FakeDomain g_domain{};
FakeThread g_thread{1};
FakeVTable g_vtableForBetManager{&g_betManager};
FakeVTable g_vtableForTableManager{&g_tableManager};
FakeVTable g_vtableForGameManager{&g_gameManager};

// Состояние «игры»: сколько раз вызвали ставку и сколько создано объектов.
// Счётчики вызовов видны снаружи процесса (экспортируются функциями ниже),
// поэтому volatile; инкремент записан как «= value + 1», потому что ++ на
// volatile-объекте в C++20 объявлен устаревшим (P1152) и под /WX опасен.
volatile i64 g_placeBetCalls = 0;
volatile i64 g_spawnCubeCalls = 0;
volatile i64 g_destroyObjectCalls = 0;
volatile float g_lastBetAmount = 0.0f;
volatile i32 g_lastChipType = 0;

void InitializeMetadata() {
    // Game.BetManager (экземплярный класс: как MonoBehaviour в живой игре).
    g_betManagerMethods[0] = FakeMethod{"PlaceBet", &g_betManager, &g_typeObject, {&g_typeFloat, &g_typeInt}, 2, 0x6, nullptr};
    g_betManagerMethods[1] = FakeMethod{"ResolveBet", &g_betManager, &g_typeLong, {}, 0, 0x6, nullptr};
    g_betManagerFields[0] = FakeField{"betAmount", &g_typeFloat, 16, false, 0, &g_betManager};
    g_betManager = FakeClass{"Game", "BetManager", nullptr, g_betManagerFields, 1, g_betManagerMethods, 2, nullptr, 0, 0x02000011};

    // Game.TableManager: создаёт столы (и умеет создавать объекты мира).
    g_tableManagerMethods[0] = FakeMethod{"SpawnTable", &g_tableManager, &g_typeObject, {&g_typeInt, &g_typeInt}, 2, 0x6, nullptr};
    g_tableManagerFields[0] = FakeField{"lastTableId", &g_typeInt, 16, false, 0, &g_tableManager};
    g_tableManager = FakeClass{"Game", "TableManager", nullptr, g_tableManagerFields, 1, g_tableManagerMethods, 1, nullptr, 0, 0x02000012};

    // Game.GameManager: синглтон с полем chips — цель poll-режима.
    g_gameManagerMethods[0] = FakeMethod{"Update", &g_gameManager, &g_typeObject, {}, 0, 0x6, nullptr};
    g_gameManagerMethods[1] = FakeMethod{"get_Instance", &g_gameManager, &g_typeObject, {}, 0, 0x16, nullptr};
    g_gameManagerFields[0] = FakeField{"chips", &g_typeLong, 0, true, 2500, &g_gameManager};
    g_gameManagerFields[1] = FakeField{"instance", &g_typeObject, 0, true, 0, &g_gameManager};
    g_gameManager = FakeClass{"Game", "GameManager", nullptr, g_gameManagerFields, 2, g_gameManagerMethods, 2, nullptr, 0, 0x02000013};
}

FakeClass* FindClass(const char* nameSpace, const char* name) {
    if (name == nullptr) return nullptr;

    for (FakeClass* klass : g_classes) {
        const bool namespaceMatches = (nameSpace == nullptr || nameSpace[0] == '\0' || nameSpace == nullptr)
                                          ? (klass->nameSpace == nullptr || klass->nameSpace[0] == '\0')
                                          : (std::strcmp(klass->nameSpace, nameSpace) == 0);
        if (namespaceMatches && std::strcmp(klass->name, name) == 0) return klass;
    }
    return nullptr;
}

FakeMethod* FindMethod(FakeClass* klass, const char* name, int paramCount) {
    if (klass == nullptr || name == nullptr) return nullptr;

    for (int index = 0; index < klass->methodCount; ++index) {
        FakeMethod& method = klass->methods[index];
        if (std::strcmp(method.name, name) != 0) continue;
        if (paramCount >= 0 && method.paramCount != paramCount) continue;
        return &method;
    }
    return nullptr;
}

FakeField* FindField(FakeClass* klass, const char* name) {
    if (klass == nullptr || name == nullptr) return nullptr;

    for (int index = 0; index < klass->fieldCount; ++index) {
        if (std::strcmp(klass->fields[index].name, name) == 0) return &klass->fields[index];
    }
    return nullptr;
}

u32 TypeSize(const FakeType* type) {
    return type != nullptr ? type->size : 4;
}

void CopyValue(const void* source, void* destination, u32 size) {
    std::memcpy(destination, source, size);
}

}  // namespace

extern "C" {

// ── Экспорты, которые ищет движок (имена и сигнатуры — как у настоящего Mono) ─

__declspec(dllexport) void* mono_get_root_domain(void) { return &g_domain; }

__declspec(dllexport) void* mono_thread_attach(void* domain) {
    (void)domain;
    return &g_thread;
}

__declspec(dllexport) void mono_thread_detach(void* thread) { (void)thread; }

__declspec(dllexport) void* mono_image_loaded(const char* name) {
    if (name != nullptr && std::strcmp(name, "Assembly-CSharp") == 0) return &g_image;
    return nullptr;
}

__declspec(dllexport) const char* mono_image_get_name(void* image) {
    return image != nullptr ? static_cast<FakeImage*>(image)->name : nullptr;
}

__declspec(dllexport) u32 mono_image_get_table_rows(void* image, int table) {
    (void)image;
    (void)table;
    return 3;
}

__declspec(dllexport) void mono_assembly_foreach(void (*callback)(void*, void*), void* user) {
    (void)callback;
    (void)user;
}

__declspec(dllexport) void* mono_domain_assembly_open(void* domain, const char* name) {
    (void)domain;
    (void)name;
    return &g_image;
}

__declspec(dllexport) void* mono_assembly_get_image(void* assembly) { return assembly; }

__declspec(dllexport) void* mono_class_from_name(void* image, const char* nameSpace, const char* name) {
    (void)image;
    return FindClass(nameSpace, name);
}

__declspec(dllexport) void* mono_class_get_parent(void* klass) {
    return klass != nullptr ? static_cast<FakeClass*>(klass)->parent : nullptr;
}

__declspec(dllexport) void* mono_class_get_image(void* klass) {
    (void)klass;
    return &g_image;
}

__declspec(dllexport) const char* mono_class_get_name(void* klass) {
    return klass != nullptr ? static_cast<FakeClass*>(klass)->name : nullptr;
}

__declspec(dllexport) const char* mono_class_get_namespace(void* klass) {
    return klass != nullptr ? static_cast<FakeClass*>(klass)->nameSpace : nullptr;
}

__declspec(dllexport) void* mono_class_vtable(void* domain, void* klass) {
    (void)domain;
    if (klass == &g_betManager) return &g_vtableForBetManager;
    if (klass == &g_tableManager) return &g_vtableForTableManager;
    if (klass == &g_gameManager) return &g_vtableForGameManager;
    return nullptr;
}

__declspec(dllexport) u32 mono_class_get_type_token(void* klass) {
    return klass != nullptr ? static_cast<FakeClass*>(klass)->typeToken : 0;
}

__declspec(dllexport) i32 mono_class_instance_size(void* klass) {
    (void)klass;
    return 32;
}

__declspec(dllexport) void* mono_class_get(void* image, u32 typeToken) {
    (void)image;
    for (FakeClass* klass : g_classes) {
        if (klass->typeToken == typeToken) return klass;
    }
    return nullptr;
}

__declspec(dllexport) void* mono_class_get_method_from_name(void* klass, const char* name, int paramCount) {
    return FindMethod(static_cast<FakeClass*>(klass), name, paramCount);
}

__declspec(dllexport) void* mono_class_get_methods(void* klass, void** iterator) {
    auto* fakeClass = static_cast<FakeClass*>(klass);
    if (fakeClass == nullptr || iterator == nullptr) return nullptr;

    auto index = reinterpret_cast<i64>(*iterator);
    if (index >= fakeClass->methodCount) return nullptr;

    *iterator = reinterpret_cast<void*>(index + 1);
    return &fakeClass->methods[index];
}

__declspec(dllexport) void* mono_class_get_fields(void* klass, void** iterator) {
    auto* fakeClass = static_cast<FakeClass*>(klass);
    if (fakeClass == nullptr || iterator == nullptr) return nullptr;

    auto index = reinterpret_cast<i64>(*iterator);
    if (index >= fakeClass->fieldCount) return nullptr;

    *iterator = reinterpret_cast<void*>(index + 1);
    return &fakeClass->fields[index];
}

__declspec(dllexport) void* mono_class_get_field_from_name(void* klass, const char* name) {
    return FindField(static_cast<FakeClass*>(klass), name);
}

__declspec(dllexport) const char* mono_method_get_name(void* method) {
    return method != nullptr ? static_cast<FakeMethod*>(method)->name : nullptr;
}

__declspec(dllexport) void* mono_method_get_class(void* method) {
    return method != nullptr ? static_cast<FakeMethod*>(method)->klass : nullptr;
}

__declspec(dllexport) u32 mono_method_get_flags(void* method, u32* implementationFlags) {
    if (implementationFlags != nullptr) *implementationFlags = 0;
    return method != nullptr ? static_cast<u32>(static_cast<FakeMethod*>(method)->flags) : 0;
}

__declspec(dllexport) u32 mono_method_get_param_count(void* method) {
    return method != nullptr ? static_cast<u32>(static_cast<FakeMethod*>(method)->paramCount) : 0;
}

__declspec(dllexport) void* mono_method_signature(void* method) {
    return method;  // сигнатура хранится в самом методе
}

__declspec(dllexport) u32 mono_signature_get_param_count(void* signature) {
    return signature != nullptr ? static_cast<u32>(static_cast<FakeMethod*>(signature)->paramCount) : 0;
}

__declspec(dllexport) void* mono_signature_get_params(void* signature, void** iterator) {
    auto* method = static_cast<FakeMethod*>(signature);
    if (method == nullptr || iterator == nullptr) return nullptr;

    auto index = reinterpret_cast<i64>(*iterator);
    if (index >= method->paramCount) return nullptr;

    *iterator = reinterpret_cast<void*>(index + 1);
    return method->params[index];
}

__declspec(dllexport) void* mono_signature_get_return_type(void* signature) {
    return signature != nullptr ? static_cast<FakeMethod*>(signature)->returnType : nullptr;
}

__declspec(dllexport) u32 mono_type_get_type(void* type) {
    return type != nullptr ? static_cast<FakeType*>(type)->code : kTypeI4;
}

__declspec(dllexport) const char* mono_type_get_name(void* type) {
    return type != nullptr ? static_cast<FakeType*>(type)->name : nullptr;
}

__declspec(dllexport) const char* mono_field_get_name(void* field) {
    return field != nullptr ? static_cast<FakeField*>(field)->name : nullptr;
}

__declspec(dllexport) void* mono_field_get_type(void* field) {
    return field != nullptr ? static_cast<FakeField*>(field)->type : nullptr;
}

__declspec(dllexport) u32 mono_field_get_offset(void* field) {
    return field != nullptr ? static_cast<FakeField*>(field)->offset : 0;
}

__declspec(dllexport) void mono_field_get_value(void* object, void* field, void* out) {
    auto* fakeField = static_cast<FakeField*>(field);
    if (fakeField == nullptr || out == nullptr) return;
    if (object == nullptr) return;
    CopyValue(static_cast<const u8*>(object) + fakeField->offset, out, TypeSize(fakeField->type));
}

__declspec(dllexport) void mono_field_static_get_value(void* vtable, void* field, void* out) {
    (void)vtable;
    auto* fakeField = static_cast<FakeField*>(field);
    if (fakeField == nullptr || out == nullptr) return;
    CopyValue(&fakeField->staticSlot, out, TypeSize(fakeField->type));
}

__declspec(dllexport) void mono_field_set_value(void* object, void* field, void* value) {
    auto* fakeField = static_cast<FakeField*>(field);
    if (fakeField == nullptr || value == nullptr || object == nullptr) return;
    CopyValue(value, static_cast<u8*>(object) + fakeField->offset, TypeSize(fakeField->type));
}

__declspec(dllexport) void mono_field_static_set_value(void* vtable, void* field, void* value) {
    (void)vtable;
    auto* fakeField = static_cast<FakeField*>(field);
    if (fakeField == nullptr || value == nullptr) return;
    CopyValue(value, &fakeField->staticSlot, TypeSize(fakeField->type));
}

__declspec(dllexport) void* mono_object_get_class(void* object) {
    (void)object;
    return &g_betManager;  // в синтетическом хосте объект всегда BetManager
}

__declspec(dllexport) void* mono_object_new(void* domain, void* klass) {
    (void)domain;
    (void)klass;
    return nullptr;
}

__declspec(dllexport) void* mono_object_unbox(void* object) { return object; }

__declspec(dllexport) void* mono_object_to_string(void* object, void* exception) {
    (void)object;
    if (exception != nullptr) exception = nullptr;
    return nullptr;
}

__declspec(dllexport) char* mono_string_to_utf8(void* text) { (void)text; return nullptr; }

__declspec(dllexport) void mono_free(void* memory) { (void)memory; }

// ── Ключевые функции: JIT и invoke ──────────────────────────────────────────

/// «Компиляция» метода: адрес нашей функции-исполнителя. Движок использует
/// этот адрес, когда цель настроена в режиме method.
__declspec(dllexport) void* mono_compile_method(void* method) {
    auto* fakeMethod = static_cast<FakeMethod*>(method);
    if (fakeMethod == nullptr) return nullptr;
    if (fakeMethod->compiled == nullptr) {
        fakeMethod->compiled = const_cast<i64*>(&g_placeBetCalls);  // любой стабильный адрес
    }
    return fakeMethod->compiled;
}

__declspec(dllexport) void* mono_method_get_unmanaged_thunk(void* method) {
    return mono_compile_method(method);
}

/// Вызов managed-метода. Именно эту функцию перехватывает движок, поэтому
/// здесь важно поведение, близкое к настоящему Mono: аргументы лежат в
/// params как указатели на значения.
__declspec(dllexport) void* mono_runtime_invoke(void* method, void* object, void** params, void* exception) {
    if (exception != nullptr) {
        *reinterpret_cast<void**>(exception) = nullptr;
    }

    auto* fakeMethod = static_cast<FakeMethod*>(method);
    if (fakeMethod == nullptr) return nullptr;

    if (std::strcmp(fakeMethod->name, "PlaceBet") == 0) {
        g_placeBetCalls = g_placeBetCalls + 1;
        if (params != nullptr && fakeMethod->paramCount >= 2) {
            if (params[0] != nullptr) g_lastBetAmount = *static_cast<const float*>(params[0]);
            if (params[1] != nullptr) g_lastChipType = *static_cast<const i32*>(params[1]);
        }
        return nullptr;
    }

    if (std::strcmp(fakeMethod->name, "SpawnTable") == 0) {
        g_spawnCubeCalls = g_spawnCubeCalls + 1;
        return &g_betManager;  // «объект стола»
    }

    if (std::strcmp(fakeMethod->name, "get_Instance") == 0) {
        return &g_gameManager;
    }

    return object;
}

// ── Поддельная фабрика объектов мира (адаптер native_export) ────────────────

/// Аналог GameObject.CreatePrimitive в терминах нативного слоя: создаёт объект
/// и возвращает его «указатель».
__declspec(dllexport) bool FakeUnity_SpawnCube(int x, int y, int z, unsigned block, void** outHandle) {
    g_spawnCubeCalls = g_spawnCubeCalls + 1;
    if (outHandle != nullptr) {
        // Хендл — наглядно различимое значение: адрес счётчика + координаты в младших битах.
        *reinterpret_cast<u64*>(outHandle) = reinterpret_cast<u64>(&g_spawnCubeCalls) ^ (static_cast<u64>(block) << 48) ^
                                             (static_cast<u64>(static_cast<u32>(x)) << 8) ^
                                             static_cast<u64>(static_cast<u32>(y));
    }
    return true;
}

__declspec(dllexport) void FakeUnity_DestroyObject(void* handle) {
    (void)handle;
    g_destroyObjectCalls = g_destroyObjectCalls + 1;
}

// ── Управление из самотеста ─────────────────────────────────────────────────

/// Вызвать метод по именам класса и метода так, как это сделала бы игра.
__declspec(dllexport) void* FakeMono_InvokeByName(const char* classFullName, void** params, const char* methodName) {
    if (classFullName == nullptr || methodName == nullptr) return nullptr;

    char buffer[128]{};
    std::strncpy(buffer, classFullName, sizeof(buffer) - 1);

    char* dot = std::strrchr(buffer, '.');
    const char* nameSpace = "Game";
    const char* name = buffer;
    if (dot != nullptr) {
        *dot = '\0';
        nameSpace = buffer;
        name = dot + 1;
    }

    FakeClass* klass = FindClass(nameSpace, name);
    if (klass == nullptr) return nullptr;

    FakeMethod* method = FindMethod(klass, methodName, -1);
    if (method == nullptr) return nullptr;

    // Вызываем ЧЕРЕЗ mono_runtime_invoke: если движок поставил хук, он увидит
    // этот вызов и опубликует событие — именно это и проверяет самотест.
    return mono_runtime_invoke(method, klass, params, nullptr);
}

/// Установить значение статического поля (для проверки poll-режима).
__declspec(dllexport) bool FakeMono_SetStaticInt64(const char* className, const char* fieldName, i64 value) {
    if (className == nullptr || fieldName == nullptr) return false;

    FakeClass* klass = FindClass("Game", className);
    if (klass == nullptr) return false;

    FakeField* field = FindField(klass, fieldName);
    if (field == nullptr) return false;

    field->staticSlot = value;
    return true;
}

__declspec(dllexport) i64 FakeMono_PlaceBetCalls(void) { return g_placeBetCalls; }

/// Что именно приняла «игра» в оригинальном методе: если детур передал вызов
/// дальше без искажений, суммы и типы фишки совпадут с тем, что отправил тест.
__declspec(dllexport) float FakeMono_LastBetAmount(void) { return g_lastBetAmount; }
__declspec(dllexport) i32 FakeMono_LastChipType(void) { return g_lastChipType; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        InitializeMetadata();
    }
    return TRUE;
}

}  // extern "C"
