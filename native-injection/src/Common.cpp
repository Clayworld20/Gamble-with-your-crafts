// ============================================================================
//  Common.cpp — логирование, работа с модулями процесса, отображение файлов.
// ============================================================================
#include "gwyfbridge/Common.h"

#include <cstdarg>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if GWYF_WINDOWS
#include <psapi.h>
#include <tlhelp32.h>

#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#endif

namespace gwyf::log {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_level = Level::Info;
std::string g_path;
u32 g_lines = 0;
constexpr u32 kMaxLines = 20000;  // защита от бесконечного роста лога

/// Идентификатор текущего потока для строки лога (только читаемость).
u64 CurrentThreadId() {
#if GWYF_WINDOWS
    return static_cast<u64>(GetCurrentThreadId());
#else
    return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

const char* LevelName(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void Open(const std::string& filename, Level minLevel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = minLevel;
    if (filename.empty()) return;

    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }

    g_file = std::fopen(filename.c_str(), "wt");
    if (g_file == nullptr) {
        return;  // нет доступа к файлу — работаем без лога, молча
    }

    g_path = filename;
    g_lines = 0;
#if GWYF_WINDOWS
    std::fprintf(g_file, "# gwyfbridge log — процесс %lu\n", GetCurrentProcessId());
#else
    std::fprintf(g_file, "# gwyfbridge log — переносимая сборка (самотест)\n");
#endif
    std::fflush(g_file);
}

void Close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fprintf(g_file, "# лог закрыт\n");
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_path.clear();
}

void SetLevel(Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

Level GetLevel() {
    return g_level;
}

void Write(Level level, const char* fmt, ...) {
    if (static_cast<u32>(level) < static_cast<u32>(g_level)) return;

    char message[2048];
    va_list args;
    va_start(args, fmt);
#if defined(_MSC_VER)
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
#else
    std::vsnprintf(message, sizeof(message), fmt, args);
#endif
    va_end(args);

    char line[2304];
    std::snprintf(line, sizeof(line), "[%8llu][tid %5llu][%s] %s\n",
                  static_cast<unsigned long long>(NowTickMs()),
                  static_cast<unsigned long long>(CurrentThreadId()), LevelName(level), message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr && g_lines < kMaxLines) {
        std::fputs(line, g_file);
        std::fflush(g_file);
        ++g_lines;
    }
#if GWYF_WINDOWS
    OutputDebugStringA(line);
#endif
}

}  // namespace gwyf::log

#if GWYF_WINDOWS

namespace gwyf {

// ── Модули процесса ─────────────────────────────────────────────────────────

bool FindModule(const char* nameSubstring, ModuleInfo& out) {
    if (nameSubstring == nullptr || *nameSubstring == '\0') return false;

    std::vector<HMODULE> modules;
    HMODULE buffer[512];
    DWORD needed = 0;

    if (!EnumProcessModules(GetCurrentProcess(), buffer, sizeof(buffer), &needed)) {
        return false;
    }

    const usize count = needed / sizeof(HMODULE);
    for (usize index = 0; index < count && index < 512; ++index) {
        modules.push_back(buffer[index]);
    }

    std::string needle = nameSubstring;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (HMODULE module : modules) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, MAX_PATH) == 0) continue;

        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (lower.find(needle) == std::string::npos) continue;

        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) continue;

        const char* leaf = std::strrchr(path, '\\');
        out.handle = module;
        out.base = info.lpBaseOfDll;
        out.size = info.SizeOfImage;
        out.name = leaf != nullptr ? leaf + 1 : path;
        out.path = path;
        return true;
    }

    return false;
}

void EnumModules(void (*callback)(const ModuleInfo&, void* user), void* user) {
    if (callback == nullptr) return;

    HMODULE buffer[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), buffer, sizeof(buffer), &needed)) return;

    const usize count = needed / sizeof(HMODULE);
    for (usize index = 0; index < count && index < 1024; ++index) {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), buffer[index], &info, sizeof(info))) continue;

        char path[MAX_PATH]{};
        GetModuleFileNameA(buffer[index], path, MAX_PATH);
        const char* leaf = std::strrchr(path, '\\');

        ModuleInfo entry{};
        entry.handle = buffer[index];
        entry.base = info.lpBaseOfDll;
        entry.size = info.SizeOfImage;
        entry.name = leaf != nullptr ? leaf + 1 : path;
        entry.path = path;
        callback(entry, user);
    }
}

void* LoadLibraryChecked(const char* path, std::string* error) {
    HMODULE module = LoadLibraryA(path);
    if (module == nullptr) {
        if (error != nullptr) {
            *error = "LoadLibraryA(" + std::string(path) + ") не удалось, GetLastError=" +
                     std::to_string(GetLastError());
        }
        return nullptr;
    }
    return module;
}

void* GetExport(HMODULE module, const char* name) {
    if (module == nullptr || name == nullptr) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

// ── Отображение файлов в память ─────────────────────────────────────────────

bool MappedFile::Open(const std::wstring& name, usize size, bool createIfMissing) {
    size_ = size;

    if (createIfMissing) {
        mapping_.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                          static_cast<DWORD>(size >> 32),
                                          static_cast<DWORD>(size & 0xFFFFFFFFull),
                                          name.c_str()));
        if (!mapping_.Valid()) return false;
        created_ = GetLastError() != ERROR_ALREADY_EXISTS;
    } else {
        mapping_.Reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str()));
        if (!mapping_.Valid()) return false;
        created_ = false;
    }

    view_ = MapViewOfFile(mapping_.Get(), FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (view_ == nullptr) {
        mapping_.Reset();
        return false;
    }

    return true;
}

}  // namespace gwyf

#endif  // GWYF_WINDOWS
