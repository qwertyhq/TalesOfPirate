"""Проверяет, отрисовывается ли рельеф на уровне.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_terrain_visible.py [карта]"

Отчёт: `Scripts/reports/check_terrain_visible.txt`.

«Плитки на уровне есть» и «землю видно» — разные утверждения. Между ними
помещается скрытый компонент, пустой меш и прозрачный материал, и в игре все
три выглядят одинаково: под ногами пусто.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402


def main(report):
    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(
        f"/Game/Maps/{map_name}")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    tiles = [a for a in actors if a.get_actor_label().startswith("Terrain_")]
    report.line(f"плиток рельефа на уровне: {len(tiles)}")

    hidden = 0
    empty = 0
    no_material = 0
    grid_material = 0
    checked = 0
    samples = []

    for actor in tiles:
        component = actor.static_mesh_component
        mesh = component.static_mesh
        if mesh is None:
            empty += 1
            continue

        checked += 1
        if actor.is_hidden_ed() or not component.is_visible():
            hidden += 1

        triangles = mesh.get_num_triangles(0)
        if triangles == 0:
            empty += 1

        materials = component.get_materials()
        if not materials or materials[0] is None:
            no_material += 1
            name = "нет"
        else:
            name = materials[0].get_name()
            if "WorldGrid" in name:
                grid_material += 1

        nanite = mesh.get_editor_property("nanite_settings")
        collision = component.get_collision_enabled()
        if len(samples) < 4:
            origin, extent = actor.get_actor_bounds(only_colliding_components=False)
            samples.append(
                f"{actor.get_actor_label()}: треугольников {triangles}, материал {name}, "
                f"центр ({origin.x:.0f}, {origin.y:.0f}, {origin.z:.0f}), "
                f"полуразмер ({extent.x:.0f}, {extent.y:.0f}, {extent.z:.0f}), "
                f"Nanite {'вкл' if nanite.enabled else 'выкл'}, столкновения {collision}")

    report.line(f"проверено: {checked}, скрытых: {hidden}, пустых: {empty}, "
                f"без материала: {no_material}, с сеткой-заглушкой: {grid_material}")
    for line in samples:
        report.line("  " + line)

    if not tiles:
        report.error("ПРОВАЛ: плиток рельефа на уровне нет")
    elif hidden == checked:
        report.error("ПРОВАЛ: все плитки скрыты")
    elif empty == len(tiles):
        report.error("ПРОВАЛ: у всех плиток пустой меш")


report = Reporter("check_terrain_visible")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
