#!/usr/bin/env bash
# Сводка о сбое сборки для тех, кто читает её через API.
#
# Журналы GitHub Actions лежат в хранилище Azure, недоступном из некоторых
# песочниц, поэтому хвосты журналов складываются в $GITHUB_STEP_SUMMARY (его
# отдаёт API check-runs) и дублируются аннотациями ошибок (их отдаёт
# check-runs/<id>/annotations). Так причину сбоя видно без скачивания архива
# журналов.
#
# Порядок разбора важен: раньше аннотации заполнялись первыми же совпадениями
# «error|FAIL» из журнала сборки, и главное — имя упавшего теста и текст
# проверки — до них не доходило. Теперь сначала обрабатывается журнал, в
# котором реально есть сводка тестов, и только потом остальные.
#
# Запускается только при падении шага; ничего не переписывает и всегда
# завершается успешно.

set -u

summary="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
shopt -s nullglob
logs=(/tmp/*.log)

# Маркеры, по которым видно упавший тест и причину проверки.
test_marker='The following tests FAILED|ПРОВАЛ|ОШИБКА|error C[0-9]+|fatal error'
generic_error='error|Error|ERROR|ошибка|Ошибка|FAILED|failed|FAIL'

# Журналы упорядочиваются: сначала те, где есть сводка тестов, затем те, где
# есть любые ошибки, затем остальные.
primary=""
secondaries=()
for f in "${logs[@]}"; do
    if grep -qE 'The following tests FAILED|ПРОВАЛ' "$f" 2>/dev/null; then
        if [ -z "${primary}" ]; then primary="$f"; else secondaries+=("$f"); fi
    elif grep -qE "${generic_error}" "$f" 2>/dev/null; then
        secondaries+=("$f")
    else
        secondaries+=("$f")
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
        grep -nE "${test_marker}" "$f" 2>/dev/null | head -40 || true
        grep -nE "${generic_error}" "$f" 2>/dev/null | head -40 || true
        echo '```'
        echo
        # Хвост журнала: у ошибок CMake/MSBuild причина часто лежит в строках,
        # которые не содержат слов error/failed.
        echo '```'
        tail -n 15 "$f" 2>/dev/null || true
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

# 1. Главное: имя упавшего теста и строки проверок.
if [ -n "${primary}" ]; then
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 10 ] && exit 0
    done < <(grep -E "${test_marker}" "${primary}" 2>/dev/null || true)
fi

# 2. Соседние строки вокруг сводки тестов: там перечислены сами тесты.
for f in "${primary}" "${secondaries[@]}"; do
    [ -z "${f}" ] && continue
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 16 ] && exit 0
    done < <(grep -A 6 'The following tests FAILED' "$f" 2>/dev/null || true)
done

# 3. Прочие ошибки из журналов.
for f in "${primary}" "${secondaries[@]}"; do
    [ -z "${f}" ] && continue
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 24 ] && exit 0
    done < <(grep -E "${generic_error}" "$f" 2>/dev/null || true)
done

# 4. Хвосты журналов целиком: причины вида «could not find any instance…».
for f in "${primary}" "${secondaries[@]}"; do
    [ -z "${f}" ] && continue
    emit "--- хвост ${f} ---"
    while IFS= read -r line; do
        emit "${line}"
        [ "${emitted}" -ge 32 ] && exit 0
    done < <(tail -n 12 "$f" 2>/dev/null || true)
done

# Отдельно выносим предупреждения: при /WX («предупреждения = ошибки») именно они
# объясняют C2220, но в аннотации попадал только сам C2220 без текста.
warned=0
for f in "${primary}" "${secondaries[@]}"; do
    [ -z "${f}" ] && continue
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        safe="${line//%/%25}"
        safe="${safe//$'\r'/}"
        echo "::warning::${safe}"
        warned=$((warned + 1))
        if [ "${warned}" -ge 8 ]; then
            exit 0
        fi
    done < <(grep -E 'warning C[0-9]+|\[-W[a-z-]+\]' "$f" 2>/dev/null || true)
done

exit 0
