// =============================================================================
//  GWYF_HookEngine.h — движок перехвата вызовов.
//
//  Что внутри:
//    * обёртка над MinHook (Windows) с реестром хуков и счётчиками вызовов;
//    * сканер сигнатур (AOB) для поиска функций в образах модулей без символов —
//      работает и на Windows (PE-секции), и на Linux (dl_iterate_phdr);
//    * хуки на события казино (ставка / создание стола) с наблюдателями.
//
//  ГДЕ ЭТО ПРИМЕНИМО. Движок перехватывает вызовы внутри СВОЕГО процесса:
//    * своя игра/сервер/агент — есть исходники, есть экспорт, хуки для телеметрии
//      и отладки (профилирование, трейсинг, регрессионные проверки);
//    * бинарник без символов — AOB-сканер находит функцию по байтовой сигнатуре;
//    * ЧУЖАЯ коммерческая игра (в том числе Gamble with Your Friends) — НЕ цель:
//      чтение памяти и внедрение ломают EULA/Steam Subscriber Agreement и приводят
//      к блокировке аккаунта. Наш гибрид подключается к своей игре через честный
//      ABI плагина (см. plugin_abi.h), а к Minecraft — через Fabric API.
//
//  На не-Windows сборке движок работает в «теневом» режиме: хуки регистрируются,
//  но перехват не ставится, а `original` указывает на саму функцию. Логика ядра,
//  сканер и тесты при этом полностью работоспособны — это и проверяет CI.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#   define GWYC_CALL __cdecl
#else
#   define GWYC_CALL
#endif

namespace gwyc::hooks {

/// Результат операции с хуком.
enum class Status {
    Ok = 0,
    AlreadyInitialized,
    NotInitialized,
    NotFound,          ///< хука с таким именем нет в реестре
    TargetNotFound,    ///< не нашли адрес функции (модуль/символ/сигнатура)
    CreateFailed,      ///< MinHook не смог создать трамплин
    EnableFailed,
    DisableFailed,
    RemoveFailed,
    PatternInvalid,
    AlreadyExists,
};

[[nodiscard]] const char* StatusName(Status status) noexcept;

/// Доступен ли реальный перехват (false в теневом режиме вне Windows).
[[nodiscard]] bool IsInterceptionAvailable() noexcept;

[[nodiscard]] Status Initialize() noexcept;
[[nodiscard]] Status Shutdown() noexcept;

/// Поставить хук на конкретный адрес.
[[nodiscard]] Status Create(const char* name, void* target, void* detour, void** original) noexcept;

/// Поставить хук на экспорт функции модуля (например kernel32.dll!Sleep).
[[nodiscard]] Status CreateOnExport(const char* name, const char* moduleName, const char* procedureName, void* detour, void** original) noexcept;

[[nodiscard]] Status Enable(const char* name) noexcept;
[[nodiscard]] Status Disable(const char* name) noexcept;
[[nodiscard]] Status EnableAll() noexcept;
[[nodiscard]] Status DisableAll() noexcept;
[[nodiscard]] Status Remove(const char* name) noexcept;

struct HookInfo {
    std::string name;
    void* target = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
    bool enabled = false;
    uint64_t calls = 0;
};

void ForEach(const std::function<void(const HookInfo&)>& visitor);

/// Отметить вызов детура (статистика). Вызывать в начале детура.
void CountCall(const char* name) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
//  Модули и экспорты
// ─────────────────────────────────────────────────────────────────────────────

struct ModuleRecord {
    std::string name;      ///< имя файла модуля ("Game.exe", "libnative.so")
    std::string path;      ///< полный путь, если ОС его отдаёт
    uintptr_t base = 0;    ///< адрес загрузки
    size_t size = 0;       ///< размер образа
    bool executable = false;  ///< главный модуль процесса
};

/// Список загруженных модулей: Windows — Toolhelp32, Linux — dl_iterate_phdr.
[[nodiscard]] std::vector<ModuleRecord> EnumerateModules() noexcept;

/// Найти модуль по подстроке имени/пути (без учёта регистра).
[[nodiscard]] bool FindModule(const char* nameSubstring, ModuleRecord& out) noexcept;

/// Адрес экспорта модуля: GetProcAddress / dlsym.
[[nodiscard]] void* ResolveExport(const char* moduleName, const char* exportName) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
//  Сканер сигнатур: "48 8B ?? E8 * *" ('?' и '*' — подстановочные байты).
// ─────────────────────────────────────────────────────────────────────────────

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;      ///< true = байт обязателен для совпадения
    bool valid = false;
    std::string error;
};

[[nodiscard]] Pattern ParsePattern(const char* idaPattern) noexcept;

/// Первое совпадение в буфере (nullptr — не найдено).
[[nodiscard]] const uint8_t* FindPatternIn(const void* begin, size_t size, const Pattern& pattern) noexcept;

/// Все совпадения в буфере, не больше maxMatches.
[[nodiscard]] std::vector<const uint8_t*> FindAllPatternsIn(const void* begin, size_t size, const Pattern& pattern, size_t maxMatches) noexcept;

/// Адрес по паттерну внутри модуля. `skipMatches` — взять N-е совпадение.
/// Сканируются только исполняемые секции (".text", PT_LOAD с X-флагом).
[[nodiscard]] void* FindPatternInModule(const char* moduleNameSubstring, const char* idaPattern, size_t skipMatches = 0) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
//  События казино
// ─────────────────────────────────────────────────────────────────────────────

/// Наблюдатель события «ставка отправлена». betContext — структура ставки движка
/// (её раскладку знает владелец хука: он и разбирает поля).
using BetObserverFn = void(GWYC_CALL*)(void* betContext);

/// Наблюдатель события «стол создан/найден».
using TableObserverFn = void(GWYC_CALL*)(const char* tableName, int32_t seats);

/// Зарегистрировать наблюдателей: их вызовет стандартный детур.
void SetBetObserver(BetObserverFn observer) noexcept;
void SetTableObserver(TableObserverFn observer) noexcept;

/// Сигнатуры обработчиков, на которые ставятся стандартные детуры.
using BetSubmitFn = int32_t(GWYC_CALL*)(void* betContext);
using TableCreateFn = void*(GWYC_CALL*)(const char* tableName, int32_t seats);

/// Поставить хуки на события казино.
///
/// `betSubmitTarget` / `tableCreateTarget` — адреса функций СВОЕЙ игры (по экспорту
/// или найденные сканером). Если передать свои детуры — используются они; иначе
/// работает стандартный детур, который зовёт наблюдателей (SetBetObserver /
/// SetTableObserver) и всё равно вызывает оригинал.
[[nodiscard]] Status InstallCasinoHooks(void* betSubmitTarget, BetSubmitFn betDetour, BetSubmitFn* betOriginal,
                                        void* tableCreateTarget, TableCreateFn tableDetour, TableCreateFn* tableCreateOriginal) noexcept;

[[nodiscard]] Status RemoveCasinoHooks() noexcept;

struct CasinoHookCounters {
    uint64_t betsObserved = 0;
    uint64_t tablesObserved = 0;
};

[[nodiscard]] CasinoHookCounters GetCasinoCounters() noexcept;

} // namespace gwyc::hooks
