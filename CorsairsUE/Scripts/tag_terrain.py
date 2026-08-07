"""Помечает плитки рельефа тегом, видимым в игре.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/tag_terrain.py [карта ...]"

Без аргументов обрабатывает все карты. Отчёт: `Scripts/reports/tag_terrain.txt`.

Метка актёра (`Terrain_…`) существует только в редакторе, а в игре её нет.
Поэтому отличить землю от построек в рантайме можно лишь тегом — он живёт в
собранном уровне. Нужно это, чтобы ставить персонажа на грунт, а не на крышу
ближайшего дома.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402

TERRAIN_TAG = "CorsairsTerrain"


def process(report, level_name):
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(
        f"/Game/Maps/{level_name}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

    tagged = 0
    for actor in actors:
        if not actor.get_actor_label().startswith("Terrain_"):
            continue
        tags = list(actor.tags)
        if unreal.Name(TERRAIN_TAG) in tags:
            continue
        tags.append(unreal.Name(TERRAIN_TAG))
        actor.set_editor_property("tags", tags)
        tagged += 1

    if tagged:
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    return tagged


def main(report):
    if RefuseIfEditorOpen(report):
        return

    only = set(sys.argv[1:])
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    levels = sorted(str(a.asset_name)
                    for a in registry.get_assets_by_path(unreal.Name("/Game/Maps"),
                                                         recursive=True)
                    if str(a.asset_class_path.asset_name) == "World")

    total = 0
    touched = 0
    for name in levels:
        if only and name not in only:
            continue
        tagged = process(report, name)
        if tagged:
            report.line(f"  {name}: помечено {tagged}")
        total += tagged
        touched += 1

    report.line(f"УРОВНЕЙ ОБРАБОТАНО: {touched}, ПЛИТОК ПОМЕЧЕНО: {total}")
    if total == 0:
        report.warn("новых плиток не нашлось — возможно, все уже помечены")


report = Reporter("tag_terrain")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
