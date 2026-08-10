"""Разрешает legacy lighting для reference-объектов Garner.

Модуль не зависит от Unreal API. Он соединяет schema-v2 source manifest с
``scene_objects`` и ``areas`` из ``gamedata.sqlite`` и возвращает один
33-float payload на каждый ``inReferenceSet`` anchor в исходном порядке.
"""

from __future__ import annotations

import math
import os
import sqlite3
from contextlib import closing
from typing import Any


PAYLOAD_FLOAT_COUNT = 33
POINT_PAYLOAD_FLOAT_COUNT = 20


def _integer(value: Any, field: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field}: ожидалось целое число")
    if value < minimum or value > maximum:
        raise ValueError(
            f"{field}: значение {value} вне [{minimum}, {maximum}]")
    return value


def _binary_flag(value: Any, field: str) -> int:
    return _integer(value, field, 0, 1)


def _source_key(record: dict[str, Any], index: int) -> tuple[int, int, int]:
    value = record.get("sourceKey")
    if not isinstance(value, dict):
        raise ValueError(f"records[{index}].sourceKey: ожидался объект")
    return (
        _integer(
            value.get("sectionIndex"),
            f"records[{index}].sourceKey.sectionIndex",
            0,
            (1 << 31) - 1,
        ),
        _integer(
            value.get("slotIndex"),
            f"records[{index}].sourceKey.slotIndex",
            0,
            (1 << 31) - 1,
        ),
        _integer(
            value.get("byteOffset"),
            f"records[{index}].sourceKey.byteOffset",
            0,
            (1 << 63) - 1,
        ),
    )


