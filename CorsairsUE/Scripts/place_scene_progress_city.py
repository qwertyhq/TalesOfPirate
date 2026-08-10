"""Собирает полный reference-город Garner из SceneMap-ассетов.

Скрипт читает schema-v2 source manifest, ставит каждую часть каждого
``inReferenceSet`` anchor с transform исходной игры и сохраняет результат в
отдельную owned-карту ``/Game/Maps/GarnerSceneProgressCity``. Исходная Garner
не изменяется.

Пути SceneMap-ассетов берутся из ``model_map.json`` с заменой его content root
на ``--content-root``. Это сохраняет проверенное соответствие
``modelId -> .lmo parts``, но физически загружает только новый namespace, а не
legacy ``/Game/All``.
"""

import argparse
import json
import math
import os
import pathlib
import posixpath
import re
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402
from scene_coordinate_basis import (                 # noqa: E402
    source_camera_to_ue,
    source_location_to_ue,
    source_scene_yaw_to_ue,
    standalone_character_yaw_to_ue,
)
from scene_lighting import resolve_reference_lighting  # noqa: E402
# Разбор half-meter raster живёт в расстановке NPC; вторая копия чтения r16
# разошлась бы с первой при первом же изменении формата.
from place_scene_progress_npcs import (              # noqa: E402
    height_grid_width,
    sample_height_cm,
)


SCHEMA_VERSION = 2
CAPTURE_TICK = 120
CAPTURE_HZ = 30.0
DEFAULT_SOURCE = "/Game/Maps/Garner"
OWNED_TARGET = "/Game/Maps/GarnerSceneProgressCity"
DEFAULT_CONTENT_ROOT = "/Game/SceneParityCityRigidV5"
DEFAULT_CHARACTER_TYPE = "1"
DEFAULT_DATABASE = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "databases", "gamedata.sqlite"))
CONTENT_DIR = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "Content"))

# Каноническая точка parity-кадра: тот же фонтанный квартал, который виден
# в оригинальном клиенте. Source XY переводится единым rigid Q-basis.
SOURCE_CHARACTER_LOCATION = (223325.0, 278475.0, 100.0)
SOURCE_CHARACTER_DIRECTION = 90.0
SOURCE_CAMERA_EYE = (223325.0, 281975.0, 5100.0)
CHARACTER_LOCATION = source_location_to_ue(*SOURCE_CHARACTER_LOCATION)
REFERENCE_CAMERA = source_camera_to_ue(
    eye=SOURCE_CAMERA_EYE, target=SOURCE_CHARACTER_LOCATION)
CHARACTER_LABEL = "SceneProgressCity_Test195126"
CAMERA_LABEL = "SceneProgressCity_Camera"
# Тот же рост, что у NPC: сборка нормируется по объединённому объёму частей.
CHARACTER_HEIGHT_CM = 176.0
DEFAULT_HEIGHT_MAP = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "Data", "Heights", "garner.height.r16"))
ACTOR_PREFIX = "SceneProgressCity_Part_"

# Ровно эти части имеют embedded BONE policy=preserveAnimated в production
# SceneMap batch. Значения: (frameCount, sampleFrame, placements).
EXPECTED_ANIMATED_PARTS = {
    (2, "by-bd002_8"): (47, 25, 1),
    (325, "nml-bd025"): (101, 18, 5),
    (475, "by-bd032_1"): (101, 18, 1),
    (476, "by-bd033_1"): (97, 22, 2),
    (478, "by-bd035_1"): (97, 22, 1),
    (513, "nml-bd199_0"): (201, 119, 5),
    (513, "nml-bd199_1"): (201, 119, 5),
    (513, "nml-bd199_2"): (201, 119, 5),
    (513, "nml-bd199_3"): (201, 119, 5),
    (513, "nml-bd199_4"): (201, 119, 5),
}
EXPECTED_ANIMATED_UNIQUE_PARTS = 10
EXPECTED_ANIMATED_PLACEMENTS = 35
EXPECTED_VISIBLE_REFERENCE_ANCHORS = 1370
EXPECTED_HIDDEN_HELPERS = 264
EXPECTED_REFERENCE_PART_PLACEMENTS = 1617

REFERENCE_TERRAIN_LABEL = "ReferenceTerrain_Garner_17_21"
REFERENCE_TERRAIN_TAG = "CorsairsReferenceTerrain"
REFERENCE_TERRAIN_MESH = (
    "/Game/Terrain/Reference/GarnerRigid/SM_Garner_17_21")
REFERENCE_TERRAIN_INSTANCE = "/Game/Terrain/Reference/Garner/MI_Garner_17_21"
REFERENCE_TERRAIN_LOCATION = (-268800.0, 217600.0, 0.0)
REFERENCE_TERRAIN_BOUNDS = (-281600.0, 217600.0, -268800.0, 230400.0)

BASE_COLOR_PARAM = "BaseColorTexture"
PLACEHOLDERS = {
    "T_White_srgb",
    "T_White_Linear",
    "DefaultTexture",
}


def parse_args(argv):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(
        description="Поставить весь Garner reference city из SceneMap")
    parser.add_argument("--manifest", required=True)
    parser.add_argument(
        "--model-map",
        default=os.path.join(script_dir, "model_map.json"))
    parser.add_argument(
        "--character-map",
        default=os.path.abspath(os.path.join(
            script_dir, "..", "Data", "character_map.json")))
    parser.add_argument("--database", default=DEFAULT_DATABASE)
    parser.add_argument("--content-root", default=DEFAULT_CONTENT_ROOT)
    parser.add_argument("--source", default=DEFAULT_SOURCE)
    parser.add_argument("--target", default=OWNED_TARGET)
    parser.add_argument("--character-type", default=DEFAULT_CHARACTER_TYPE)
    return parser.parse_args(argv)


def require_asset_path(value, name):
    if not value.startswith("/Game/") or value.endswith("/"):
        raise RuntimeError(f"{name}: нужен путь /Game/..., получено {value!r}")


def validate_args(args):
    require_asset_path(args.source, "--source")
    require_asset_path(args.target, "--target")
    require_asset_path(args.content_root, "--content-root")
    if args.content_root == "/Game/All" or args.content_root.startswith("/Game/All/"):
        raise RuntimeError("legacy /Game/All запрещён для SceneMap city")
    if args.target != OWNED_TARGET:
        raise RuntimeError(
            f"отказ удаления: скрипт владеет только {OWNED_TARGET}, "
            f"получено {args.target}")
    if args.source == args.target:
        raise RuntimeError("исходная и целевая карты совпадают")
    for path, name in ((args.manifest, "manifest"),
                       (args.model_map, "model-map"),
                       (args.character_map, "character-map"),
                       (args.database, "database")):
        if not os.path.isfile(path):
            raise RuntimeError(f"нет {name}: {path}")


