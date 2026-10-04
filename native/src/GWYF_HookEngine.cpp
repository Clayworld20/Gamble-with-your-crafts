// =============================================================================
//  GWYF_HookEngine.cpp — реализация движка перехвата.
//  Windows: MinHook. Остальные платформы: теневой режим (см. заголовок).
// =============================================================================
#include "GWYF_HookEngine.h"

#include <atomic>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#   include <MinHook.h>
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   include <tlhelp32.h>
#   pragma comment(lib, "psapi.lib")
#else
#   include <dlfcn.h>
#   include <link.h>
#endif

namespace gwyc::hooks {
namespace {

struct HookEntry {
    std::string name;
    void* target = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
    bool enabled = false;
    bool created = false;
};

std::mutex g_registryMutex;
std::vector<HookEntry> g_registry;
bool g_initialized = false;

/// Счётчики вызовов: фиксированная таблица без блокировок — детур может выполняться
/// в самом горячем коде, поэтому там недопустимы ни мьютексы, ни аллокации.
constexpr size_t kMaxCounters = 32;
struct Counter {
    std::atomic<uint64_t> calls{0};
    char name[40]{};
};
Counter g_counters[kMaxCounters];

int FindCounterSlot(const char* name) noexcept
{
    if (name == nullptr) return -1;
    for (int i = 0; i < static_cast<int>(kMaxCounters); ++i) {
        const char* slot = g_counters[i].name;
        if (slot[0] != '\0' && std::strncmp(slot, name, sizeof(Counter::name) - 1) == 0) return i;
    }
    return -1;
}

int ClaimCounterSlot(const char* name) noexcept
{
    for (int i = 0; i < static_cast<int>(kMaxCounters); ++i) {
        if (g_counters[i].name[0] != '\0') continue;
        std::strncpy(g_counters[i].name, name, sizeof(Counter::name) - 1);
        g_counters[i].name[sizeof(Counter::name) - 1] = '\0';
        return i;
    }
    return -1;
}

HookEntry* FindEntry(const char* name) noexcept
{
    if (name == nullptr) return nullptr;
    for (auto& entry : g_registry) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

#if defined(_WIN32)
Status FromMh(MH_STATUS status) noexcept
{
    switch (status) {
        case MH_OK:               return Status::Ok;
        case MH_ERROR_ALREADY_INITIALIZED: return Status::AlreadyInitialized;
        case MH_ERROR_NOT_INITIALIZED:     return Status::NotInitialized;
        case MH_ERROR_NOT_CREATED:         return Status::NotFound;
        case MH_ERROR_ALREADY_CREATED:     return Status::AlreadyExists;
        case MH_ERROR_ENABLED:             return Status::Ok;
        case MH_ERROR_DISABLED:            return Status::Ok;
        case MH_ERROR_NOT_EXECUTABLE:      return Status::TargetNotFound;
        case MH_ERROR_UNSUPPORTED_FUNCTION:return Status::CreateFailed;
        case MH_ERROR_MEMORY_ALLOC:        return Status::CreateFailed;
        case MH_ERROR_MEMORY_PROTECT:      return Status::CreateFailed;
        case MH_ERROR_MODULE_NOT_FOUND:    return Status::TargetNotFound;
        case MH_ERROR_FUNCTION_NOT_FOUND:  return Status::TargetNotFound;
        default:                           return Status::CreateFailed;
    }
}

/// Привести страницу памяти в изменяемое состояние. MinHook делает это сам, но
/// для ручных правок (например, патча байтов) нужен отдельный помощник.
[[maybe_unused]] bool MakeWritable(void* address, size_t size) noexcept
{
    DWORD oldProtect = 0;
    return VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;
}
#endif

} // namespace

const char* StatusName(Status status) noexcept
{
    switch (status) {
        case Status::Ok:                 return "ok";
        case Status::AlreadyInitialized: return "движок уже инициализирован";
        case Status::NotInitialized:     return "движок не инициализирован";
        case Status::NotFound:           return "хук не найден в реестре";
        case Status::TargetNotFound:     return "адрес функции не найден";
        case Status::CreateFailed:       return "не удалось создать хук (нет трамплина)";
        case Status::EnableFailed:       return "не удалось включить хук";
        case Status::DisableFailed:      return "не удалось выключить хук";
        case Status::RemoveFailed:       return "не удалось снять хук";
        case Status::PatternInvalid:     return "паттерн сигнатуры некорректен";
        case Status::AlreadyExists:      return "хук с таким именем уже есть";
    }
    return "неизвестная ошибка";
}

bool IsInterceptionAvailable() noexcept
{
#if defined(_WIN32)
    return true;
#else
    // На Linux/macOS трамплины MinHook недоступны: движок работает в теневом режиме,
    // логика бриджа и тесты при этом полностью работоспособны.
    return false;
#endif
}

Status Initialize() noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (g_initialized) return Status::AlreadyInitialized;

#if defined(_WIN32)
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return FromMh(status);
#endif

