"""Снимает эталонную страницу земли сверху и снизу — замер обхода треугольников.

Обход треугольников после Interchange никем не измерялся: писатели рельефа
разворачивают порядок индексов заранее, считая, что движок этого не делает, а
проверить было нечем. Материал страницы односторонний, поэтому вопрос решается
кадром: земля обязана быть видна сверху и не видна снизу. Обратная картина
означает, что предкомпенсация в ``TerrainMeshWriter.cpp`` и
``TerrainPageMeshWriter.cpp`` лишняя.

Считать обход по данным меша нельзя: у страницы включён Nanite, и LOD0 — это
грубый fallback в 251 треугольник вместо 32768, то есть уже не те треугольники,
которые рисуются.

Запуск — только полноценным редактором: коммандлет ``-run=pythonscript`` не
крутит Slate, HighResShot в нём не срабатывает и молчит.

    UnrealEditor CorsairsUE/CorsairsUE.uproject \
        -ExecutePythonScript="CorsairsUE/Scripts/check_terrain_winding.py" \
        -TerrainWindingShots=<абсолютный каталог> -nosplash -nop4 -NoSound
"""

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import unreal

from report import Reporter
from scene_capture_validation import validate_scene_progress_png


MAP_PACKAGE = "/Game/Maps/GarnerSceneProgressCity"
REFERENCE_LABEL = "ReferenceTerrain_Garner_17_21"
REFERENCE_TAG = "CorsairsReferenceTerrain"
PROBE_CAMERA_LABEL = "TerrainWindingProbe_Camera"
SHOT_WIDTH = 1280
SHOT_HEIGHT = 720
# 60 м над и под плоскостью страницы: страница 128 м, при вертикальном FOV
# такой высоты хватает, чтобы в кадр попала только земля.
PROBE_DISTANCE_CM = 6000.0


def command_parameter(name):
    _tokens, _switches, parameters = unreal.SystemLibrary.parse_command_line(
        unreal.SystemLibrary.get_command_line())
    value = parameters.get(name)
    if not value:
        raise RuntimeError(f"нет -{name}=<абсолютный каталог>")
    directory = Path(value)
    if not directory.is_absolute() or not directory.is_dir():
        raise RuntimeError(f"каталог снимков не найден: {directory}")
    return directory


def reference_terrain_actor():
    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    matches = [actor for actor in actors
               if actor.get_actor_label() == REFERENCE_LABEL
               or REFERENCE_TAG in {str(tag) for tag in actor.tags}]
    if len(matches) != 1:
        raise RuntimeError(
            f"эталонная страница земли: ожидался 1 actor, получено {len(matches)}")
    return matches[0]


def hide_everything_but(keep):
    """Прячет всю геометрию, кроме страницы земли.

    Без этого сверху виден город, а не земля, и кадр ничего не доказывает.
    Скрытие живёт только в редакторе и в карту не сохраняется — уровень после
    замера не сохраняется вовсе.
    """
    hidden = 0
    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    for actor in actors:
        if actor == keep:
            continue
        if isinstance(actor, (unreal.StaticMeshActor, unreal.SkeletalMeshActor)):
            actor.set_is_temporarily_hidden_in_editor(True)
            hidden += 1
        elif isinstance(actor, unreal.ExponentialHeightFog):
            actor.set_is_temporarily_hidden_in_editor(True)
            hidden += 1
    return hidden