def _reference_records(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 2:
        raise ValueError("source manifest: ожидалась schemaVersion=2")

    records = manifest.get("records")
    stats = manifest.get("stats")
    if not isinstance(records, list) or not isinstance(stats, dict):
        raise ValueError("source manifest: отсутствуют records/stats")

    references = []
    source_keys = []
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValueError(f"records[{index}]: ожидался объект")
        if record.get("inReferenceSet") is not True:
            continue
        if record.get("type") != 0 or record.get("disposition") != "scene-model":
            raise ValueError(
                f"records[{index}]: reference anchor должен быть scene-model type=0")

        model_id = _integer(
            record.get("modelId"), f"records[{index}].modelId", 1, (1 << 31) - 1)
        island = _integer(record.get("island"), f"records[{index}].island", 0, 255)
        tile_color = _integer(
            record.get("tileColor565"),
            f"records[{index}].tileColor565",
            0,
            0xFFFF,
        )
        section_present = record.get("terrainSectionPresent")
        if not isinstance(section_present, bool):
            raise ValueError(
                f"records[{index}].terrainSectionPresent: ожидался bool")

        normalized = dict(record)
        normalized["modelId"] = model_id
        normalized["island"] = island
        normalized["tileColor565"] = tile_color
        references.append(normalized)
        source_keys.append(_source_key(record, index))

    expected_count = _integer(
        stats.get("referenceObjectCount"),
        "stats.referenceObjectCount",
        0,
        (1 << 63) - 1,
    )
    if expected_count != len(references):
        raise ValueError(
            "stats.referenceObjectCount: "
            f"ожидалось {expected_count}, найдено {len(references)}")
    if len(set(source_keys)) != len(source_keys):
        raise ValueError("source manifest: duplicate reference sourceKey")
    return references


def _packed_rgb8(value: Any, field: str) -> tuple[int, int, int]:
    packed = _integer(value, field, -(1 << 31), (1 << 32) - 1) & 0xFFFFFFFF
    return (
        (packed >> 16) & 0xFF,
        (packed >> 8) & 0xFF,
        packed & 0xFF,
    )


def _normalized_rgb(rgb: tuple[int, int, int]) -> tuple[float, float, float]:
    return tuple(float(channel) / 255.0 for channel in rgb)


def _legacy_tile_rgb8(packed: int) -> tuple[int, int, int]:
    """Повторяет legacy BGRA565 без восстановления младших битов."""
    return (
        (packed & 0x001F) << 3,
        (packed & 0x07E0) >> 3,
        (packed & 0xF800) >> 8,
    )


def _ue_direction(value: Any, field: str) -> tuple[float, float, float]:
    if not isinstance(value, str):
        raise ValueError(f"{field}: ожидалась строка x,y,z")
    parts = value.split(",")
    if len(parts) != 3:
        raise ValueError(f"{field}: ожидалось три компонента")
    components = tuple(float(part.strip()) for part in parts)
    if not all(math.isfinite(component) for component in components):
        raise ValueError(f"{field}: компоненты должны быть конечными")
    x, y, z = components
    length = math.sqrt(x * x + y * y + z * z)
    if length == 0.0:
        raise ValueError(f"{field}: нулевое направление")
    return x / length, -y / length, z / length


def _mode_name(
    env_enabled: int,
    point_enabled: int,
    shade_enabled: int,
    area: sqlite3.Row | None,
) -> str:
    if not (env_enabled or point_enabled or shade_enabled):
        return "legacy-unlit"
    if area is None:
        return "legacy-inherited-outside-capture"
    if env_enabled and shade_enabled:
        return "legacy-area-shade"
    if env_enabled:
        return "legacy-area"
    if shade_enabled:
        return "legacy-shade"
    return "legacy-point"


def _payload(
    record: dict[str, Any],
    scene_object: sqlite3.Row,
    area: sqlite3.Row | None,
) -> tuple[float, ...]:
    model_id = record["modelId"]
    env_enabled = _binary_flag(
        scene_object["enable_env_light"],
        f"scene_objects[{model_id}].enable_env_light",
    )
    point_enabled = _binary_flag(
        scene_object["enable_point_light"],
        f"scene_objects[{model_id}].enable_point_light",
    )
    shade_enabled = _binary_flag(
        scene_object["shade_flag"], f"scene_objects[{model_id}].shade_flag")
    size_flag = _binary_flag(
        scene_object["size_flag"], f"scene_objects[{model_id}].size_flag")

    if point_enabled:
        raise ValueError(
            f"scene_objects[{model_id}]: point lighting требует point-source resolver")

    lighting_enabled = float(env_enabled or point_enabled or shade_enabled)
    zero_rgb = (0.0, 0.0, 0.0)
    if lighting_enabled == 0.0:
        ambient = (1.0, 1.0, 1.0)
        light_direction = zero_rgb
        directional_color = zero_rgb
    elif area is None:
        # У Island=0 исходный renderer наследует предыдущее состояние. Эти
        # anchors находятся вне capture; neutral white не подменяет их area 1.
        ambient = (1.0, 1.0, 1.0)
        light_direction = zero_rgb
        directional_color = zero_rgb
    else:
        area_id = int(area["id"])
        area_env_rgb8 = _packed_rgb8(
            area["env_color"], f"areas[{area_id}].env_color")
        if shade_enabled:
            tile_rgb8 = _legacy_tile_rgb8(record["tileColor565"])
            multiplier = area_env_rgb8 if size_flag else (255, 255, 255)
            ambient_rgb8 = tuple(
                tile * environment // 255
                for tile, environment in zip(tile_rgb8, multiplier)
            )
            ambient = _normalized_rgb(ambient_rgb8)
        else:
            ambient = _normalized_rgb(area_env_rgb8)

        if env_enabled:
            light_direction = _ue_direction(
                area["light_dir"], f"areas[{area_id}].light_dir")
            directional_color = _normalized_rgb(_packed_rgb8(
                area["light_color"], f"areas[{area_id}].light_color"))
        else:
            light_direction = zero_rgb
            directional_color = zero_rgb

    values = (
        lighting_enabled,
        float(env_enabled),
        float(point_enabled),
        float(shade_enabled),
        *ambient,
        *light_direction,
        *directional_color,
        *((0.0,) * POINT_PAYLOAD_FLOAT_COUNT),
    )
    payload = tuple(float(value) for value in values)
    if len(payload) != PAYLOAD_FLOAT_COUNT:
        raise AssertionError(
            f"lighting payload: {len(payload)} вместо {PAYLOAD_FLOAT_COUNT}")
    return payload


def resolve_reference_lighting(
    manifest: dict[str, Any],
    database_path: str | os.PathLike[str],
) -> list[dict[str, Any]]:
    """Возвращает lighting resolution для каждого reference anchor."""
    references = _reference_records(manifest)
    model_ids = sorted({record["modelId"] for record in references})

    with closing(sqlite3.connect(os.fspath(database_path))) as database:
        database.row_factory = sqlite3.Row
        placeholders = ",".join("?" for _ in model_ids)
        scene_rows = list(database.execute(
            "SELECT id, name, enable_env_light, enable_point_light, "
            "shade_flag, size_flag FROM scene_objects "
            f"WHERE id IN ({placeholders}) ORDER BY id",
            model_ids,
        )) if model_ids else []
        scene_objects = {int(row["id"]): row for row in scene_rows}
        missing_models = sorted(set(model_ids) - set(scene_objects))
        if missing_models:
            raise ValueError(
                f"scene_objects: отсутствуют modelId {missing_models}")

        required_areas = sorted({
            record["island"]
            for record in references
            if record["island"] != 0
            and any((
                scene_objects[record["modelId"]]["enable_env_light"],
                scene_objects[record["modelId"]]["enable_point_light"],
                scene_objects[record["modelId"]]["shade_flag"],
            ))
        })
        area_placeholders = ",".join("?" for _ in required_areas)
        area_rows = list(database.execute(
            "SELECT id, env_color, light_color, light_dir FROM areas "
            f"WHERE id IN ({area_placeholders}) ORDER BY id",
            required_areas,
        )) if required_areas else []
        areas = {int(row["id"]): row for row in area_rows}
        missing_areas = sorted(set(required_areas) - set(areas))
        if missing_areas:
            raise ValueError(f"areas: отсутствуют island {missing_areas}")

        resolved = []
        for index, record in enumerate(references):
            scene_object = scene_objects[record["modelId"]]
            flags = (
                _binary_flag(
                    scene_object["enable_env_light"],
                    f"scene_objects[{record['modelId']}].enable_env_light",
                ),
                _binary_flag(
                    scene_object["enable_point_light"],
                    f"scene_objects[{record['modelId']}].enable_point_light",
                ),
                _binary_flag(
                    scene_object["shade_flag"],
                    f"scene_objects[{record['modelId']}].shade_flag",
                ),
            )
            area = areas.get(record["island"]) if any(flags) else None
            payload = _payload(record, scene_object, area)
            resolved.append({
                "sourceKey": _source_key(record, index),
                "modelId": record["modelId"],
                "mode": _mode_name(*flags, area),
                "payload": payload,
                "diagnostics": {
                    "flags": flags,
                    "sizeFlag": _binary_flag(
                        scene_object["size_flag"],
                        f"scene_objects[{record['modelId']}].size_flag",
                    ),
                    "island": record["island"],
                    "terrainSectionPresent": record["terrainSectionPresent"],
                    "tileColor565": record["tileColor565"],
                    "areaId": int(area["id"]) if area is not None else None,
                    "pointGroups": "zero",
                },
            })
    return resolved
