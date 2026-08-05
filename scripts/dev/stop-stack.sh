#!/usr/bin/env bash
#
# Останавливает связку, поднятую через run-stack.sh с DETACH=1.
# В обычном режиме run-stack.sh гасит серверы сам по Ctrl+C.

set -uo pipefail

Stop() {
    local name="$1" pattern="$2"
    local pids
    pids="$(pgrep -f "${pattern}" || true)"
    if [[ -z "${pids}" ]]; then
        printf "  %-14s не запущен\n" "${name}"
        return
    fi
    # shellcheck disable=SC2086
    kill ${pids} 2>/dev/null || true
    printf "  %-14s остановлен\n" "${name}"
}

# Ищем по каталогу сборки, а не по имени с расширением .dll. Сборка проекта с
# OutputType=Exe кладёт рядом управляемую сборку Corsairs.<Имя>Server.dll и
# нативный apphost Corsairs.<Имя>Server без расширения; работает именно
# apphost, поэтому шаблон с .dll не находит ничего — неотличимо от того, что
# сервер не запущен.
echo "Останавливаю связку..."
Stop GameServer   "build-posix/GameServer"
Stop Gate         "Corsairs.GateServer/bin"
Stop Group        "Corsairs.GroupServer/bin"
Stop Account      "Corsairs.AccountServer/bin"
echo "SQL Server оставлен работать: docker stop corsairs-mssql — если нужен."
