"""Проверяет точную reference-страницу земли на owned-карте города."""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402


MAP_PACKAGE = "/Game/Maps/GarnerSceneProgressCity"
REFERENCE_LABEL = "ReferenceTerrain_Garner_17_21"
REFERENCE_TAG = "CorsairsReferenceTerrain"
HIDDEN_OVERLAP_TAG = "CorsairsReferenceTerrainHiddenOverlap"
REFERENCE_MESH = "/Game/Terrain/Reference/Garner/SM_Garner_17_21"
REFERENCE_INSTANCE = "/Game/Terrain/Reference/Garner/MI_Garner_17_21"
REFERENCE_LOCATION = (217600.0, -268800.0, 0.0)
REFERENCE_BOUNDS = (217600.0, -281600.0, 230400.0, -268800.0)


def asset_package_path(asset):
    if asset is None:
        return ""
    object_path = str(asset.get_path_name())
    package_path, separator, object_name = object_path.rpartition(".")
    if separator and package_path.rsplit("/", 1)[-1] == object_name:
        return package_path
    return object_path


def actor_bounds_xy(actor):
    origin, extent = actor.get_actor_bounds(False)
    return (
        float(origin.x - extent.x), float(origin.y - extent.y),
        float(origin.x + extent.x), float(origin.y + extent.y),
    )


def bounds_overlap(left, right):
    return (left[2] > right[0] and left[0] < right[2]
            and left[3] > right[1] and left[1] < right[3])


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def main(report):
    require(not RefuseIfEditorOpen(report), "редактор уже открыт")
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    require(levels.load_level(MAP_PACKAGE), f"не загружена карта {MAP_PACKAGE}")

    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    references = [
        actor for actor in actors
        if actor.get_actor_label() == REFERENCE_LABEL
        or REFERENCE_TAG in {str(tag) for tag in actor.tags}
    ]
    require(
        len(references) == 1,
        f"REFERENCE_TERRAIN: ожидался 1 actor, получено {len(references)}")

    reference = references[0]
    component = reference.static_mesh_component
    require(
        asset_package_path(component.static_mesh) == REFERENCE_MESH,
        f"REFERENCE_TERRAIN mesh={asset_package_path(component.static_mesh)}")
    require(
        asset_package_path(component.get_material(0)) == REFERENCE_INSTANCE,
        f"REFERENCE_TERRAIN material={asset_package_path(component.get_material(0))}")
    actual_location = reference.get_actor_location()
    require(
        all(math.isclose(float(getattr(actual_location, axis)), expected,
                         abs_tol=0.01)
            for axis, expected in zip(("x", "y", "z"), REFERENCE_LOCATION)),
        f"REFERENCE_TERRAIN location={actual_location}")
    actual_bounds = actor_bounds_xy(reference)
    require(
        all(math.isclose(actual, expected, abs_tol=1.0)
            for actual, expected in zip(actual_bounds, REFERENCE_BOUNDS)),
        f"REFERENCE_TERRAIN bounds={actual_bounds}")

    overlapping = [
        actor for actor in actors
        if actor != reference
        and actor.get_actor_label().startswith("Terrain_")
        and bounds_overlap(actor_bounds_xy(actor), REFERENCE_BOUNDS)
    ]
    visible = [
        actor.get_actor_label() for actor in overlapping
        if not actor.is_hidden_ed()
        or HIDDEN_OVERLAP_TAG not in {str(tag) for tag in actor.tags}
    ]
    require(not visible, f"VISIBLE_LEGACY_OVERLAPS: {visible}")
    report.line(
        f"PASS: reference=1 overlaps={len(overlapping)} visibleLegacyOverlaps=0")


report = Reporter("verify_scene_progress_terrain")
try:
    main(report)
except Exception as exc:                         # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
