"""Холодно проверяет сериализованный camera/exposure contract progress-карты."""

import math
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
EXPECTED_LOCATION = CAMERA_BASIS["eye"]
EXPECTED_ROTATION = (
    CAMERA_BASIS["pitch"], CAMERA_BASIS["yaw"], 0.0)
EXPECTED_FOV = 54.0222067
FLOAT_TOLERANCE = 0.0001


def close_enough(actual, expected):
    return math.isclose(
        float(actual), float(expected), rel_tol=0.0, abs_tol=FLOAT_TOLERANCE)


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
    manual = unreal.AutoExposureMethod.AEM_MANUAL

    location = camera.get_actor_location()
    rotation = camera.get_actor_rotation()
    field_of_view = component.get_editor_property("field_of_view")
    blend_weight = component.get_editor_property("post_process_blend_weight")
    exposure = {
        "override_auto_exposure_method": settings.get_editor_property(
            "override_auto_exposure_method"),
        "auto_exposure_method": settings.get_editor_property(
            "auto_exposure_method"),
        "override_auto_exposure_apply_physical_camera_exposure": (
            settings.get_editor_property(
                "override_auto_exposure_apply_physical_camera_exposure")),
        "auto_exposure_apply_physical_camera_exposure": (
            settings.get_editor_property(
                "auto_exposure_apply_physical_camera_exposure")),
        "override_auto_exposure_bias": settings.get_editor_property(
            "override_auto_exposure_bias"),
        "auto_exposure_bias": settings.get_editor_property(
            "auto_exposure_bias"),
    }

    report.line(
        "PROPERTY CONTRACT: "
        "PostProcessSettings.override_auto_exposure_method; "
        "auto_exposure_method=AutoExposureMethod.AEM_MANUAL; "
        "override_auto_exposure_apply_physical_camera_exposure; "
        "auto_exposure_apply_physical_camera_exposure; "
        "override_auto_exposure_bias; auto_exposure_bias")
    report.line(
        f"CAMERA: count=1 location=({location.x:.6f},{location.y:.6f},"
        f"{location.z:.6f}) rotation=(pitch={rotation.pitch:.6f},"
        f"yaw={rotation.yaw:.6f},roll={rotation.roll:.6f}) "
        f"HFOV={float(field_of_view):.7f} blendWeight={float(blend_weight):.6f}")
    report.line(
        "EXPOSURE: "
        f"methodOverride={exposure['override_auto_exposure_method']} "
        f"method={exposure['auto_exposure_method']} "
        "physicalOverride="
        f"{exposure['override_auto_exposure_apply_physical_camera_exposure']} "
        f"physical={exposure['auto_exposure_apply_physical_camera_exposure']} "
        f"biasOverride={exposure['override_auto_exposure_bias']} "
        f"bias={float(exposure['auto_exposure_bias']):.6f}")

    issues = []
    for actual, expected, name in (
        (location.x, EXPECTED_LOCATION[0], "location.x"),
        (location.y, EXPECTED_LOCATION[1], "location.y"),
        (location.z, EXPECTED_LOCATION[2], "location.z"),
        (rotation.pitch, EXPECTED_ROTATION[0], "rotation.pitch"),
        (rotation.yaw, EXPECTED_ROTATION[1], "rotation.yaw"),
        (rotation.roll, EXPECTED_ROTATION[2], "rotation.roll"),
        (field_of_view, EXPECTED_FOV, "field_of_view"),
        (blend_weight, 1.0, "post_process_blend_weight"),
    ):
        if not close_enough(actual, expected):
            issues.append(f"{name}={actual!r}, ожидалось {expected!r}")

    for actual, expected, name in (
        (exposure["override_auto_exposure_method"], True,
         "override_auto_exposure_method"),
        (exposure["auto_exposure_method"], manual, "auto_exposure_method"),
        (exposure["override_auto_exposure_apply_physical_camera_exposure"],
         True, "override_auto_exposure_apply_physical_camera_exposure"),
        (exposure["auto_exposure_apply_physical_camera_exposure"],
         False, "auto_exposure_apply_physical_camera_exposure"),
        (exposure["override_auto_exposure_bias"], True,
         "override_auto_exposure_bias"),
    ):
        if actual != expected:
            issues.append(f"{name}={actual!r}, ожидалось {expected!r}")
    if not close_enough(exposure["auto_exposure_bias"], 0.0):
        issues.append(
            f"auto_exposure_bias={exposure['auto_exposure_bias']!r}, "
            "ожидалось 0.0")

    if issues:
        for issue in issues:
            report.error(issue)
        report.line(f"READBACK FAIL: issues={len(issues)}")
        report.close()
        raise RuntimeError(f"camera readback issues={len(issues)}")

    report.line("READBACK PASS: camera=1 transform=exact exposure=manual-neutral")


report = Reporter("verify_scene_progress_camera")
main(report)
report.close()
