"""Сериализует эталонную камеру и neutral manual exposure в progress-карту."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402
from scene_coordinate_basis import source_camera_to_ue  # noqa: E402


LEVEL_PATH = "/Game/Maps/GarnerSceneProgressCity"
CAMERA_LABEL = "SceneProgressCity_Camera"
SOURCE_CAMERA_EYE = (223325.0, 281975.0, 5100.0)
SOURCE_CAMERA_TARGET = (223325.0, 278475.0, 100.0)
CAMERA_BASIS = source_camera_to_ue(
    eye=SOURCE_CAMERA_EYE, target=SOURCE_CAMERA_TARGET)
CAMERA_LOCATION = unreal.Vector(*CAMERA_BASIS["eye"])
CAMERA_ROTATION = unreal.Rotator(
    roll=0.0, pitch=CAMERA_BASIS["pitch"], yaw=CAMERA_BASIS["yaw"])
CAMERA_FOV = 54.0222067


def main(report):
    if RefuseIfEditorOpen(report):
        raise RuntimeError("редактор уже открыт")

    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level(LEVEL_PATH):
        raise RuntimeError(f"не загружена карта {LEVEL_PATH}")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    cameras = [
        actor for actor in actors.get_all_level_actors()
        if actor.get_actor_label() == CAMERA_LABEL
    ]
    if len(cameras) != 1:
        raise RuntimeError(
            f"camera {CAMERA_LABEL}: ожидалась 1, получено {len(cameras)}")
    camera = cameras[0]
    if not isinstance(camera, unreal.CameraActor):
        raise RuntimeError(f"{CAMERA_LABEL}: actor не CameraActor")

    component = camera.get_editor_property("camera_component")
    settings = component.get_editor_property("post_process_settings")

    camera.set_actor_location(CAMERA_LOCATION, False, False)
    camera.set_actor_rotation(CAMERA_ROTATION, False)
    component.set_editor_property("field_of_view", CAMERA_FOV)
    component.set_editor_property("post_process_blend_weight", 1.0)

    settings.set_editor_property("override_auto_exposure_method", True)
    settings.set_editor_property(
        "auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL)
    settings.set_editor_property(
        "override_auto_exposure_apply_physical_camera_exposure", True)
    settings.set_editor_property(
        "auto_exposure_apply_physical_camera_exposure", False)
    settings.set_editor_property("override_auto_exposure_bias", True)
    settings.set_editor_property("auto_exposure_bias", 0.0)
    component.set_editor_property("post_process_settings", settings)

    saved = levels.save_current_level()
    if saved is False:
        raise RuntimeError(f"не сохранена карта {LEVEL_PATH}")

    location = camera.get_actor_location()
    rotation = camera.get_actor_rotation()
    saved_settings = component.get_editor_property("post_process_settings")
    report.line(
        f"SAVED: level={LEVEL_PATH} camera={CAMERA_LABEL} count=1 "
        f"saveResult={saved!r}")
    report.line(
        f"CAMERA: location=({location.x:.6f},{location.y:.6f},"
        f"{location.z:.6f}) rotation=(pitch={rotation.pitch:.6f},"
        f"yaw={rotation.yaw:.6f},roll={rotation.roll:.6f}) "
        f"HFOV={float(component.get_editor_property('field_of_view')):.7f} "
        "blendWeight="
        f"{float(component.get_editor_property('post_process_blend_weight')):.6f}")
    report.line(
        "EXPOSURE: "
        "methodOverride="
        f"{saved_settings.get_editor_property('override_auto_exposure_method')} "
        f"method={saved_settings.get_editor_property('auto_exposure_method')} "
        "physicalOverride="
        f"{saved_settings.get_editor_property('override_auto_exposure_apply_physical_camera_exposure')} "
        "physical="
        f"{saved_settings.get_editor_property('auto_exposure_apply_physical_camera_exposure')} "
        "biasOverride="
        f"{saved_settings.get_editor_property('override_auto_exposure_bias')} "
        f"bias={float(saved_settings.get_editor_property('auto_exposure_bias')):.6f}")
    report.line("PATCH PASS: camera transform/FOV and manual-neutral exposure saved")


report = Reporter("patch_scene_progress_camera")
main(report)
report.close()
