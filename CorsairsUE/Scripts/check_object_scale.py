"""Замеряет размеры моделей сцены.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_object_scale.py"

Отчёт: `Scripts/reports/check_object_scale.txt`.

Масштаб мира задан клеткой карты: сто сантиметров на клетку, персонаж ростом
метр семьдесят шесть. Если дом выходит в полсотни метров, а фонарь — в
двадцать, значит модели пришли в своих единицах, и мир построен из деталей
разного размера. Проверяется это одним способом — измерением.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402


def main(report):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = [a for a in registry.get_assets_by_path(unreal.Name("/Game/All"), recursive=True)
              if str(a.asset_class_path.asset_name) == "StaticMesh"]
    report.line(f"моделей сцены: {len(assets)}")

    heights = []
    for asset in assets[:400]:
        mesh = unreal.load_asset(f"{asset.package_name}.{asset.asset_name}")
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        bounds = mesh.get_bounds()
        heights.append((bounds.box_extent.z * 2.0, str(asset.asset_name)))

    if not heights:
        report.error("ПРОВАЛ: ни одной модели не измерено")
        return

    heights.sort()
    median = heights[len(heights) // 2]
    report.line(f"измерено: {len(heights)}")
    report.line(f"  медиана высоты: {median[0]:.0f} см ({median[1]})")
    report.line(f"  минимум: {heights[0][0]:.0f} см ({heights[0][1]})")
    report.line(f"  максимум: {heights[-1][0]:.0f} см ({heights[-1][1]})")
    report.line("  самые высокие:")
    for height, name in heights[-5:]:
        report.line(f"    {name}: {height:.0f} см")

    # Дом в оригинале — этаж-два, то есть от трёх до десяти метров. Медиана
    # заметно выше означает, что всё вокруг великанское.
    if median[0] > 2000.0:
        report.error(f"ПРОВАЛ: медиана {median[0]:.0f} см — модели непомерно велики")


report = Reporter("check_object_scale")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