def load_reference_records(path):
    with open(path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)

    if manifest.get("schemaVersion") != SCHEMA_VERSION:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО/SCHEMA: ожидалась schemaVersion=2, "
            f"получено {manifest.get('schemaVersion')}")

    records = manifest.get("records")
    stats = manifest.get("stats")
    if not isinstance(records, list) or not isinstance(stats, dict):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: нет records/stats")

    expected_source = stats.get("sourceRecordCount")
    if expected_source != len(records):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО source records: "
            f"stats={expected_source}, actual={len(records)}")

    scene_count = stats.get("sceneModelCount")
    effect_count = stats.get("deferredEffectCount")
    if (not isinstance(scene_count, int) or not isinstance(effect_count, int)
            or scene_count + effect_count != len(records)):
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО: sceneModelCount + "
            "deferredEffectCount != sourceRecordCount")

    references = [record for record in records
                  if record.get("inReferenceSet") is True]
    expected_references = stats.get("referenceObjectCount")
    if expected_references != len(references):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО reference anchors: "
            f"stats={expected_references}, actual={len(references)}")
    if any(record.get("type") != 0 for record in references):
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО: reference set содержит не type-0 record")

    source_keys = []
    required_fields = ("modelId", "x", "y", "zCm", "sourceYawDegrees")
    for index, record in enumerate(references):
        if any(field not in record for field in required_fields):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО/FIELDS: reference[{index}] неполон")
        key = record.get("sourceKey", {})
        stable_key = (key.get("sectionIndex"), key.get("slotIndex"),
                      key.get("byteOffset"))
        if any(value is None for value in stable_key):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО/FIELDS: reference[{index}].sourceKey")
        source_keys.append(stable_key)

    if len(set(source_keys)) != len(source_keys):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: duplicate sourceKey")
    return references, stats


def load_model_map(path):
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    source_root = data.get("contentRoot")
    models = data.get("models")
    if not isinstance(source_root, str) or not isinstance(models, dict):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: model-map неполон")
    require_asset_path(source_root, "model-map.contentRoot")
    return source_root.rstrip("/"), models


def filter_visual_references(references, database_path):
    """Legacy renderer draws scene object type 0; types 1+ are debug helpers."""
    connection = sqlite3.connect(os.path.abspath(database_path))
    try:
        model_types = {
            int(model_id): int(model_type)
            for model_id, model_type in connection.execute(
                'SELECT id, "type" FROM scene_objects')
        }
    finally:
        connection.close()

    missing = sorted({
        int(record["modelId"]) for record in references
        if int(record["modelId"]) not in model_types
    })
    if missing:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: scene_objects rows missing {missing}")

    visual = [
        record for record in references
        if model_types[int(record["modelId"])] == 0
    ]
    helpers = [
        record for record in references
        if model_types[int(record["modelId"])] != 0
    ]
    if len(visual) + len(helpers) != len(references):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: visual/helper partition")
    return visual, helpers


def remap_asset_path(path, source_root, content_root):
    prefix = source_root + "/"
    if not isinstance(path, str) or not path.startswith(prefix):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: путь вне {source_root}: {path}")
    result = content_root.rstrip("/") + path[len(source_root):]
    if not result.startswith(content_root.rstrip("/") + "/"):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: path escape {path}")
    return result


def skeletal_path_for(static_path):
    marker = "/StaticMeshes/"
    if marker not in static_path:
        return static_path
    return static_path.replace(marker, "/SkeletalMeshes/", 1)


def animation_path_for(skeletal_path):
    if "/SkeletalMeshes/" not in skeletal_path:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: не skeletal path {skeletal_path}")
    return skeletal_path + "_Anim"


def load_scene_mesh_asset(static_path, skeletal_path):
    """Выбирает animated sibling раньше возможного stale StaticMesh."""
    skeletal = unreal.load_asset(skeletal_path)
    if isinstance(skeletal, unreal.SkeletalMesh):
        return skeletal, "skeletal", skeletal_path
    static = unreal.load_asset(static_path)
    if isinstance(static, unreal.StaticMesh):
        return static, "static", static_path
    return None, None, None


def read_animation_contract(mesh, animation, animation_path):
    mesh_skeleton = mesh.get_editor_property("skeleton")
    animation_skeleton = animation.get_editor_property("skeleton")
    if mesh_skeleton is None or animation_skeleton != mesh_skeleton:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: skeleton mismatch {animation_path}")

    data_model = animation.get_editor_property("data_model_interface")
    if data_model is None:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: data model {animation_path}")
    frame_count = int(data_model.get_number_of_keys())
    frame_intervals = int(data_model.get_number_of_frames())
    if frame_count <= 0 or frame_intervals != frame_count - 1:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: frames {animation_path}: "
            f"keys={frame_count} intervals={frame_intervals}")

    frame_rate = data_model.get_frame_rate()
    numerator = int(frame_rate.numerator)
    denominator = int(frame_rate.denominator)
    if numerator != int(CAPTURE_HZ) or denominator != 1:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: frameRate {animation_path}: "
            f"{numerator}/{denominator}")
    expected_length = frame_intervals / CAPTURE_HZ
    if not math.isclose(
            float(animation.get_play_length()), expected_length,
            rel_tol=0.0, abs_tol=1.0e-4):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION: duration {animation_path}: "
            f"{animation.get_play_length()}/{expected_length}")

    sample_frame = (CAPTURE_TICK - 1) % frame_count
    return {
        "animation": animation,
        "animation_path": animation_path,
        "frame_count": frame_count,
        "sample_frame": sample_frame,
        "sample_seconds": sample_frame / CAPTURE_HZ,
    }


