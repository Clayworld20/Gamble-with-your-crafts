// ============================================================================
//  PatternScan.h — поиск по сигнатурам (AOB) и разрешение адресов.
//
//  Нужен там, где имя метода недоступно: нативные функции движка
//  (UnityPlayer.dll, GameAssembly.dll и т.п.). Сканер поддерживает
//  классический синтаксис Cheat Engine/x64dbg:
//      "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0A"
//  где "??" — любой байт.
//
//  Сканирование ведётся по образу модуля (как он лежит в памяти), поэтому
//  учитываются релокации — искать нужно ту же последовательность, что видна
//  в отладчике, привязанном к живому процессу.
// ============================================================================
#pragma once

#include "Common.h"

#include <optional>
#include <string>
#include <vector>

namespace gwyf::scan {

/// Байт шаблона: значение + признак «любой».
struct PatternByte {
    u8 value = 0;
    bool wildcard = false;
};

/// Разобранный шаблон.
struct Pattern {
    std::vector<PatternByte> bytes;
    bool valid = false;
    std::string error;
};

/// Разобрать строку вида "48 8B ?? 05".
Pattern Parse(const std::string& text);

/// Найти первое вхождение шаблона в диапазоне памяти.
void* FindFirst(const void* base, usize size, const Pattern& pattern, usize startOffset = 0);

/// Найти все вхождения (ограничение — maxResults).
std::vector<void*> FindAll(const void* base, usize size, const Pattern& pattern, usize maxResults = 64);

/// Найти шаблон в модуле процесса (по имени модуля, например "UnityPlayer.dll").
void* FindInModule(const char* moduleNameSubstring, const Pattern& pattern, usize startOffset = 0);

/// Разрешить адрес по RVA от базы модуля.
void* ResolveRva(const char* moduleNameSubstring, u64 rva);

/// Разрешить адрес по RVA + смещение, прочитанное из инструкции вида
/// "48 8B 05 <disp32>" (типичный доступ к глобальной переменной в x64).
/// Возвращает адрес целевого объекта, если удалось пройти цепочку смещений.
void* ResolveRipRelative(const void* instruction, i32 instructionLength, i32 dispOffset);

/// Пройти цепочку указателей: base -> [base+offsets[0]] -> ... -> [.. + offsets[n]].
/// Нужно для доступа к статическим синглтонам ("GameManager::instance").
void* ResolvePointerPath(void* base, const std::vector<i64>& offsets);

/// Прочитать указатель из памяти процесса (с проверкой доступности страницы).
bool SafeRead(const void* address, void* out, usize size);

/// Проверить, что страница по адресу читаема и/или исполняема.
bool IsReadable(const void* address);
bool IsExecutable(const void* address);

/// Найти таблицу виртуальных функций класса (RTTI-скан по строке имени класса).
/// Полезно, когда нужно достучаться до интерфейса движка, но нет экспорта.
void* FindVtableByClassName(const char* moduleNameSubstring, const std::string& className);

}  // namespace gwyf::scan
