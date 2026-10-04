// ============================================================================
//  Common.h — WinAPI-специфика модуля gwyfbridge.
//
//  Модуль состоит из трёх нативных компонентов:
//    * GWYF_HookEngine.dll    — живёт внутри процесса игры Gamble With Your Friends;
//    * gwyf_native_bridge.dll — загружается JVM Minecraft через Fabric-мод;
//    * gwyfbridge.exe         — инжектор, диагностика, самотест.
//
//  Общие правила кода:
//    * никаких исключений через границу DLL — только коды возврата и лог;
//    * в хуках (то есть на игровом потоке) запрещены блокировки, аллокации
//      и любые операции, способные занять больше микросекунд.
//
//  Всё, что не зависит от Windows (типы, время, логи, барьеры памяти, события),
//  объявлено в Base.h: благодаря этому протокол и очередь вокселей собираются
//  и прогоняются самотестом на любой платформе.
// ============================================================================
#pragma once

#include "Base.h"

#if GWYF_WINDOWS

namespace gwyf {

// ── Точное время ────────────────────────────────────────────────────────────

/// Отсчёт времени: QueryPerformanceCounter (микросекундная точность).
inline i64 ElapsedUs(LARGE_INTEGER start, LARGE_INTEGER end) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0;
    return (end.QuadPart - start.QuadPart) * 1000000ll / freq.QuadPart;
}

inline LARGE_INTEGER QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value;
}

/// Сообщение в отладчик (полезно, когда лог ещё не открыт).
inline void DebugPrint(const char* text) {
    ::OutputDebugStringA(text);
}

// ── RAII для WinAPI ─────────────────────────────────────────────────────────

/// Владеющая обёртка для HANDLE (события, отображения памяти, процессы).
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~UniqueHandle() { Reset(); }

    void Reset() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

    void Reset(HANDLE h) noexcept {
        Reset();
        handle_ = h;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE* Put() noexcept {
        Reset();
        return &handle_;
    }

    [[nodiscard]] HANDLE Release() noexcept {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

private:
    HANDLE handle_ = nullptr;
};

/// Отображение файла в память — наш транспорт между процессами.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() {
        if (view_ != nullptr) UnmapViewOfFile(view_);
    }

    /// Открыть существующий сегмент либо создать новый нужного размера.
    /// Размер должен совпадать у обеих сторон, поэтому он зашит в ABI-заголовок
    /// (см. IpcProtocol.h: kRegionSize).
    bool Open(const std::wstring& name, usize size, bool createIfMissing);

    [[nodiscard]] void* Data() const noexcept { return view_; }
    [[nodiscard]] bool Valid() const noexcept { return view_ != nullptr; }
    [[nodiscard]] usize Size() const noexcept { return size_; }

    /// Регион создали именно мы? (Нужно, чтобы понять, кто инициализирует данные.)
    [[nodiscard]] bool Created() const noexcept { return created_; }

private:
    UniqueHandle mapping_;
    void* view_ = nullptr;
    usize size_ = 0;
    bool created_ = false;
};

// ── Модули процесса ─────────────────────────────────────────────────────────

struct ModuleInfo {
    HMODULE handle = nullptr;
    void* base = nullptr;
    usize size = 0;
    std::string name;
    std::string path;
};

/// Найти загруженный модуль по подстроке имени (без учёта регистра).
bool FindModule(const char* nameSubstring, ModuleInfo& out);

/// Перечислить все модули процесса (диагностика, поиск паттернов).
void EnumModules(void (*callback)(const ModuleInfo&, void* user), void* user);

/// Загрузить библиотеку и вернуть её базовый адрес (или nullptr).
void* LoadLibraryChecked(const char* path, std::string* error = nullptr);

/// Получить адрес экспорта из модуля.
void* GetExport(HMODULE module, const char* name);

}  // namespace gwyf

#endif  // GWYF_WINDOWS