def validate_animated_city_census(references, resolved):
    reference_counts = {}
    for record in references:
        model_id = int(record["modelId"])
        reference_counts[model_id] = reference_counts.get(model_id, 0) + 1

    actual = {}
    for model_id, parts in resolved.items():
        for descriptor in parts:
            if descriptor["kind"] != "skeletal":
                continue
            key = (int(model_id), descriptor["stem"])
            if key in actual:
                raise RuntimeError(
                    f"НЕВЕРНОЕ_КОЛИЧЕСТВО animated duplicate {key}")
            actual[key] = (
                descriptor["frame_count"],
                descriptor["sample_frame"],
                reference_counts.get(int(model_id), 0),
            )
            if descriptor["animation_path"] != animation_path_for(
                    descriptor["path"]):
                raise RuntimeError(
                    f"ОТСУТСТВУЕТ_ANIMATION: path mismatch {key}")

    actual_keys = set(actual)
    expected_keys = set(EXPECTED_ANIMATED_PARTS)
    if actual_keys != expected_keys:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО animated parts: "
            f"missing={sorted(expected_keys - actual_keys)} "
            f"unexpected={sorted(actual_keys - expected_keys)}")
    for key, expected in EXPECTED_ANIMATED_PARTS.items():
        got = actual[key]
        if got[0] != expected[0]:
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_ANIMATION frameCount {key}: "
                f"{got[0]}/{expected[0]}")
        if got[1] != expected[1]:
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_ANIMATION sampleFrame {key}: "
                f"{got[1]}/{expected[1]}")
        if got[2] != expected[2]:
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО animated placements {key}: "
                f"{got[2]}/{expected[2]}")
        if got[1] != (CAPTURE_TICK - 1) % got[0]:
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_ANIMATION tick119 sampleFrame {key}: {got[1]}")

    unique_parts = len(actual)
    placements = sum(value[2] for value in actual.values())
    if (unique_parts != EXPECTED_ANIMATED_UNIQUE_PARTS
            or placements != EXPECTED_ANIMATED_PLACEMENTS):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО animated: uniqueParts={unique_parts}/"
            f"{EXPECTED_ANIMATED_UNIQUE_PARTS} placements={placements}/"
            f"{EXPECTED_ANIMATED_PLACEMENTS}")
    return {"uniqueParts": unique_parts, "placements": placements}


def preflight_meshes(report, references, source_root, models, content_root):
    """Загружает все требуемые части до изменения карты."""
    model_ids = sorted({record["modelId"] for record in references})
    resolved = {}
    asset_cache = {}
    missing = []

    for model_id in model_ids:
        source_paths = models.get(str(model_id))
        if not isinstance(source_paths, list) or not source_paths:
            missing.append(f"modelId={model_id}: нет parts в model-map")
            continue

        parts = []
        for source_path in source_paths:
            static_path = remap_asset_path(
                source_path, source_root, content_root)
            skeletal_path = skeletal_path_for(static_path)

            descriptor = asset_cache.get(static_path)
            if descriptor is None:
                asset, kind, actual_path = load_scene_mesh_asset(
                    static_path, skeletal_path)
                if asset is None:
                    missing.append(
                        f"modelId={model_id}: {static_path} "
                        f"(и {skeletal_path})")
                    continue

                descriptor = {
                    "asset": asset,
                    "kind": kind,
                    "path": actual_path,
                    "stem": actual_path.rsplit("/", 1)[-1],
                }
                if kind == "static":
                    allowed = {
                        unreal.BlendMode.BLEND_OPAQUE,
                        unreal.BlendMode.BLEND_MASKED,
                    }
                    descriptor["disallow_nanite"] = any(
                        slot.material_interface is not None
                        and slot.material_interface.get_blend_mode()
                        not in allowed
                        for slot in asset.get_editor_property(
                            "static_materials"))
                else:
                    descriptor["disallow_nanite"] = False
                    animation_path = animation_path_for(skeletal_path)
                    animation = unreal.load_asset(animation_path)
                    if not isinstance(animation, unreal.AnimSequence):
                        missing.append(
                            f"modelId={model_id}: {animation_path}")
                        continue
                    try:
                        descriptor.update(read_animation_contract(
                            asset, animation, animation_path))
                    except RuntimeError as error:
                        missing.append(f"modelId={model_id}: {error}")
                        continue
                asset_cache[static_path] = descriptor
            parts.append(descriptor)

        if len(parts) == len(source_paths):
            resolved[model_id] = parts

    if missing:
        report.error(f"ОТСУТСТВУЕТ_MESH: {len(missing)}")
        for issue in missing[:20]:
            report.error("  " + issue)
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: {len(missing)} parts")

    expected_placements = sum(
        len(resolved[record["modelId"]]) for record in references)
    unique_parts = {
        part["path"]: part
        for parts in resolved.values()
        for part in parts
    }
    static_parts = sum(
        1 for part in unique_parts.values() if part["kind"] == "static")
    skeletal_parts = len(unique_parts) - static_parts
    animated_census = validate_animated_city_census(references, resolved)
    report.line(
        f"MESH PREFLIGHT: models={len(resolved)} uniqueParts={len(unique_parts)} "
        f"static={static_parts} skeletal={skeletal_parts} "
        f"expectedPlacements={expected_placements}")
    report.line(
        f"ANIMATION PREFLIGHT: uniqueParts="
        f"{animated_census['uniqueParts']}/"
        f"{EXPECTED_ANIMATED_UNIQUE_PARTS} placements="
        f"{animated_census['placements']}/"
        f"{EXPECTED_ANIMATED_PLACEMENTS} captureTick={CAPTURE_TICK} hz=30")
    return resolved, expected_placements


def validate_city_scope_counts(
        references, helper_references, expected_placements):
    if len(references) != EXPECTED_VISIBLE_REFERENCE_ANCHORS:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО visible anchors: "
            f"{len(references)}/{EXPECTED_VISIBLE_REFERENCE_ANCHORS}")
    if len(helper_references) != EXPECTED_HIDDEN_HELPERS:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО hidden helpers: "
            f"{len(helper_references)}/{EXPECTED_HIDDEN_HELPERS}")
    if expected_placements != EXPECTED_REFERENCE_PART_PLACEMENTS:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО part placements: "
            f"{expected_placements}/{EXPECTED_REFERENCE_PART_PLACEMENTS}")


def texture_name_for(material_name):
    wanted = re.sub(
        r"_(bmp|dds|png|tga|jpg|jpeg)\d*$", "", material_name,
        flags=re.IGNORECASE)
    return wanted if wanted != material_name else None


def texture_assets_by_name(content_root):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    result = {}
    for asset in registry.get_assets_by_path(
            unreal.Name(content_root), recursive=True):
        if str(asset.asset_class_path.asset_name) != "Texture2D":
            continue
        result.setdefault(str(asset.asset_name), []).append(asset)
    return result


def model_root_for(asset_path):
    for marker in ("/StaticMeshes/", "/SkeletalMeshes/"):
        if marker in asset_path:
            return asset_path.split(marker, 1)[0]
    return asset_path.rsplit("/", 1)[0]


