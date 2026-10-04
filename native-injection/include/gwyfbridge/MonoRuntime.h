// ============================================================================
//  MonoRuntime.h — динамический адаптер к runtime Mono внутри Unity-игры.
//
//  ЗАЧЕМ ЭТО, А НЕ «ПОИСК ОФСЕТОВ»
//  ---------------------------------------------------------------
//  Gamble With Your Friends — Unity **Mono** (Assembly-CSharp.dll), значит
//  игра исполняет CIL в Mono, а Mono экспортирует наружу публичный C-API
//  (mono_class_from_name, mono_runtime_invoke, mono_compile_method…).
//  Это даёт то, чего не даёт поиск офсетов:
//    * классы/методы/поля ищутся ПО ИМЕНИ из декомпилированной сборки,
//      а не по адресам, которые ломаются после каждого патча игры;
//    * значения полей читаются через mono_field_get_value — без знания
//      фактической раскладки структур в памяти;
//    * метод можно вызвать через mono_runtime_invoke — это рабочий путь
//      для «заспавнить объект в игре» без инжекции managed-кода.
//
//  ВАЖНО ПРО ABI: все сигнатуры ниже выверены по публичным заголовкам Mono
//  (mono/metadata/object.h, class.h, loader.h, threads.h, appdomain.h, image.h).
//  Это единственная причина, по которой вызовы через GetProcAddress безопасны:
//  несогласованное объявление дало бы повреждение стека на x64.
// ============================================================================
#pragma once

#include "Common.h"

#include <functional>
#include <vector>

namespace gwyf::mono {

// ── Непрозрачные типы Mono (нам нужны только как указатели) ─────────────────
using MonoDomain = void;
using MonoAssembly = void;
using MonoImage = void;
using MonoClass = void;
using MonoMethod = void;
using MonoClassField = void;
using MonoObject = void;
using MonoThread = void;
using MonoVTable = void;
using MonoArray = void;
using MonoType = void;
using MonoMethodSignature = void;
using MonoString = void;

/// Описание метода (для команды dump и отладки профиля).
struct MethodInfo {
    void* method = nullptr;      // MonoMethod*
    std::string name;
    std::string returnType;
    u32 paramCount = 0;
    u32 flags = 0;
    u32 iflags = 0;
};

/// Классификация типа аргумента/поля по CIL-типу. Нужна для корректного
/// чтения значений из void** params: float читается как float, а не как int.
enum class ArgKind : u8 {
    Unknown = 0,
    Bool,      ///< 1 байт
    Int8,      ///< 1 байт
    Int16,     ///< 2 байта
    Int32,     ///< 4 байта
    Int64,     ///< 8 байт
    Float32,   ///< R4
    Float64,   ///< R8
    Object,    ///< ссылка (объект, строка, массив) — в params лежит сам указатель
};

/// Описание поля.
struct FieldInfo {
    void* field = nullptr;
    std::string name;
    std::string type;
    i32 offset = -1;
    bool isStatic = false;
};

/// Указатели на функции Mono. Заполняются в Load().
struct Api {
    void* (*get_root_domain)() = nullptr;
    MonoThread* (*thread_attach)(MonoDomain*) = nullptr;
    void (*thread_detach)(MonoThread*) = nullptr;

    MonoImage* (*image_loaded)(const char*) = nullptr;
    const char* (*image_get_name)(MonoImage*) = nullptr;
    void (*assembly_foreach)(void (*func)(void*, void*), void* user_data) = nullptr;
    MonoAssembly* (*domain_assembly_open)(MonoDomain*, const char*) = nullptr;
    MonoImage* (*assembly_get_image)(MonoAssembly*) = nullptr;

    MonoClass* (*class_from_name)(MonoImage*, const char*, const char*) = nullptr;
    MonoClass* (*class_get_parent)(MonoClass*) = nullptr;
    MonoClass* (*class_get_image)(MonoClass*) = nullptr;
    const char* (*class_get_name)(MonoClass*) = nullptr;
    const char* (*class_get_namespace)(MonoClass*) = nullptr;
    MonoVTable* (*class_vtable)(MonoDomain*, MonoClass*) = nullptr;
    u32 (*class_get_type_token)(MonoClass*) = nullptr;
    i32 (*class_instance_size)(MonoClass*) = nullptr;
    MonoClass* (*class_get)(MonoImage*, u32) = nullptr;
    u32 (*image_get_table_rows)(MonoImage*, int) = nullptr;

