"""Делает расставленный уровень пригодным для просмотра: свет и камера.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_level_view.py /Game/Maps/Garner"

Отчёт: `Scripts/reports/setup_level_view.txt`.

Расстановка создаёт уровень с нуля, а новый уровень пуст по свету — открытая
карта выглядит чёрной независимо от того, стоят объекты или нет. Отличить
«темно» от «пусто» на глаз невозможно, поэтому свет ставится сразу.

Камера — вторая половина той же задачи. Карта garner около четырёх километров
в поперечнике, а камера редактора стартует в начале координат, где объектов
нет. Скрипт вычисляет середину занятой области по инстансам и ставит камеру
над ней.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

# Сколько инстансов у каждого компонента опрашивать для оценки границ.
# Полный обход сотни тысяч трансформаций ничего не уточнит, а времени займёт
# заметно больше.
BOUNDS_SAMPLE = 50

# Высота камеры над центром мира и наклон вниз.
CAMERA_HEIGHT = 60000.0
CAMERA_PITCH = -35.0


def world_center(report):
    """Середина занятой объектами области по выборке инстансов."""
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    min_xyz = [float("inf")] * 3
    max_xyz = [float("-inf")] * 3
    sampled = 0

    for actor in actor_subsystem.get_all_level_actors():
        hism = actor.get_component_by_class(
            unreal.HierarchicalInstancedStaticMeshComponent)
        if hism is None:
            continue
        count = hism.get_instance_count()
        for index in range(min(count, BOUNDS_SAMPLE)):
            location = hism.get_instance_transform(index, True).translation
            for axis, value in enumerate((location.x, location.y, location.z)):
                min_xyz[axis] = min(min_xyz[axis], value)
                max_xyz[axis] = max(max_xyz[axis], value)
            sampled += 1

    if sampled == 0:
        report.warn("инстансов не нашлось, камера останется в начале координат")
        return unreal.Vector(0.0, 0.0, 0.0)

    center = unreal.Vector(
        (min_xyz[0] + max_xyz[0]) / 2.0,
        (min_xyz[1] + max_xyz[1]) / 2.0,
        (min_xyz[2] + max_xyz[2]) / 2.0)
    report.line(
        f"занятая область: X {min_xyz[0]:.0f}..{max_xyz[0]:.0f}, "
        f"Y {min_xyz[1]:.0f}..{max_xyz[1]:.0f} (по {sampled} инстансам)")
    report.line(f"центр: ({center.x:.0f}, {center.y:.0f}, {center.z:.0f})")
    return center


def spawn_once(report, actor_class, label):
    """Ставит актёра, если такого на уровне ещё нет.

    Повторный запуск скрипта не должен плодить вторые солнца.
    """
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for existing in actor_subsystem.get_all_level_actors():
        if isinstance(existing, actor_class):
            report.line(f"{label}: уже есть")
            return existing

    actor = actor_subsystem.spawn_actor_from_class(
        actor_class, unreal.Vector(0.0, 0.0, 100000.0), unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        report.warn(f"{label}: не создан")
        return None

    actor.set_actor_label(label)
    report.line(f"{label}: добавлен")
    return actor


def main(report):
    level_path = sys.argv[1] if len(sys.argv) > 1 else "/Game/Maps/Garner"

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.load_level(level_path)
    report.line(f"уровень: {level_path}")

    sun = spawn_once(report, unreal.DirectionalLight, "Sun")
    if sun is not None:
        # Наклон вниз и вбок: отвесный свет делает вертикальные стены
        # одинаково плоскими, и форма зданий не читается.
        sun.set_actor_rotation(unreal.Rotator(0.0, -45.0, 30.0), False)

    spawn_once(report, unreal.SkyAtmosphere, "SkyAtmosphere")
    spawn_once(report, unreal.SkyLight, "SkyLight")

    center = world_center(report)

    camera_location = unreal.Vector(center.x, center.y, center.z + CAMERA_HEIGHT)
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(
        camera_location, unreal.Rotator(0.0, CAMERA_PITCH, 0.0))
    report.line(
        f"камера: ({camera_location.x:.0f}, {camera_location.y:.0f}, "
        f"{camera_location.z:.0f}), наклон {CAMERA_PITCH}")

    subsystem.save_current_level()
    report.line("УСПЕХ: уровень сохранён")


report = Reporter("setup_level_view")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
