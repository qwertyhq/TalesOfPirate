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
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402


SCHEMA_VERSION = 2
DEFAULT_SOURCE = "/Game/Maps/Garner"
OWNED_TARGET = "/Game/Maps/GarnerSceneProgressCity"
DEFAULT_CONTENT_ROOT = "/Game/SceneParityCity"
DEFAULT_CHARACTER_TYPE = "1"

# Каноническая точка parity-кадра: тот же фонтанный квартал, который виден
# в оригинальном клиенте. Координата Y уже переведена в систему UE.
CHARACTER_LOCATION = (223325.0, -278475.0, 100.0)
CHARACTER_LABEL = "SceneProgressCity_Test195126"
CAMERA_LABEL = "SceneProgressCity_Camera"
ACTOR_PREFIX = "SceneProgressCity_Part_"

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
                       (args.character_map, "character-map")):
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
                asset = unreal.load_asset(static_path)
                kind = "static"
                actual_path = static_path
                if not isinstance(asset, unreal.StaticMesh):
                    asset = unreal.load_asset(skeletal_path)
                    kind = "skeletal"
                    actual_path = skeletal_path

                if kind == "static" and not isinstance(asset, unreal.StaticMesh):
                    missing.append(
                        f"modelId={model_id}: {static_path} "
                        f"(и {skeletal_path})")
                    continue
                if kind == "skeletal" and not isinstance(asset, unreal.SkeletalMesh):
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
    report.line(
        f"MESH PREFLIGHT: models={len(resolved)} uniqueParts={len(unique_parts)} "
        f"static={static_parts} skeletal={skeletal_parts} "
        f"expectedPlacements={expected_placements}")
    return resolved, expected_placements


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


def recreate_target_level(report, source, target):
    library = unreal.EditorAssetLibrary
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not library.does_asset_exist(source):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH/MAP: исходной карты нет {source}")

    levels.load_level(source)
    report.line(f"исходная карта загружена: {source}")
    if library.does_asset_exist(target):
        if not library.delete_asset(target):
            raise RuntimeError(f"НЕВЕРНОЕ_КОЛИЧЕСТВО: не удалён {target}")
        report.line(f"предыдущий owned target удалён: {target}")

    duplicate = library.duplicate_asset(source, target)
    if duplicate is None or not library.does_asset_exist(target):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: не дублирована карта {source} -> {target}")
    levels.load_level(target)
    report.line(f"рабочая карта загружена: {target}")
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


def unwind_degrees(value):
    result = math.fmod(value, 360.0)
    if result > 180.0:
        result -= 360.0
    elif result < -180.0:
        result += 360.0
    return result


def anchor_transform(record):
    location = unreal.Vector(
        float(record["x"]), -float(record["y"]), float(record["zCm"]))
    yaw = unwind_degrees(180.0 - float(record["sourceYawDegrees"]))
    rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw)
    return location, rotation


def actor_label(record, part_index, stem):
    key = record["sourceKey"]
    return (
        f"{ACTOR_PREFIX}S{key['sectionIndex']}_I{key['slotIndex']}_"
        f"P{part_index}_{stem}")


def spawn_part(actors, descriptor, location, rotation, label):
    if descriptor["kind"] == "static":
        actor = actors.spawn_actor_from_class(
            unreal.StaticMeshActor, location, rotation)
        if actor is not None:
            actor.static_mesh_component.set_static_mesh(descriptor["asset"])
    else:
        actor = actors.spawn_actor_from_class(
            unreal.SkeletalMeshActor, location, rotation)
        if actor is not None:
            actor.skeletal_mesh_component.set_skeletal_mesh(descriptor["asset"])

    if actor is None:
        return None
    actor.set_actor_scale3d(unreal.Vector(1.0, 1.0, 1.0))
    actor.set_actor_label(label)
    return actor