    g_initialized = true;
    return Status::Ok;
}

Status Shutdown() noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

#if defined(_WIN32)
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
#endif

    g_registry.clear();
    g_initialized = false;
    return Status::Ok;
}

Status Create(const char* name, void* target, void* detour, void** original) noexcept
{
    if (name == nullptr || target == nullptr || detour == nullptr) return Status::TargetNotFound;

    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;
    if (FindEntry(name) != nullptr) return Status::AlreadyExists;

    HookEntry entry;
    entry.name = name;
    entry.target = target;
    entry.detour = detour;

#if defined(_WIN32)
    void* trampoline = nullptr;
    const MH_STATUS status = MH_CreateHook(target, detour, &trampoline);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) return FromMh(status);
    entry.original = trampoline;
    entry.created = true;
#else
    // Теневой режим: оригинал — это сама функция. Хук не установлен, но реестр,
    // статистика и логика обработчиков работают идентично Windows-сборке.
    entry.original = target;
    entry.created = true;
#endif

    if (original != nullptr) *original = entry.original;

    if (entry.name.size() < sizeof(Counter::name))
        ClaimCounterSlot(entry.name.c_str());

    g_registry.push_back(std::move(entry));
    return Status::Ok;
}

Status CreateOnExport(const char* name, const char* moduleName, const char* procedureName, void* detour, void** original) noexcept
{
    void* target = ResolveExport(moduleName, procedureName);
    if (target == nullptr) return Status::TargetNotFound;
    return Create(name, target, detour, original);
}

Status Enable(const char* name) noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

    HookEntry* entry = FindEntry(name);
    if (entry == nullptr) return Status::NotFound;

#if defined(_WIN32)
    const MH_STATUS status = MH_EnableHook(entry->target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) return FromMh(status);
#endif

    entry->enabled = true;
    return Status::Ok;
}

Status Disable(const char* name) noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

    HookEntry* entry = FindEntry(name);
    if (entry == nullptr) return Status::NotFound;

#if defined(_WIN32)
    const MH_STATUS status = MH_DisableHook(entry->target);
    if (status != MH_OK && status != MH_ERROR_DISABLED) return FromMh(status);
#endif

    entry->enabled = false;
    return Status::Ok;
}

Status EnableAll() noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

#if defined(_WIN32)
    const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) return FromMh(status);
#endif

    for (auto& entry : g_registry) entry.enabled = true;
    return Status::Ok;
}

Status DisableAll() noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

#if defined(_WIN32)
    MH_DisableHook(MH_ALL_HOOKS);
#endif

    for (auto& entry : g_registry) entry.enabled = false;
    return Status::Ok;
}

Status Remove(const char* name) noexcept
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!g_initialized) return Status::NotInitialized;

    for (auto it = g_registry.begin(); it != g_registry.end(); ++it) {
        if (it->name != name) continue;

#if defined(_WIN32)
        MH_DisableHook(it->target);
        const MH_STATUS status = MH_RemoveHook(it->target);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) return FromMh(status);
