"""Проверяет, что положение объекта внутри модели доехало до меша в UE.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_model_matrix.py"

Отчёт: `Scripts/reports/check_model_matrix.txt`.

Конвертер пишет матрицу объекта в трансформ узла glTF. Дальше есть два исхода,
и различить их можно только по факту: Interchange либо запекает трансформ узла
в вершины статического меша, либо оставляет его на актёре сцены. Во втором
случае правка до UE не доезжает — расстановка грузит ассет меша напрямую, и
трансформ узла при этом теряется.

Проверка на by-bd001 (Argent Palace): четыре части с известными смещениями из
исходного файла. Если запекание произошло, центры их границ разойдутся; если
нет — совпадут.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

# Смещения из by-bd001.lmo в единицах модели, в порядке частей _0.._3.
# Прочитаны прямо из MatLocal каждого геометрического объекта.
EXPECTED = [
    (-1.24, -3.08, 0.0),
    (-5.67, -0.38, 0.0),
    (-1.23, 2.80, 0.0),
    (-0.50, -8.99, 0.0),
]

# Сколько сантиметров UE в одной единице модели.
UNITS_PER_MODEL_UNIT = 100.0


def main(report):
    report.line("=== положение частей by-bd001 в импортированных мешах ===")

    centers = []
    for index in range(len(EXPECTED)):
        path = f"/Game/All/by-bd001_{index}/StaticMeshes/by-bd001_{index}"
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            report.error(f"НЕТ_МЕША {path}")
            return

        origin = mesh.get_bounds().origin
        centers.append((origin.x, origin.y, origin.z))

        # Смещение по X переносится в UE один к одному, поэтому по нему
        # сверяемся напрямую. Y и Z в UE меняются местами относительно glTF
        # и вдобавок смешаны с собственными габаритами меша — там сравнивать
        # с ожидаемым числом нечего.
        expected_x = EXPECTED[index][0] * UNITS_PER_MODEL_UNIT
        report.line(
            f"часть _{index}: центр ({origin.x:8.2f}, {origin.y:8.2f}, "
            f"{origin.z:8.2f}), ожидаемый X {expected_x:8.2f}")

    spread = max(abs(a[i] - b[i])
                 for a in centers for b in centers for i in range(3))
    report.line(f"разброс центров: {spread:.1f} см")

    # Если трансформ потерян, все части совпадут по X с точностью до
    # собственных габаритов — а они у этих частей почти одинаковы.
    x_spread = max(abs(a[0] - b[0]) for a in centers for b in centers)
    report.line(f"разброс по X: {x_spread:.1f} см")

    if x_spread < 1.0:
        report.error("ПРОВАЛ: части стоят в одной точке по X — трансформ узла "
                     "не запечён в меш, расстановка соберёт здание неправильно")
    else:
        report.line("УСПЕХ: части разнесены, трансформ узла доехал до меша")


report = Reporter("check_model_matrix")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
