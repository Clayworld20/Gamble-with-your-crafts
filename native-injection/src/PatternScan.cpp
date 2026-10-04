// ============================================================================
//  PatternScan.cpp — реализация сканера сигнатур.
// ============================================================================
#include "gwyfbridge/PatternScan.h"

#include <cctype>
#include <cstdlib>

namespace gwyf::scan {

namespace {

bool HexValue(char ch, u8& out) {
    if (ch >= '0' && ch <= '9') {
        out = static_cast<u8>(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        out = static_cast<u8>(ch - 'a' + 10);
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        out = static_cast<u8>(ch - 'A' + 10);
        return true;
    }
    return false;
}

/// Проверить соответствие по смещению (без выхода за границы).
bool MatchAt(const u8* data, usize size, usize offset, const Pattern& pattern) {
    if (offset + pattern.bytes.size() > size) return false;

    for (usize index = 0; index < pattern.bytes.size(); ++index) {
        const PatternByte& byte = pattern.bytes[index];
        if (byte.wildcard) continue;
        if (data[offset + index] != byte.value) return false;
    }
    return true;
}

}  // namespace

Pattern Parse(const std::string& text) {
    Pattern pattern;

    usize index = 0;
    while (index < text.size()) {
        // Пропускаем разделители.
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
        if (index >= text.size()) break;

        // Комментарий до конца строки.
        if (text[index] == '#' || text[index] == ';') {
            while (index < text.size() && text[index] != '\n') ++index;
            continue;
        }

        if (text[index] == '?') {
            pattern.bytes.push_back(PatternByte{0, true});
            // Пропускаем второй символ маски ("??" или "?").
            ++index;
            if (index < text.size() && text[index] == '?') ++index;
            continue;
        }

        if (index + 1 >= text.size()) {
            pattern.error = "незавершённый байт в шаблоне";
            return pattern;
        }

        u8 high = 0;
        u8 low = 0;
        if (!HexValue(text[index], high) || !HexValue(text[index + 1], low)) {
            pattern.error = std::string("не hex-символ в шаблоне: '") + text[index] + text[index + 1] + "'";
            return pattern;
        }

        pattern.bytes.push_back(PatternByte{static_cast<u8>((high << 4) | low), false});
        index += 2;
    }

    if (pattern.bytes.empty()) {
        pattern.error = "пустой шаблон";
        return pattern;
    }

    pattern.valid = true;
    return pattern;
}

void* FindFirst(const void* base, usize size, const Pattern& pattern, usize startOffset) {
    if (base == nullptr || !pattern.valid) return nullptr;
    if (pattern.bytes.size() > size) return nullptr;

    const auto* data = static_cast<const u8*>(base);
    const usize limit = size - pattern.bytes.size();

    for (usize offset = startOffset; offset <= limit; ++offset) {
        if (MatchAt(data, size, offset, pattern)) {
            return const_cast<u8*>(data + offset);
        }
    }
    return nullptr;
}

std::vector<void*> FindAll(const void* base, usize size, const Pattern& pattern, usize maxResults) {
    std::vector<void*> results;
    if (base == nullptr || !pattern.valid) return results;

    const auto* data = static_cast<const u8*>(base);
    const usize limit = size - pattern.bytes.size();

    for (usize offset = 0; offset <= limit && results.size() < maxResults; ++offset) {
        if (MatchAt(data, size, offset, pattern)) {
            results.push_back(const_cast<u8*>(data + offset));
        }
    }
    return results;
}

void* FindInModule(const char* moduleNameSubstring, const Pattern& pattern, usize startOffset) {
    ModuleInfo module{};
    if (!FindModule(moduleNameSubstring, module)) {
        GWYF_WARN("Модуль «%s» не найден для поиска по сигнатуре", moduleNameSubstring);
        return nullptr;
    }

    void* found = FindFirst(module.base, module.size, pattern, startOffset);
    GWYF_INFO("Поиск «%s» в %s: %s", moduleNameSubstring, module.name.c_str(),
              found != nullptr ? "найдено" : "НЕ найдено");
    return found;
}

void* ResolveRva(const char* moduleNameSubstring, u64 rva) {
    ModuleInfo module{};
    if (!FindModule(moduleNameSubstring, module)) return nullptr;
    if (rva >= module.size) {
        GWYF_WARN("RVA 0x%llX выходит за размер модуля %s (0x%zX байт)",
                  static_cast<unsigned long long>(rva), module.name.c_str(), module.size);
        return nullptr;
    }
    return static_cast<u8*>(module.base) + rva;
}

void* ResolveRipRelative(const void* instruction, i32 instructionLength, i32 dispOffset) {
    if (instruction == nullptr) return nullptr;

    const auto* bytes = static_cast<const u8*>(instruction);
    i32 displacement = 0;
    std::memcpy(&displacement, bytes + dispOffset, sizeof(displacement));

    // Адрес вычисляется от конца инструкции: RIP относителен следующей команде.
    const auto address = reinterpret_cast<u64>(bytes) + instructionLength + displacement;
    return reinterpret_cast<void*>(address);
}

void* ResolvePointerPath(void* base, const std::vector<i64>& offsets) {
    if (base == nullptr) return nullptr;

    auto current = reinterpret_cast<u64>(base);
    for (i64 offset : offsets) {
        const u64 target = current + static_cast<u64>(offset);
        u64 value = 0;
        if (!SafeRead(reinterpret_cast<const void*>(target), &value, sizeof(value))) {
            GWYF_DEBUG("ResolvePointerPath: чтение по %p запрещено", reinterpret_cast<void*>(target));
            return nullptr;
        }
        if (value == 0) return nullptr;
        current = value;
    }

    return reinterpret_cast<void*>(current);
}

bool SafeRead(const void* address, void* out, usize size) {
    if (address == nullptr || out == nullptr || size == 0) return false;

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;

    const DWORD protection = info.Protect & 0xFF;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                          protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    // Проверяем, что запрошенный диапазон целиком лежит в этом регионе.
    const auto start = reinterpret_cast<const u8*>(address);
    const auto regionEnd = static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
    if (start + size > regionEnd) return false;

    std::memcpy(out, address, size);
    return true;
}

bool IsReadable(const void* address) {
    u8 probe = 0;
    return SafeRead(address, &probe, sizeof(probe));
}

bool IsExecutable(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;

    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

void* FindVtableByClassName(const char* moduleNameSubstring, const std::string& className) {
    ModuleInfo module{};
    if (!FindModule(moduleNameSubstring, module)) return nullptr;
    if (className.empty()) return nullptr;

    // В MSVC RTTI имя класса лежит в типе-дескрипторе как ".?AV<Имя>@@".
    std::string marker = ".?AV" + className + "@@";
    Pattern pattern;
    pattern.valid = true;
    for (char ch : marker) {
        pattern.bytes.push_back(PatternByte{static_cast<u8>(ch), false});
    }

    void* nameAddress = FindFirst(module.base, module.size, pattern);
    if (nameAddress == nullptr) {
        GWYF_DEBUG("RTTI-имя класса %s в %s не найдено", className.c_str(), module.name.c_str());
        return nullptr;
    }

    // Полная раскрутка RTTI (TypeDescriptor -> ClassHierarchyDescriptor -> CompleteObjectLocator
    // -> vtable) требует учёта разрядности и версии компилятора, поэтому здесь мы
    // возвращаем адрес самого дескриптора имени — его достаточно для диагностики,
    // а точный указатель на vtable пользователь берёт из отладчика и заносит
    // в профиль как RVA (см. README, раздел «Поиск целей»).
    GWYF_INFO("RTTI-дескриптор %s найден по адресу %p (в профиль нужен RVA таблицы vtable)",
              className.c_str(), nameAddress);
    return nameAddress;
}

}  // namespace gwyf::scan
