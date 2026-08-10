"""Импортирует запечённое альбедо рельефа Garner и ставит МI на страницы.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/apply_garner_terrain_albedo.py <каталог-png>"

Отчёт: `Scripts/reports/apply_garner_terrain_albedo.txt`.

Пайплайн:
1. 64 PNG (2048×2048 RGBA) импортируются в /Game/Terrain/GarnerBlend.
2. Мастер-материал `M_GarnerTerrainPage` с параметром Albedo и UV-remap
   (UV меша — это координаты клеток: origin = 128 × page-index).
3. По MI на страницу `MI_Garner_XX_YY` с параметрами Albedo/OriginU/OriginV.
4. Актёры `Terrain_garner_terrain_XX_YY` в /Game/Maps/Garner получают свои MI.

Повторный запуск обязан давать тот же результат: ассеты заменяются, материал
переконфигурируется, повторного MI не создаётся.
"""

import os
import glob
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

CONTENT_ROOT = "/Game/Terrain/GarnerBlend"
MASTER_NAME = "M_GarnerTerrainPage"
MASTER_PATH = f"{CONTENT_ROOT}/{MASTER_NAME}"
MAP_PATH = "/Game/Maps/Garner"
PAGE_RE = re.compile(r"garner_page_(\d{2})_(\d{2})\.png$")
EXPECTED_PAGES = 64
PAGE_UV_SIZE = 128.0


