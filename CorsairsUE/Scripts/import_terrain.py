"""Импорт террейна карты в UE и проверка данных проходимости.

Запускается редактором:

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/import_terrain.py <база-карты>"

где `<база-карты>` — путь без расширения к результату AssetConverter,
например `/tmp/maps-out/garner`. Рядом должны лежать `.terrain.json`,
`.height.r16`, `.block.raw` и `.region.raw`.

Скрипт не создаёт Landscape сам: сборка ландшафта требует запущенного
редактора с RHI, а здесь важнее проверить, что данные согласованы и пригодны
для импорта. Он читает метаданные, сверяет фактические размеры файлов с
объявленными и печатает статистику проходимости — то, по чему потом строится
навигация.
"""

import json
import os
import struct
import sys

import unreal


def load_metadata(base):
    path = base + ".terrain.json"
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def check_size(path, expected, label):
    actual = os.path.getsize(path)
    if actual != expected:
        unreal.log_error(
            f"РАЗМЕР_НЕ_СОШЁЛСЯ {label}: файл {actual} байт, ожидалось {expected}")
        return False
    unreal.log(f"РАЗМЕР_ОК {label}: {actual} байт")
    return True


def height_stats(path, tiles):
    """Диапазон высот в файле .r16 и его согласованность с метаданными."""
    with open(path, "rb") as handle:
        data = handle.read()
    values = struct.unpack(f"<{tiles}H", data)
    return min(values), max(values)


def block_stats(path, tiles):
    """Сколько тайлов непроходимы хотя бы одной четвертью."""
    with open(path, "rb") as handle:
        data = handle.read()

    blocked = 0
    fully_blocked = 0
    for i in range(tiles):
        quads = data[i * 4:i * 4 + 4]
        non_zero = sum(1 for q in quads if q != 0)
        if non_zero:
            blocked += 1
        if non_zero == 4:
            fully_blocked += 1
    return blocked, fully_blocked


def main():
    args = sys.argv[1:]
    if not args:
        unreal.log_error("нужен аргумент: <база-карты без расширения>")
        return

    base = args[0]
    meta = load_metadata(base)

    grid_w = meta["gridWidth"]
    grid_h = meta["gridHeight"]
    tiles = grid_w * grid_h

    unreal.log(f"КАРТА {os.path.basename(base)} сетка={grid_w}x{grid_h} тайлов={tiles}")
    unreal.log(f"СЕКЦИИ присутствует={meta['sectionsPresent']} из {meta['sectionsTotal']}")

    ok = True
    ok &= check_size(base + ".height.r16", tiles * 2, "height.r16")
    ok &= check_size(base + ".block.raw", tiles * 4, "block.raw")
    ok &= check_size(base + ".region.raw", tiles * 2, "region.raw")

    low, high = height_stats(base + ".height.r16", tiles)
    raw_low, raw_high = meta["heightRangeRaw"]
    # Кодировка объявлена в метаданных: uint16 = (rawHeight + 128) * 256.
    expect_low = (raw_low + 128) * 256
    expect_high = (raw_high + 128) * 256
    if (low, high) != (expect_low, expect_high):
        unreal.log_error(
            f"ВЫСОТЫ_НЕ_СОШЛИСЬ файл=[{low}, {high}], метаданные дают [{expect_low}, {expect_high}]")
        ok = False
    else:
        metres_low = raw_low * meta["heightUnitMeters"]
        metres_high = raw_high * meta["heightUnitMeters"]
        unreal.log(f"ВЫСОТЫ_ОК диапазон [{low}, {high}] = "
                   f"[{metres_low:.1f} м, {metres_high:.1f} м]")

    blocked, fully = block_stats(base + ".block.raw", tiles)
    if blocked != meta["blockedTiles"]:
        unreal.log_error(
            f"ПРОХОДИМОСТЬ_НЕ_СОШЛАСЬ файл={blocked}, метаданные={meta['blockedTiles']}")
        ok = False
    else:
        share = 100.0 * blocked / tiles if tiles else 0.0
        unreal.log(f"ПРОХОДИМОСТЬ_ОК непроходимых тайлов={blocked} ({share:.1f}%), "
                   f"полностью закрытых={fully}")

    unreal.log(f"ИТОГ {'ВСЁ_СОГЛАСОВАНО' if ok else 'ЕСТЬ_РАСХОЖДЕНИЯ'}")


main()