class WindingProbe:
    def __init__(self, directory):
        self.directory = directory
        self.report = Reporter("check_terrain_winding")
        self.shots = [
            ("сверху", PROBE_DISTANCE_CM, -90.0,
             directory / "terrain-page-from-above.png"),
            ("снизу", -PROBE_DISTANCE_CM, 90.0,
             directory / "terrain-page-from-below.png"),
        ]
        for _name, _height, _pitch, path in self.shots:
            if path.exists():
                path.unlink()
        self.index = 0
        self.tick = 0
        self.stage_start = 0
        self.prepared = False
        self.requested_at = None
        self.last_size = None
        self.stable = 0
        self.camera = None
        self.centre = None
        self.handle = unreal.register_slate_post_tick_callback(self.on_tick)

    def finish(self):
        unreal.unregister_slate_post_tick_callback(self.handle)
        self.report.close()
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    def fail(self, message):
        self.report.error(message)
        self.finish()
        raise RuntimeError(message)

    def prepare(self):
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not levels.load_level(MAP_PACKAGE):
            self.fail(f"не загружена карта {MAP_PACKAGE}")
        terrain = reference_terrain_actor()
        origin, extent = terrain.get_actor_bounds(False)
        self.centre = origin
        self.report.line(
            f"СТРАНИЦА: centre=({origin.x:.0f},{origin.y:.0f},{origin.z:.0f}) "
            f"extent=({extent.x:.0f},{extent.y:.0f},{extent.z:.0f})")
        component = terrain.static_mesh_component
        material = component.get_material(0)
        base = material.get_editor_property("parent") if material else None
        two_sided = base.get_editor_property("two_sided") if base else None
        if two_sided is not False:
            self.fail(
                f"материал страницы two_sided={two_sided}: замер обхода кадром "
                "имеет смысл только на односторонней земле")
        self.report.line("МАТЕРИАЛ: односторонний, обход виден в кадре")
        hidden = hide_everything_but(terrain)
        self.report.line(f"СКРЫТО ПОСТОРОННЕЙ ГЕОМЕТРИИ: {hidden}")

        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        self.camera = actors.spawn_actor_from_class(
            unreal.CameraActor, origin, unreal.Rotator())
        if self.camera is None:
            self.fail("не создана камера замера")
        self.camera.set_actor_label(PROBE_CAMERA_LABEL)

    def aim(self):
        _name, height, pitch, _path = self.shots[self.index]
        location = unreal.Vector(
            self.centre.x, self.centre.y, self.centre.z + height)
        self.camera.set_actor_location(location, False, False)
        self.camera.set_actor_rotation(
            unreal.Rotator(roll=0.0, pitch=pitch, yaw=0.0), False)
        unreal.EditorLevelLibrary.pilot_level_actor(self.camera)

    def request(self):
        _name, _height, _pitch, path = self.shots[self.index]
        world = unreal.EditorLevelLibrary.get_editor_world()
        unreal.SystemLibrary.execute_console_command(
            world, f"HighResShot {SHOT_WIDTH}x{SHOT_HEIGHT} filename={path}")
        self.requested_at = self.tick
        self.last_size = None
        self.stable = 0

    def on_tick(self, _delta_time):
        self.tick += 1
        # Уровень готовится заново перед каждым снимком. Без перезагрузки
        # второй HighResShot просто не срабатывает: файл не появляется вовсе, и
        # замер упирается в таймаут (проверено 2026-08-10). Уровень не
        # сохраняется, поэтому повтор безвреден.
        if not self.prepared:
            if self.tick >= 5:
                self.prepare()
                self.prepared = True
                self.stage_start = self.tick
            return

        step = self.tick - self.stage_start
        if step == 15:
            self.aim()
            return
        if step == 35:
            unreal.AutomationUtilsBlueprintLibrary.finish_all_asset_compilation()
            return
        if step == 55:
            self.request()
            return
        if self.requested_at is None:
            return

        name, _height, _pitch, path = self.shots[self.index]
        if path.is_file():
            size = path.stat().st_size
            if size > 0 and size == self.last_size:
                self.stable += 1
            else:
                self.stable = 0
                self.last_size = size
            if self.stable >= 10:
                width, height = validate_scene_progress_png(
                    path, SHOT_WIDTH, SHOT_HEIGHT)
                self.report.line(
                    f"СНИМОК {name}: {path} {width}x{height} байт={size}")
                self.index += 1
                self.requested_at = None
                if self.index >= len(self.shots):
                    self.report.line(
                        "ГОТОВО: сравнить кадры — земля обязана быть видна "
                        "сверху и не видна снизу")
                    self.finish()
                    return
                self.prepared = False
                self.stage_start = self.tick
                return
        if self.tick - self.requested_at > 900:
            self.fail(f"снимок не дождался: {path}")


unreal.EditorPythonScripting.set_keep_python_script_alive(True)
PROBE = WindingProbe(command_parameter("TerrainWindingShots"))
