#!/usr/bin/env python3
"""Проверка, что раскладка данных для Java совпадает с нативной частью.

Нативная часть считает хеш FNV-1a от текстового описания раскладки, а в
BridgeRecords.java лежит константа. Если кто-то поменяет структуру только с
одной стороны, обмен начнёт портить значения — этот скрипт ловит расхождение
на сборке, а не в игре.

Использование:
    python3 tools/check_wire_contract.py <хеш от selftest --print-layout-hash>
                                       [путь к BridgeRecords.java]
"""

import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    native_hash = sys.argv[1].strip().upper()
    java_path = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        Path(__file__).resolve().parent.parent / "mod/src/main/java/dev/gwyfbridge/mc/BridgeRecords.java"

    if not java_path.exists():
        print(f"ОШИБКА: не найден {java_path}")
        return 2

    text = java_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"LAYOUT_HASH\s*=\s*0x([0-9A-Fa-f]+)", text)
    if not match:
        print("ОШИБКА: в BridgeRecords.java не найдена константа LAYOUT_HASH")
        return 1

    java_hash = match.group(1).upper().lstrip("0") or "0"
    native_normalized = native_hash.lstrip("0") or "0"

    print(f"хеш раскладки: нативная часть #{native_hash}, BridgeRecords.java #{match.group(1).upper()}")

    if native_normalized != java_hash:
        print("\nРАСХОЖДЕНИЕ: раскладка данных для JVM изменилась с одной стороны.")
        print("Обновите LAYOUT_HASH в BridgeRecords.java (и проверьте смещения полей!)")
        return 1

    print("Раскладка совпадает — Java и C++ читают одни и те же байты одинаково.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
