"""Холодно проверяет сериализованный legacy-lighting в progress-карте."""

import argparse
import json
import math
import os
import re
import sqlite3
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter                          # noqa: E402
from scene_lighting import resolve_reference_lighting  # noqa: E402


MAP_PACKAGE = "/Game/Maps/GarnerSceneProgressCity"
EXPECTED_VISIBLE_REFERENCE_ANCHORS = 1370
EXPECTED_REFERENCE_PART_PLACEMENTS = 1617
ACTOR_PATTERN = re.compile(
    r"^SceneProgressCity_Part_S(?P<section>\d+)_I(?P<slot>\d+)_P\d+_")


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--database", required=True)
    return parser.parse_args(argv)


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def visual_manifest(manifest, database):
    connection = sqlite3.connect(os.path.abspath(database))
    try:
        model_types = {
            int(model_id): int(model_type)
            for model_id, model_type in connection.execute(
                'SELECT id, "type" FROM scene_objects')
        }
    finally:
        connection.close()
    reference_records = [
        record for record in manifest.get("records", [])
        if record.get("inReferenceSet") is True
    ]
    missing_model_ids = sorted({
        int(record["modelId"])
        for record in reference_records
        if int(record["modelId"]) not in model_types
    })
    if missing_model_ids:
        raise RuntimeError(
            f"missing model IDs: {missing_model_ids[:20]}")
    records = [
        record for record in reference_records
        if model_types[int(record["modelId"])] == 0
    ]
    return {
        "schemaVersion": 2,
        "stats": {"referenceObjectCount": len(records)},
        "records": records,
    }


def validate_visual_manifest_scope(manifest):
    records = manifest.get("records")
    if (not isinstance(records, list)
            or len(records) != EXPECTED_VISIBLE_REFERENCE_ANCHORS):
        actual = len(records) if isinstance(records, list) else None
        raise RuntimeError(
            "lighting anchors: "
            f"{actual}/{EXPECTED_VISIBLE_REFERENCE_ANCHORS}")


def load_verified_level(levels, package):
    if not levels.load_level(package):
        raise RuntimeError(f"не загружена карта: {package}")


def payload_map(manifest, database):
    lighting = resolve_reference_lighting(manifest, database)
    result = {}
    for item in lighting:
        section, slot, _offset = item["sourceKey"]
        key = (section, slot)
        if key in result:
            raise RuntimeError(f"duplicate label source key: {key}")
        result[key] = tuple(item["payload"])
    return result, lighting


def component_for(actor):
    if isinstance(actor, unreal.StaticMeshActor):
        return actor.static_mesh_component
    if isinstance(actor, unreal.SkeletalMeshActor):
        return actor.skeletal_mesh_component
    return None


def read_default_payload(component):
    value = component.get_editor_property("custom_primitive_data")
    return tuple(float(item) for item in value.get_editor_property("data"))


def float32(value):
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def main(report, args):
    database = os.path.abspath(args.database)
    manifest = visual_manifest(
        load_manifest(os.path.abspath(args.manifest)), database)
    validate_visual_manifest_scope(manifest)
    expected, lighting = payload_map(manifest, database)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    load_verified_level(levels, MAP_PACKAGE)

    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    seen = set()
    part_count = 0
    for actor in actors:
        match = ACTOR_PATTERN.match(actor.get_actor_label())
        if match is None:
            continue
        key = (int(match.group("section")), int(match.group("slot")))
        if key not in expected:
            raise RuntimeError(f"неизвестный lighting source key: {key}")
        component = component_for(actor)
        if component is None:
            raise RuntimeError(f"нет mesh component: {actor.get_actor_label()}")
        actual = read_default_payload(component)
        wanted = tuple(float32(value) for value in expected[key])
        if (len(actual) != 33 or len(wanted) != 33
                or any(not math.isfinite(value) for value in actual)
                or actual != wanted):
            differences = [
                (index, wanted_value, actual_value)
                for index, (wanted_value, actual_value)
                in enumerate(zip(wanted, actual))
                if wanted_value != actual_value
            ]
            raise RuntimeError(
                f"lighting payload mismatch: {actor.get_actor_label()} "
                f"diff={differences[:3]}")
        if (component.get_editor_property("cast_shadow")
                or component.get_editor_property(
                    "affect_dynamic_indirect_lighting")
                or component.get_editor_property(
                    "affect_distance_field_lighting")):
            raise RuntimeError(
                f"UE lighting flags активны: {actor.get_actor_label()}")
        seen.add(key)
        part_count += 1

    if (seen != set(expected)
            or part_count != EXPECTED_REFERENCE_PART_PLACEMENTS):
        raise RuntimeError(
            f"lighting coverage: anchors={len(seen)}/{len(expected)} "
            f"parts={part_count}/{EXPECTED_REFERENCE_PART_PLACEMENTS}")
    modes = {}
    for item in lighting:
        modes[item["mode"]] = modes.get(item["mode"], 0) + 1
    report.line(
        f"PASS: map={MAP_PACKAGE} anchors={len(seen)} parts={part_count} "
        f"payload=33 shadows=0 modes={modes}")


report = Reporter("verify_scene_progress_lighting")
try:
    main(report, parse_args(sys.argv[1:]))
finally:
    report.close()
