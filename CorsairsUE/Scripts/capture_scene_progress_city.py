"""Снимает native HighResShot progress-карты без ручного UI-ввода."""

import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import unreal

from scene_capture_validation import validate_scene_progress_png
from scene_coordinate_basis import source_camera_to_ue


SOURCE_CAMERA_EYE = (223325.0, 281975.0, 5100.0)
SOURCE_CAMERA_TARGET = (223325.0, 278475.0, 100.0)
CAMERA_BASIS = source_camera_to_ue(
    eye=SOURCE_CAMERA_EYE, target=SOURCE_CAMERA_TARGET)
CAMERA_LOCATION = unreal.Vector(*CAMERA_BASIS["eye"])
CAMERA_ROTATION = unreal.Rotator(
    roll=0.0, pitch=CAMERA_BASIS["pitch"], yaw=CAMERA_BASIS["yaw"])
SHOT_WIDTH = 1920
SHOT_HEIGHT = 1080
CAMERA_LABEL = "SceneProgressCity_Camera"


def command_parameter(name):
    _tokens, _switches, parameters = unreal.SystemLibrary.parse_command_line(
        unreal.SystemLibrary.get_command_line())
    value = parameters.get(name)
    if not value:
        raise RuntimeError(f"нет -{name}=<absolute.png>")
    return value


class CaptureAfterSettle:
    def __init__(self, output_path):
        self.output_path = Path(output_path)
        if not self.output_path.is_absolute():
            raise RuntimeError("SceneProgressShot должен быть absolute path")
        if self.output_path.suffix.lower() != ".png":
            raise RuntimeError("SceneProgressShot должен оканчиваться на .png")
        if not self.output_path.parent.is_dir():
            raise RuntimeError(
                f"каталог screenshot не существует: {self.output_path.parent}")
        if self.output_path.exists():
            raise RuntimeError(
                f"screenshot уже существует: {self.output_path}")
        self.tick_count = 0
        self.shot_requested = False
        self.last_size = None
        self.stable_ticks = 0
        self.handle = unreal.register_slate_post_tick_callback(self.on_tick)

    def finish(self):
        unreal.unregister_slate_post_tick_callback(self.handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    def on_tick(self, _delta_time):
        self.tick_count += 1
        if self.tick_count == 10:
            world = unreal.EditorLevelLibrary.get_editor_world()
            unreal.SystemLibrary.execute_console_command(
                world, "r.EyeAdaptationQuality 0")
            unreal.SystemLibrary.execute_console_command(
                world, "r.DefaultFeature.AutoExposure 0")
            unreal.log(
                "SCENE_PROGRESS_EXPOSURE eye_adaptation=off "
                "auto_exposure=off")

        if self.tick_count == 30:
            actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
            cameras = [
                actor for actor in actors.get_all_level_actors()
                if actor.get_actor_label() == CAMERA_LABEL
            ]
            if len(cameras) != 1:
                raise RuntimeError(
                    f"camera {CAMERA_LABEL}: ожидалась 1, получено {len(cameras)}")
            camera = cameras[0]
            camera.set_actor_location(CAMERA_LOCATION, False, False)
            camera.set_actor_rotation(CAMERA_ROTATION, False)
            # Pilot uses the saved CameraComponent FOV. Setting only viewport
            # location/rotation silently leaves the editor's default 90-degree FOV.
            unreal.EditorLevelLibrary.pilot_level_actor(camera)
            unreal.log(
                "SCENE_PROGRESS_CAMERA "
                "location=(-281975,223325,5100) "
                "rotation=(pitch=-55.00798,yaw=0,roll=0) "
                "HFOV=54.0222067 piloted=true")

        if self.tick_count == 60:
            # Loading the city discovers Nanite usage for its material instances
            # and queues new shader permutations.  A timed delay is not a proof
            # that those permutations are usable; drain the engine's compilation
            # queue and the render-thread completion commands before the shot.
            unreal.AutomationUtilsBlueprintLibrary.finish_all_asset_compilation()
            unreal.log("SCENE_PROGRESS_COMPILATION_PASS")

        if self.tick_count == 120:
            world = unreal.EditorLevelLibrary.get_editor_world()
            command = (
                f"HighResShot {SHOT_WIDTH}x{SHOT_HEIGHT} "
                f"filename={self.output_path}")
            unreal.SystemLibrary.execute_console_command(world, command)
            self.shot_requested = True
            unreal.log(f"SCENE_PROGRESS_SHOT_REQUEST {command}")

        if self.shot_requested and self.output_path.is_file():
            size = self.output_path.stat().st_size
            if size > 0 and size == self.last_size:
                self.stable_ticks += 1
            else:
                self.stable_ticks = 0
                self.last_size = size
            if self.stable_ticks >= 10:
                width, height = validate_scene_progress_png(
                    self.output_path, SHOT_WIDTH, SHOT_HEIGHT)
                unreal.log(
                    f"SCENE_PROGRESS_SHOT_PASS path={self.output_path} "
                    f"bytes={size} dimensions={width}x{height}")
                self.finish()
                return

        if self.tick_count >= 900:
            self.finish()
            message = f"SCENE_PROGRESS_SHOT_TIMEOUT path={self.output_path}"
            unreal.log_error(message)
            raise RuntimeError(message)


unreal.EditorPythonScripting.set_keep_python_script_alive(True)
CAPTURE = CaptureAfterSettle(command_parameter("SceneProgressShot"))