    MonoMethod* (*class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
    MonoType* (*signature_get_params)(MonoMethodSignature*, void**) = nullptr;
    u32 (*type_get_type)(MonoType*) = nullptr;
    MonoMethod* (*class_get_methods)(MonoClass*, void**) = nullptr;
    MonoClassField* (*class_get_fields)(MonoClass*, void**) = nullptr;
    MonoClassField* (*class_get_field_from_name)(MonoClass*, const char*) = nullptr;

    const char* (*method_get_name)(MonoMethod*) = nullptr;
    MonoMethodSignature* (*method_signature)(MonoMethod*) = nullptr;
    u32 (*method_get_flags)(MonoMethod*, u32*) = nullptr;
    MonoClass* (*method_get_class)(MonoMethod*) = nullptr;
    u32 (*signature_get_param_count)(MonoMethodSignature*) = nullptr;

    const char* (*field_get_name)(MonoClassField*) = nullptr;
    MonoType* (*field_get_type)(MonoClassField*) = nullptr;
    const char* (*type_get_name)(MonoType*) = nullptr;
    u32 (*field_get_offset)(MonoClassField*) = nullptr;

    MonoObject* (*runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
    void* (*compile_method)(MonoMethod*) = nullptr;
    void* (*method_get_unmanaged_thunk)(MonoMethod*) = nullptr;

    void (*field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void (*field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    void (*field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void (*field_static_set_value)(MonoVTable*, MonoClassField*, void*) = nullptr;

    MonoClass* (*object_get_class)(MonoObject*) = nullptr;
    MonoObject* (*object_new)(MonoDomain*, MonoClass*) = nullptr;
    void* (*object_unbox)(MonoObject*) = nullptr;
    MonoString* (*object_to_string)(MonoObject*, MonoObject**) = nullptr;
    char* (*string_to_utf8)(MonoString*) = nullptr;
    void (*free_memory)(void*) = nullptr;

    /// Адрес самой mono_runtime_invoke — универсальная точка перехвата
    /// вызовов managed-кода (в том числе рефлексии и Mirror-сообщений).
    void* runtime_invoke_address = nullptr;
};

/// Загрузить Mono. Пробует mono-2.0-bdwgc.dll (Unity 2019+), mono-2.0.dll, mono.dll.
bool Load(std::string* error = nullptr);

/// Загружен ли модуль (не значит, что runtime уже готов).
bool IsLoaded();

/// Готов ли runtime: получен root domain.
bool RuntimeReady();

/// Имя фактически загруженного модуля (для логов и профиля).
const char* ModuleName();

/// Таблица загруженных функций.
const Api& Fn();

/// Для режима «не Unity»: подменить таблицу функций своей реализацией
/// (используется самотестом, чтобы прогонять тот же код на синтетическом хосте).
void OverrideApiForTesting(const Api& api, const char* moduleName);

/// Домен по умолчанию.
void* RootDomain();

/// Присоединить текущий поток к runtime (нужно, если мы вызываем managed-код
/// из своего потока). Возвращает false, если runtime не готов.
bool AttachCurrentThread();

/// Отсоединить текущий поток.
void DetachCurrentThread();

// ── Поиск сущностей ─────────────────────────────────────────────────────────

/// Найти образ сборки ("Assembly-CSharp"). Если runtime ещё не готов —
/// вернуть nullptr (вызывающий код повторяет попытку).
void* FindImage(const char* assemblyName);

/// Найти класс: пространство имён может быть пустым.
void* FindClass(const char* assemblyName, const char* nameSpace, const char* className);

/// Найти вложенный класс (Outer/Inner) — часто именно так устроены
/// генерируемые Mirror-классы и вложенные менеджеры.
void* FindNestedClass(const char* assemblyName, const char* outerNameSpace, const char* outerClass, const char* nestedClass);

/// Найти метод по имени. paramCount < 0 — подходит любая сигнатура.
void* FindMethod(void* klass, const char* name, int paramCount);

/// Поиск метода с подъёмом по иерархии наследования.
void* FindMethodInHierarchy(void* klass, const char* name, int paramCount);

/// Поиск поля с подъёмом по иерархии.
void* FindField(void* klass, const char* name);

/// JIT-адрес метода: именно его перехватывает MinHook, чтобы ловить события.
void* CompileMethod(void* method);

/// Стабильный thunk для вызова метода напрямую из нативного кода.
void* UnmanagedThunk(void* method);

// ── Вызовы managed-кода ─────────────────────────────────────────────────────

/// Вызвать метод. Возвращает MonoObject* (или nullptr). Если внутри было
/// исключение — текст попадёт в exceptionText.
void* Invoke(void* method, void* instance, void** args, std::string* exceptionText);

/// Вызвать метод без аргументов и с защитой от исключений.
void* Invoke(void* method, void* instance, std::string* exceptionText = nullptr);

/// Создать экземпляр managed-класса.
void* NewObject(void* klass, std::string* exceptionText = nullptr);

/// ── Классификация типов ────────────────────────────────────────────────────

/// Тип CIL (MONO_TYPE_*) → ArgKind. Доступно, если экспорт mono_type_get_type есть.
ArgKind ClassifyType(void* monoType);

/// Разобрать сигнатуру метода и вернуть типы аргументов по порядку.
/// Пустой вектор — сигнатура недоступна (тогда используем shape из профиля).
std::vector<ArgKind> ClassifyArgs(void* method);

/// Прочитать значение из void** params по известному типу аргумента.
/// Для ссылок возвращает указатель, для значений — само значение.
void ExtractArg(void* params, u32 index, ArgKind kind, i64& intOut, double& floatOut, bool& isFloat);

/// Прочитать поле объекта, автоматически определив его тип.
bool ReadFieldTyped(void* instance, void* field, i64& intOut, double& floatOut, bool& isFloat);

/// Прочитать статическое поле, автоматически определив его тип.
bool ReadStaticFieldTyped(void* klass, void* field, i64& intOut, double& floatOut, bool& isFloat);

/// Размер экземпляра класса в байтах (-1, если runtime не отдал функцию).
/// Нужен для проверки ABI структур: перед вызовом set_position(Vector3)
/// мы обязаны убедиться, что Vector3 занимает ровно 12 байт, иначе неверный
/// размер приведёт к чтению чужой памяти внутри runtime.
i32 ClassInstanceSize(void* klass);

/// Строка описания типа аргумента (для отчётов dump).
const char* ArgKindName(ArgKind kind);

// ── Поля ────────────────────────────────────────────────────────────────────

i32 FieldOffset(void* field);

/// Прочитать поле экземпляра по уже найденному MonoClassField.
template <typename T>
bool ReadField(void* instance, void* field, T& out) {
    if (instance == nullptr || field == nullptr || !Fn().field_get_value) return false;
    Fn().field_get_value(instance, field, &out);
    return true;
}

/// Прочитать поле экземпляра по имени (с подъёмом по базовым классам).
template <typename T>
bool ReadFieldByName(void* instance, void* klass, const char* name, T& out) {
    void* field = FindField(klass, name);
    if (field == nullptr) return false;
    return ReadField(instance, field, out);
}

/// Прочитать статическое поле класса.
template <typename T>
bool ReadStaticField(void* klass, void* field, T& out) {
    if (klass == nullptr || field == nullptr || !Fn().field_static_get_value) return false;
    MonoVTable* vtable = Fn().class_vtable(RootDomain(), klass);
    if (vtable == nullptr) return false;
    Fn().field_static_get_value(vtable, field, &out);
    return true;
}

template <typename T>
bool ReadStaticFieldByName(void* klass, const char* name, T& out) {
    void* field = FindField(klass, name);
    if (field == nullptr) return false;
    return ReadStaticField(klass, field, out);
}

// ── Диагностика ─────────────────────────────────────────────────────────────

std::string ClassFullName(void* klass);
std::string MethodSignature(void* method);
std::string FieldSignature(void* field);
std::vector<MethodInfo> EnumMethods(void* klass);
std::vector<FieldInfo> EnumFields(void* klass);
std::string DescribeClass(void* klass);

/// Прямое чтение памяти по адресу (для диагностики и для «сырых» структур).
template <typename T>
T ReadAt(void* address) {
    T value{};
    if (address != nullptr) std::memcpy(&value, address, sizeof(T));
    return value;
}

/// Сохранить отчёт (dump класса/скан) в файл. Используется командами.
bool WriteReport(const std::string& path, const std::string& text);

/// Отчёт по методу: имя, параметры с типами (для команды dump).
std::string DescribeMethodArgs(void* method);

}  // namespace gwyf::mono
