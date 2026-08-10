"""Печатает границы импортированных мешей — чем проверяется базис координат.

Вопрос, на который отвечает скрипт: как Interchange раскладывает оси glTF по
осям Unreal. Ответ нельзя вывести из документации с достаточной уверенностью,
а от него зависит и расстановка объектов, и генерация рельефа. Границы меша,
координаты которого известны на входе, дают его прямым измерением.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_axes.py <путь-ассета> [<путь-ассета> ...]"

Отчёт: `Scripts/reports/check_axes.txt`.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402


def describe(report, asset_path):
    asset = unreal.load_asset(asset_path)
    if asset is None:
        report.warn(f"не загрузился: {asset_path}")
        return

    if isinstance(asset, unreal.StaticMesh):
        bounds = asset.get_bounds()
    elif isinstance(asset, unreal.SkeletalMesh):
        bounds = asset.get_bounds()
    else:
        report.warn(f"не меш: {asset_path} ({type(asset).__name__})")
        return

    origin = bounds.origin
    extent = bounds.box_extent
    report.line(f"{asset_path}")
    report.line(f"  центр  X={origin.x:12.1f} Y={origin.y:12.1f} Z={origin.z:12.1f}")
    report.line(f"  размах X={extent.x:12.1f} Y={extent.y:12.1f} Z={extent.z:12.1f}")
    report.line(f"  от     X={origin.x - extent.x:12.1f} "
                f"Y={origin.y - extent.y:12.1f} Z={origin.z - extent.z:12.1f}")
    report.line(f"  до     X={origin.x + extent.x:12.1f} "
                f"Y={origin.y + extent.y:12.1f} Z={origin.z + extent.z:12.1f}")


def main(report):
    paths = sys.argv[1:]
    if not paths:
        report.error("не задан путь ассета")
        return
    for path in paths:
        describe(report, path)


report = Reporter("check_axes")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