def choose_local_texture(texture_assets, texture_name, asset_path):
    model_root = model_root_for(asset_path)
    candidates = [entry for entry in texture_assets.get(texture_name, [])
                  if str(entry.package_name).startswith(model_root + "/Textures/")]
    if len(candidates) != 1:
        paths = [str(entry.package_name) for entry in candidates]
        return None, (
            f"{asset_path}: Texture2D {texture_name}, "
            f"локальных кандидатов={len(candidates)} {paths}")

    entry = candidates[0]
    texture = unreal.load_asset(f"{entry.package_name}.{entry.asset_name}")
    if not isinstance(texture, unreal.Texture2D):
        return None, f"{entry.package_name}: это не Texture2D"
    return texture, ""


def material_slots(descriptor):
    if descriptor["kind"] == "static":
        return descriptor["asset"].get_editor_property("static_materials")
    return descriptor["asset"].get_editor_property("materials")


def bind_scene_textures(report, resolved, content_root):
    """Двухфазно связывает каждый используемый MIC с локальной текстурой."""
    textures = texture_assets_by_name(content_root)
    planned = {}
    missing = []

    descriptors = {
        part["path"]: part
        for parts in resolved.values()
        for part in parts
    }
    for descriptor in descriptors.values():
        slots = material_slots(descriptor)
        if not slots:
            missing.append(f"{descriptor['path']}: нет material slots")
            continue
        for slot_index, slot in enumerate(slots):
            material = slot.material_interface
            if not isinstance(material, unreal.MaterialInstanceConstant):
                missing.append(
                    f"{descriptor['path']} slot={slot_index}: нет MIC")
                continue

            material_path = material.get_path_name()
            if material_path in planned:
                continue
            texture_name = texture_name_for(material.get_name())
            if texture_name is None:
                missing.append(
                    f"{material_path}: имя не задаёт исходную текстуру")
                continue
            texture, error = choose_local_texture(
                textures, texture_name, descriptor["path"])
            if texture is None:
                missing.append(error)
                continue
            planned[material_path] = (material, texture, texture_name)

    if missing:
        report.error(f"ОТСУТСТВУЕТ_TEXTURE: {len(missing)}")
        for issue in missing[:20]:
            report.error("  " + issue)
        raise RuntimeError(f"ОТСУТСТВУЕТ_TEXTURE: {len(missing)} bindings")

    library = unreal.MaterialEditingLibrary
    for material, texture, texture_name in planned.values():
        library.set_material_instance_texture_parameter_value(
            material, unreal.Name(BASE_COLOR_PARAM), texture)
        if not unreal.EditorAssetLibrary.save_loaded_asset(
                material, only_if_is_dirty=False):
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_TEXTURE: MIC не сохранился "
                f"{material.get_path_name()}")

        current = library.get_material_instance_texture_parameter_value(
            material, unreal.Name(BASE_COLOR_PARAM))
        current_name = current.get_name() if current is not None else None
        if (current is None or current_name in PLACEHOLDERS
                or current_name != texture_name):
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_TEXTURE: {material.get_path_name()}."
                f"{BASE_COLOR_PARAM}={current_name}, ожидалась {texture_name}")

    report.line(
        f"TEXTURE BINDINGS: MIC={len(planned)} real={len(planned)} "
        "placeholders=0")


MAP_TRANSACTION_TEMP = OWNED_TARGET + "__TxnTemp"
MAP_TRANSACTION_BACKUP = OWNED_TARGET + "__TxnBackup"


def owned_map_transaction_paths(target):
    if target != OWNED_TARGET:
        raise RuntimeError(
            f"отказ map transaction: owned target={OWNED_TARGET}, "
            f"получено {target}")
    return MAP_TRANSACTION_TEMP, MAP_TRANSACTION_BACKUP


def owned_map_package_file(asset_path):
    if not asset_path.startswith("/Game/"):
        raise RuntimeError(f"путь карты вне /Game: {asset_path}")
    relative = asset_path.removeprefix("/Game/")
    return pathlib.Path(CONTENT_DIR).joinpath(*relative.split("/")).with_suffix(
        ".umap")


def delete_owned_map(library, asset_path):
    """Удаляет служебную карту транзакции и проверяет, что её не стало.

    В коммандлете `-run=pythonscript` и `delete_asset`, и
    `delete_loaded_asset` для `.umap` возвращают успех, ничего не удаляя:
    пакет остаётся и на диске, и в реестре (проверено 2026-08-10, ни одной
    записи в логе). Первая же попытка публикации после этого утыкается в
    «одновременно существуют canonical и backup», и карту приходится чистить
    руками.

    Поэтому результат вызова проверяется, а при молчаливом отказе пакет
    убирается с диска и реестр пересканируется — без пересканирования
    `does_asset_exist` продолжает видеть уже удалённый файл. Удалять так
    разрешено только два собственных пути транзакции: canonical карта под эту
    ветку не попадает никогда.
    """
    if asset_path not in (MAP_TRANSACTION_TEMP, MAP_TRANSACTION_BACKUP):
        raise RuntimeError(
            f"отказ удаления не служебной карты транзакции: {asset_path}")
    library.delete_asset(asset_path)
    if not library.does_asset_exist(asset_path):
        return True

    package = owned_map_package_file(asset_path)
    if package.is_file():
        package.unlink()
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(
        [posixpath.dirname(asset_path)], force_rescan=True)
    return not library.does_asset_exist(asset_path)


def recover_owned_map_transaction(report, library, target):
    """Восстанавливает только exact owned temp/backup либо fail-closed."""
    temp, backup = owned_map_transaction_paths(target)
    has_target = library.does_asset_exist(target)
    has_temp = library.does_asset_exist(temp)
    has_backup = library.does_asset_exist(backup)

    if has_target and has_backup:
        raise RuntimeError(
            "неоднозначное map transaction state: canonical и backup "
            f"существуют одновременно {target}, {backup}; temp={has_temp}")

    if has_backup and not has_target:
        if not library.rename_asset(backup, target):
            raise RuntimeError(
                f"map transaction recovery: не восстановлен {backup} -> {target}")
        report.line(f"MAP TRANSACTION RECOVERY: backup восстановлен {target}")
        has_target = True
        has_backup = False

    if has_temp:
        if not delete_owned_map(library, temp):
            raise RuntimeError(
                f"map transaction recovery: не удалён stale temp {temp}")
        report.line(f"MAP TRANSACTION RECOVERY: stale temp удалён {temp}")
        has_temp = False

    if has_backup:
        raise RuntimeError(
            f"неоднозначное map transaction recovery: backup остался {backup}")


