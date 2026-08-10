"""Плоскость моря для Garner и плотность тумана — как в оригинале.

Оригинал не рисует рельеф в «отсутствующих» клетках: там на нулевой высоте
рисуется полупрозрачная плоскость моря (`MPMap::RenderSea`, уровень —
`SEA_LEVEL 0.0f` в MPTile.h, цвет по умолчанию `ARGB(0xcf, 140, 140, 220)`).
Наш бейк альбедо честно оставил эти клетки чёрными, и без моря они выглядят
дырами в мире — включая целый «чёрный угол» на юго-востоке города.

Заодно ослабляется туман: с плотностью 0.008 кадр выбеливается настолько,
что доля тёплых пикселей падает впятеро против эталона живого клиента
(зафиксировано на сверке этапа 1). Оригинальная дымка едва заметна.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_garner_sea.py"

Отчёт: `Scripts/reports/setup_garner_sea.txt`.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402

LEVEL_PATH = "/Game/Maps/Garner"
SEA_LABEL = "GarnerSea"
SEA_MATERIAL_DIR = "/Game/Terrain"
SEA_MATERIAL_NAME = "M_GarnerSea"

# Карта — 4096 клеток по 100 см. Плоскость Engine/BasicShapes/Plane —
# 100х100 см, значит масштаб равен числу клеток.
MAP_CELLS = 4096
CELL_CM = 100.0
PLANE_CM = 100.0

# Цвет и прозрачность — из MPMap.cpp:74: ARGB(0xcf, 140, 140, 220).
SEA_COLOR = (140.0 / 255.0, 140.0 / 255.0, 220.0 / 255.0)
SEA_OPACITY = 0xCF / 255.0

# Ослабленный туман. Прежние 0.008 давали молочный кадр.
FOG_DENSITY = 0.002


def ensure_sea_material(report):
    """Создаёт (или находит) полупрозрачный unlit-материал моря."""
    path = f"{SEA_MATERIAL_DIR}/{SEA_MATERIAL_NAME}"
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        report.line(f"материал уже есть: {path}")
        return existing

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        SEA_MATERIAL_NAME, SEA_MATERIAL_DIR, unreal.Material,
        unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model",
                                 unreal.MaterialShadingModel.MSM_UNLIT)
    # Море видно и с изнанки: под воду камера в оригинале не ныряет, но
    # прозрачная плоскость с одной стороной выглядит дырой при взгляде снизу.
    material.set_editor_property("two_sided", True)

    library = unreal.MaterialEditingLibrary
    color = library.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -400, -100)
    color.set_editor_property("constant",
                              unreal.LinearColor(*SEA_COLOR, 1.0))
    library.connect_material_property(
        color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    opacity = library.create_material_expression(
        material, unreal.MaterialExpressionConstant, -400, 100)
    opacity.set_editor_property("r", SEA_OPACITY)
    library.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY)

    library.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
    report.line(f"материал создан: {path}")
    return material


def main(report):
    if RefuseIfEditorOpen(report):
        return

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(LEVEL_PATH):
        report.error(f"уровень не открылся: {LEVEL_PATH}")
        return

    material = ensure_sea_material(report)

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # Прежняя плоскость убирается: повторный запуск не должен плодить копии.
    removed = 0
    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label() == SEA_LABEL:
            actor_subsystem.destroy_actor(actor)
            removed += 1
    if removed:
        report.line(f"убрано прежних плоскостей: {removed}")

    plane = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Plane")
    if plane is None:
        report.error("нет /Engine/BasicShapes/Plane")
        return

    # Центр карты в мире: source (204800, 204800) -> поворотом Q в
    # (-204800, 204800). Высота — уровень моря оригинала, ноль.
    half_span = MAP_CELLS * CELL_CM / 2.0
    location = unreal.Vector(-half_span, half_span, 0.0)
    actor = actor_subsystem.spawn_actor_from_class(
        unreal.StaticMeshActor, location, unreal.Rotator(0.0, 0.0, 0.0))
    actor.set_actor_label(SEA_LABEL)
    component = actor.static_mesh_component
    component.set_static_mesh(plane)
    component.set_material(0, material)
    scale = MAP_CELLS * CELL_CM / PLANE_CM
    actor.set_actor_scale3d(unreal.Vector(scale, scale, 1.0))
    # Сквозь воду не ходят и не стреляют трассировкой земли: коллизия
    # плоскости только мешала бы, у оригинала море — чистая картинка.
    component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    report.line(f"море: центр=({location.x:.0f}, {location.y:.0f}, 0) "
                f"масштаб={scale:.0f}")

    fog_fixed = 0
    for actor in actor_subsystem.get_all_level_actors():
        if isinstance(actor, unreal.ExponentialHeightFog):
            fog = actor.get_editor_property("component")
            fog.set_editor_property("fog_density", FOG_DENSITY)
            fog_fixed += 1
    report.line(f"туман: плотность {FOG_DENSITY} у {fog_fixed} актёров")

    subsystem.save_current_level()
    report.line(f"УСПЕХ: {LEVEL_PATH}")


report = Reporter("setup_garner_sea")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
