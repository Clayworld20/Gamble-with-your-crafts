// ============================================================================
//  HookEngine.h — обёртка над MinHook.
//
//  Почему MinHook: он делает то, что нам нужно, и делает это аккуратно —
//  дизассемблирует пролог цели, переносит переписанные инструкции в трамплин
//  и умеет корректно снимать хуки. Писать свой дизассемблер длины инструкций
//  (HDE64 уже внутри MinHook) ради трёх хуков — неоправданный риск.
//
//  ПРАВИЛА БЕЗОПАСНОСТИ
//    * Установка/снятие хуков — только на «спокойном» этапе, когда целевые
//      методы ещё не выполняются на других потоках (или с заморозкой потоков,
//      см. FreezeOtherThreads — используется для критичных целей).
//    * В самом детуре нельзя: аллоцировать, блокировать мьютексы, вызывать
//      логирование файла, ждать. Только захват аргументов, запись в кольцо
//      и SetEvent — см. комментарии в GWYF_HookEngine.cpp.
// ============================================================================
#pragma once

#include "Common.h"

#include <functional>

#include <mutex>
#include <string>
#include <vector>

namespace gwyf::hook {

/// Описание установленного хука (для диагностики и корректного снятия).
struct HookEntry {
    std::string name;
    void* target = nullptr;
    void* detour = nullptr;
    void* trampoline = nullptr;
    bool enabled = false;
};

/// Инициализация MinHook (идемпотентно).
bool Initialize(std::string* error = nullptr);

/// Установить хук. target — адрес функции, detour — наша функция,
/// original (необязательно) получит адрес трамплина для вызова оригинала.
bool Install(const std::string& name, void* target, void* detour, void** original = nullptr);

/// Установить хук вместе с вызовом колбэка сразу после установки
/// (удобно, когда нужно выставить дополнительные условия).
bool InstallAndEnable(const std::string& name, void* target, void* detour, void** original = nullptr);

/// Включить/выключить конкретный хук по адресу цели.
bool SetEnabled(void* target, bool enabled);

/// Снять один хук.
bool Remove(void* target, bool restoreOriginal = true);

/// Снять все хуки в обратном порядке установки.
void UninstallAll();

/// Список установленных хуков (копия).
std::vector<HookEntry> Installed();

/// Есть ли уже хук на этот адрес.
bool Has(void* target);

/// Остановить все остальные потоки процесса, выполнить действие и возобновить.
/// Нужно, когда целевая функция может исполняться прямо сейчас, а мы её
/// переписываем: без заморозки есть окно, в котором поток влетит в середину
/// переписанного пролога. Возвращает количество замороженных потоков.
u32 FreezeOtherThreads(const std::function<void()>& action);

/// Диагностика: сколько памяти занято трамплинами MinHook.
std::string Describe();

/// Прочитать первые байты функции (для логов «похоже/не похоже на нужный метод»).
std::string DumpBytes(const void* address, u32 count);

}  // namespace gwyf::hook