def abort_owned_map_transaction(
        report, library, levels, source, target):
    """Убирает незавершённый exact temp, сохраняя/восстанавливая old target."""
    temp, backup = owned_map_transaction_paths(target)
    if not levels.load_level(source):
        raise RuntimeError(
            f"map transaction abort: исходная карта не загружена {source}")
    has_target = library.does_asset_exist(target)
    has_temp = library.does_asset_exist(temp)
    has_backup = library.does_asset_exist(backup)
    if has_target and has_temp and has_backup:
        raise RuntimeError(
            "неоднозначное map transaction abort state: одновременно существуют "
            f"{target}, {temp}, {backup}")
    if has_backup and not has_target:
        if not library.rename_asset(backup, target):
            raise RuntimeError(
                f"map transaction abort: не восстановлен {backup} -> {target}")
        report.line(f"MAP TRANSACTION ABORT: old target восстановлен {target}")
        has_target = True
        has_backup = False
    if has_temp:
        if not delete_owned_map(library, temp):
            raise RuntimeError(
                f"map transaction abort: не удалён temp {temp}")
        report.line(f"MAP TRANSACTION ABORT: temp удалён {temp}")
    if has_backup:
        raise RuntimeError(
            f"неоднозначное map transaction abort state: backup остался {backup}")


def rollback_published_map(
        report, library, levels, source, target, had_target):
    """Откатывает уже переименованный temp, пока exact backup ещё существует."""
    temp, backup = owned_map_transaction_paths(target)
    if not levels.load_level(source):
        raise RuntimeError(
            f"map transaction rollback: исходная карта не загружена {source}")
    if library.does_asset_exist(target):
        if library.does_asset_exist(temp):
            raise RuntimeError(
                f"map transaction rollback: temp уже существует {temp}")
        if not library.rename_asset(target, temp):
            raise RuntimeError(
                f"map transaction rollback: не убран published target {target}")
    if had_target:
        if (not library.does_asset_exist(backup)
                or not library.rename_asset(backup, target)):
            raise RuntimeError(
                f"map transaction rollback: old target не восстановлен {backup}")
    if library.does_asset_exist(temp):
        if not delete_owned_map(library, temp):
            raise RuntimeError(
                f"map transaction rollback: не удалён failed publish {temp}")
    report.line("MAP TRANSACTION ROLLBACK: previous target восстановлен")


def publish_owned_map_transaction(
        report, library, levels, source, target):
    temp, backup = owned_map_transaction_paths(target)
    if not library.does_asset_exist(temp):
        raise RuntimeError(f"map transaction publish: нет temp {temp}")
    if library.does_asset_exist(backup):
        raise RuntimeError(f"map transaction publish: stale backup {backup}")
    if not levels.load_level(source):
        raise RuntimeError(
            f"map transaction publish: исходная карта не загружена {source}")

    had_target = library.does_asset_exist(target)
    if had_target and not library.rename_asset(target, backup):
        raise RuntimeError(
            f"map transaction publish: не создан backup {target} -> {backup}")

    if not library.rename_asset(temp, target):
        if had_target:
            if not library.rename_asset(backup, target):
                raise RuntimeError(
                    f"map transaction publish rollback: не восстановлен {target}")
        raise RuntimeError(
            f"map transaction publish rename failed {temp} -> {target}")

    if not levels.load_level(target):
        rollback_published_map(
            report, library, levels, source, target, had_target)
        raise RuntimeError(
            f"map transaction publish: canonical map не загрузилась {target}")

    if had_target and not delete_owned_map(library, backup):
        rollback_published_map(
            report, library, levels, source, target, had_target)
        raise RuntimeError(
            f"map transaction publish: backup не удалён {backup}")
    if (not library.does_asset_exist(target)
            or library.does_asset_exist(temp)
            or library.does_asset_exist(backup)):
        raise RuntimeError(
            "map transaction publish: неверное финальное состояние")
    report.line(
        f"MAP TRANSACTION PUBLISHED: {temp} -> {target}; backup/temp=0")


def run_owned_map_transaction(
        report, source, target, build, readback, library=None, levels=None):
    library = library or unreal.EditorAssetLibrary
    levels = levels or unreal.get_editor_subsystem(
        unreal.LevelEditorSubsystem)
    temp, _backup = owned_map_transaction_paths(target)
    if source in (target, temp, MAP_TRANSACTION_BACKUP):
        raise RuntimeError(f"map transaction: недопустимый source {source}")

    recover_owned_map_transaction(report, library, target)
    if not library.does_asset_exist(source):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH/MAP: исходной карты нет {source}")
    if not levels.load_level(source):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MESH/MAP: исходная карта не загружена {source}")
    report.line(f"исходная карта загружена: {source}")

    try:
        duplicate = library.duplicate_asset(source, temp)
        if duplicate is None or not library.does_asset_exist(temp):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО: не дублирована карта {source} -> {temp}")
        if not levels.load_level(temp):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО: temp карта не загружена {temp}")
        report.line(f"рабочая temp карта загружена: {temp}")

        build(levels)
        if not levels.save_current_level():
            raise RuntimeError(f"map transaction: temp карта не сохранена {temp}")
        if not library.does_asset_exist(temp):
            raise RuntimeError(f"map transaction: после save нет temp {temp}")
        if not levels.load_level(temp):
            raise RuntimeError(f"map transaction: temp readback не загрузился {temp}")
        readback()
        report.line(f"MAP TRANSACTION READBACK: {temp}")
        publish_owned_map_transaction(
            report, library, levels, source, target)
    except Exception:
        abort_owned_map_transaction(
            report, library, levels, source, target)
        raise
    return levels


def component_mesh_asset(component):
    if isinstance(component, unreal.StaticMeshComponent):
        return component.get_editor_property("static_mesh")
    if isinstance(component, unreal.SkeletalMeshComponent):
        return component.get_editor_property("skeletal_mesh_asset")
    return None


def legacy_scene_asset_paths(actor):
    paths = []
    component_types = (
        unreal.StaticMeshComponent,
        unreal.SkeletalMeshComponent,
    )
    for component_type in component_types:
        for component in actor.get_components_by_class(component_type):
            asset = component_mesh_asset(component)
            if asset is None:
                continue
            path = asset.get_path_name()
            if path.startswith("/Game/All/"):
                paths.append(path)
    return paths


def remove_legacy_scene_actors(report):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    removed = 0
    removed_by_asset = 0
    for actor in list(actors.get_all_level_actors()):
        label = actor.get_actor_label()
        legacy_paths = legacy_scene_asset_paths(actor)
        if (label.startswith("Inst_") or label.startswith("Obj_")
                or legacy_paths):
            if actors.destroy_actor(actor):
                removed += 1
                if legacy_paths:
                    removed_by_asset += 1

    leftovers = [
        (actor.get_actor_label(), legacy_scene_asset_paths(actor))
        for actor in actors.get_all_level_actors()
        if legacy_scene_asset_paths(actor)
    ]
    report.line(
        f"legacy scene actors удалено: {removed} "
        f"(по /Game/All asset: {removed_by_asset})")
    if leftovers:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: осталось /Game/All actors="
            f"{len(leftovers)} {leftovers[:5]}")


