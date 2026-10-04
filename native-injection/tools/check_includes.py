#!/usr/bin/env python3
"""Проверка «используется без заголовка» для нативного модуля.

Зачем: libstdc++ (Linux/g++) подтягивает многие заголовки транзитивно, а MSVC —
нет. Из-за этого сборка на Linux зелёная, а на Windows падает на `std::transform`
без <algorithm> (реальный случай из CI). Скрипт ищет такие места до отправки.

Что делает: разбирает src/, include/ и tools/**, собирает транзитивное множество
подключённых заголовков (свои заголовки обходятся рекурсивно) и сравнивает с
таблицей «средство std::* → обязательный заголовок».

Запуск:  python3 native-injection/tools/check_includes.py
Код возврата 1, если есть замечания (в CI это ошибка).
"""

import re, glob, os

FACILITY = {
    'transform': 'algorithm', 'sort': 'algorithm', 'find': 'algorithm', 'find_if': 'algorithm',
    'remove': 'algorithm', 'remove_if': 'algorithm', 'unique': 'algorithm', 'reverse': 'algorithm',
    'min': 'algorithm', 'max': 'algorithm', 'min_element': 'algorithm', 'max_element': 'algorithm',
    'clamp': 'algorithm', 'count': 'algorithm', 'count_if': 'algorithm', 'any_of': 'algorithm',
    'all_of': 'algorithm', 'none_of': 'algorithm', 'fill': 'algorithm', 'copy': 'algorithm',
    'copy_if': 'algorithm', 'lower_bound': 'algorithm', 'upper_bound': 'algorithm',
    'binary_search': 'algorithm', 'equal': 'algorithm', 'for_each': 'algorithm', 'replace': 'algorithm',
    'accumulate': 'numeric', 'iota': 'numeric',
    'tolower': 'cctype', 'toupper': 'cctype', 'isdigit': 'cctype', 'isspace': 'cctype',
    'string': 'string', 'to_string': 'string', 'stoi': 'string', 'stoul': 'string', 'stoll': 'string',
    'string_view': 'string_view',
    'vector': 'vector', 'array': 'array', 'map': 'map', 'unordered_map': 'unordered_map', 'set': 'set',
    'unordered_set': 'unordered_set', 'deque': 'deque', 'list': 'list', 'queue': 'queue', 'pair': 'utility',
    'memcpy': 'cstring', 'memset': 'cstring', 'memcmp': 'cstring', 'strlen': 'cstring', 'strcmp': 'cstring',
    'strncmp': 'cstring', 'strncpy': 'cstring', 'strstr': 'cstring', 'strrchr': 'cstring',
    'printf': 'cstdio', 'snprintf': 'cstdio', 'fopen': 'cstdio', 'FILE': 'cstdio', 'fprintf': 'cstdio',
    'fclose': 'cstdio', 'fread': 'cstdio', 'fwrite': 'cstdio', 'vsnprintf': 'cstdio', 'fgets': 'cstdio',
    'atomic': 'atomic', 'atomic_load': 'atomic', 'atomic_store': 'atomic', 'thread': 'thread',
    'mutex': 'mutex', 'lock_guard': 'mutex', 'unique_lock': 'mutex', 'chrono': 'chrono',
    'unique_ptr': 'memory', 'shared_ptr': 'memory', 'make_unique': 'memory', 'make_shared': 'memory',
    'optional': 'optional', 'nullopt': 'optional', 'function': 'functional', 'numeric_limits': 'limits',
    'move': 'utility', 'forward': 'utility', 'swap': 'utility', 'runtime_error': 'stdexcept',
    'invalid_argument': 'stdexcept', 'out_of_range': 'stdexcept', 'condition_variable': 'condition_variable',
    'abort': 'cstdlib', 'malloc': 'cstdlib', 'free': 'cstdlib', 'getenv': 'cstdlib', 'atoi': 'cstdlib',
    'strtol': 'cstdlib', 'strtoul': 'cstdlib', 'size_t': 'cstddef', 'uintptr_t': 'cstdint',
    'int32_t': 'cstdint', 'uint32_t': 'cstdint', 'int64_t': 'cstdint', 'uint64_t': 'cstdint',
    'uint8_t': 'cstdint', 'int16_t': 'cstdint', 'uint16_t': 'cstdint', 'errno': 'cerrno',
    'va_list': 'cstdarg', 'va_start': 'cstdarg', 'va_end': 'cstdarg', 'fmod': 'cmath', 'sqrt': 'cmath',
    'floor': 'cmath', 'ceil': 'cmath', 'lround': 'cmath', 'llround': 'cmath', 'isfinite': 'cmath',
    'wstring': 'string', 'filesystem': 'filesystem', 'regex': 'regex', 'hash': 'functional',
}

def strip_comments(text):
    """Убирает // и /* */, чтобы упоминания в комментариях не считались кодом."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def includes_of(path, seen=None):
    """Все заголовки, видимые из файла: свои — рекурсивно, системные — как есть."""
    seen = seen or set()
    if path in seen or not os.path.exists(path):
        return set()
    seen.add(path)
    text = strip_comments(open(path, encoding='utf-8').read())
    found = set()
    for m in re.finditer(r'#\s*include\s*[<"]([^>"]+)[>"]', text):
        name = m.group(1)
        found.add(os.path.basename(name))
        if not name.startswith('<'):
            local = name if os.path.exists(name) else None
            if local is None:
                for prefix in ('include/', 'src/', 'include/gwyfbridge/'):
                    if os.path.exists(prefix + name):
                        local = prefix + name
                        break
            if local:
                found |= includes_of(local, seen)
    return found

# Пути считаем относительно самого скрипта (tools/ лежит внутри модуля),
# иначе проверка «проходит» на пустом списке файлов, если запустить её
# из корня репозитория.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

files = sorted(glob.glob('src/*.cpp') + glob.glob('src/*.h') + glob.glob('include/gwyfbridge/*.h')
               + glob.glob('tools/**/*.cpp', recursive=True))
reports = []
for path in files:
    text = strip_comments(open(path, encoding='utf-8').read())
    have = includes_of(path)
    for name in sorted(set(re.findall(r'std::([A-Za-z_][A-Za-z0-9_]*)', text))):
        header = FACILITY.get(name)
        if header and header not in have and f'{header}.h' not in have:
            reports.append((path, name, header))

for path, name, header in reports:
    print(f"{path}: std::{name} — нужен <{header}>")

if reports:
    print(f"\nЗамечаний: {len(reports)}. На MSVC такая сборка не соберётся — добавьте заголовки.")
    raise SystemExit(1)

print("Проверка пройдена: все средства std::* подключены явно.")
