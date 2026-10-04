// ============================================================================
//  MonoRuntime.cpp — загрузка Mono и работа с managed-объектами по именам.
// ============================================================================
#include "gwyfbridge/MonoRuntime.h"

#include <algorithm>
#include <utility>
#include <cstdio>

namespace gwyf::mono {

namespace {

Api g_api{};
bool g_loaded = false;
bool g_runtimeError = false;
std::string g_moduleName;
std::string g_loadError;
u32 g_attachCount = 0;

/// Описание экспорта: имя функции Mono и куда положить указатель.
struct ExportSlot {
    const char* name;
    void** slot;
    bool required;
};

constexpr const char* kCandidateModules[] = {
    "mono-2.0-bdwgc.dll",  // Unity 2019+ (Boehm GC)
    "mono-2.0-sgen.dll",   // Unity 2018.x
    "mono-2.0.dll",        // классические сборки Mono
    "mono.dll",            // очень старые Unity
    "fake_unity_mono.dll", // синтетический хост для самотестов
};

std::vector<ExportSlot> BuildExportTable() {
    return {
        {"mono_get_root_domain", reinterpret_cast<void**>(&g_api.get_root_domain), true},
        {"mono_thread_attach", reinterpret_cast<void**>(&g_api.thread_attach), true},
        {"mono_thread_detach", reinterpret_cast<void**>(&g_api.thread_detach), false},

        {"mono_image_loaded", reinterpret_cast<void**>(&g_api.image_loaded), false},
        {"mono_image_get_name", reinterpret_cast<void**>(&g_api.image_get_name), false},
        {"mono_assembly_foreach", reinterpret_cast<void**>(&g_api.assembly_foreach), false},
        {"mono_domain_assembly_open", reinterpret_cast<void**>(&g_api.domain_assembly_open), false},
        {"mono_assembly_get_image", reinterpret_cast<void**>(&g_api.assembly_get_image), false},

        {"mono_class_from_name", reinterpret_cast<void**>(&g_api.class_from_name), true},
        {"mono_class_get_parent", reinterpret_cast<void**>(&g_api.class_get_parent), false},
        {"mono_class_get_image", reinterpret_cast<void**>(&g_api.class_get_image), false},
        {"mono_class_get_name", reinterpret_cast<void**>(&g_api.class_get_name), true},
        {"mono_class_get_namespace", reinterpret_cast<void**>(&g_api.class_get_namespace), true},
        {"mono_class_vtable", reinterpret_cast<void**>(&g_api.class_vtable), true},
        {"mono_class_get_type_token", reinterpret_cast<void**>(&g_api.class_get_type_token), false},
        {"mono_class_instance_size", reinterpret_cast<void**>(&g_api.class_instance_size), false},
        {"mono_class_instance_size", reinterpret_cast<void**>(&g_api.class_instance_size), false},
        {"mono_class_get", reinterpret_cast<void**>(&g_api.class_get), false},
        {"mono_image_get_table_rows", reinterpret_cast<void**>(&g_api.image_get_table_rows), false},

        {"mono_class_get_method_from_name", reinterpret_cast<void**>(&g_api.class_get_method_from_name), true},
        {"mono_class_get_methods", reinterpret_cast<void**>(&g_api.class_get_methods), false},
        {"mono_class_get_fields", reinterpret_cast<void**>(&g_api.class_get_fields), false},
        {"mono_class_get_field_from_name", reinterpret_cast<void**>(&g_api.class_get_field_from_name), true},

        {"mono_method_get_name", reinterpret_cast<void**>(&g_api.method_get_name), true},
        {"mono_method_signature", reinterpret_cast<void**>(&g_api.method_signature), false},
        {"mono_method_get_flags", reinterpret_cast<void**>(&g_api.method_get_flags), false},
        {"mono_method_get_class", reinterpret_cast<void**>(&g_api.method_get_class), false},
        {"mono_signature_get_param_count", reinterpret_cast<void**>(&g_api.signature_get_param_count), false},
        {"mono_signature_get_params", reinterpret_cast<void**>(&g_api.signature_get_params), false},
        {"mono_type_get_type", reinterpret_cast<void**>(&g_api.type_get_type), false},

        {"mono_field_get_name", reinterpret_cast<void**>(&g_api.field_get_name), false},
        {"mono_field_get_type", reinterpret_cast<void**>(&g_api.field_get_type), false},
        {"mono_type_get_name", reinterpret_cast<void**>(&g_api.type_get_name), false},
        {"mono_field_get_offset", reinterpret_cast<void**>(&g_api.field_get_offset), true},

        {"mono_runtime_invoke", reinterpret_cast<void**>(&g_api.runtime_invoke), true},
        {"mono_compile_method", reinterpret_cast<void**>(&g_api.compile_method), true},
        {"mono_method_get_unmanaged_thunk", reinterpret_cast<void**>(&g_api.method_get_unmanaged_thunk), false},

        {"mono_field_get_value", reinterpret_cast<void**>(&g_api.field_get_value), true},
        {"mono_field_static_get_value", reinterpret_cast<void**>(&g_api.field_static_get_value), true},
        {"mono_field_set_value", reinterpret_cast<void**>(&g_api.field_set_value), false},
        {"mono_field_static_set_value", reinterpret_cast<void**>(&g_api.field_static_set_value), false},

        {"mono_object_get_class", reinterpret_cast<void**>(&g_api.object_get_class), true},
        {"mono_object_new", reinterpret_cast<void**>(&g_api.object_new), false},
        {"mono_object_unbox", reinterpret_cast<void**>(&g_api.object_unbox), false},
        {"mono_object_to_string", reinterpret_cast<void**>(&g_api.object_to_string), false},
        {"mono_string_to_utf8", reinterpret_cast<void**>(&g_api.string_to_utf8), false},
        {"mono_free", reinterpret_cast<void**>(&g_api.free_memory), false},
    };
}

HMODULE OpenMonoModule(const std::string& name) {
    // Сначала проверяем, не загружен ли модуль уже (обычный случай: игра сама
    // подняла Mono, и грузить второй экземпляр категорически нельзя).
    if (HMODULE existing = GetModuleHandleA(name.c_str())) {
        return existing;
    }
    return LoadLibraryA(name.c_str());
}

}  // namespace

bool Load(std::string* error) {
    if (g_loaded) return true;

    std::vector<std::string> candidates;

    // Переменная окружения позволяет подменить модуль — этим пользуется
    // самотест (синтетический хост) и это удобно при отладке нестандартных сборок.
    char overrideName[260]{};
    const DWORD overrideLen = GetEnvironmentVariableA("GWYF_MONO_MODULE", overrideName, sizeof(overrideName));
    if (overrideLen > 0 && overrideLen < sizeof(overrideName)) {
        candidates.emplace_back(overrideName);
    }
    for (const char* name : kCandidateModules) {
        candidates.emplace_back(name);
    }

    HMODULE module = nullptr;
    for (const std::string& candidate : candidates) {
        HMODULE handle = OpenMonoModule(candidate);
        if (handle == nullptr) continue;

        // Проверяем, что в модуле действительно есть ключевые экспорты:
        // так мы не примем за Mono случайную DLL с похожим именем.
        if (GetProcAddress(handle, "mono_get_root_domain") == nullptr) {
            GWYF_DEBUG("Модуль %s не похож на Mono — пропускаем", candidate.c_str());
            if (GetModuleHandleA(candidate.c_str()) == nullptr) FreeLibrary(handle);
            continue;
        }

        module = handle;
        g_moduleName = candidate;
        break;
    }

    if (module == nullptr) {
        const std::string text =
            "Mono не найден ни под одним из имён. Ожидались mono-2.0-bdwgc.dll / mono-2.0.dll / mono.dll. "
            "Если игра собрана на IL2CPP, этот адаптер неприменим (см. README, раздел про IL2CPP).";
        if (error != nullptr) *error = text;
        g_loadError = text;
        GWYF_WARN("%s", text.c_str());
        return false;
    }

    std::vector<ExportSlot> table = BuildExportTable();
    std::string missingRequired;

    for (const ExportSlot& slot : table) {
        *slot.slot = GetExport(module, slot.name);
        if (*slot.slot == nullptr) {
            if (slot.required) {
                missingRequired += std::string(missingRequired.empty() ? "" : ", ") + slot.name;
            } else {
                GWYF_DEBUG("Mono: опциональный экспорт %s отсутствует", slot.name);
            }
        }
    }

    if (!missingRequired.empty()) {
        const std::string text = "в модуле " + g_moduleName + " нет обязательных экспортов: " + missingRequired;
        if (error != nullptr) *error = text;
        g_loadError = text;
        GWYF_ERROR("%s", text.c_str());
        return false;
    }

    g_api.runtime_invoke_address = reinterpret_cast<void*>(g_api.runtime_invoke);
    g_loaded = true;
    GWYF_INFO("Mono загружен из %s, mono_runtime_invoke=%p", g_moduleName.c_str(), g_api.runtime_invoke_address);
    return true;
}

bool IsLoaded() {
    return g_loaded;
}

bool RuntimeReady() {
    if (!g_loaded || g_api.get_root_domain == nullptr) return false;
    if (g_runtimeError) return false;
    return g_api.get_root_domain() != nullptr;
}

const char* ModuleName() {
    return g_moduleName.c_str();
}

const Api& Fn() {
    return g_api;
}

void OverrideApiForTesting(const Api& api, const char* moduleName) {
    g_api = api;
    g_moduleName = moduleName != nullptr ? moduleName : "override";
    g_loaded = true;
    g_api.runtime_invoke_address = reinterpret_cast<void*>(g_api.runtime_invoke);
}

void* RootDomain() {
    if (!g_loaded || g_api.get_root_domain == nullptr) return nullptr;
    return g_api.get_root_domain();
}

bool AttachCurrentThread() {
    if (!RuntimeReady() || g_api.thread_attach == nullptr) return false;

    void* domain = g_api.get_root_domain();
    void* thread = g_api.thread_attach(domain);
    if (thread == nullptr) {
        GWYF_WARN("mono_thread_attach вернул nullptr — вызывать managed-код из этого потока нельзя");
        return false;
    }

    ++g_attachCount;
    return true;
}

void DetachCurrentThread() {
    if (!g_loaded || g_api.thread_detach == nullptr) return;
    g_api.thread_detach(nullptr);
}

// ── Поиск сущностей ─────────────────────────────────────────────────────────

namespace {

std::vector<void*> g_assemblyScratch;

void CollectAssembly(void* data, void* /*user*/) {
    if (data != nullptr) g_assemblyScratch.push_back(data);
}

}  // namespace

void* FindImage(const char* assemblyName) {
    if (!RuntimeReady() || assemblyName == nullptr) return nullptr;

    // Основной путь: mono_image_loaded("Assembly-CSharp").
    if (g_api.image_loaded != nullptr) {
        if (void* image = g_api.image_loaded(assemblyName)) {
            return image;
        }
    }

    // Резервный путь: перебор загруженных сборок (устойчив к версиям Mono,
    // где mono_image_loaded не экспортируется).
    if (g_api.assembly_foreach == nullptr || g_api.assembly_get_image == nullptr) return nullptr;

    g_assemblyScratch.clear();
    g_api.assembly_foreach(&CollectAssembly, nullptr);

    const std::string wanted = ToLowerAscii(assemblyName);

    for (void* assembly : g_assemblyScratch) {
        void* image = g_api.assembly_get_image(assembly);
        if (image == nullptr) continue;
        if (g_api.image_get_name == nullptr) return image;

        const char* name = g_api.image_get_name(image);
        if (name == nullptr) continue;

        const std::string lower = ToLowerAscii(name);

        // Имя образа приходит без расширения ("Assembly-CSharp").
        if (lower.find(wanted) != std::string::npos) return image;
        if (lower + ".dll" == wanted || lower + ".exe" == wanted) return image;
    }

    return nullptr;
}

void* FindClass(const char* assemblyName, const char* nameSpace, const char* className) {
    if (!RuntimeReady() || className == nullptr) return nullptr;

    void* image = assemblyName != nullptr ? FindImage(assemblyName) : nullptr;
    if (image == nullptr) return nullptr;

    void* klass = g_api.class_from_name(image, nameSpace != nullptr ? nameSpace : "", className);
    if (klass == nullptr) {
        GWYF_DEBUG("Класс %s.%s не найден в образе %s",
                   nameSpace != nullptr ? nameSpace : "", className, assemblyName);
    }
    return klass;
}

void* FindNestedClass(const char* assemblyName, const char* outerNameSpace, const char* outerClass, const char* nestedClass) {
    if (nestedClass == nullptr) return nullptr;

    // В Mono вложенные классы видны как "Outer/Inner" при поиске по образу.
    void* image = assemblyName != nullptr ? FindImage(assemblyName) : nullptr;
    if (image == nullptr) return nullptr;

    std::string combined = std::string(outerClass != nullptr ? outerClass : "") + "/" + nestedClass;
    return g_api.class_from_name(image, outerNameSpace != nullptr ? outerNameSpace : "", combined.c_str());
}

void* FindMethod(void* klass, const char* name, int paramCount) {
    if (!RuntimeReady() || klass == nullptr || name == nullptr) return nullptr;
    return g_api.class_get_method_from_name(klass, name, paramCount);
}

void* FindMethodInHierarchy(void* klass, const char* name, int paramCount) {
    void* current = klass;
    int depth = 0;

    while (current != nullptr && depth < 32) {
        if (void* method = FindMethod(current, name, paramCount)) return method;
        if (g_api.class_get_parent == nullptr) break;
        current = g_api.class_get_parent(current);
        ++depth;
    }

    return nullptr;
}

void* FindField(void* klass, const char* name) {
    if (!RuntimeReady() || klass == nullptr || name == nullptr) return nullptr;

    void* current = klass;
    int depth = 0;

    // Поля базовых классов лежат по своим офсетам, поэтому подъём обязателен:
    // у Mirror/Unity почти вся полезная state живёт в базовых типах.
    while (current != nullptr && depth < 32) {
        if (void* field = g_api.class_get_field_from_name(current, name)) return field;
        if (g_api.class_get_parent == nullptr) break;
        current = g_api.class_get_parent(current);
        ++depth;
    }

    return nullptr;
}

void* CompileMethod(void* method) {
    if (!RuntimeReady() || method == nullptr || g_api.compile_method == nullptr) return nullptr;

    // mono_compile_method заставляет JIT скомпилировать метод и возвращает
    // указатель на нативный код. Именно он становится целью хука.
    void* code = g_api.compile_method(method);
    if (code == nullptr) {
        GWYF_WARN("mono_compile_method вернул nullptr — метод, скорее всего, ещё не скомпилирован JIT");
    }
    return code;
}

void* UnmanagedThunk(void* method) {
    if (!RuntimeReady() || method == nullptr || g_api.method_get_unmanaged_thunk == nullptr) return nullptr;
    return g_api.method_get_unmanaged_thunk(method);
}

// ── Вызовы ──────────────────────────────────────────────────────────────────

std::string DescribeException(void* exception) {
    if (exception == nullptr) return std::string();

    if (g_api.object_get_class == nullptr || g_api.class_get_name == nullptr) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%p", exception);
        return std::string("исключение ") + buffer;
    }

