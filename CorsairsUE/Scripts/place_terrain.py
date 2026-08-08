"""Ставит плитки рельефа на уровень.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/place_terrain.py <карта> <путь-уровня>"

Пример:
    ... Scripts/place_terrain.py garner /Game/Maps/Garner

Отчёт: `Scripts/reports/place_terrain.txt`.

Плитки уже несут мировые координаты в вершинах, поэтому ставятся в начало
координат без сдвига — как и актёры объектов. Скрипт добавляет их к
существующему уровню, а не создаёт новый: объекты расставляются отдельно и
терять их незачем.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402
from reference_terrain_rules import assert_legacy_target_allowed  # noqa: E402

TERRAIN_ROOT = "/Game/Terrain"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    if len(sys.argv) < 3:
        report.error("нужны аргументы: <карта> <путь-уровня>")
        return

    map_name, level_path = sys.argv[1], sys.argv[2]

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.load_level(level_path)
    report.line(f"уровень: {level_path}")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # Прежние плитки убираются: иначе повторный запуск наложит вторую копию
    # рельефа поверх первой, и получится дрожание граней.
    removed = 0
    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label().startswith("Terrain_"):
            assert_legacy_target_allowed(
                "place_terrain", actor.get_path_name())
            actor_subsystem.destroy_actor(actor)
            removed += 1
    if removed:
        report.line(f"убрано прежних плиток: {removed}")

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(unreal.Name(TERRAIN_ROOT), recursive=True)

    placed = 0
    skipped = 0
    for asset in assets:
        name = str(asset.asset_name)
        if not name.startswith(f"{map_name}_terrain_"):
            continue

        mesh = unreal.load_asset(str(asset.package_name) + "." + name)
        if not isinstance(mesh, unreal.StaticMesh):
            skipped += 1
            continue

        assert_legacy_target_allowed("place_terrain", level_path)
        actor = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0))
        if actor is None:
            skipped += 1
            continue

        assert_legacy_target_allowed(
            "place_terrain", str(asset.package_name))
        actor.static_mesh_component.set_static_mesh(mesh)
        assert_legacy_target_allowed("place_terrain", actor.get_path_name())
        actor.set_actor_label(f"Terrain_{name}")
        placed += 1

    subsystem.save_current_level()

    report.line(f"ПЛИТОК РАССТАВЛЕНО {placed}")
    if skipped:
        report.warn(f"пропущено ассетов: {skipped}")
    if placed == 0:
        report.error(f"ПРОВАЛ: в {TERRAIN_ROOT} не нашлось плиток для карты {map_name}")


report = Reporter("place_terrain")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