def configure_master():
    """Строит (или переконфигурирует) граф мастер-материала страницы."""
    library = unreal.MaterialEditingLibrary
    existing = unreal.load_asset(MASTER_PATH)
    if isinstance(existing, unreal.Material):
        expressions = library.get_material_expressions(existing)
        if expressions:
            report_line(f"MASTER уже есть: {len(expressions)} expressions")
            return existing

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(
        MASTER_NAME, CONTENT_ROOT, unreal.Material,
        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f"не создан мастер-материал {MASTER_PATH}")

    tex = library.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, 0, 0)
    tex.set_editor_property("parameter_name", unreal.Name("Albedo"))

    uv = library.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -800, 0)
    origin_x = library.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -600, -120)
    origin_x.set_editor_property("parameter_name", unreal.Name("OriginU"))
    origin_x.set_editor_property("default_value", 0.0)
    origin_y = library.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -600, 120)
    origin_y.set_editor_property("parameter_name", unreal.Name("OriginV"))
    origin_y.set_editor_property("default_value", 0.0)
    append_uv = library.create_material_expression(
        material, unreal.MaterialExpressionAppendVector, -400, 0)
    library.connect_material_expressions(origin_x, "", append_uv, "A")
    library.connect_material_expressions(origin_y, "", append_uv, "B")
    subtract = library.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -300, 0)
    library.connect_material_expressions(uv, "", subtract, "A")
    library.connect_material_expressions(append_uv, "", subtract, "B")
    scale = library.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -200, 0)
    library.connect_material_expressions(subtract, "", scale, "A")
    const = library.create_material_expression(
        material, unreal.MaterialExpressionConstant, -300, 220)
    const.set_editor_property("r", 1.0 / PAGE_UV_SIZE)
    library.connect_material_expressions(const, "", scale, "B")
    library.connect_material_expressions(scale, "", tex, "UVs")

    library.connect_material_property(
        tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = library.create_material_expression(
        material, unreal.MaterialExpressionConstant, 0, 220)
    roughness.set_editor_property("r", 0.9)
    library.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    material.set_editor_property("two_sided", False)
    library.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(
            material, only_if_is_dirty=False):
        raise RuntimeError(f"мастер не сохранился {MASTER_PATH}")
    report_line("MASTER создан и сохранён")
    return material


report_ref = None


def report_line(text):
    if report_ref is not None:
        report_ref.line(text)


def import_texture(png_path):
    stem = os.path.splitext(os.path.basename(png_path))[0]
    asset_name = "T_" + stem.replace("garner_page_", "Garner_")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = png_path
    task.destination_path = CONTENT_ROOT
    task.automated = True
    task.replace_existing = True
    task.save = True
    tools.import_asset_tasks([task])
    list(task.get_objects())
    texture = unreal.load_asset(f"{CONTENT_ROOT}/{asset_name}.{asset_name}")
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(f"нет Texture2D {asset_name} после импорта {png_path}")
    return texture


def make_page_instance(master, texture, page_x, page_y):
    name = f"MI_Garner_{page_x:02d}_{page_y:02d}"
    full = f"{CONTENT_ROOT}/{name}"
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    instance = unreal.load_asset(full)
    if not isinstance(instance, unreal.MaterialInstanceConstant):
        instance = tools.create_asset(
            name, CONTENT_ROOT, unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
        if instance is None:
            raise RuntimeError(f"не создан MI {full}")
    library = unreal.MaterialEditingLibrary
    library.set_material_instance_parent(instance, master)
    library.set_material_instance_texture_parameter_value(
        instance, unreal.Name("Albedo"), texture)
    library.set_material_instance_scalar_parameter_value(
        instance, unreal.Name("OriginU"), float(page_x * PAGE_UV_SIZE))
    library.set_material_instance_scalar_parameter_value(
        instance, unreal.Name("OriginV"), float(page_y * PAGE_UV_SIZE))
    if not unreal.EditorAssetLibrary.save_loaded_asset(
            instance, only_if_is_dirty=False):
        raise RuntimeError(f"MI не сохранился {full}")

    readback_tex = library.get_material_instance_texture_parameter_value(
        instance, unreal.Name("Albedo"))
    readback_x = library.get_material_instance_scalar_parameter_value(
        instance, unreal.Name("OriginU"))
    if (readback_tex is None
            or readback_tex.get_path_name() != texture.get_path_name()
            or abs(float(readback_x) - float(page_x * PAGE_UV_SIZE)) > 1e-4):
        raise RuntimeError(f"readback MI неверен {full}")
    return instance


def retexture_map(page_instances):
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level(MAP_PATH):
        raise RuntimeError(f"не загрузилась карта {MAP_PATH}")
    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    terrain = [a for a in actors
               if a.get_actor_label().startswith("Terrain_garner_terrain_")]
    if len(terrain) != EXPECTED_PAGES:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО terrain actors: {len(terrain)}/"
            f"{EXPECTED_PAGES}")
    applied = 0
    for actor in terrain:
        suffix = actor.get_actor_label().rsplit("garner_terrain_", 1)[-1]
        instance = page_instances.get(suffix)
        if instance is None:
            raise RuntimeError(f"нет MI для страницы {suffix}")
        actor.static_mesh_component.set_material(0, instance)
        applied += 1
    if not levels.save_current_level():
        raise RuntimeError(f"карта {MAP_PATH} не сохранена")
    return applied


def main():
    if RefuseIfEditorOpen(report_ref):
        raise RuntimeError("графический UnrealEditor открыт")
    if len(sys.argv) != 2:
        raise RuntimeError(f"нужен каталог PNG, получено {sys.argv[1:]}")
    pngs = sorted(glob.glob(os.path.join(os.path.abspath(sys.argv[1]), "*.png")))
    matched = [(path, PAGE_RE.search(os.path.basename(path))) for path in pngs]
    matched = [(path, m) for path, m in matched if m]
    if len(matched) != EXPECTED_PAGES:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО PNG: {len(matched)}/{EXPECTED_PAGES}")

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([CONTENT_ROOT], force_rescan=True)

    master = configure_master()
    page_instances = {}
    for path, m in matched:
        page_x, page_y = int(m.group(1)), int(m.group(2))
        texture = import_texture(path)
        instance = make_page_instance(master, texture, page_x, page_y)
        page_instances[f"{page_x:02d}_{page_y:02d}"] = instance
    report_line(f"IMPORT+MI: {len(page_instances)} страниц")

    applied = retexture_map(page_instances)
    report_line(f"MAP MATERIALS: переназначено {applied}/{EXPECTED_PAGES}")
    report_line("ИТОГО PASS: рельеф Garner перешёл на запечённое альбедо")


report_ref = Reporter("apply_garner_terrain_albedo")
try:
    main()
except Exception as exc:                             # noqa: BLE001
    report_ref.exception(exc)
    raise
finally:
    report_ref.close()