#endif

        g_registry.erase(it);
        return Status::Ok;
    }

    return Status::NotFound;
}

void ForEach(const std::function<void(const HookInfo&)>& visitor)
{
    if (!visitor) return;

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const auto& entry : g_registry) {
        HookInfo info;
        info.name = entry.name;
        info.target = entry.target;
        info.detour = entry.detour;
        info.original = entry.original;
        info.enabled = entry.enabled;

        const int slot = FindCounterSlot(entry.name.c_str());
        info.calls = (slot >= 0) ? g_counters[slot].calls.load(std::memory_order_relaxed) : 0;
        visitor(info);
    }
}

void CountCall(const char* name) noexcept
{
    const int slot = FindCounterSlot(name);
    if (slot >= 0) g_counters[slot].calls.fetch_add(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Сканер сигнатур
// ─────────────────────────────────────────────────────────────────────────────

namespace {

int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Pattern ParsePattern(const char* idaPattern) noexcept
{
    Pattern pattern;
    if (idaPattern == nullptr) {
        pattern.error = "пустая строка паттерна";
        return pattern;
    }

    const char* cursor = idaPattern;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0') break;

        if (*cursor == '?' || *cursor == '*') {
            // '?' и '??' и '*' — подстановочный байт.
            while (*cursor == '?' || *cursor == '*') ++cursor;
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            continue;
        }

        const int high = HexValue(cursor[0]);
        const int low = HexValue(cursor[1]);
        if (high < 0 || low < 0) {
            pattern.error = "в паттерне ожидались hex-байты вроде \"48 8B ?? E8\"";
            return pattern;
        }

        pattern.bytes.push_back(static_cast<uint8_t>((high << 4) | low));
        pattern.mask.push_back(true);
        cursor += 2;
    }

    if (pattern.bytes.empty()) {
        pattern.error = "паттерн не содержит ни одного байта";
        return pattern;
    }

    if (pattern.bytes.size() > 4096) {
        pattern.error = "паттерн слишком длинный (максимум 4096 байт)";
        return pattern;
    }

    pattern.valid = true;
    return pattern;
}

const uint8_t* FindPatternIn(const void* begin, size_t size, const Pattern& pattern) noexcept
{
    if (!pattern.valid || begin == nullptr) return nullptr;

    const auto* bytes = static_cast<const uint8_t*>(begin);
    const size_t needle = pattern.bytes.size();
    if (size < needle) return nullptr;

    const uint8_t firstByte = pattern.bytes[0];
    const bool firstFixed = pattern.mask[0];
    const size_t limit = size - needle;

    size_t offset = 0;
    while (offset <= limit) {
        if (firstFixed) {
            // Первый байт известен — прыгаем по нему, а не проверяем каждый офсет:
            // на больших образах (десятки МБ) это даёт ускорение на порядок.
            const void* hit = std::memchr(bytes + offset, firstByte, limit - offset + 1);
            if (hit == nullptr) return nullptr;
            offset = static_cast<size_t>(static_cast<const uint8_t*>(hit) - bytes);
        }

        bool matched = true;
        for (size_t i = 1; i < needle; ++i) {
            if (!pattern.mask[i]) continue;
            if (bytes[offset + i] != pattern.bytes[i]) { matched = false; break; }
        }

        if (matched) return bytes + offset;
        ++offset;
    }

    return nullptr;
}

std::vector<const uint8_t*> FindAllPatternsIn(const void* begin, size_t size, const Pattern& pattern, size_t maxMatches) noexcept
{
    std::vector<const uint8_t*> results;
    if (!pattern.valid || begin == nullptr || maxMatches == 0) return results;

    const auto* bytes = static_cast<const uint8_t*>(begin);
    size_t scanned = 0;

    while (scanned < size && results.size() < maxMatches) {
        const uint8_t* hit = FindPatternIn(bytes + scanned, size - scanned, pattern);
        if (hit == nullptr) break;

        results.push_back(hit);
        scanned = static_cast<size_t>(hit - bytes) + 1;
    }

    return results;
}

// ── Модули процесса ──────────────────────────────────────────────────────────

namespace {

std::string ToLowerAscii(const char* text)
{
    std::string result = (text != nullptr) ? text : "";
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return result;
}

/// Раскладка модуля: от чего отсчитывать смещения секций и какие секции сканировать.
struct ModuleLayout {
    uint8_t* base = nullptr;    ///< nullptr означает «vaddr абсолютны» (статический образ)
    size_t imageSize = 0;
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> sections;
};

#if defined(_WIN32)

bool DescribeModule(uintptr_t baseAddress, ModuleLayout& out) noexcept
{
    if (baseAddress == 0) return false;

    // Берём HMODULE по адресу внутри модуля: так работает и для главного, и для
    // загруженных DLL, чего не даёт GetModuleHandle(nullptr).
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(baseAddress), &module) == FALSE) {
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    out.base = base;
    out.imageSize = nt->OptionalHeader.SizeOfImage;
    out.sections.clear();

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9]{};
        std::memcpy(name, section->Name, 8);

        const size_t start = section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        if (start + size > out.imageSize) size = (out.imageSize > start) ? (out.imageSize - start) : 0;
        out.sections.emplace_back(name, std::make_pair(start, size));
    }

    return true;
}

