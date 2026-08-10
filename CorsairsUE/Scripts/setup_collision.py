"""Включает столкновения по треугольникам у рельефа и застройки.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_collision.py"

Отчёт: `Scripts/reports/setup_collision.txt`.

Interchange импортирует меши без простых примитивов столкновений, а режим по
умолчанию (CTF_USE_DEFAULT) означает «использовать простые, если они есть».
Их нет — цепляться не за что, и персонаж проваливается сквозь землю.

Для рельефа и статичной застройки верный режим — CTF_USE_COMPLEX_AS_SIMPLE:
столкновения считаются по самим треугольникам. Аппроксимировать выпуклыми
оболочками рельеф бессмысленно, а здания оригинала имеют проёмы и арки,
которые оболочка заклеила бы.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

TARGET_PATHS = ["/Game/Terrain", "/Game/All"]


def apply_to_path(report, root):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()

    # Реестр ассетов в headless-режиме пуст, пока его не попросят обойти
    # каталог. Без этого get_assets_by_path возвращает ничего — и обход
    # заканчивается отчётом «изменено 0», неотличимым от «всё уже сделано».
    registry.scan_paths_synchronous([root], force_rescan=True)
    assets = registry.get_assets_by_path(unreal.Name(root), recursive=True)
    if not assets:
        report.error(f"{root}: реестр не отдал ни одного ассета")
        return 0

    # Считается каждый исход отдельно. Прежде «не меш» и «уже настроено» оба
    # молча пропускались, и по отчёту нельзя было понять, есть у застройки
    # столкновения или скрипт до неё не добрался.
    meshes = 0
    changed = 0
    already = 0
    skipped = 0
    for asset in assets:
        if str(asset.asset_class_path.asset_name) != "StaticMesh":
            continue
        meshes += 1

        mesh = unreal.load_asset(f"{asset.package_name}.{asset.asset_name}")
        if not isinstance(mesh, unreal.StaticMesh):
            skipped += 1
            continue

        body = mesh.get_editor_property("body_setup")
        if body is None:
            skipped += 1
            continue

        current = body.get_editor_property("collision_trace_flag")
        if current == unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE:
            already += 1
            continue

        body.set_editor_property("collision_trace_flag",
                                 unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False)
        changed += 1

    report.line(f"{root}: ассетов {len(assets)}, из них мешей {meshes}; "
                f"настроено сейчас {changed}, уже было {already}, "
                f"без формы {skipped}")
    return changed


def main(report):
    if RefuseIfEditorOpen(report):
        return

    total = 0
    for root in TARGET_PATHS:
        total += apply_to_path(report, root)

    report.line(f"ВСЕГО изменено мешей: {total}")
    if total == 0:
        report.line("ни один меш не изменён — режим уже выставлен у всех "
                    "найденных. Если ассетов найдено ноль, выше стоит ошибка.")


report = Reporter("setup_collision")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
