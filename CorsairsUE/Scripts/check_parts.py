"""Замеряет каждую часть тела персонажа отдельно.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_parts.py"

Отчёт: `Scripts/reports/check_parts.txt`.

Собранный персонаж выглядит целым, а границы всей сборки смещены от капсулы
на девять метров — впятеро больше самой фигуры. Такое бывает, когда одна из
частей уехала при конвертации: на вид её не замечаешь, но охватывающий объём
она растягивает, и всё, что считается по нему, promахивается.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

PARTS = [f"/Game/All/000000000{i}/SkeletalMeshes/000000000{i}" for i in range(5)]


def main(report):
    for path in PARTS:
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.SkeletalMesh):
            report.warn(f"{path}: не загрузился")
            continue
        bounds = mesh.get_bounds()
        origin, extent = bounds.origin, bounds.box_extent
        report.line(f"{os.path.basename(path)}: центр "
                    f"({origin.x:.0f}, {origin.y:.0f}, {origin.z:.0f}), "
                    f"полуразмер ({extent.x:.0f}, {extent.y:.0f}, {extent.z:.0f})")

        # Часть, чей центр далеко от нуля, и есть виновник смещения.
        if abs(origin.x) > 100 or abs(origin.y) > 100 or abs(origin.z) > 100:
            report.error(f"  ПРОВАЛ: центр части далеко от начала координат")


report = Reporter("check_parts")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