bool SectionIsExecutable(const char* name) noexcept
{
    if (name == nullptr || name[0] == '\0') return false;
    if (std::strncmp(name, ".text", 5) == 0) return true;
    if (std::strncmp(name, "CODE", 4) == 0) return true;
    if (std::strncmp(name, ".code", 5) == 0) return true;
    return false;
}

#else

struct ElfCollect {
    uintptr_t loadBias = 0;
    ModuleLayout* layout = nullptr;
    bool found = false;
};

bool DescribeModule(uintptr_t loadBias, ModuleLayout& out) noexcept
{
    out.base = (loadBias == 0) ? nullptr : reinterpret_cast<uint8_t*>(loadBias);
    out.imageSize = 0;
    out.sections.clear();

    ElfCollect collect;
    collect.loadBias = loadBias;
    collect.layout = &out;

    dl_iterate_phdr([](struct dl_phdr_info* entry, size_t, void* data) -> int {
        auto* self = static_cast<ElfCollect*>(data);
        if (static_cast<uintptr_t>(entry->dlpi_addr) != self->loadBias) return 0;

        for (int i = 0; i < entry->dlpi_phnum; ++i) {
            const ElfW(Phdr)& phdr = entry->dlpi_phdr[i];
            if (phdr.p_type != PT_LOAD) continue;

            const bool executable = (phdr.p_flags & PF_X) != 0;
            char name[24]{};
            std::snprintf(name, sizeof(name), "%s%02d", executable ? ".text.seg" : ".data.seg", i);
            self->layout->sections.emplace_back(name, std::make_pair(static_cast<size_t>(phdr.p_vaddr),
                                                                    static_cast<size_t>(phdr.p_memsz)));

            const size_t end = static_cast<size_t>(phdr.p_vaddr + phdr.p_memsz);
            if (end > self->layout->imageSize) self->layout->imageSize = end;
        }

        self->found = true;
        return 1;   // нужный модуль найден — обход прекращаем
    }, &collect);

    return collect.found && out.imageSize != 0;
}

bool SectionIsExecutable(const char* name) noexcept
{
    if (name == nullptr) return false;
    return std::strncmp(name, ".text", 5) == 0;
}

#endif

} // namespace

