#!/usr/bin/env bash
#
# Поднимает всю серверную связку для локальной разработки на macOS/Linux.
#
# Порядок запуска задан зависимостями: Account держит учётные записи, Group
# ходит в Account за аутентификацией, Gate ходит в Group, GameServer ходит в
# Gate. Обратный порядок дал бы каскад ретраев на старте.
#
#   Client ──1973──> Gate ──1975──> Group ──1978──> Account
#                     ^
#                    1971
#                     |
#                 GameServer
#
# Строки подключения задаются переменными окружения, а не правкой
# appsettings.json: в репозитории лежит конфигурация для Windows с
# Trusted_Connection, а вне Windows интегрированной аутентификации нет.
# Двойное подчёркивание в имени переменной означает вложенность ключа.
#
# Префикс CORSAIRS_ обязателен. ServerHost.createWebBuilder добавляет
# appsettings.json уже поверх стандартных источников конфигурации, поэтому
# файл перекрывает переменные окружения без префикса — побеждает последний
# добавленный источник. Последним там идёт AddEnvironmentVariables("CORSAIRS_"),
# и только он перебивает файл.

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly LOG_DIR="${REPO_ROOT}/server/logs"
readonly DOTNET="${DOTNET:-${HOME}/.dotnet/dotnet}"

readonly DB_HOST="${CORSAIRS_DB_HOST:-127.0.0.1}"
readonly DB_USER="${CORSAIRS_DB_USER:-sa}"
readonly DB_PASS="${CORSAIRS_DB_PASS:-Corsairs!Dev2026}"

readonly SQL_COMMON="User Id=${DB_USER};Password=${DB_PASS};TrustServerCertificate=True;Encrypt=False"

export CORSAIRS_ConnectionStrings__AccountDb="Server=${DB_HOST};Database=AccountServer;${SQL_COMMON}"
export CORSAIRS_ConnectionStrings__GameDb="Server=${DB_HOST};Database=GameDB;${SQL_COMMON}"

mkdir -p "${LOG_DIR}"

pids=()

Cleanup() {
    echo ""
    echo "Останавливаю связку..."
    for pid in "${pids[@]}"; do
        kill "${pid}" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap Cleanup EXIT INT TERM

# Ждёт, пока порт начнёт принимать соединения. Без этого следующий сервер
# стартует раньше зависимости и уходит в цикл переподключений.
WaitForPort() {
    local name="$1" port="$2" timeout="${3:-40}"
    local waited=0
    while ! nc -z 127.0.0.1 "${port}" 2>/dev/null; do
        if [[ ${waited} -ge ${timeout} ]]; then
            echo "  ✗ ${name}: порт ${port} не открылся за ${timeout} с — смотри ${LOG_DIR}/${name}.log"
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "  ✓ ${name} слушает ${port} (${waited} с)"
}

StartDotnet() {
    local name="$1" project="$2" port="$3"
    echo "→ ${name}"
    # --no-build обязателен: иначе dotnet run сперва компилирует, и ожидание
    # порта истекает на сборке, а не на запуске. Сборка — отдельный шаг ниже.
    "${DOTNET}" run --project "${REPO_ROOT}/${project}" --configuration Release --no-build \
        >"${LOG_DIR}/${name}.log" 2>&1 &
    pids+=("$!")
    WaitForPort "${name}" "${port}"
}

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "Собираю серверы..."
    for project in \
        sources/Dotnet/Servers/Account/Corsairs.AccountServer \
        sources/Dotnet/Servers/Group/Corsairs.GroupServer \
        sources/Dotnet/Servers/Gate/Corsairs.GateServer
    do
        "${DOTNET}" build "${REPO_ROOT}/${project}" -c Release --nologo -v quiet >/dev/null
    done
    echo "  ✓ собрано"
fi

echo "Проверяю SQL Server на ${DB_HOST}..."
if ! nc -z "${DB_HOST}" 1433 2>/dev/null; then
    echo "  ✗ SQL Server недоступен. Запусти: docker start corsairs-mssql"
    exit 1
fi
echo "  ✓ SQL Server отвечает"

StartDotnet Account sources/Dotnet/Servers/Account/Corsairs.AccountServer 1978
StartDotnet Group   sources/Dotnet/Servers/Group/Corsairs.GroupServer     1975
StartDotnet Gate    sources/Dotnet/Servers/Gate/Corsairs.GateServer       1973

echo "→ GameServer"
(
    cd "${REPO_ROOT}/server/GameServer"
    exec "${REPO_ROOT}/sources/Server/GameServer/build-posix/GameServer"
) >"${LOG_DIR}/GameServer.log" 2>&1 &
pids+=("$!")

echo ""
echo "Связка поднята. Клиентский порт: 1973. Логи: ${LOG_DIR}"
echo "Ctrl+C — остановить."
wait