    void* klass = g_api.object_get_class(exception);
    const char* name = klass != nullptr ? g_api.class_get_name(klass) : nullptr;
    const char* nameSpace = nullptr;
    if (klass != nullptr && g_api.class_get_namespace != nullptr) {
        nameSpace = g_api.class_get_namespace(klass);
    }

    std::string text = "managed-исключение ";
    if (nameSpace != nullptr && *nameSpace != '\0') text += std::string(nameSpace) + ".";
    text += name != nullptr ? name : "<без имени>";
    return text;
}

void* Invoke(void* method, void* instance, void** args, std::string* exceptionText) {
    if (!RuntimeReady() || method == nullptr) {
        if (exceptionText != nullptr) *exceptionText = "runtime Mono не готов или метод не найден";
        return nullptr;
    }

    void* exception = nullptr;
    void* result = g_api.runtime_invoke(method, instance, args, &exception);

    if (exception != nullptr) {
        const std::string text = DescribeException(exception);
        if (exceptionText != nullptr) *exceptionText = text;
        GWYF_WARN("Вызов managed-метода: %s", text.c_str());
    } else if (exceptionText != nullptr) {
        exceptionText->clear();
    }

    return result;
}

void* Invoke(void* method, void* instance, std::string* exceptionText) {
    return Invoke(method, instance, nullptr, exceptionText);
}

