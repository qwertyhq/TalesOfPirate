"""Проверяет, что расставленные объекты действительно попали на уровень.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_placement.py /Game/Maps/Garner"

Отчёт: `Scripts/reports/check_placement.txt`.

Спрашивает у компонентов их собственное число инстансов и границы, а не
пересчитывает то, что расстановка собиралась добавить. Разница существенна:
`place_objects.py` считал длину списка трансформаций на входе, и молчаливый
отказ `add_instance` выглядел бы как полный успех.

Границы уровня печатаются отдельно: если инстансы есть, но все стоят в начале
координат, размер мира выдаст это сразу.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402


def main(report):
    level_path = sys.argv[1] if len(sys.argv) > 1 else "/Game/Maps/Garner"

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.load_level(level_path)
    report.line(f"уровень: {level_path}")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_subsystem.get_all_level_actors()
    report.line(f"актёров на уровне: {len(actors)}")

    total_instances = 0
    instanced_actors = 0
    empty_actors = []
    skeletal_actors = 0
    no_mesh = []

    min_xyz = [float("inf")] * 3
    max_xyz = [float("-inf")] * 3

    for actor in actors:
        hism = actor.get_component_by_class(
            unreal.HierarchicalInstancedStaticMeshComponent)
        if hism is not None:
            instanced_actors += 1
            count = hism.get_instance_count()
            total_instances += count
            if count == 0:
                empty_actors.append(actor.get_actor_label())
            if hism.static_mesh is None:
                no_mesh.append(actor.get_actor_label())

            # Границы берём по инстансам, а не по актёру: актёр стоит в нуле,
            # и его собственное положение о размахе мира ничего не говорит.
            for index in range(min(count, 200)):
                transform = hism.get_instance_transform(index, True)
                location = transform.translation
                for axis, value in enumerate((location.x, location.y, location.z)):
                    min_xyz[axis] = min(min_xyz[axis], value)
                    max_xyz[axis] = max(max_xyz[axis], value)
            continue

        if actor.get_component_by_class(unreal.SkeletalMeshComponent) is not None:
            skeletal_actors += 1

    report.line(f"актёров с инстансами: {instanced_actors}")
    report.line(f"актёров со скелетным мешем: {skeletal_actors}")
    report.line(f"ИНСТАНСОВ ВСЕГО: {total_instances}")

    if min_xyz[0] != float("inf"):
        size = [max_xyz[i] - min_xyz[i] for i in range(3)]
        report.line(
            f"размах мира (по выборке инстансов): "
            f"X {min_xyz[0]:.0f}..{max_xyz[0]:.0f}, "
            f"Y {min_xyz[1]:.0f}..{max_xyz[1]:.0f}, "
            f"Z {min_xyz[2]:.0f}..{max_xyz[2]:.0f}")
        report.line(f"то есть {size[0] / 100000:.1f} x {size[1] / 100000:.1f} км")

    if empty_actors:
        report.warn(f"БЕЗ ИНСТАНСОВ актёров={len(empty_actors)}: {empty_actors[:5]}")
    if no_mesh:
        report.warn(f"БЕЗ МЕША актёров={len(no_mesh)}: {no_mesh[:5]}")

    if total_instances == 0:
        report.error("ПРОВАЛ: на уровне нет ни одного инстанса")
    else:
        report.line("УСПЕХ: инстансы на месте")


report = Reporter("check_placement")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
