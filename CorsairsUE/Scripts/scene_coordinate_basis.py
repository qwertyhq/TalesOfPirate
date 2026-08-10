"""Единый rigid-basis для переноса координат исходной карты в Unreal."""

import math


def unwind_degrees(value):
    result = math.fmod(float(value), 360.0)
    if result > 180.0:
        result -= 360.0
    elif result < -180.0:
        result += 360.0
    return result


def _clean(value):
    return 0.0 if abs(value) < 1.0e-12 else value


def source_location_to_ue(x, y, z):
    """Поворачивает source XY на +90°, не отражая систему координат."""
    return (-float(y), float(x), float(z))


def source_scene_yaw_to_ue(source_yaw_degrees):
    """Actor yaw для SceneMap с обычным, не зеркальным local basis."""
    return unwind_degrees(float(source_yaw_degrees) - 90.0)


def rotate_source_direction(source_direction_degrees):
    """Переводит legacy character direction в единичный UE XY-вектор."""
    radians = math.radians(float(source_direction_degrees))
    return (_clean(math.cos(radians)), _clean(math.sin(radians)))


def standalone_character_yaw_to_ue(source_direction_degrees):
    """Учитывает native +Y forward импортированного character mesh."""
    return unwind_degrees(float(source_direction_degrees) - 90.0)


def source_camera_to_ue(*, eye, target):
    ue_eye = source_location_to_ue(*eye)
    ue_target = source_location_to_ue(*target)
    dx = ue_target[0] - ue_eye[0]
    dy = ue_target[1] - ue_eye[1]
    dz = ue_target[2] - ue_eye[2]
    yaw = math.degrees(math.atan2(dy, dx))
    pitch = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
    yaw_radians = math.radians(yaw)
    right = (
        _clean(-math.sin(yaw_radians)),
        _clean(math.cos(yaw_radians)),
        0.0,
    )
    return {
        "eye": ue_eye,
        "target": ue_target,
        "pitch": round(pitch, 6),
        "yaw": _clean(round(yaw, 6)),
        "right": right,
    }