void* NewObject(void* klass, std::string* exceptionText) {
    if (!RuntimeReady() || klass == nullptr || g_api.object_new == nullptr) {
        if (exceptionText != nullptr) *exceptionText = "создание объекта недоступно";
        return nullptr;
    }

    void* object = g_api.object_new(g_api.get_root_domain(), klass);
    if (object == nullptr && exceptionText != nullptr) {
        *exceptionText = "mono_object_new вернул nullptr";
    }
    return object;
}

// ── Поля ────────────────────────────────────────────────────────────────────

i32 FieldOffset(void* field) {
    if (!RuntimeReady() || field == nullptr || g_api.field_get_offset == nullptr) return -1;
    return static_cast<i32>(g_api.field_get_offset(field));
}

// ── Классификация типов ─────────────────────────────────────────────────────
//
//  Значения MONO_TYPE_* взяты из mono/metadata/blob.h (публичный контракт ABI).
namespace {
constexpr u32 kMonoTypeBoolean = 0x02;
constexpr u32 kMonoTypeI1 = 0x04;
constexpr u32 kMonoTypeU1 = 0x05;
constexpr u32 kMonoTypeI2 = 0x06;
constexpr u32 kMonoTypeU2 = 0x07;
constexpr u32 kMonoTypeI4 = 0x08;
constexpr u32 kMonoTypeU4 = 0x09;
constexpr u32 kMonoTypeI8 = 0x0a;
constexpr u32 kMonoTypeU8 = 0x0b;
constexpr u32 kMonoTypeR4 = 0x0c;
constexpr u32 kMonoTypeR8 = 0x0d;
constexpr u32 kMonoTypeString = 0x0e;
constexpr u32 kMonoTypePtr = 0x0f;
constexpr u32 kMonoTypeObject = 0x1c;
constexpr u32 kMonoTypeSzArray = 0x1d;
constexpr u32 kMonoTypeClass = 0x12;
constexpr u32 kMonoTypeValuetype = 0x11;
constexpr u32 kMonoTypeEnum = 0x55;
}  // namespace