def actor_bounds_xy(actor):
    origin, extent = actor.get_actor_bounds(False)
    return (
        float(origin.x - extent.x), float(origin.y - extent.y),
        float(origin.x + extent.x), float(origin.y + extent.y),
    )


def bounds_overlap(left, right):
    return (left[2] > right[0] and left[0] < right[2]
            and left[3] > right[1] and left[1] < right[3])


def place_reference_terrain(report, levels):
    mesh = unreal.load_asset(REFERENCE_TERRAIN_MESH)
    instance = unreal.load_asset(REFERENCE_TERRAIN_INSTANCE)
    if not isinstance(mesh, unreal.StaticMesh):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MESH: {REFERENCE_TERRAIN_MESH}")
    if not isinstance(instance, unreal.MaterialInstanceConstant):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MATERIAL: {REFERENCE_TERRAIN_INSTANCE}")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    matches = [
        actor for actor in actors.get_all_level_actors()
        if actor.get_actor_label() == REFERENCE_TERRAIN_LABEL
        or REFERENCE_TERRAIN_TAG in {str(tag) for tag in actor.tags}
    ]
    if len(matches) > 1:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: reference terrain actors={len(matches)}")
    if matches:
        reference = matches[0]
    else:
        reference = actors.spawn_actor_from_class(
            unreal.StaticMeshActor,
            unreal.Vector(*REFERENCE_TERRAIN_LOCATION),
            unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
        if reference is None:
            raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: reference terrain actor=0")
        if not reference.rename(
                REFERENCE_TERRAIN_LABEL, levels.get_current_level()):
            raise RuntimeError("reference terrain actor не переименован")

    reference.set_actor_label(REFERENCE_TERRAIN_LABEL, mark_dirty=True)
    reference.set_editor_property(
        "tags", [unreal.Name(REFERENCE_TERRAIN_TAG)])
    reference.set_actor_location(
        unreal.Vector(*REFERENCE_TERRAIN_LOCATION), False, False)
    reference.set_actor_rotation(
        unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), False)
    reference.set_actor_scale3d(unreal.Vector(1.0, 1.0, 1.0))
    reference.static_mesh_component.set_static_mesh(mesh)
    reference.static_mesh_component.set_material(0, instance)

    removed = []
    for actor in list(actors.get_all_level_actors()):
        if actor == reference or not actor.get_actor_label().startswith("Terrain_"):
            continue
        if not bounds_overlap(actor_bounds_xy(actor), REFERENCE_TERRAIN_BOUNDS):
            continue
        label = actor.get_actor_label()
        if not actors.destroy_actor(actor):
            raise RuntimeError(f"не удалён overlapping terrain actor {label}")
        removed.append(label)

    remaining_overlaps = [
        actor.get_actor_label()
        for actor in actors.get_all_level_actors()
        if actor != reference
        and actor.get_actor_label().startswith("Terrain_")
        and bounds_overlap(actor_bounds_xy(actor), REFERENCE_TERRAIN_BOUNDS)
    ]
    if remaining_overlaps:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО: остались overlapping Terrain actors "
            f"{remaining_overlaps}")
    reference_bounds = actor_bounds_xy(reference)
    if any(abs(actual - expected) > 1.0 for actual, expected in zip(
            reference_bounds, REFERENCE_TERRAIN_BOUNDS)):
        raise RuntimeError(
            f"НЕВЕРНЫЕ_ГРАНИЦЫ reference terrain: {reference_bounds}/"
            f"{REFERENCE_TERRAIN_BOUNDS}")
    report.line(
        f"REFERENCE TERRAIN: actor=1 removedLegacyOverlaps={len(removed)} "
        f"remainingLegacyOverlaps=0 labels={sorted(removed)}")


def anchor_transform(record):
    location = unreal.Vector(*source_location_to_ue(
        record["x"], record["y"], record["zCm"]))
    yaw = source_scene_yaw_to_ue(record["sourceYawDegrees"])
    rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw)
    return location, rotation


def actor_label(record, part_index, stem):
    key = record["sourceKey"]
    return (
        f"{ACTOR_PREFIX}S{key['sectionIndex']}_I{key['slotIndex']}_"
        f"P{part_index}_{stem}")


def verify_skeletal_capture_pose(component, descriptor):
    expected_position = descriptor["sample_frame"] / CAPTURE_HZ
    animation_data = component.get_editor_property("animation_data")
    expected_state = {
        "anim_to_play": descriptor["animation"],
        "saved_looping": False,
        "saved_playing": False,
        "saved_position": expected_position,
        "saved_play_rate": 1.0,
    }
    for name, expected in expected_state.items():
        actual = animation_data.get_editor_property(name)
        if isinstance(expected, float):
            matches = math.isclose(
                float(actual), expected, rel_tol=0.0, abs_tol=1.0e-5)
        else:
            matches = actual == expected
        if not matches:
            raise RuntimeError(
                f"ОТСУТСТВУЕТ_ANIMATION serialized {name}: "
                f"{actual!r}/{expected!r}")
    if component.get_editor_property("pause_anims") is not True:
        raise RuntimeError("ОТСУТСТВУЕТ_ANIMATION: pause_anims=False")
    if component.is_playing():
        raise RuntimeError("ОТСУТСТВУЕТ_ANIMATION: component всё ещё playing")
    if not math.isclose(
            float(component.get_position()), expected_position,
            rel_tol=0.0, abs_tol=1.0e-5):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION position: "
            f"{component.get_position()}/{expected_position}")
    return expected_position


def freeze_skeletal_capture_pose(component, descriptor):
    expected_sample = (CAPTURE_TICK - 1) % descriptor["frame_count"]
    if descriptor["sample_frame"] != expected_sample:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_ANIMATION sampleFrame: "
            f"{descriptor['sample_frame']}/{expected_sample}")
    position = descriptor["sample_frame"] / CAPTURE_HZ
    component.modify()
    component.override_animation_data(
        descriptor["animation"], False, False, position, 1.0)
    component.set_editor_property("pause_anims", True)
    return verify_skeletal_capture_pose(component, descriptor)


