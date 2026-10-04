// ============================================================================
//  Base.h — переносимая основа: типы, время, логирование, барьеры памяти.
//
//  Разделение на Base.h (переносимое) и Common.h (Windows-специфика) сделано
//  сознательно: благодаря этому протокол, профиль, зеркало вокселей и кольца
//  можно тестировать на любой платформе (самотест в CI), а привязка к WinAPI
//  остаётся в одном месте.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <ctime>

#if defined(_WIN32)
#define GWYF_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#define GWYF_WINDOWS 0
#include <time.h>
#endif

namespace gwyf {

/// Нижний регистр для ASCII (имена модулей, классов, ключи профиля).
///
/// Своя реализация, а не std::transform + std::tolower: та тянет <algorithm> и
/// <cctype>, зависит от локали, а в MSVC на сужающем преобразовании ещё и выдаёт
/// предупреждение внутри заголовков стандартной библиотеки — при /WX это ошибка
/// сборки (проверено на CI). Здесь поведение одинаково на всех платформах.
[[nodiscard]] inline std::string ToLowerAscii(std::string_view text)
{
    std::string result(text);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

/// Узкая строка → широкая.
///
/// Имена объектов ядра и пути в WinAPI — UTF-16, поэтому конвертируем осознанно.
/// Раньше на этих местах стоял range-конструктор std::wstring(char*, char*):
/// он делает то же самое, но MSVC на /W4 помечает такое присваивание C4244
/// в заголовке стандартной библиотеки (xstring/xutility), а с /WX это отказ
/// сборки — проверено на CI. Заодно снимаем зависимость от локали.
[[nodiscard]] inline std::wstring ToWideUtf8(std::string_view text)
{
    std::wstring result;
    if (text.empty()) return result;

#if GWYF_WINDOWS
    // 0x7FFFFFFF — предел параметра cchWideChar; для имён регионов он недостижим,
    // но проверка есть, чтобы не полагаться на удачу.
    if (text.size() <= static_cast<std::size_t>(0x7FFFFFFF)) {
        const int sourceLength = static_cast<int>(text.size());
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0);
        if (written > 0) {
            result.resize(static_cast<std::size_t>(written));
            ::MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, result.data(), written);
            return result;
        }
        // Поток не является корректным UTF-8 (например, файл профиля в CP1251).
        // Тогда идём ниже и расширяем побайтово — без исключений и потерь.
    }
#endif

    result.reserve(text.size());
    for (const char ch : text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return result;
}

/// Широкая строка → узкая (UTF-8).
///
/// Нужна там, где wide-имя попадает в отчёт, лог или текстовый ключ. Явное
/// преобразование обязательно ещё и потому, что неявное конструирование
/// std::string из wide-итераторов — это C4244 внутри заголовков STL, а с /WX —
/// ошибка сборки. На не-Windows платформах берём младший байт: для ASCII
/// результат идентичен, а round-trip с ToWideUtf8 сохраняется.
[[nodiscard]] inline std::string ToNarrowUtf8(std::wstring_view text)
{
    std::string result;
    if (text.empty()) return result;

#if GWYF_WINDOWS
    if (text.size() <= static_cast<std::size_t>(0x7FFFFFFF)) {
        const int sourceLength = static_cast<int>(text.size());
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);
        if (written > 0) {
            result.resize(static_cast<std::size_t>(written));
            ::WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, result.data(), written, nullptr, nullptr);
            return result;
        }
    }
#endif

    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(static_cast<char>(static_cast<unsigned int>(ch) & 0xFFu));
    }
    return result;
}

// ── Базовые типы ────────────────────────────────────────────────────────────

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

/// Барьер компилятора: запрещает перестановку инструкций на уровне оптимизатора.
#if defined(_MSC_VER)
#define GWYF_COMPILER_BARRIER() _ReadWriteBarrier()
#else
#define GWYF_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

inline u64 NowUnixMs() {
#if GWYF_WINDOWS
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li{};
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return li.QuadPart / 10000ull - 11644473600000ull;
#else
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000ull + static_cast<u64>(ts.tv_nsec / 1000000);
#endif
}

inline u64 NowTickMs() {
#if GWYF_WINDOWS
    return GetTickCount64();
#else
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000ull + static_cast<u64>(ts.tv_nsec / 1000000);
#endif
}

/// Короткий сон без завязки на WinAPI.
inline void SleepMs(u32 milliseconds) {
#if GWYF_WINDOWS
    Sleep(milliseconds);
#else
    struct timespec ts{};
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

// ── Объекты синхронизации (переносимая обёртка) ─────────────────────────────
//
//  На Windows — это HANDLE события (SetEvent/WaitForSingleObject). На других
//  платформах объект отсутствует: потребитель просто ждёт таймаут. Такой
//  вариант используется самотестом, который прогоняет всю логику очередей
//  внутри одного процесса и проверяет, что ABI и кольца не разъезжаются.

#if GWYF_WINDOWS
using WakeHandle = HANDLE;
#else
using WakeHandle = void*;
#endif

/// Разбудить потребителя (если на платформе есть события).
inline void SignalWake(WakeHandle handle) {
#if GWYF_WINDOWS
    if (handle != nullptr) {
        SetEvent(handle);
    }
#else
    (void)handle;
#endif
}

/// Дождаться пробуждения. false = таймаут.
inline bool WaitOnHandle(WakeHandle handle, u32 timeoutMs) {
#if GWYF_WINDOWS
    if (handle == nullptr) {
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(handle, timeoutMs) == WAIT_OBJECT_0;
#else
    (void)handle;
    SleepMs(timeoutMs);
    return false;
#endif
}

// ── Атомарные операции над «сырыми» слотами кольцевых буферов ───────────────
//
//  Слоты колец лежат в разделяемой памяти, поэтому std::atomic применять
//  нельзя (нет гарантии, что партнёр использует ту же реализацию). Работаем
//  через volatile + барьеры, что корректно для x86-64 (TSO) и явно описано
//  в комментариях IpcProtocol.h.

inline u64 AtomicLoadAcquire(const volatile u64* slot) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 value = *slot;
    GWYF_COMPILER_BARRIER();
    return value;
}

inline void AtomicStoreRelease(volatile u64* slot, u64 value) {
    GWYF_COMPILER_BARRIER();
    *slot = value;
    std::atomic_thread_fence(std::memory_order_release);
}

}  // namespace gwyf

// ── Логирование ─────────────────────────────────────────────────────────────

namespace gwyf::log {

enum class Level : u32 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

/// Открыть лог-файл. Пустое имя — писать только в отладчик/консоль.
void Open(const std::string& filename, Level minLevel = Level::Info);

void Close();

void SetLevel(Level level);

Level GetLevel();

void Write(Level level, const char* fmt, ...);

/// Перенаправить лог в собственный приёмник (используется самотестом).
using Sink = void (*)(Level level, const char* text, void* user);
void SetSink(Sink sink, void* user);

}  // namespace gwyf::log

#define GWYF_LOG(level, ...) ::gwyf::log::Write(::gwyf::log::Level::level, __VA_ARGS__)
#define GWYF_TRACE(...) GWYF_LOG(Trace, __VA_ARGS__)
#define GWYF_DEBUG(...) GWYF_LOG(Debug, __VA_ARGS__)
#define GWYF_INFO(...) GWYF_LOG(Info, __VA_ARGS__)
#define GWYF_WARN(...) GWYF_LOG(Warn, __VA_ARGS__)
#define GWYF_ERROR(...) GWYF_LOG(Error, __VA_ARGS__)
