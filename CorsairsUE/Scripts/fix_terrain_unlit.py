"""Задаёт способ затенения мастер-материала рельефа.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/fix_terrain_unlit.py [lit|unlit]"

По умолчанию — `lit`. Отчёт: `Scripts/reports/fix_terrain_unlit.txt`.

Зачем выбор. Застройка приезжает пятью неосвещаемыми родительскими
материалами: оригинал рисует фиксированным конвейером DX9, и освещение там
живёт в данных, а не считается движком. Рельеф собран иначе — освещаемым,
и возникает соблазн привести его к тому же виду.

Проверено кадром: не надо. Неосвещаемый рельеф теряет затенение от солнца,
становится плоским и ещё светлее — молочность земли только усиливается.
Её причина другая: запечённое альбедо даёт четыре пикселя на клетку карты,
и вблизи узор мостовой усредняется в ровное пятно. Лечится разрешением
бейка, а не способом затенения.

Скрипт оставлен как переключатель: гипотезу удобно проверить снова, не
собирая мастер заново.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402

MASTER_PATH = "/Game/Terrain/GarnerBlend/M_GarnerTerrainPage"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    mode = (sys.argv[1] if len(sys.argv) > 1 else "lit").lower()
    if mode not in ("lit", "unlit"):
        report.error(f"режим только lit или unlit, получено {mode!r}")
        return

    material = unreal.EditorAssetLibrary.load_asset(MASTER_PATH)
    if material is None:
        report.error(f"мастер не найден: {MASTER_PATH}")
        return

    library = unreal.MaterialEditingLibrary
    texture_node = None
    for expression in library.get_material_expressions(material):
        if isinstance(expression,
                      (unreal.MaterialExpressionTextureSampleParameter2D,
                       unreal.MaterialExpressionTextureSample)):
            texture_node = expression
            break
    if texture_node is None:
        report.error("в мастере нет узла текстуры")
        return

    was = material.get_editor_property("shading_model")
    if mode == "unlit":
        material.set_editor_property(
            "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        library.connect_material_property(
            texture_node, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        target = "эмиссию"
    else:
        material.set_editor_property(
            "shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        library.connect_material_property(
            texture_node, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
        # Эмиссия отключается явно: оставшаяся связь светила бы поверх
        # базового цвета, и земля выбеливалась бы сильнее прежнего.
        library.disconnect_material_property(
            material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        target = "базовый цвет"

    library.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(
            material, only_if_is_dirty=False):
        report.error("мастер не сохранился")
        return

    report.line(f"шейдинг: {was} -> {mode}")
    report.line(f"текстура подключена в {target}")
    report.line(f"УСПЕХ: {MASTER_PATH}")


report = Reporter("fix_terrain_unlit")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
