#!/usr/bin/env bash
# Сводка о сбое сборки для тех, кто читает её через API.
#
# Журналы GitHub Actions лежат в хранилище Azure, недоступном из некоторых
# песочниц, поэтому журналы дублируются аннотациями (их отдаёт
# check-runs/<id>/annotations), а подробная выжимка — в $GITHUB_STEP_SUMMARY.
#
# Аннотаций на проверку GitHub показывает немного, поэтому важен порядок:
# сначала место падения теста (самотест печатает «ПРОВАЛ: …» и, при аварийном
# завершении, секцию с адресом сбоя), затем хвост журнала упавшего теста,
# затем имена упавших тестов и только в конце предупреждения компилятора.
# Полный отфильтрованный текст по-прежнему уходит в сводку шага.
#
# Запускается только при падении шага; ничего не переписывает и всегда
# завершается успешно.

set -u

summary="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
shopt -s nullglob

# Журналы отдельных тестов идут первыми: в них виден последний заголовок секции
# перед падением, тогда как ctest пишет все тесты в один файл.
logs=(/tmp/selftest-*.log /tmp/selftest*.log /tmp/*.log)

failure_marker='ПРОВАЛ|The following tests FAILED|ОШИБКА|error C[0-9]+|fatal error|LNK[0-9]+'
generic_error='error|Error|ERROR|ошибка|Ошибка|FAILED|failed|FAIL'

# Основной журнал: тот, где есть сообщение о провале проверки или падении.
primary=""
for f in "${logs[@]}"; do
    if grep -qE 'ПРОВАЛ|The following tests FAILED' "$f" 2>/dev/null; then
        primary="$f"
        break
    fi
done

{
    echo "## Диагностика сборки"
    echo
    if [ "${#logs[@]}" -eq 0 ]; then
        echo "Журналов в /tmp нет — сбой произошёл до запуска команд."
    fi

    for f in "${logs[@]}"; do
        echo "### \`${f}\`"
        echo
        echo '```'
        grep -nE "${failure_marker}" "$f" 2>/dev/null | head -30 || true
        grep -nE "${generic_error}" "$f" 2>/dev/null | head -20 || true
        echo '```'
        echo
        echo '```'
        tail -n 20 "$f" 2>/dev/null || true
        echo '```'
        echo
    done
} >> "${summary}"

emitted=0
emit() {
    local line="$1"
    [ -z "${line}" ] && return 0
    local safe="${line//%/%25}"
    safe="${safe//$'\r'/}"
    echo "::error::${safe}"
    emitted=$((emitted + 1))
}

# 1. Место падения: сообщения самотеста (в том числе от обработчика аварий,
#    который печатает секцию и адрес сбоя) и имена упавших тестов.
seen=""
for f in "${primary}" "${logs[@]}"; do
    [ -z "${f}" ] && continue
    [ "${f}" = "${seen}" ] && continue
    seen="$f"
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 6 ] && break 2
    done < <(grep -E 'ПРОВАЛ|The following tests FAILED|^[[:space:]]+[0-9]+ - ' "$f" 2>/dev/null | tail -n 4 || true)
done

# 2. Хвост журнала теста: последняя начатая секция = место сбоя. Если основной
#    журнал — ctest (там все тесты вместе), хвосты берутся из отдельных прогонов.
tails=0
for f in "${primary}" /tmp/selftest-*.log; do
    [ -z "${f}" ] && continue
    [ -f "${f}" ] || continue
    [ "${emitted}" -ge 8 ] && break
    emit "--- хвост ${f} ---"
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 8 ] && break 2
    done < <(tail -n 5 "${f}" 2>/dev/null || true)
    tails=$((tails + 1))
    [ "${tails}" -ge 2 ] && break
done

# 3. Ошибки компиляции/линковки, если шаг упал до тестов.
if [ "${emitted}" -lt 8 ]; then
    for f in "${logs[@]}"; do
        while IFS= read -r line; do
            emit "${line}"
            [ "${emitted}" -ge 8 ] && break 2
        done < <(grep -E 'error C[0-9]+|fatal error|LNK[0-9]+|CMake Error' "$f" 2>/dev/null | head -6 || true)
    done
fi

# 4. Предупреждения: при /WX («предупреждения = ошибки») именно они объясняют
#    C2220, но в аннотации раньше попадал только сам C2220 без текста.
warned=0
for f in "${logs[@]}"; do
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        safe="${line//%/%25}"
        safe="${safe//$'\r'/}"
        echo "::warning::${safe}"
        warned=$((warned + 1))
        if [ "${warned}" -ge 3 ]; then
            exit 0
        fi
    done < <(grep -E 'warning C[0-9]+|\[-W[a-z-]+\]' "$f" 2>/dev/null || true)
done

exit 0