std::vector<ModuleRecord> EnumerateModules() noexcept
{
    std::vector<ModuleRecord> modules;

#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return modules;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            ModuleRecord record;
            char nameUtf8[260]{};
            WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
            record.name = nameUtf8;

            char pathUtf8[1024]{};
            WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, pathUtf8, sizeof(pathUtf8), nullptr, nullptr);
            record.path = pathUtf8;

            record.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            record.size = entry.modBaseSize;
            record.executable = modules.empty();
            modules.push_back(std::move(record));
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
#else
    dl_iterate_phdr([](struct dl_phdr_info* entry, size_t, void* data) -> int {
        auto* modules = static_cast<std::vector<ModuleRecord>*>(data);

        ModuleRecord record;
        record.path = (entry->dlpi_name != nullptr && entry->dlpi_name[0] != '\0') ? entry->dlpi_name : "";
        const size_t slash = record.path.find_last_of('/');
        record.name = (slash == std::string::npos) ? record.path : record.path.substr(slash + 1);
        if (record.name.empty()) record.name = "(main)";
        record.base = static_cast<uintptr_t>(entry->dlpi_addr);
        record.executable = modules->empty();

        size_t size = 0;
        for (int i = 0; i < entry->dlpi_phnum; ++i) {
            const ElfW(Phdr)& phdr = entry->dlpi_phdr[i];
            if (phdr.p_type == PT_LOAD && phdr.p_vaddr + phdr.p_memsz > size) {
                size = static_cast<size_t>(phdr.p_vaddr + phdr.p_memsz);
            }
        }
        record.size = size;

        modules->push_back(std::move(record));
        return 0;
    }, &modules);
#endif

    return modules;
}

bool FindModule(const char* nameSubstring, ModuleRecord& out) noexcept
{
    if (nameSubstring == nullptr || nameSubstring[0] == '\0') return false;

    const std::string needle = ToLowerAscii(nameSubstring);
    for (const ModuleRecord& record : EnumerateModules()) {
        const std::string haystack = ToLowerAscii(record.name.c_str()) + "|" + ToLowerAscii(record.path.c_str());
        if (haystack.find(needle) == std::string::npos) continue;
        out = record;
        return true;
    }

    return false;
}

void* ResolveExport(const char* moduleName, const char* exportName) noexcept
{
    if (moduleName == nullptr || exportName == nullptr) return nullptr;

#if defined(_WIN32)
    HMODULE module = GetModuleHandleA(moduleName);
    if (module == nullptr) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, exportName));
#else
    void* handle = dlopen(moduleName, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) handle = dlopen(moduleName, RTLD_NOW);
    if (handle == nullptr) return nullptr;

    void* symbol = dlsym(handle, exportName);
    dlclose(handle);
    return symbol;
#endif
}

