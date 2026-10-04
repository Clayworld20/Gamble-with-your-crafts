#!/usr/bin/env python3
"""Проверка контракта JNI между Java-классом мода и нативной библиотекой.

Зачем: JNI связывает стороны именами функций, а имена собираются из имени
пакета, класса и метода. Опечатка в одном символе даёт не ошибку компиляции, а
`UnsatisfiedLinkError` в рантайме — то есть уже в игре. Этот скрипт сверяет
объявления из автоматически сгенерированного заголовка (`javac -h`) с
реализациями в C++ и падает, если наборы не совпали.

Использование:
    python3 tools/check_jni_contract.py <путь к dev_gwyfbridge_mc_BridgeNative.h> \
        <путь к src/Native_JNI_Bridge.cpp>
"""

import re
import sys
from pathlib import Path

# Имя функции в заголовке от javac: Java_<пакет>_<класс>_<метод>
EXPORT_PATTERN = re.compile(r"\b(Java_[A-Za-z0-9_]+)\s*\(")


def collect_exports(text: str) -> dict[str, int]:
    """Собрать имена JNI-функций и число аргументов (без JNIEnv* и jclass)."""
    exports: dict[str, int] = {}

    for match in re.finditer(r"\b(Java_[A-Za-z0-9_]+)\s*\(([^)]*)\)", text):
        name = match.group(1)
        raw_args = match.group(2)

        args = []
        for chunk in raw_args.split(","):
            chunk = chunk.strip()
            if not chunk:
                continue
            args.append(chunk)

        # Первые два аргумента JNI (JNIEnv*, jclass/jobject) не считаем.
        count = max(0, len(args) - 2)
        if name in exports and exports[name] != count:
            print(f"  (предупреждение: {name} объявлена с разным числом аргументов)")
        exports[name] = count

    return exports


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    header_path = Path(sys.argv[1])
    source_path = Path(sys.argv[2])

    for path in (header_path, source_path):
        if not path.exists():
            print(f"ОШИБКА: файл не найден: {path}")
            return 2

    java_exports = collect_exports(header_path.read_text(encoding="utf-8", errors="replace"))
    cpp_exports = collect_exports(source_path.read_text(encoding="utf-8", errors="replace"))

    if not java_exports:
        print(f"ОШИБКА: в {header_path} не найдено ни одной JNI-функции — "
              "проверьте, что заголовок сгенерирован ключом javac -h")
        return 1

    missing_in_cpp = sorted(set(java_exports) - set(cpp_exports))
    missing_in_java = sorted(set(cpp_exports) - set(java_exports))
    arity_mismatch = sorted(
        name for name in set(java_exports) & set(cpp_exports)
        if java_exports[name] != cpp_exports[name]
    )

    print(f"Java-объявлений: {len(java_exports)}, реализаций в C++: {len(cpp_exports)}")

    if not missing_in_cpp and not missing_in_java and not arity_mismatch:
        print("Контракт JNI совпадает полностью:")
        for name in sorted(java_exports):
            print(f"  {name} — {java_exports[name]} аргументов (без JNIEnv*)")
        return 0

    print("\nРАСХОЖДЕНИЯ:")
    for name in missing_in_cpp:
        print(f"  нет реализации в C++:        {name}")
    for name in missing_in_java:
        print(f"  нет объявления в Java:       {name}")
    for name in arity_mismatch:
        print(f"  разное число аргументов:     {name} (java {java_exports[name]}, cpp {cpp_exports[name]})")

    return 1


if __name__ == "__main__":
    sys.exit(main())
