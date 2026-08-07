"""Осматривает уровень: что на нём есть и где это лежит.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_world.py [карта]"

Отчёт: `Scripts/reports/check_world.txt`.

Нужен, когда игра показывает пустое небо: причин у этого две — либо на уровне
нет геометрии, либо игрок оказался вне её границ. Различить их можно только
сравнив положение игрока с границами того, что на уровне лежит.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402


def main(report):
    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    level = f"/Game/Maps/{map_name}"

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    report.line(f"уровень {level}: актёров {len(actors)}")

    kinds = {}
    terrain_bounds = None
    instance_bounds = None
    instances_total = 0

    for actor in actors:
        cls = actor.get_class().get_name()
        kinds[cls] = kinds.get(cls, 0) + 1

        label = actor.get_actor_label()
        origin, extent = actor.get_actor_bounds(only_colliding_components=False)

        if label.startswith("Terrain_"):
            lo = (origin.x - extent.x, origin.y - extent.y, origin.z - extent.z)
            hi = (origin.x + extent.x, origin.y + extent.y, origin.z + extent.z)
            terrain_bounds = merge(terrain_bounds, lo, hi)

        for component in actor.get_components_by_class(
                unreal.HierarchicalInstancedStaticMeshComponent):
            count = component.get_instance_count()
            instances_total += count
            if count > 0:
                lo = (origin.x - extent.x, origin.y - extent.y, origin.z - extent.z)
                hi = (origin.x + extent.x, origin.y + extent.y, origin.z + extent.z)
                instance_bounds = merge(instance_bounds, lo, hi)

    report.line("классы актёров: " + ", ".join(
        f"{k}: {v}" for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:8]))
    report.line(f"инстансов в компонентах: {instances_total}")
    describe(report, "рельеф", terrain_bounds)
    describe(report, "объекты", instance_bounds)

    if terrain_bounds is None:
        report.error("ПРОВАЛ: на уровне нет ни одного актёра рельефа")
    if instances_total == 0:
        report.error("ПРОВАЛ: на уровне нет размещённых объектов")


def merge(current, lo, hi):
    if current is None:
        return [list(lo), list(hi)]
    for i in range(3):
        current[0][i] = min(current[0][i], lo[i])
        current[1][i] = max(current[1][i], hi[i])
    return current


def describe(report, title, bounds):
    if bounds is None:
        report.warn(f"{title}: не найдено")
        return
    lo, hi = bounds
    report.line(f"{title}: X {lo[0]:.0f}..{hi[0]:.0f}, "
                f"Y {lo[1]:.0f}..{hi[1]:.0f}, Z {lo[2]:.0f}..{hi[2]:.0f}")


report = Reporter("check_world")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