void* FindPatternInModule(const char* moduleNameSubstring, const char* idaPattern, size_t skipMatches) noexcept
{
    ModuleRecord record;
    if (!FindModule(moduleNameSubstring, record)) return nullptr;

    const Pattern pattern = ParsePattern(idaPattern);
    if (!pattern.valid) return nullptr;

    ModuleLayout layout;
    if (!DescribeModule(record.base, layout)) return nullptr;

    // Сканируем только исполняемые секции: данные и ресурсы смысла не имеют.
    for (const auto& [name, range] : layout.sections) {
        if (!SectionIsExecutable(name.c_str())) continue;

        const size_t sectionSize = range.second;
        if (sectionSize == 0) continue;

        const uint8_t* sectionStart = reinterpret_cast<const uint8_t*>(range.first);
        if (layout.base != nullptr) sectionStart = layout.base + range.first;
        if (sectionStart == nullptr) continue;

        // Не выходим за границы образа.
        size_t scanSize = sectionSize;
        if (layout.base != nullptr && layout.imageSize != 0) {
            const size_t offset = range.first;
            if (offset >= layout.imageSize) continue;
            if (offset + scanSize > layout.imageSize) scanSize = layout.imageSize - offset;
        }

        const std::vector<const uint8_t*> hits = FindAllPatternsIn(sectionStart, scanSize, pattern, skipMatches + 1);
        if (hits.size() > skipMatches) return const_cast<uint8_t*>(hits[skipMatches]);
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Хуки на события казино
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr const char* kHookBetSubmit = "casino.bet.submit";
constexpr const char* kHookTableCreate = "casino.table.create";

BetSubmitFn g_betOriginal = nullptr;
TableCreateFn g_tableOriginal = nullptr;
BetObserverFn g_betObserver = nullptr;
TableObserverFn g_tableObserver = nullptr;

int32_t GWYC_CALL BetSubmitDetour(void* betContext)
{
    CountCall(kHookBetSubmit);

    // Наблюдатель — код владельца процесса: он знает раскладку структуры ставки
    // (своей игры) и превращает её в сообщение моста. Оригинал вызывается всегда,
    // поэтому поведение игры не меняется — хук работает как трейсер.
    if (g_betObserver != nullptr) g_betObserver(betContext);
    if (g_betOriginal != nullptr) return g_betOriginal(betContext);
    return 0;
}

void* GWYC_CALL TableCreateDetour(const char* tableName, int32_t seats)
{
    CountCall(kHookTableCreate);

    if (g_tableObserver != nullptr) g_tableObserver(tableName, seats);
    if (g_tableOriginal != nullptr) return g_tableOriginal(tableName, seats);
    return nullptr;
}

} // namespace

void SetBetObserver(BetObserverFn observer) noexcept
{
    g_betObserver = observer;
}

void SetTableObserver(TableObserverFn observer) noexcept
{
    g_tableObserver = observer;
}

Status InstallCasinoHooks(void* betSubmitTarget, BetSubmitFn betDetour, BetSubmitFn* betOriginal,
                          void* tableCreateTarget, TableCreateFn tableDetour, TableCreateFn* tableCreateOriginal) noexcept
{
    if (betSubmitTarget != nullptr) {
        void* original = nullptr;
        void* detour = (betDetour != nullptr) ? reinterpret_cast<void*>(betDetour) : reinterpret_cast<void*>(&BetSubmitDetour);

        const Status status = Create(kHookBetSubmit, betSubmitTarget, detour, &original);
        if (status != Status::Ok) return status;

        if (betOriginal != nullptr) *betOriginal = reinterpret_cast<BetSubmitFn>(original);
        g_betOriginal = reinterpret_cast<BetSubmitFn>(original);

        const Status enableStatus = Enable(kHookBetSubmit);
        if (enableStatus != Status::Ok) return enableStatus;
    }

    if (tableCreateTarget != nullptr) {
        void* original = nullptr;
        void* detour = (tableDetour != nullptr) ? reinterpret_cast<void*>(tableDetour) : reinterpret_cast<void*>(&TableCreateDetour);

        const Status status = Create(kHookTableCreate, tableCreateTarget, detour, &original);
        if (status != Status::Ok) return status;

        if (tableCreateOriginal != nullptr) *tableCreateOriginal = reinterpret_cast<TableCreateFn>(original);
        g_tableOriginal = reinterpret_cast<TableCreateFn>(original);

        const Status enableStatus = Enable(kHookTableCreate);
        if (enableStatus != Status::Ok) return enableStatus;
    }

    return Status::Ok;
}

Status RemoveCasinoHooks() noexcept
{
    g_betOriginal = nullptr;
    g_tableOriginal = nullptr;
    g_betObserver = nullptr;
    g_tableObserver = nullptr;

    const Status first = Remove(kHookBetSubmit);
    const Status second = Remove(kHookTableCreate);

    if (first == Status::Ok || second == Status::Ok) return Status::Ok;
    return first;
}

CasinoHookCounters GetCasinoCounters() noexcept
{
    CasinoHookCounters counters;
    ForEach([&counters](const HookInfo& info) {
        if (info.name == kHookBetSubmit) counters.betsObserved = info.calls;
        if (info.name == kHookTableCreate) counters.tablesObserved = info.calls;
    });
    return counters;
}

} // namespace gwyc::hooks
