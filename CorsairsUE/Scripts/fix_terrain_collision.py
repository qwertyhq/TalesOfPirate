"""Даёт плиткам рельефа осязаемую поверхность.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/fix_terrain_collision.py"

Отчёт: `Scripts/reports/fix_terrain_collision.txt`.

Плитки видны, но сквозь них проваливается всё: у меша нет коллизионной
геометрии, а включённый Nanite её и не строит — луч трассировки проходит
насквозь, и персонаж встаёт на первую попавшуюся крышу.

Лечится двумя правками: рельефу выключается Nanite (он ему не нужен — это
пологая сетка без мелких деталей) и включается использование самой геометрии
как коллизии.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402

TERRAIN_ROOT = "/Game/Terrain"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = [a for a in registry.get_assets_by_path(unreal.Name(TERRAIN_ROOT), recursive=True)
              if str(a.asset_class_path.asset_name) == "StaticMesh"]
    report.line(f"плиток найдено: {len(assets)}")

    fixed = 0
    already = 0
    rebuild_warned = []
    for asset in assets:
        mesh = unreal.load_asset(f"{asset.package_name}.{asset.asset_name}")
        if not isinstance(mesh, unreal.StaticMesh):
            continue

        changed = False

        nanite = mesh.get_editor_property("nanite_settings")
        if nanite.enabled:
            nanite.enabled = False
            mesh.set_editor_property("nanite_settings", nanite)
            changed = True

        body = mesh.get_editor_property("body_setup")
        if body is not None:
            # «Сложная как простая»: коллизией служит сама видимая сетка.
            # Отдельных примитивов у рельефа нет и быть не может — форма
            # произвольная.
            if body.get_editor_property("collision_trace_flag") != \
                    unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE:
                body.set_editor_property(
                    "collision_trace_flag",
                    unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
                changed = True

        # Смена настроек сама по себе физику не пересоберёт: данные для неё
        # строятся при сборке меша, а её надо запросить. Правка параметров
        # уровня детализации — единственный способ сделать это из Python.
        try:
            subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
            build = subsystem.get_lod_build_settings(mesh, 0)
            build.recompute_normals = build.recompute_normals
            subsystem.set_lod_build_settings(mesh, 0, build)
            changed = True
        except Exception as exc:                        # noqa: BLE001
            if not rebuild_warned:
                report.warn(f"пересборка недоступна: {exc}")
                rebuild_warned.append(True)

        if changed:
            unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False)
            fixed += 1
        else:
            already += 1

    report.line(f"ПЛИТОК ИСПРАВЛЕНО: {fixed}")
    report.line(f"  уже настроены: {already}")
    if fixed == 0 and already == 0:
        report.error("ПРОВАЛ: плиток рельефа не нашлось")


report = Reporter("fix_terrain_collision")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