i32 ClassInstanceSize(void* klass) {
    if (klass == nullptr || g_api.class_instance_size == nullptr) return -1;
    return g_api.class_instance_size(klass);
}

const char* ArgKindName(ArgKind kind) {
    switch (kind) {
        case ArgKind::Bool: return "bool";
        case ArgKind::Int8: return "int8";
        case ArgKind::Int16: return "int16";
        case ArgKind::Int32: return "int32";
        case ArgKind::Int64: return "int64";
        case ArgKind::Float32: return "float";
        case ArgKind::Float64: return "double";
        case ArgKind::Object: return "object";
        default: return "unknown";
    }
}

ArgKind ClassifyType(void* monoType) {
    if (monoType == nullptr || g_api.type_get_type == nullptr) return ArgKind::Unknown;

    switch (g_api.type_get_type(monoType)) {
        case kMonoTypeBoolean: return ArgKind::Bool;
        case kMonoTypeI1: return ArgKind::Int8;
        case kMonoTypeU1: return ArgKind::Int8;
        case kMonoTypeI2: return ArgKind::Int16;
        case kMonoTypeU2: return ArgKind::Int16;
        case kMonoTypeI4: return ArgKind::Int32;
        case kMonoTypeU4: return ArgKind::Int32;
        case kMonoTypeI8: return ArgKind::Int64;
        case kMonoTypeU8: return ArgKind::Int64;
        case kMonoTypeR4: return ArgKind::Float32;
        case kMonoTypeR8: return ArgKind::Float64;
        case kMonoTypeString:
        case kMonoTypeObject:
        case kMonoTypeSzArray:
        case kMonoTypeClass:
            return ArgKind::Object;
        case kMonoTypePtr:
            return ArgKind::Int64;
        case kMonoTypeEnum:
            // Перечисление по умолчанию четырёхбайтовое (int); long-перечисления
            // Mono отдаёт как I8 и попадают в ветку kMonoTypeI8 выше.
            return ArgKind::Int32;
        case kMonoTypeValuetype:
            // Значимый тип лежит в аргументе целиком; читаем первые четыре байта —
            // этого достаточно для идентификаторов и счётчиков, которые пишет профиль.
            return ArgKind::Int32;
        default:
            // Прочие типы (char 0x03, genericinst 0x15 и т.п.): поведение прежнее —
            // считаем 4-байтовым. Это осознанная осторожность: менять классификацию
            // «неизвестного» типа значило бы ослабить проверку раскладки, которая
            // защищает стек детура.
            return ArgKind::Int32;
    }
}

