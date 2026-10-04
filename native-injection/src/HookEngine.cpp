// ============================================================================
//  HookEngine.cpp — установка и снятие хуков через MinHook.
// ============================================================================
#include "gwyfbridge/HookEngine.h"

#include <MinHook.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>

namespace gwyf::hook {

namespace {

std::mutex g_mutex;
bool g_initialized = false;
std::vector<HookEntry> g_hooks;

const char* MhStatusText(MH_STATUS status) {
    switch (status) {
        case MH_OK: return "MH_OK";
        case MH_ERROR_ALREADY_INITIALIZED: return "уже инициализирован";
        case MH_ERROR_NOT_INITIALIZED: return "не инициализирован";
        case MH_ERROR_ALREADY_CREATED: return "хук на этот адрес уже создан";
        case MH_ERROR_NOT_CREATED: return "хук не создан";
        case MH_ERROR_ENABLED: return "хук уже включён";
        case MH_ERROR_DISABLED: return "хук выключен";
        case MH_ERROR_NOT_EXECUTABLE: return "целевой адрес не исполняемый — цель определена неверно";
        case MH_ERROR_UNSUPPORTED_FUNCTION: return "пролог функции не поддерживается MinHook";
        case MH_ERROR_MEMORY_ALLOC: return "не удалось выделить память под трамплин";
        case MH_ERROR_MEMORY_PROTECT: return "не удалось изменить защиту страницы";
        case MH_ERROR_MODULE_NOT_FOUND: return "модуль не найден";
        case MH_ERROR_FUNCTION_NOT_FOUND: return "функция не найдена";
        default: return "неизвестный статус MinHook";
    }
}

HookEntry* Find(void* target) {
    for (auto& entry : g_hooks) {
        if (entry.target == target) return &entry;
    }
    return nullptr;
}

}  // namespace

bool Initialize(std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return true;

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        const std::string text = std::string("MH_Initialize: ") + MhStatusText(status);
        if (error != nullptr) *error = text;
        GWYF_ERROR("%s", text.c_str());
        return false;
    }

    g_initialized = true;
    GWYF_INFO("MinHook инициализирован");
    return true;
}

bool Install(const std::string& name, void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr) {
        GWYF_ERROR("Хук «%s»: пустой адрес цели или детура", name.c_str());
        return false;
    }

    if (!Initialize()) return false;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (Find(target) != nullptr) {
        GWYF_WARN("Хук на %p уже установлен — повторная установка пропущена", target);
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        GWYF_ERROR("MH_CreateHook(%s, %p): %s", name.c_str(), target, MhStatusText(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);  // не оставляем «полухук»
        GWYF_ERROR("MH_EnableHook(%s): %s", name.c_str(), MhStatusText(status));
        return false;
    }

    HookEntry entry{};
    entry.name = name;
    entry.target = target;
    entry.detour = detour;
    entry.trampoline = original != nullptr ? *original : nullptr;
    entry.enabled = true;
    g_hooks.push_back(entry);

    GWYF_INFO("Хук установлен: %-24s target=%p trampoline=%p", name.c_str(), target, entry.trampoline);
    return true;
}

bool InstallAndEnable(const std::string& name, void* target, void* detour, void** original) {
    return Install(name, target, detour, original);
}

bool SetEnabled(void* target, bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    HookEntry* entry = Find(target);
    if (entry == nullptr) return false;

    const MH_STATUS status = enabled ? MH_EnableHook(target) : MH_DisableHook(target);
    if (status != MH_OK) {
        GWYF_WARN("Переключение хука «%s» не удалось: %s", entry->name.c_str(), MhStatusText(status));
        return false;
    }

    entry->enabled = enabled;
    return true;
}

bool Remove(void* target, bool restoreOriginal) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find_if(g_hooks.begin(), g_hooks.end(), [target](const HookEntry& entry) {
        return entry.target == target;
    });
    if (it == g_hooks.end()) return false;

    if (it->enabled) {
        MH_DisableHook(target);
    }
    MH_RemoveHook(target);
    (void)restoreOriginal;

    GWYF_INFO("Хук снят: %s", it->name.c_str());
    g_hooks.erase(it);
    return true;
}

void UninstallAll() {
    std::lock_guard<std::mutex> lock(g_mutex);

    // Снимаем в обратном порядке: хуки могли ставиться один на другой.
    for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it) {
        if (it->enabled) MH_DisableHook(it->target);
        MH_RemoveHook(it->target);
    }

    const usize count = g_hooks.size();
    g_hooks.clear();

    if (g_initialized) {
        MH_Uninitialize();
        g_initialized = false;
    }

    GWYF_INFO("Снято хуков: %zu, MinHook остановлен", count);
}

std::vector<HookEntry> Installed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hooks;
}

bool Has(void* target) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Find(target) != nullptr;
}

u32 FreezeOtherThreads(const std::function<void()>& action) {
    const DWORD selfId = GetCurrentThreadId();
    const DWORD selfPid = GetCurrentProcessId();

    std::vector<HANDLE> suspended;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != selfPid) continue;
                if (entry.th32ThreadID == selfId) continue;

                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;

                if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                    suspended.push_back(thread);
                } else {
                    CloseHandle(thread);
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (action) action();

    for (HANDLE thread : suspended) {
        ResumeThread(thread);
        CloseHandle(thread);
    }

    return static_cast<u32>(suspended.size());
}

std::string Describe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_hooks.empty()) return "хуков нет";

    std::string text = "установлено хуков: " + std::to_string(g_hooks.size());
    for (const auto& entry : g_hooks) {
        text += "\n  • " + entry.name + " target=" + [&] {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%p", entry.target);
            return std::string(buffer);
        }() + (entry.enabled ? " (включён)" : " (выключен)");
    }
    return text;
}

std::string DumpBytes(const void* address, u32 count) {
    if (address == nullptr || count == 0) return std::string();

    const auto* bytes = static_cast<const u8*>(address);
    std::string text;
    text.reserve(count * 3);

    char buffer[8]{};
    for (u32 index = 0; index < count; ++index) {
        std::snprintf(buffer, sizeof(buffer), "%02X ", bytes[index]);
        text += buffer;
    }
    return text;
}

}  // namespace gwyf::hook