def spawn_part(actors, descriptor, location, rotation, label):
    if descriptor["kind"] == "static":
        actor = actors.spawn_actor_from_class(
            unreal.StaticMeshActor, location, rotation)
        if actor is not None:
            actor.static_mesh_component.set_editor_property(
                "disallow_nanite", descriptor["disallow_nanite"])
            actor.static_mesh_component.set_static_mesh(descriptor["asset"])
    else:
        actor = actors.spawn_actor_from_class(
            unreal.SkeletalMeshActor, location, rotation)
        if actor is not None:
            actor.skeletal_mesh_component.set_skeletal_mesh(descriptor["asset"])
            freeze_skeletal_capture_pose(
                actor.skeletal_mesh_component, descriptor)

    if actor is None:
        return None
    actor.set_actor_scale3d(unreal.Vector(1.0, 1.0, 1.0))
    actor.set_actor_label(label)
    return actor


def source_key_tuple(record):
    key = record["sourceKey"]
    return (key["sectionIndex"], key["slotIndex"], key["byteOffset"])


def resolve_city_lighting(references, database_path):
    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "stats": {"referenceObjectCount": len(references)},
        "records": references,
    }
    lighting = resolve_reference_lighting(manifest, database_path)
    if len(lighting) != len(references):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО lighting: {len(lighting)}/"
            f"{len(references)}")
    for index, (record, resolved_light) in enumerate(zip(references, lighting)):
        if (source_key_tuple(record) != tuple(resolved_light["sourceKey"])
                or record["modelId"] != resolved_light["modelId"]):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО lighting order: index={index}")
    return lighting


def apply_component_lighting(actor, descriptor, payload):
    if len(payload) != 33 or not all(math.isfinite(value) for value in payload):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО lighting payload: {len(payload)}/33 finite")
    component = (
        actor.static_mesh_component
        if descriptor["kind"] == "static"
        else actor.skeletal_mesh_component
    )
    component.modify()
    component.set_default_custom_primitive_data_float_array(
        0, [float(value) for value in payload])
    component.set_cast_shadow(False)
    component.set_affect_dynamic_indirect_lighting(False)
    component.set_affect_distance_field_lighting(False)


def place_reference_city(
        report, references, resolved, expected_placements, lighting):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    placed_static = 0
    placed_skeletal = 0
    nanite_disallowed = 0
    complete_anchors = 0

    mode_counts = {}
    for record, resolved_light in zip(references, lighting):
        location, rotation = anchor_transform(record)
        parts = resolved[record["modelId"]]
        placed_for_anchor = 0
        for part_index, descriptor in enumerate(parts):
            actor = spawn_part(
                actors, descriptor, location, rotation,
                actor_label(record, part_index, descriptor["stem"]))
            if actor is None:
                continue
            apply_component_lighting(
                actor, descriptor, resolved_light["payload"])
            placed_for_anchor += 1
            if descriptor["kind"] == "static":
                placed_static += 1
                nanite_disallowed += int(descriptor["disallow_nanite"])
            else:
                placed_skeletal += 1
        if placed_for_anchor == len(parts):
            complete_anchors += 1
        mode = resolved_light["mode"]
        mode_counts[mode] = mode_counts.get(mode, 0) + 1

    placed = placed_static + placed_skeletal
    report.line(
        f"CITY PLACEMENT: anchors={complete_anchors}/{len(references)} "
        f"parts={placed}/{expected_placements} static={placed_static} "
        f"skeletal={placed_skeletal} frozenAtTick119={placed_skeletal} "
        f"disallowNanite={nanite_disallowed}")
    report.line(
        f"LEGACY LIGHTING: anchors={len(references)} payloadFloats=33 modes="
        + ",".join(
            f"{mode}:{mode_counts[mode]}" for mode in sorted(mode_counts)))
    if complete_anchors != len(references) or placed != expected_placements:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: anchors={complete_anchors}/"
            f"{len(references)}, parts={placed}/{expected_placements}")
    if placed_skeletal != EXPECTED_ANIMATED_PLACEMENTS:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО animated placements: "
            f"{placed_skeletal}/{EXPECTED_ANIMATED_PLACEMENTS}")


def resolve_character_appearance(path, character_type):
    with open(path, "r", encoding="utf-8") as handle:
        catalog = json.load(handle)
    entry = catalog.get("characters", {}).get(str(character_type))
    if not isinstance(entry, dict):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: characters.{character_type}")

    parts = entry.get("parts")
    if parts is None:
        module_index = str(entry.get("moduleIndex", ""))
        parts = []
        for item_id in entry.get("defaultItemIds", []):
            mesh = (catalog.get("items", {}).get(str(item_id), {})
                    .get("meshesByModule", {}).get(module_index))
            if not mesh:
                raise RuntimeError(
                    f"ОТСУТСТВУЕТ_MESH: item={item_id} module={module_index}")
            parts.append(mesh)

    animation = entry.get("animation")
    if (not isinstance(parts, list) or len(parts) != 5
            or not all(isinstance(part, str) and part.startswith("/Game/")
                       for part in parts)):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: characters.{character_type} parts != 5")
    if not isinstance(animation, str) or not animation.startswith("/Game/"):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MESH: characters.{character_type}.animation")
    return parts, animation


def character_ground_height_cm(source_x, source_y):
    path = pathlib.Path(DEFAULT_HEIGHT_MAP)
    if not path.is_file():
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH/HEIGHT: {path}")
    width = height_grid_width(path)
    with path.open("rb") as handle:
        return sample_height_cm(handle, width, int(source_x), int(source_y))


def load_character_meshes(parts, animation):
    meshes = []
    for path in parts:
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: {path}")
        meshes.append(mesh)
    sequence = unreal.load_asset(animation)
    if not isinstance(sequence, unreal.AnimSequence):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: animation {animation}")
    return meshes, sequence


def combined_character_extent(meshes):
    """Низ и высота всей сборки в единицах ассета.

    Границы одной части роста не показывают: основная часть сборки — голова,
    её низ висит на высоте плеч. Поэтому объём считается по всем пяти частям
    сразу, как это делала прежняя сборка в C++.
    """
    lowest = None
    highest = None
    for mesh in meshes:
        bounds = mesh.get_bounds()
        bottom = float(bounds.origin.z) - float(bounds.box_extent.z)
        top = float(bounds.origin.z) + float(bounds.box_extent.z)
        lowest = bottom if lowest is None else min(lowest, bottom)
        highest = top if highest is None else max(highest, top)
    height = (highest or 0.0) - (lowest or 0.0)
    if not math.isfinite(height) or height <= 0.0:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: высота сборки персонажа {height}")
    return lowest, height


