"""Создаёт материал рельефа и назначает его плиткам.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_terrain_material.py"

Отчёт: `Scripts/reports/setup_terrain_material.txt`.

У плиток рельефа стоит стандартный `WorldGridMaterial` — серая сетка. Здесь
создаётся простой материал с параметром текстуры и по экземпляру на каждую
использованную текстуру; плитке назначается экземпляр с её преобладающей
текстурой.

Почему одна текстура на плитку, а не смешивание слоёв. Слои выгружены
(`<карта>.layers.raw`), и полноценный материал смешивал бы до четырёх текстур
по картам весов. Но карты весов надо ещё построить и разложить по плиткам, а
узловой граф с четырьмя слоями и переходами собирается в редакторе
материалов — из кода получится нечитаемая простыня. Одна преобладающая
текстура уже даёт узнаваемую землю вместо серой сетки, а полноценный материал
собирается поверх этого же набора данных.

Развёртка рельефа задана в клетках карты, поэтому текстура повторяется каждую
клетку — ровно так тайлил её оригинальный движок.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

MATERIAL_PATH = "/Game/Terrain/Materials"
MATERIAL_NAME = "M_Terrain"
TEXTURE_ROOT = "/Game/Terrain/Textures"
TERRAIN_ROOT = "/Game/Terrain"
TEXTURE_PARAM = "Diffuse"


def create_base_material(report):
    """Создаёт материал с параметром текстуры, если его ещё нет."""
    full = f"{MATERIAL_PATH}/{MATERIAL_NAME}"
    existing = unreal.load_asset(full)
    if isinstance(existing, unreal.Material):
        report.line("базовый материал: уже есть")
        return existing

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(MATERIAL_NAME, MATERIAL_PATH,
                                  unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        report.error("базовый материал не создан")
        return None

    library = unreal.MaterialEditingLibrary

    sampler = library.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0)
    sampler.set_editor_property("parameter_name", unreal.Name(TEXTURE_PARAM))

    library.connect_material_property(sampler, "RGB",
                                      unreal.MaterialProperty.MP_BASE_COLOR)

    # Рельеф не блестит: зеркальность в ноль, шероховатость почти в единицу.
    roughness = library.create_material_expression(
        material, unreal.MaterialExpressionConstant, -400, 250)
    roughness.set_editor_property("r", 0.9)
    library.connect_material_property(roughness, "",
                                      unreal.MaterialProperty.MP_ROUGHNESS)

    specular = library.create_material_expression(
        material, unreal.MaterialExpressionConstant, -400, 350)
    specular.set_editor_property("r", 0.0)
    library.connect_material_property(specular, "",
                                      unreal.MaterialProperty.MP_SPECULAR)

    library.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
    report.line("базовый материал: создан")
    return material


def instance_for(report, base, texture_name, cache):
    """Экземпляр материала под конкретную текстуру, по одному на текстуру."""
    if texture_name in cache:
        return cache[texture_name]

    path = f"{MATERIAL_PATH}/MI_{texture_name}"
    instance = unreal.load_asset(path)

    if not isinstance(instance, unreal.MaterialInstanceConstant):
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        instance = tools.create_asset(
            f"MI_{texture_name}", MATERIAL_PATH,
            unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
        if instance is None:
            cache[texture_name] = None
            return None
        unreal.MaterialEditingLibrary.set_material_instance_parent(instance, base)

    texture = unreal.load_asset(f"{TEXTURE_ROOT}/{texture_name}")
    if isinstance(texture, unreal.Texture2D):
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
            instance, unreal.Name(TEXTURE_PARAM), texture)
        unreal.EditorAssetLibrary.save_loaded_asset(instance, only_if_is_dirty=False)
    else:
        report.warn(f"текстуры нет: {texture_name}")

    cache[texture_name] = instance
    return instance


def main(report):
    if RefuseIfEditorOpen(report):
        return

    table_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "Data", "terrain_tile_textures.json")
    if not os.path.exists(table_path):
        report.error(f"нет таблицы плиток: {table_path}")
        return

    with open(table_path, "r", encoding="utf-8") as handle:
        tiles = json.load(handle).get("tiles", {})
    report.line(f"плиток в таблице: {len(tiles)}")

    base = create_base_material(report)
    if base is None:
        return

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    meshes = {str(a.asset_name): a
              for a in registry.get_assets_by_path(unreal.Name(TERRAIN_ROOT),
                                                   recursive=True)
              if str(a.asset_class_path.asset_name) == "StaticMesh"}

    cache = {}
    assigned = 0
    without_entry = 0

    for name, asset in meshes.items():
        texture_name = tiles.get(name)
        if texture_name is None:
            without_entry += 1
            continue

        instance = instance_for(report, base, texture_name, cache)
        if instance is None:
            continue

        mesh = unreal.load_asset(f"{asset.package_name}.{name}")
        if not isinstance(mesh, unreal.StaticMesh):
            continue

        materials = mesh.get_editor_property("static_materials")
        if not materials:
            continue
        materials[0].material_interface = instance
        mesh.set_editor_property("static_materials", materials)
        unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False)
        assigned += 1

    report.line(f"ПЛИТОК С МАТЕРИАЛОМ: {assigned}")
    report.line(f"  экземпляров материала: {len(cache)}")
    if without_entry:
        report.warn(f"без записи в таблице: {without_entry} — остались с сеткой")
    if assigned == 0:
        report.error("ПРОВАЛ: ни одной плитке не назначен материал")


report = Reporter("setup_terrain_material")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