std::vector<ArgKind> ClassifyArgs(void* method) {
    std::vector<ArgKind> result;
    if (method == nullptr || g_api.method_signature == nullptr || g_api.signature_get_params == nullptr ||
        g_api.type_get_type == nullptr) {
        return result;
    }

    void* signature = g_api.method_signature(method);
    if (signature == nullptr) return result;

    void* iterator = nullptr;
    int guard = 0;
    while (void* type = g_api.signature_get_params(signature, &iterator)) {
        if (++guard > 64) break;
        result.push_back(ClassifyType(type));
    }

    return result;
}

void ExtractArg(void* params, u32 index, ArgKind kind, i64& intOut, double& floatOut, bool& isFloat) {
    intOut = 0;
    floatOut = 0.0;
    isFloat = false;

    if (params == nullptr) return;
    auto** slots = static_cast<void**>(params);
    if (slots[index] == nullptr) return;

    switch (kind) {
        case ArgKind::Bool:
        case ArgKind::Int8:
            intOut = *static_cast<const i8*>(slots[index]);
            break;
        case ArgKind::Int16:
            intOut = *static_cast<const i16*>(slots[index]);
            break;
        case ArgKind::Int32:
            intOut = *static_cast<const i32*>(slots[index]);
            break;
        case ArgKind::Int64:
            intOut = *static_cast<const i64*>(slots[index]);
            break;
        case ArgKind::Float32:
            floatOut = *static_cast<const float*>(slots[index]);
            isFloat = true;
            intOut = static_cast<i64>(floatOut);
            break;
        case ArgKind::Float64:
            floatOut = *static_cast<const double*>(slots[index]);
            isFloat = true;
            intOut = static_cast<i64>(floatOut);
            break;
        case ArgKind::Object:
            // Для ссылочных типов в params лежит сам указатель на объект.
            intOut = reinterpret_cast<i64>(slots[index]);
            break;
        default:
            intOut = reinterpret_cast<i64>(slots[index]);
            break;
    }
}

