#!/usr/bin/env bash
#
# Запускает оригинальный клиент на DirectX 9 под CrossOver — визуальный эталон
# для сверки с переносом на Unreal Engine.
#
# Перед запуском должна работать серверная связка: ./scripts/dev/run-stack.sh
#
# Две тонкости, каждая из которых поодиночке валит клиент:
#
#  1. **Рабочая директория — `Client/`, а не `Client/system/`.** Клиент читает
#     базы по относительному пути `../databases/`, как и делает `start.bat`
#     строкой `start system\Game.exe`. Из `Client/system/` путь не разрешается,
#     и клиент падает с минидампом.
#
#  2. **Настоящая `d3dx9_43.dll` вместо реализации Wine.** Wine компилирует
#     HLSL своим `vkd3d-shader`, который не берёт шейдеры движка: они падают с
#     `D3DXCompileShader failed`, и рендер не инициализируется вовсе. Библиотека
#     от Microsoft (из DirectX End-User Runtime) справляется. Порядок загрузки
#     переключается на `native,builtin` — сперва файл с диска.
#
set -euo pipefail

BOTTLE="${CORSAIRS_BOTTLE:-Corsairs64}"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SYSTEM32="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/windows/system32"

if [[ ! -x "$CX/wine" ]]; then
    echo "CrossOver не найден: $CX/wine" >&2
    exit 1
fi

# Бутылка обязана быть 64-битной: клиент собран под x64, в 32-битной он падает
# с 0xc000007b ещё до первой строки лога.
if [[ ! -d "$SYSTEM32" ]]; then
    echo "Нет 64-битной бутылки '$BOTTLE'. Создать:" >&2
    echo "  $CX/wine --bottle $BOTTLE --create --template win10_64" >&2
    exit 1
fi

for dll in d3dx9_43 d3dcompiler_43 d3dcompiler_47; do
    if [[ ! -f "$SYSTEM32/$dll.dll" ]]; then
        echo "Нет $dll.dll в бутылке." >&2
        echo "Взять из DirectX End-User Runtime или с машины Windows:" >&2
        echo "  scp <windows>:C:/Windows/System32/$dll.dll '$SYSTEM32/'" >&2
        exit 1
    fi
    "$CX/wine" --bottle "$BOTTLE" -- reg add 'HKCU\Software\Wine\DllOverrides' \
        /v "$dll" /t REG_SZ /d native,builtin /f >/dev/null 2>&1
done

cd "$ROOT/Client"
echo "Клиент запускается. Журналы: Client/log/game/"
exec "$CX/wine" --bottle "$BOTTLE" -- "$ROOT/Client/system/Game.exe" pKcfT0PcaX
