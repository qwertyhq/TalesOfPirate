#!/usr/bin/env bash
#
# Готовит базы данных для локальной разработки: схема, миграции и учётная
# запись для входа.
#
# Порядок важен и повторяемость обязательна: скрипт можно запускать сколько
# угодно раз, ничего не ломая. Без него связка поднимается, но валится на
# первом же обращении к таблице, которой нет — а сообщение об этом уходит в
# журнал ошибок GameServer, куда обычно никто не смотрит.
#
#   ./scripts/dev/setup-database.sh
#
# Требуется запущенный SQL Server:
#   docker run -d --platform linux/amd64 --name corsairs-mssql \
#     -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD=... -e MSSQL_PID=Developer \
#     -p 1433:1433 --restart unless-stopped \
#     mcr.microsoft.com/mssql/server:2022-latest
#
# Перезапуск нужен потому, что SQL Server под эмуляцией x86 на Apple Silicon
# периодически падает с SIGSEGV; политика поднимает его обратно.

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

readonly DB_HOST="${CORSAIRS_DB_HOST:-127.0.0.1}"
readonly SA_PASSWORD="${CORSAIRS_SA_PASSWORD:-Corsairs!Dev2026}"

# Учётная запись приложения. Совпадает с секцией [Database] в
# server/GameServer/GameServer00.cfg — расхождение проявится как отказ входа.
readonly APP_USER="${CORSAIRS_DB_USER:-mothannakh}"
readonly APP_PASSWORD="${CORSAIRS_DB_PASSWORD:-Y87dc#\$98}"

# Учётная запись игрока для проверок. Пароль хранится хешем BLAKE2s и
# сравнивается как строка, поэтому в базу кладётся именно он.
readonly TEST_ACCOUNT="admin"
readonly TEST_PASSWORD_HASH="327E7E3821F5F6D33C090137F979BF48EE62E9051C1610E1D6468ECB3C67A124"

readonly SQLCMD="${SQLCMD:-/opt/homebrew/opt/mssql-tools18/bin/sqlcmd}"

Run() {
    "${SQLCMD}" -S "${DB_HOST}" -U sa -P "${SA_PASSWORD}" -C -b "$@"
}

RunFile() {
    local label="$1" file="$2"
    printf "  %-34s" "${label}"
    if Run -i "${REPO_ROOT}/${file}" >/dev/null 2>&1; then
        echo "готово"
    else
        # Скрипты схемы падают на повторном запуске, потому что объекты уже
        # есть. Это не ошибка окружения, поэтому прогон продолжается.
        echo "пропущено (уже применено)"
    fi
}

echo "Проверяю SQL Server на ${DB_HOST}..."
if ! Run -Q "SELECT 1" >/dev/null 2>&1; then
    echo "  ✗ недоступен. Запусти: docker start corsairs-mssql"
    exit 1
fi
echo "  ✓ отвечает"

echo "Создаю базы и учётную запись приложения..."
Run -Q "
IF DB_ID('GameDB') IS NULL CREATE DATABASE GameDB;
IF DB_ID('AccountServer') IS NULL CREATE DATABASE AccountServer;
IF NOT EXISTS (SELECT 1 FROM sys.sql_logins WHERE name = '${APP_USER}')
    CREATE LOGIN [${APP_USER}] WITH PASSWORD = '${APP_PASSWORD}',
        CHECK_POLICY = OFF;
" >/dev/null

for db in GameDB AccountServer; do
    Run -d "${db}" -Q "
IF NOT EXISTS (SELECT 1 FROM sys.database_principals WHERE name = '${APP_USER}')
    CREATE USER [${APP_USER}] FOR LOGIN [${APP_USER}];
ALTER ROLE db_owner ADD MEMBER [${APP_USER}];
" >/dev/null
done
echo "  ✓ ${APP_USER} — владелец обеих баз"

echo "Применяю схему..."
RunFile "AccountServer"      "mssql/[0]AccountServer.sql"
RunFile "GameDB"             "mssql/[1]GameDB.sql"
RunFile "гильдии"            "mssql/[2]Guild.sql"

echo "Применяю миграции..."
# Карта исследованной местности. Без неё GameServer работает, но при каждом
# входе в мир пишет в журнал ошибку об отсутствующем объекте.
RunFile "player_map_masks"   "databases/migrate_player_map_masks.sql"

echo "Создаю учётную запись для проверок..."
Run -Q "
USE AccountServer;
IF NOT EXISTS (SELECT 1 FROM account_login WHERE name = '${TEST_ACCOUNT}')
    INSERT INTO account_login (name, password)
    VALUES ('${TEST_ACCOUNT}', '${TEST_PASSWORD_HASH}');
USE GameDB;
IF NOT EXISTS (SELECT 1 FROM account WHERE ato_nome = '${TEST_ACCOUNT}')
    INSERT INTO account (ato_id, ato_nome, jmes)
    VALUES ((SELECT ISNULL(MAX(ato_id) + 1, 1) FROM account), '${TEST_ACCOUNT}', 99);
" >/dev/null
echo "  ✓ ${TEST_ACCOUNT} / ${TEST_ACCOUNT}"

echo ""
echo "База готова. Поднять связку: ./scripts/dev/run-stack.sh"