bool ReadFieldTyped(void* instance, void* field, i64& intOut, double& floatOut, bool& isFloat) {
    intOut = 0;
    floatOut = 0.0;
    isFloat = false;

    if (instance == nullptr || field == nullptr || !g_loaded) return false;
    if (g_api.field_get_type == nullptr || g_api.type_get_type == nullptr) return false;

    const ArgKind kind = ClassifyType(g_api.field_get_type(field));

    switch (kind) {
        case ArgKind::Bool:
        case ArgKind::Int8: {
            i8 value = 0;
            g_api.field_get_value(instance, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Int16: {
            i16 value = 0;
            g_api.field_get_value(instance, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Int32: {
            i32 value = 0;
            g_api.field_get_value(instance, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Int64: {
            i64 value = 0;
            g_api.field_get_value(instance, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Float32: {
            float value = 0.0f;
            g_api.field_get_value(instance, field, &value);
            floatOut = value;
            isFloat = true;
            intOut = static_cast<i64>(value);
            return true;
        }
        case ArgKind::Float64: {
            double value = 0.0;
            g_api.field_get_value(instance, field, &value);
            floatOut = value;
            isFloat = true;
            intOut = static_cast<i64>(value);
            return true;
        }
        case ArgKind::Object: {
            void* value = nullptr;
            g_api.field_get_value(instance, field, &value);
            intOut = reinterpret_cast<i64>(value);
            return true;
        }
        default:
            return false;
    }
}

bool ReadStaticFieldTyped(void* klass, void* field, i64& intOut, double& floatOut, bool& isFloat) {
    if (klass == nullptr || field == nullptr || g_api.class_vtable == nullptr) return false;

    void* vtable = g_api.class_vtable(RootDomain(), klass);
    if (vtable == nullptr) return false;

    intOut = 0;
    floatOut = 0.0;
    isFloat = false;

    const ArgKind kind = g_api.field_get_type != nullptr ? ClassifyType(g_api.field_get_type(field)) : ArgKind::Int32;

    switch (kind) {
        case ArgKind::Float32: {
            float value = 0.0f;
            g_api.field_static_get_value(vtable, field, &value);
            floatOut = value;
            isFloat = true;
            intOut = static_cast<i64>(value);
            return true;
        }
        case ArgKind::Float64: {
            double value = 0.0;
            g_api.field_static_get_value(vtable, field, &value);
            floatOut = value;
            isFloat = true;
            intOut = static_cast<i64>(value);
            return true;
        }
        case ArgKind::Int8: {
            i8 value = 0;
            g_api.field_static_get_value(vtable, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Int16: {
            i16 value = 0;
            g_api.field_static_get_value(vtable, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Int64: {
            i64 value = 0;
            g_api.field_static_get_value(vtable, field, &value);
            intOut = value;
            return true;
        }
        case ArgKind::Object: {
            void* value = nullptr;
            g_api.field_static_get_value(vtable, field, &value);
            intOut = reinterpret_cast<i64>(value);
            return true;
        }
        default: {
            i32 value = 0;
            g_api.field_static_get_value(vtable, field, &value);
            intOut = value;
            return true;
        }
    }
}

// ── Диагностика ─────────────────────────────────────────────────────────────

std::string DescribeMethodArgs(void* method) {
    if (method == nullptr) return "<null>";

    std::string text;
    if (g_api.method_get_name != nullptr) {
        const char* name = g_api.method_get_name(method);
        text = name != nullptr ? name : "<имя неизвестно>";
    }
    text += "(";

    const std::vector<ArgKind> kinds = ClassifyArgs(method);
    if (kinds.empty()) {
        u32 count = 0;
        if (g_api.method_signature != nullptr && g_api.signature_get_param_count != nullptr) {
            if (void* signature = g_api.method_signature(method)) {
                count = g_api.signature_get_param_count(signature);
            }
        }
        text += std::to_string(count) + " параметров, типы недоступны)";
        return text;
    }

    for (usize index = 0; index < kinds.size(); ++index) {
        if (index > 0) text += ", ";
        text += ArgKindName(kinds[index]);
    }
    text += ")";
    return text;
}

std::string ClassFullName(void* klass) {
    if (klass == nullptr || !g_loaded) return "<null>";

    std::string text;
    if (g_api.class_get_namespace != nullptr) {
        const char* nameSpace = g_api.class_get_namespace(klass);
        if (nameSpace != nullptr && *nameSpace != '\0') text = std::string(nameSpace) + ".";
    }
    if (g_api.class_get_name != nullptr) {
        const char* name = g_api.class_get_name(klass);
        text += name != nullptr ? name : "<без имени>";
    }
    return text;
}

std::string MethodSignature(void* method) {
    if (method == nullptr || !g_loaded) return "<null>";

    std::string text;
    if (g_api.method_get_name != nullptr) {
        const char* name = g_api.method_get_name(method);
        text = name != nullptr ? name : "<имя неизвестно>";
    }

    u32 paramCount = 0;
    if (g_api.method_signature != nullptr && g_api.signature_get_param_count != nullptr) {
        if (void* signature = g_api.method_signature(method)) {
            paramCount = g_api.signature_get_param_count(signature);
        }
    }

    text += "(" + std::to_string(paramCount) + " параметров)";

    void* klass = g_api.method_get_class != nullptr ? g_api.method_get_class(method) : nullptr;
    if (klass != nullptr) {
        text += " в " + ClassFullName(klass);
    }
    return text;
}

std::string FieldSignature(void* field) {
    if (field == nullptr || !g_loaded) return "<null>";

    std::string text;
    if (g_api.field_get_name != nullptr) {
        const char* name = g_api.field_get_name(field);
        text = name != nullptr ? name : "<имя неизвестно>";
    }
    if (g_api.type_get_name != nullptr && g_api.field_get_type != nullptr) {
        if (void* type = g_api.field_get_type(field)) {
            const char* typeName = g_api.type_get_name(type);
            if (typeName != nullptr) text += ": " + std::string(typeName);
        }
    }

    const i32 offset = FieldOffset(field);
    if (offset >= 0) text += " @+" + std::to_string(offset);
    return text;
}

std::vector<MethodInfo> EnumMethods(void* klass) {
    std::vector<MethodInfo> result;
    if (klass == nullptr || !g_loaded || g_api.class_get_methods == nullptr) return result;

    void* iterator = nullptr;
    int guard = 0;
    while (void* method = g_api.class_get_methods(klass, &iterator)) {
        if (++guard > 4096) break;  // страховка от бесконечного итератора

        MethodInfo info{};
        info.method = method;
        if (g_api.method_get_name != nullptr) {
            const char* name = g_api.method_get_name(method);
            info.name = name != nullptr ? name : "";
        }
        if (g_api.method_get_flags != nullptr) {
            u32 iflags = 0;
            info.flags = g_api.method_get_flags(method, &iflags);
            info.iflags = iflags;
        }
        if (g_api.method_signature != nullptr && g_api.signature_get_param_count != nullptr) {
            if (void* signature = g_api.method_signature(method)) {
                info.paramCount = g_api.signature_get_param_count(signature);
            }
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const MethodInfo& left, const MethodInfo& right) {
        if (left.name != right.name) return left.name < right.name;
        return left.paramCount < right.paramCount;
    });
    return result;
}

std::vector<FieldInfo> EnumFields(void* klass) {
    std::vector<FieldInfo> result;
    if (klass == nullptr || !g_loaded || g_api.class_get_fields == nullptr) return result;

    void* iterator = nullptr;
    int guard = 0;
    while (void* field = g_api.class_get_fields(klass, &iterator)) {
        if (++guard > 4096) break;

        FieldInfo info{};
        info.field = field;
        if (g_api.field_get_name != nullptr) {
            const char* name = g_api.field_get_name(field);
            info.name = name != nullptr ? name : "";
        }
        if (g_api.type_get_name != nullptr && g_api.field_get_type != nullptr) {
            if (void* type = g_api.field_get_type(field)) {
                const char* typeName = g_api.type_get_name(type);
                info.type = typeName != nullptr ? typeName : "";
            }
        }
        info.offset = FieldOffset(field);
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const FieldInfo& left, const FieldInfo& right) {
        return left.offset < right.offset;
    });
    return result;
}

std::string DescribeClass(void* klass) {
    if (klass == nullptr) return "класс не найден";

    std::string text = "Класс " + ClassFullName(klass) + "\n";
    text += "  методы (имя, число параметров):\n";

    for (const MethodInfo& method : EnumMethods(klass)) {
        text += "    " + method.name + "(" + std::to_string(method.paramCount) + ")\n";
    }

    text += "  поля (имя, тип, смещение в объекте):\n";
    for (const FieldInfo& field : EnumFields(klass)) {
        text += "    " + field.name + " : " + field.type + " @+" + std::to_string(field.offset) + "\n";
    }

    return text;
}

bool WriteReport(const std::string& path, const std::string& text) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wt") != 0 || file == nullptr) return false;
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return true;
}

}  // namespace gwyf::mono
