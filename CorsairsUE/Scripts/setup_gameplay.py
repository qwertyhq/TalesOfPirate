"""Готовит уровень к игре: режим, точка появления и оси ввода.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_gameplay.py /Game/Maps/Garner"

Отчёт: `Scripts/reports/setup_gameplay.txt`.

Расстановка и импорт готовят геометрию, но уровень без PlayerStart и режима
игры не запускается: движок ставит игрока в начало координат, где у нас пусто,
и берёт стандартный пешеход вместо нашего персонажа.

Оси ввода живут в Config/DefaultInput.ini и правятся отдельно: в UE 5.8
устаревший ввод не выставлен в Python — InputSettings не отдаёт axis_mappings.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

def place_player_start(report, center):
    """Ставит точку появления над центром города."""
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    for actor in actor_subsystem.get_all_level_actors():
        if isinstance(actor, unreal.PlayerStart):
            report.line("точка появления: уже есть")
            return actor

    # Появление приподнято над рельефом: точная высота земли в этой точке
    # неизвестна, а падение с запасом безопаснее застревания в грунте.
    location = unreal.Vector(center.x, center.y, center.z + 2000.0)
    actor = actor_subsystem.spawn_actor_from_class(
        unreal.PlayerStart, location, unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        report.warn("точка появления не создана")
        return None

    actor.set_actor_label("PlayerStart")
    report.line(f"точка появления: ({location.x:.0f}, {location.y:.0f}, {location.z:.0f})")
    return actor


def world_center(report):
    """Середина занятой объектами области."""
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    min_xyz = [float("inf")] * 3
    max_xyz = [float("-inf")] * 3
    sampled = 0

    for actor in actor_subsystem.get_all_level_actors():
        hism = actor.get_component_by_class(
            unreal.HierarchicalInstancedStaticMeshComponent)
        if hism is None:
            continue
        for index in range(min(hism.get_instance_count(), 50)):
            location = hism.get_instance_transform(index, True).translation
            for axis, value in enumerate((location.x, location.y, location.z)):
                min_xyz[axis] = min(min_xyz[axis], value)
                max_xyz[axis] = max(max_xyz[axis], value)
            sampled += 1

    if sampled == 0:
        report.warn("инстансов не нашлось, центр принят за начало координат")
        return unreal.Vector(0.0, 0.0, 0.0)

    return unreal.Vector((min_xyz[0] + max_xyz[0]) / 2.0,
                         (min_xyz[1] + max_xyz[1]) / 2.0,
                         (min_xyz[2] + max_xyz[2]) / 2.0)


def main(report):
    if RefuseIfEditorOpen(report):
        return

    level_path = sys.argv[1] if len(sys.argv) > 1 else "/Game/Maps/Garner"

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.load_level(level_path)
    report.line(f"уровень: {level_path}")

    place_player_start(report, world_center(report))

    # Режим игры задаётся на уровне: так осмотр других карт не тянет за собой
    # подключение к серверу.
    world_settings = unreal.EditorLevelLibrary.get_editor_world().get_world_settings()
    game_mode = unreal.load_class(None, "/Script/CorsairsGame.CorsairsGameMode")
    if game_mode is None:
        report.error("класс режима игры не найден — проект собран?")
        return
    world_settings.set_editor_property("default_game_mode", game_mode)
    report.line("режим игры: CorsairsGameMode")

    subsystem.save_current_level()
    report.line("УСПЕХ: уровень сохранён")


report = Reporter("setup_gameplay")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
