#!/usr/bin/env bash
# Сводка о сбое сборки для тех, кто читает её через API.
#
# Журналы GitHub Actions лежат в хранилище Azure, недоступном из некоторых
# песочниц, поэтому хвосты журналов складываются в $GITHUB_STEP_SUMMARY (его
# отдаёт API check-runs) и дублируются аннотациями ошибок (их отдаёт
# check-runs/<id>/annotations). Так причину сбоя видно без скачивания архива
# журналов.
#
# Запускается только при падении шага; ничего не переписывает и всегда
# завершается успешно.

set -u

summary="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
shopt -s nullglob
logs=(/tmp/*.log)

pattern='error|Error|ERROR|ошибка|Ошибка|FAILED|failed|FAIL|warning|Warning'

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
        grep -nE "${pattern}" "${f}" 2>/dev/null | head -60 || echo "(совпадений нет)"
        echo '```'
        echo
    done
} >> "${summary}"

# Первые аннотации — самое важное: они видны прямо в списке проверок.
emitted=0
for f in "${logs[@]}"; do
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        # % и переводы строк экранируются по правилам workflow-команд.
        safe="${line//%/%25}"
        safe="${safe//$'\r'/}"
        echo "::error::${safe}"
        emitted=$((emitted + 1))
        if [ "${emitted}" -ge 8 ]; then
            exit 0
        fi
    done < <(grep -E 'error|Error|FAILED|FAIL' "${f}" 2>/dev/null || true)
done

# Отдельно выносим предупреждения: при /WX («предупреждения = ошибки») именно они
# объясняют C2220, но в аннотации попадал только сам C2220 без текста.
warned=0
for f in "${logs[@]}"; do
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        safe="${line//%/%25}"
        safe="${safe//$'\r'/}"
        echo "::warning::${safe}"
        warned=$((warned + 1))
        if [ "${warned}" -ge 8 ]; then
            exit 0
        fi
    done < <(grep -E 'warning C[0-9]+|\[-W[a-z-]+\]' "${f}" 2>/dev/null || true)
done

# Хвост журнала целиком: у ошибок CMake/MSBuild причина часто лежит в строках,
# которые не содержат слов error/failed (перечисление путей, «could not find…»),
# и до аннотаций такие строки раньше не доходили.
tailed=0
for f in "${logs[@]}"; do
    echo "::error::--- хвост ${f} ---"
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        safe="${line//%/%25}"
        safe="${safe//$'\r'/}"
        echo "::error::${safe}"
    done < <(tail -n 12 "${f}" 2>/dev/null || true)
    tailed=$((tailed + 1))
    if [ "${tailed}" -ge 2 ]; then
        break
    fi
done

exit 0
