"""Назначает материал плиткам рельефа прямо на уровне.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/apply_terrain_material.py [карта]"

Отчёт: `Scripts/reports/apply_terrain_material.txt`.

Материал, записанный в сам ассет меша, до уровня не доехал: компонент актёра
держит собственный список материалов, выставленный при размещении, и он
перекрывает материал ассета. Поэтому здесь материал ставится компоненту —
единственному месту, откуда его берёт отрисовка.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402
from reference_terrain_rules import assert_legacy_target_allowed  # noqa: E402

MATERIAL_PATH = "/Game/Terrain/Materials"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    table_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "Data", "terrain_tile_textures.json")
    with open(table_path, "r", encoding="utf-8") as handle:
        tiles = json.load(handle).get("tiles", {})

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(
        f"/Game/Maps/{map_name}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

    cache = {}
    applied = 0
    missing_entry = []
    missing_material = []

    for actor in actors:
        label = actor.get_actor_label()
        if not label.startswith("Terrain_"):
            continue

        # Метка актёра — «Terrain_» плюс имя ассета, а таблица ключуется
        # именем ассета.
        asset_name = label[len("Terrain_"):]
        texture_name = tiles.get(asset_name)
        if texture_name is None:
            missing_entry.append(asset_name)
            continue

        instance = cache.get(texture_name)
        if instance is None:
            instance = unreal.load_asset(f"{MATERIAL_PATH}/MI_{texture_name}")
            cache[texture_name] = instance
        if not isinstance(instance, unreal.MaterialInstanceConstant):
            missing_material.append(texture_name)
            continue

        assert_legacy_target_allowed(
            "apply_terrain_material",
            actor.static_mesh_component.get_static_mesh().get_path_name())
        actor.static_mesh_component.set_material(0, instance)
        applied += 1

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()

    report.line(f"ПЛИТОК С МАТЕРИАЛОМ: {applied}")
    if missing_entry:
        report.warn(f"нет записи в таблице у {len(missing_entry)}: {missing_entry[:5]}")
    if missing_material:
        report.warn(f"нет материала для текстур: {sorted(set(missing_material))[:5]}")
    if applied == 0:
        report.error("ПРОВАЛ: ни одной плитке материал не назначен")


report = Reporter("apply_terrain_material")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