def place_reference_city(report, references, resolved, expected_placements):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    placed_static = 0
    placed_skeletal = 0
    complete_anchors = 0

    for record in references:
        location, rotation = anchor_transform(record)
        parts = resolved[record["modelId"]]
        placed_for_anchor = 0
        for part_index, descriptor in enumerate(parts):
            actor = spawn_part(
                actors, descriptor, location, rotation,
                actor_label(record, part_index, descriptor["stem"]))
            if actor is None:
                continue
            placed_for_anchor += 1
            if descriptor["kind"] == "static":
                placed_static += 1
            else:
                placed_skeletal += 1
        if placed_for_anchor == len(parts):
            complete_anchors += 1

    placed = placed_static + placed_skeletal
    report.line(
        f"CITY PLACEMENT: anchors={complete_anchors}/{len(references)} "
        f"parts={placed}/{expected_placements} static={placed_static} "
        f"skeletal={placed_skeletal}")
    if complete_anchors != len(references) or placed != expected_placements:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: anchors={complete_anchors}/"
            f"{len(references)}, parts={placed}/{expected_placements}")


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


def require_callable(actor, name):
    method = getattr(actor, name, None)
    if not callable(method):
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MESH/API: {actor.get_class().get_name()}.{name}")
    return method


def spawn_character(report, parts, animation):
    character_class = unreal.load_class(
        None, "/Script/CorsairsGame.CorsairsPlayerCharacter")
    if character_class is None:
        raise RuntimeError("ОТСУТСТВУЕТ_MESH: CorsairsPlayerCharacter class")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = actors.spawn_actor_from_class(
        character_class,
        unreal.Vector(*CHARACTER_LOCATION),
        unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    if actor is None:
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: character actor=0")

    set_body_mesh = require_callable(actor, "set_body_mesh")
    add_body_part = require_callable(actor, "add_body_part")
    finish_body = require_callable(actor, "finish_body")
    set_body_animation = require_callable(actor, "set_body_animation")
    if not set_body_mesh(parts[0]):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: {parts[0]}")
    for part in parts[1:]:
        if not add_body_part(part):
            raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: {part}")
    finish_body()
    if not set_body_animation(animation):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH: animation {animation}")
    actor.set_actor_label(CHARACTER_LABEL)
    report.line("CHARACTER: type1 parts=5/5 animation=1/1")


def look_at_rotation(origin, target):
    dx = target.x - origin.x
    dy = target.y - origin.y
    dz = target.z - origin.z
    return unreal.Rotator(
        roll=0.0,
        pitch=math.degrees(math.atan2(dz, math.hypot(dx, dy))),
        yaw=math.degrees(math.atan2(dy, dx)))


def place_camera(report):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    # Оригинал: 35 м по плоскости, 50 м по высоте, vertical FOV 32 degrees.
    # Для viewport 16:9 это horizontal FOV 54.0222067 degrees.
    # Original eye=(2233.25,2749.75,51.0) m; source +Y maps to UE -Y.
    location = unreal.Vector(223325.0, -274975.0, 5100.0)
    target = unreal.Vector(*CHARACTER_LOCATION)
    rotation = look_at_rotation(location, target)
    camera = actors.spawn_actor_from_class(
        unreal.CameraActor, location, rotation)
    if camera is None:
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: camera actor=0")
    camera.set_actor_label(CAMERA_LABEL)
    camera.get_editor_property("camera_component").set_editor_property(
        "field_of_view", 54.0222067)
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(location, rotation)
    report.line(
        "CAMERA: parity target=(223325,-278475,100) "
        "eye=(223325,-274975,5100) HFOV=54.0222067")


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

    references, stats = load_reference_records(os.path.abspath(args.manifest))
    source_root, models = load_model_map(os.path.abspath(args.model_map))
    resolved, expected_placements = preflight_meshes(
        report, references, source_root, models, args.content_root)
    bind_scene_textures(report, resolved, args.content_root)
    parts, animation = resolve_character_appearance(
        os.path.abspath(args.character_map), args.character_type)

    levels = recreate_target_level(report, args.source, args.target)
    remove_legacy_scene_actors(report)
    place_reference_city(report, references, resolved, expected_placements)
    spawn_character(report, parts, animation)
    place_camera(report)
    verify_saved_actor_count(report, expected_placements)

    levels.save_current_level()
    if not unreal.EditorAssetLibrary.does_asset_exist(args.target):
        raise RuntimeError(f"НЕВЕРНОЕ_КОЛИЧЕСТВО: карта не сохранена {args.target}")
    report.line(
        f"УСПЕХ: {args.target}; referenceAnchors="
        f"{stats['referenceObjectCount']}; partActors={expected_placements}")


report = Reporter("place_scene_progress_city")
try:
    main(report, parse_args(sys.argv[1:]))
finally:
    report.close()
