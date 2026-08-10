"""Снимает HighResShot полного Garner City из parity-точки у фонтана.

    UnrealEditor CorsairsUE.uproject \\
        -ExecutePythonScript="$PWD/CorsairsUE/Scripts/capture_garner_city.py" \\
        -GarnerCityShot=<absolute.png>

Ловушка 14: съёмка — только полноценным редактором (Slate тикает), а не
коммандлетом. Камера ставится в ту же source-позицию, что и кадр эталона
`garner-live-reference-2026-08-09.png`: eye (223325, 281975, 5100) в
source-координатах, наведение на фонтан (223325, 278475, 100) — это
горячая проверка того, что полный город стоит в том же rigid-Q базисе,
что и эталонный квартал.
"""

import math
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import unreal

from scene_capture_validation import validate_scene_progress_png
from scene_coordinate_basis import source_camera_to_ue


def _env_vec(name, default):
    """Опциональный оверрайд камеры: `x,y,z` в source-координатах."""
    raw = os.environ.get(name)
    if not raw:
        return default
    parts = [float(item) for item in raw.split(",")]
    if len(parts) != 3 or not all(math.isfinite(v) for v in parts):
        raise RuntimeError(f"{name}: нужно три числа, получено {raw!r}")
    return tuple(parts)


SOURCE_CAMERA_EYE = _env_vec(
    "KIMI_CAM_EYE", (223325.0, 281975.0, 5100.0))
SOURCE_CAMERA_TARGET = _env_vec(
    "KIMI_CAM_TARGET", (223325.0, 278475.0, 100.0))
CAMERA_BASIS = source_camera_to_ue(
    eye=SOURCE_CAMERA_EYE, target=SOURCE_CAMERA_TARGET)
CAMERA_LOCATION = unreal.Vector(*CAMERA_BASIS["eye"])
CAMERA_ROTATION = unreal.Rotator(
    roll=0.0, pitch=CAMERA_BASIS["pitch"], yaw=CAMERA_BASIS["yaw"])
EXPECTED_PITCH = round(CAMERA_BASIS["pitch"], 5)
EXPECTED_YAW = round(CAMERA_BASIS["yaw"], 5)
SHOT_WIDTH = 1920
SHOT_HEIGHT = 1080


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
            raise RuntimeError("GarnerCityShot должен быть absolute path")
        if self.output_path.suffix.lower() != ".png":
            raise RuntimeError("GarnerCityShot должен оканчиваться на .png")
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
            unreal.log("GARNER_CITY_EXPOSURE eye_adaptation=off auto_exposure=off")

        if self.tick_count == 30:
            # HighResShot снимает текущий viewport, поэтому камеру ставим
            # в viewport напрямую — через pilot было бы нужен actor в карте,
            # а полная Garner специально не хранит парадную камеру.
            unreal.EditorLevelLibrary.set_level_viewport_camera_info(
                CAMERA_LOCATION, CAMERA_ROTATION)
            unreal.log(
                f"GARNER_CITY_CAMERA location=({CAMERA_LOCATION.x:.0f},"
                f"{CAMERA_LOCATION.y:.0f},{CAMERA_LOCATION.z:.0f}) "
                f"rotation=(pitch={EXPECTED_PITCH},yaw={EXPECTED_YAW},roll=0)")

        if self.tick_count == 120:
            # Музейще шейдеров: первая загрузка 47k объектов полной Garner
            # открывает десятки material instance permutations — дождаться
            # очередь компиляции до кадра, иначе розовые массивы.
            unreal.AutomationUtilsBlueprintLibrary.finish_all_asset_compilation()
            unreal.log("GARNER_CITY_COMPILATION_PASS")

        if self.tick_count == 240:
            world = unreal.EditorLevelLibrary.get_editor_world()
            command = (
                f"HighResShot {SHOT_WIDTH}x{SHOT_HEIGHT} "
                f"filename={self.output_path}")
            unreal.SystemLibrary.execute_console_command(world, command)
            self.shot_requested = True
            unreal.log(f"GARNER_CITY_SHOT_REQUEST {command}")

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
                    f"GARNER_CITY_SHOT_DONE {self.output_path} "
                    f"{width}x{height}")
                self.finish()


# Класс завершает синхронный проход, а кадр снимается по графику тиков:
# Python должен пережить конец скрипта, иначе редактор выходит сразу.
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
try:
    unwrap = CaptureAfterSettle(command_parameter("GarnerCityShot"))
except Exception as exc:
    unreal.log_error(f"GARNER_CITY_SHOT_FAILED {exc}")
    unreal.EditorPythonScripting.set_keep_python_script_alive(False)
    raise