def spawn_character(report, parts, animation):
    """Ставит screenshot-манекен пятью SkeletalMeshActor в одной точке.

    Игровой ``ACorsairsPlayerCharacter`` собирает облик через
    ``ApplyAppearance`` и каталог экипировки, и ни один из этих путей в Python
    не выведен. Добавить части компонентами тоже нельзя: ни
    ``add_component_by_class``, ни ``add_instance_component`` редакторный
    Python UE 5.8 не отдаёт. Поэтому каждая часть — отдельный актор в общем
    transform; скелет у всех частей один, поза одна и та же, и сборка
    выглядит единым телом.

    Поза заморожена на нулевом кадре — так же, как анимированные постройки
    квартала замораживаются на 119-м: кадр парности должен быть повторим.
    """
    meshes, sequence = load_character_meshes(parts, animation)
    lowest, height = combined_character_extent(meshes)
    scale = CHARACTER_HEIGHT_CM / height

    # Подошвы на землю: низ объединённого объёма ставится на поверхность.
    # Высота берётся из того же half-meter raster, что и у NPC, — исходная
    # константа парности описывает точку наведения камеры, а не землю.
    ground_z = character_ground_height_cm(
        SOURCE_CHARACTER_LOCATION[0], SOURCE_CHARACTER_LOCATION[1])
    location = source_location_to_ue(
        SOURCE_CHARACTER_LOCATION[0], SOURCE_CHARACTER_LOCATION[1],
        ground_z - lowest * scale)
    rotation = unreal.Rotator(
        roll=0.0,
        pitch=0.0,
        yaw=standalone_character_yaw_to_ue(SOURCE_CHARACTER_DIRECTION))

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for index, mesh in enumerate(meshes):
        actor = actors.spawn_actor_from_class(
            unreal.SkeletalMeshActor, unreal.Vector(*location), rotation)
        if actor is None:
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО: character part actor={index}")
        actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))
        component = actor.skeletal_mesh_component
        component.set_skeletal_mesh(mesh)
        component.set_editor_property(
            "visibility_based_anim_tick_option",
            unreal.VisibilityBasedAnimTickOption
            .ALWAYS_TICK_POSE_AND_REFRESH_BONES)
        component.override_animation_data(sequence, False, False, 0.0, 1.0)
        actor.set_actor_label(f"{CHARACTER_LABEL}_Part{index}", mark_dirty=True)

    report.line(
        f"CHARACTER: type1 parts={len(meshes)}/5 animation=1/1 "
        f"scale={scale:.3f} groundZ={ground_z:.0f}")


def character_part_actors(actors):
    return [actor for actor in actors.get_all_level_actors()
            if actor.get_actor_label().startswith(f"{CHARACTER_LABEL}_Part")]


def verify_saved_character(report):
    """Читает манекен с диска: пять акторов, ни одного лишнего.

    Части — отдельные акторы, и потерять их так же легко, как и что угодно
    другое в расстановке: молчаливая пропажа четырёх выглядела бы как удачный
    прогон.
    """
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    parts = character_part_actors(actors)
    if len(parts) != 5:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: частей персонажа после save {len(parts)}/5")
    report.line(f"CHARACTER READBACK: parts={len(parts)}/5")


def place_camera(report):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    # Оригинал: 35 м по плоскости, 50 м по высоте, vertical FOV 32 degrees.
    # Для viewport 16:9 это horizontal FOV 54.0222067 degrees.
    # Q=(-sourceY, sourceX) сохраняет handedness: camera смотрит вдоль UE +X.
    location = unreal.Vector(*REFERENCE_CAMERA["eye"])
    rotation = unreal.Rotator(
        roll=0.0,
        pitch=REFERENCE_CAMERA["pitch"],
        yaw=REFERENCE_CAMERA["yaw"])
    camera = actors.spawn_actor_from_class(
        unreal.CameraActor, location, rotation)
    if camera is None:
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: camera actor=0")
    camera.set_actor_label(CAMERA_LABEL)
    component = camera.get_editor_property("camera_component")
    component.set_editor_property("field_of_view", 54.0222067)
    component.set_editor_property("post_process_blend_weight", 1.0)
    settings = component.get_editor_property("post_process_settings")
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
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(location, rotation)
    report.line(
        "CAMERA: parity target=(-278475,223325,100) "
        "eye=(-281975,223325,5100) yaw=0 HFOV=54.0222067")


def verify_saved_actor_count(report, expected_placements):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actual = sum(
        1 for actor in actors.get_all_level_actors()
        if actor.get_actor_label().startswith(ACTOR_PREFIX))
    report.line(
        f"SAVED ACTOR COUNT: {actual}/{expected_placements}")
    if actual != expected_placements:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО saved actors: "
            f"{actual}/{expected_placements}")


def main(report, args):
    if RefuseIfEditorOpen(report):
        raise RuntimeError("редактор уже открыт")
    validate_args(args)

    source_references, stats = load_reference_records(
        os.path.abspath(args.manifest))
    references, helper_references = filter_visual_references(
        source_references, os.path.abspath(args.database))
    report.line(
        f"VISUAL FILTER: sourceAnchors={len(source_references)} "
        f"visibleType0={len(references)} hiddenHelpers={len(helper_references)}")
    lighting = resolve_city_lighting(
        references, os.path.abspath(args.database))
    source_root, models = load_model_map(os.path.abspath(args.model_map))
    resolved, expected_placements = preflight_meshes(
        report, references, source_root, models, args.content_root)
    validate_city_scope_counts(
        references, helper_references, expected_placements)
    bind_scene_textures(report, resolved, args.content_root)
    parts, animation = resolve_character_appearance(
        os.path.abspath(args.character_map), args.character_type)

    def build_temp_level(levels):
        remove_legacy_scene_actors(report)
        place_reference_terrain(report, levels)
        place_reference_city(
            report, references, resolved, expected_placements, lighting)
        spawn_character(report, parts, animation)
        place_camera(report)
        verify_saved_actor_count(report, expected_placements)
        validate_city_scope_counts(
            references, helper_references, expected_placements)

    def readback_temp_level():
        verify_saved_actor_count(report, expected_placements)
        validate_city_scope_counts(
            references, helper_references, expected_placements)
        verify_saved_character(report)

    run_owned_map_transaction(
        report, args.source, args.target,
        build_temp_level, readback_temp_level)
    if not unreal.EditorAssetLibrary.does_asset_exist(args.target):
        raise RuntimeError(f"НЕВЕРНОЕ_КОЛИЧЕСТВО: карта не сохранена {args.target}")
    report.line(
        f"УСПЕХ: {args.target}; sourceReferenceAnchors="
        f"{stats['referenceObjectCount']}; visualAnchors={len(references)}; "
        f"hiddenHelpers={len(helper_references)}; "
        f"partActors={expected_placements}")


report = Reporter("place_scene_progress_city")
try:
    main(report, parse_args(sys.argv[1:]))
finally:
    report.close()
