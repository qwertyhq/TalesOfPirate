"""Ставит весь Garner City из полного манифеста сцены в /Game/Maps/Garner.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/place_garner_city.py --manifest <scene.json.objects.json>"

Отчёт: `Scripts/reports/place_garner_city.txt`.

Отличия от place_scene_progress_city: фильтр ``inReferenceSet`` снят —
ставятся все 46991 статических placement; статика группируется по
уникальной части в ISM-компоненты с per-instance custom data (33-float
legacy lighting payload); скелетные части остаются актёрами с
замороженной позой. Числовые константы эталонного квартала не
дублируются — проверки считаются от манифеста.

Неприкосновенное: базис Q(x,y,z)=(-y,x,z), yaw объектов сцены
`source_yaw - 90`, точность — перенаправление из `scene_coordinate_basis`.
Старая /Game/Maps/Garner зеркальной эпохи пересобирается заново через
map transaction: temp → publish → удаление backup.
"""

import argparse
import json
import math
import os
import pathlib
import posixpath
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402
from scene_coordinate_basis import (                 # noqa: E402
    source_location_to_ue,
    source_scene_yaw_to_ue,
)
from scene_lighting import resolve_reference_lighting  # noqa: E402

# Разбор half-meter raster живёт в расстановке NPC; вторая копия чтения r16
# разошлась бы с первой при первом же изменении формата.
from place_scene_progress_npcs import (              # noqa: E402
    height_grid_width,
    sample_height_cm,
)

SCHEMA_VERSION = 2
CAPTURE_HZ = 30.0
OWNED_TARGET = "/Game/Maps/Garner"
CONTENT_ROOT = "/Game/GarnerCity"
TERRAIN_ROOT = "/Game/Terrain/GarnerCityQ"
TERRAIN_MATERIALS_ROOT = "/Game/Terrain/Materials"
LIGHTING_PAYLOAD_FLOATS = 33

TERRAIN_TILES_TABLE = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "Data", "terrain_tile_textures.json"))
DEFAULT_DATABASE = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "databases", "gamedata.sqlite"))
DEFAULT_HEIGHT_MAP = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "Data", "Heights", "garner.height.r16"))
CONTENT_DIR = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "Content"))

# Счётчик дыр: двух моделей нет даже в scene_objects — у них нет ни «имени»,
# ни geometry. Каждую дыру считаем явно, а не молча пропускаем.
MISSING_MODEL_IDS = (91, 92)

# Сервисные маркеры: в scene_objects поле type — это SCENEOBJ_TYPE оригинала
# (Scene.h:55): 1 — место сидения/прислонения, 2 — маркер проходимости,
# 3 — точечный свет, 6 — источник звука. Их меши (yyyy0**.lmo) — цветные
# квады для редактора карт; оригинальный клиент раскладывает такие объекты
# по служебным спискам и как геометрию не рисует. Без фильтра площадь
# зарастает красными «Sit» и зелёными «Passable» плашками — 815 штук на
# garner. Список берётся из базы при загрузке, не зашивается.
SERVICE_MODEL_TYPES = (1, 2, 3, 4, 6)
EXPECTED_SOURCE_RECORDS = 50017
EXPECTED_SCENE_MODELS = 46991
EXPECTED_MISSING_PLACEMENTS = 11
EXPECTED_STATIC_PARTS_MIN = 1

PLACEHOLDER_TEXTURES = ("T_White_srgb", "T_White_Linear", "DefaultTexture")

ACTOR_PREFIX = "GarnerCity_"
TERRAIN_ACTOR_PREFIX = "Terrain_"

SPAWN_SOURCE_XY = (223325.0, 278475.0)


def parse_args(argv):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(
        description="Поставить весь Garner City из полного манифеста сцены")
    parser.add_argument("--manifest", required=True)
    parser.add_argument(
        "--model-map",
        default=os.path.join(script_dir, "..", "..",
                             "artifacts", "garner_model_map.json"))
    parser.add_argument("--database", default=DEFAULT_DATABASE)
    parser.add_argument("--content-root", default=CONTENT_ROOT)
    parser.add_argument("--terrain-root", default=TERRAIN_ROOT)
    parser.add_argument("--target", default=OWNED_TARGET)
    return parser.parse_args(argv)


def validate_args(args):
    if not args.target == OWNED_TARGET:
        raise RuntimeError(
            f"отказ: скрипт владеет только {OWNED_TARGET}, получено {args.target}")
    for path, name in ((args.manifest, "manifest"),
                       (args.model_map, "model-map"),
                       (args.database, "database")):
        if not os.path.isfile(path):
            raise RuntimeError(f"нет {name}: {path}")


def load_service_model_ids(database_path):
    """Модели сервисных маркеров из scene_objects (SCENEOBJ_TYPE != 0)."""
    db = sqlite3.connect(database_path)
    try:
        placeholders = ",".join("?" * len(SERVICE_MODEL_TYPES))
        rows = db.execute(
            f"SELECT id FROM scene_objects WHERE type IN ({placeholders})",
            SERVICE_MODEL_TYPES).fetchall()
    finally:
        db.close()
    return {int(row[0]) for row in rows}


def load_scene_records(path, service_ids=frozenset()):
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
    if stats.get("sourceRecordCount") != len(records):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: stats sourceRecordCount")
    scene = [r for r in records if r.get("type") == 0]
    if len(scene) != stats.get("sceneModelCount"):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: sceneModelCount")
    # Двух моделей нет ни в каталоге, ни в базе: выкидываем явно, со счётом.
    visual = [r for r in scene if int(r["modelId"]) not in MISSING_MODEL_IDS]
    if len(scene) - len(visual) != EXPECTED_MISSING_PLACEMENTS:
        raise RuntimeError(
            "НЕВЕРНОЕ_КОЛИЧЕСТВО: holes отсутствующих моделей != "
            f"{EXPECTED_MISSING_PLACEMENTS}")
    missing_report = {}
    for r in scene:
        if int(r["modelId"]) in MISSING_MODEL_IDS:
            missing_report.setdefault(int(r["modelId"]), 0)
            missing_report[int(r["modelId"])] += 1

    # Сервисные маркеры выкидываются со счётом, как и дыры: оригинал их не
    # рисует (см. SERVICE_MODEL_TYPES выше).
    service_report = {}
    if service_ids:
        kept = []
        for r in visual:
            model_id = int(r["modelId"])
            if model_id in service_ids:
                service_report[model_id] = service_report.get(model_id, 0) + 1
            else:
                kept.append(r)
        visual = kept
    return visual, missing_report, service_report


def load_model_map(path, content_root):
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    source_root = data.get("contentRoot")
    models = data.get("models")
    if not isinstance(source_root, str) or not isinstance(models, dict):
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: model-map неполон")
    if source_root.rstrip("/") != content_root.rstrip("/"):
        raise RuntimeError(
            f"model-map contentRoot={source_root}, а --content-root {content_root}")
    return models


def compute_lighting(records, database_path):
    synthetic = {
        "schemaVersion": SCHEMA_VERSION,
        "stats": {"referenceObjectCount": len(records)},
        "records": [dict(r, inReferenceSet=True) for r in records],
    }
    lighting = resolve_reference_lighting(
        synthetic, os.path.abspath(database_path))
    if len(lighting) != len(records):
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО lighting: {len(lighting)}/{len(records)}")
    for record, resolved_light in zip(records, lighting):
        key = resolved_light.get("sourceKey")
        source = record.get("sourceKey", {})
        if (tuple(key) != (source.get("sectionIndex"),
                           source.get("slotIndex"),
                           source.get("byteOffset"))
                or record["modelId"] != resolved_light["modelId"]):
            raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО lighting order")
    return lighting


def read_capture_metadata(part_stem):
    """Читает captureTick/sampleFrame из glTF extras, если они есть."""
    candidate = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..",
        "artifacts", "models-scene", f"{part_stem}.gltf")
    candidate = os.path.abspath(candidate)
    if not os.path.isfile(candidate):
        return None
    with open(candidate, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    for node in document.get("nodes", []):
        capture = (node.get("extras") or {}).get("corsairsLegacyCapture")
        if not isinstance(capture, dict):
            continue
        bone = capture.get("bone") or {}
        if bone.get("policy") != "preserveAnimated":
            continue
        frame_count = bone.get("frameCount")
        sample_frame = bone.get("sampleFrame")
        if isinstance(frame_count, int) and isinstance(sample_frame, int):
            return {"frameCount": frame_count, "sampleFrame": sample_frame}
    return None


def resolve_parts(records, models, content_root):
    model_ids = sorted({int(r["modelId"]) for r in records})
    resolved = {}
    missing = []
    placed_count_static = 0
    placed_count_skinned = 0
    for model_id in model_ids:
        source_paths = models.get(str(model_id))
        if not isinstance(source_paths, list) or not source_paths:
            missing.append(f"modelId={model_id}: нет parts в model-map")
            continue

        descriptors = []
        for source_path in source_paths:
            static_path = source_path
            skeletal_path = source_path.replace(
                "/StaticMeshes/", "/SkeletalMeshes/", 1)
            stem = skeletal_path.rsplit("/", 1)[-1]

            skeletal = unreal.load_asset(skeletal_path)
            if isinstance(skeletal, unreal.SkeletalMesh):
                anim = unreal.load_asset(skeletal_path + "_Anim")
                if not isinstance(anim, unreal.AnimSequence):
                    missing.append(f"{skeletal_path}: нет {skeletal_path}_Anim")
                    continue
                meta = read_capture_metadata(stem)
                if meta is None:
                    sample_seconds = 0.0
                else:
                    sample_seconds = meta["sampleFrame"] / CAPTURE_HZ
                descriptors.append({
                    "kind": "skeletal",
                    "asset": skeletal,
                    "path": skeletal_path,
                    "stem": stem,
                    "animation": anim,
                    "sample_seconds": sample_seconds,
                })
                placed_count_skinned += 1
                continue

            static = unreal.load_asset(static_path)
            if isinstance(static, unreal.StaticMesh):
                allowed = {
                    unreal.BlendMode.BLEND_OPAQUE,
                    unreal.BlendMode.BLEND_MASKED,
                }
                disallow_nanite = any(
                    slot.material_interface is not None
                    and slot.material_interface.get_blend_mode() not in allowed
                    for slot in static.get_editor_property("static_materials"))
                descriptors.append({
                    "kind": "static",
                    "asset": static,
                    "path": static_path,
                    "stem": stem,
                    "disallow_nanite": disallow_nanite,
                })
                placed_count_static += 1
                continue

            missing.append(f"{static_path} (и {skeletal_path})")

        if len(descriptors) == len(source_paths):
            resolved[model_id] = descriptors

    if missing:
        raise RuntimeError(
            f"ОТСУТСТВУЕТ_MESH: {len(missing)} parts: {missing[:10]}")
    return resolved


def owned_map_package_file(asset_path):
    if not asset_path.startswith("/Game/"):
        raise RuntimeError(f"путь карты вне /Game: {asset_path}")
    relative = asset_path.removeprefix("/Game/")
    return pathlib.Path(CONTENT_DIR).joinpath(
        *relative.split("/")).with_suffix(".umap")


def delete_owned_map(library, asset_path):
    """Удаляет служебную карту транзакции и проверяет, что её не стало.

    `delete_asset` для `.umap` в коммандлете молча возвращает успех, ничего
    не удаляя: пакет остаётся и на диске, и в реестре (замерено на ветке
    этапа 1). Тогда первая же publish опирается на «canonical и backup
    одновременно», и карту приходится чистить руками. Результат вызова
    всегда перечитывается, а при молчаливом отказе пакет убирается с диска
    и реестр пересканируется.
    """
    temp, backup = transaction_paths()
    if asset_path not in (temp, backup):
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


def transaction_paths():
    return OWNED_TARGET + "__TxnTemp", OWNED_TARGET + "__TxnBackup"


def recover_transaction(report, library):
    temp, backup = transaction_paths()
    has_target = library.does_asset_exist(OWNED_TARGET)
    has_temp = library.does_asset_exist(temp)
    has_backup = library.does_asset_exist(backup)
    if has_target and has_backup:
        raise RuntimeError(
            "неоднозначное map transaction state: canonical и backup "
            f"существуют одновременно {OWNED_TARGET}, {backup}; temp={has_temp}")
    if has_backup and not has_target:
        if not library.rename_asset(backup, OWNED_TARGET):
            raise RuntimeError(
                f"transaction recovery: не восстановлен {backup} -> {OWNED_TARGET}")
        report.line("MAP TRANSACTION RECOVERY: backup восстановлен")
        has_target = True
        has_backup = False
    if has_temp:
        if not delete_owned_map(library, temp):
            raise RuntimeError(
                f"transaction recovery: не удалён stale temp {temp}")
        report.line("MAP TRANSACTION RECOVERY: stale temp удалён")
    if has_backup:
        raise RuntimeError(
            f"неоднозначное transaction recovery: backup остался {backup}")
    return has_target


def begin_build_empty_temp(report, levels):
    """Создаёт пустую temp-карту и загружает её для постройки."""
    temp, _backup = transaction_paths()
    if not levels.new_level(temp):
        raise RuntimeError(f"map transaction: не создана temp карта {temp}")
    return temp


def publish_temp(report, library, levels, had_target):
    temp, backup = transaction_paths()
    if not library.does_asset_exist(temp):
        raise RuntimeError(f"transaction publish: нет temp {temp}")
    if library.does_asset_exist(backup):
        raise RuntimeError(f"transaction publish: stale backup {backup}")

    if had_target and not library.rename_asset(OWNED_TARGET, backup):
        raise RuntimeError(
            f"transaction publish: не создан backup {OWNED_TARGET} -> {backup}")
    if not library.rename_asset(temp, OWNED_TARGET):
        if had_target:
            if not library.rename_asset(backup, OWNED_TARGET):
                raise RuntimeError(
                    f"transaction publish rollback: не восстановлен {OWNED_TARGET}")
        raise RuntimeError(
            f"transaction publish rename failed {temp} -> {OWNED_TARGET}")

    if not levels.load_level(OWNED_TARGET):
        raise RuntimeError(
            f"transaction publish: canonical не загрузилась {OWNED_TARGET}")
    if had_target and not delete_owned_map(library, backup):
        raise RuntimeError(
            f"transaction publish: backup не удалён {backup}")
    if (not library.does_asset_exist(OWNED_TARGET)
            or library.does_asset_exist(temp)
            or library.does_asset_exist(backup)):
        raise RuntimeError("transaction publish: неверное финальное состояние")
    report.line(f"MAP TRANSACTION PUBLISHED: {temp} -> {OWNED_TARGET}")


def apply_component_lighting(component, payload):
    if len(payload) != LIGHTING_PAYLOAD_FLOATS:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО lighting payload: {len(payload)}/"
            f"{LIGHTING_PAYLOAD_FLOATS}")
    component.modify()
    component.set_default_custom_primitive_data_float_array(
        0, [float(value) for value in payload])
    component.set_cast_shadow(False)
    component.set_affect_dynamic_indirect_lighting(False)
    component.set_affect_distance_field_lighting(False)


def place_terrain(report, actors):
    """Ставит 64 rigid-Q страницы в начале координат, с dominant-texture MI."""
    with open(TERRAIN_TILES_TABLE, "r", encoding="utf-8") as handle:
        tiles = json.load(handle).get("tiles", {})

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([TERRAIN_ROOT], force_rescan=True)
    assets = registry.get_assets_by_path(
        unreal.Name(TERRAIN_ROOT), recursive=True)
    meshes = []
    for asset in assets:
        if str(asset.asset_class_path.asset_name) != "StaticMesh":
            continue
        mesh = unreal.load_asset(f"{asset.package_name}.{asset.asset_name}")
        if isinstance(mesh, unreal.StaticMesh):
            meshes.append(mesh)
    if len(meshes) != 64:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО terrain meshes: {len(meshes)}/64 "
            f"в {TERRAIN_ROOT}")

    placed = 0
    baked_count = 0
    without_material = []
    for mesh in sorted(meshes, key=lambda item: item.get_name()):
        actor = actors.spawn_actor_from_class(
            unreal.StaticMeshActor,
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
        if actor is None:
            raise RuntimeError(f"НЕВЕРНОЕ_КОЛИЧЕСТВО: terrain actor {mesh.get_name()}")
        actor.static_mesh_component.set_static_mesh(mesh)
        label = f"{TERRAIN_ACTOR_PREFIX}{mesh.get_name()}"
        actor.set_actor_label(label)

        # Запечённое альбедо — основной вариант; dominant-инстанс из таблицы
        # — резервный fallback на случай отсутствия бейки.
        suffix = mesh.get_name().rsplit("garner_terrain_", 1)[-1]
        baked = unreal.load_asset(
            f"/Game/Terrain/GarnerBlend/MI_Garner_{suffix}")
        if isinstance(baked, unreal.MaterialInstanceConstant):
            actor.static_mesh_component.set_material(0, baked)
            baked_count += 1
            placed += 1
            continue

        texture_name = tiles.get(mesh.get_name())
        instance_path = (
            f"{TERRAIN_MATERIALS_ROOT}/MI_{texture_name}"
            if texture_name else None)
        if instance_path is not None:
            instance = unreal.load_asset(instance_path)
            if not isinstance(instance, unreal.MaterialInstanceConstant):
                raise RuntimeError(
                    f"ОТСУТСТВУЕТ_MATERIAL: {instance_path} для {label}")
            actor.static_mesh_component.set_material(0, instance)
        else:
            without_material.append(mesh.get_name())
        placed += 1

    report.line(
        f"TERRAIN: pages={placed}/64 bakedMI={baked_count} "
        f"withDominantMI={placed - baked_count - len(without_material)} "
        f"withoutMI={sorted(without_material)}")


def place_static_ism(report, actors, placements):
    """Группирует статические части по уникальному мешу в ISM компоненты.

    Один actor на уникальный mesh; instancing вместо тысячи акторов —
    на 47k объектов редактор не выживает. Каждый инстанс несёт свой
    33-float lighting payload в per-instance custom data.
    """
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)

    # Группировка: asset path -> [(location, rotation, payload, label)]
    groups = {}
    for placement in placements:
        path = placement["part"]["path"]
        groups.setdefault(path, []).append(placement)

    actor_count = 0
    instance_count = 0
    nanite_disallowed = 0

    for path in sorted(groups):
        parts = groups[path]
        part = parts[0]["part"]
        actor = actors.spawn_actor_from_class(
            unreal.StaticMeshActor,
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
        if actor is None:
            raise RuntimeError(f"НЕВЕРНОЕ_КОЛИЧЕСТВО: ISM actor для {path}")

        handles = subsystem.k2_gather_subobject_data_for_instance(actor)
        if not handles:
            raise RuntimeError(f"ISM: нет corrupt rank/subroots у актёра {path}")
        params = unreal.AddNewSubobjectParams(
            parent_handle=handles[0],
            new_class=unreal.InstancedStaticMeshComponent,
            blueprint_context=None)
        handle, fail_reason = subsystem.add_new_subobject(params)
        if not fail_reason.is_empty():
            raise RuntimeError(f"ISM: subobject не добавлен {path}: {fail_reason}")
        data = subsystem.k2_find_subobject_data_from_handle(handle)
        component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
        if not isinstance(component, unreal.InstancedStaticMeshComponent):
            raise RuntimeError(f"ISM: subobject не ISM компонент у {path}")

        component.set_editor_property("static_mesh", part["asset"])
        component.set_editor_property("disallow_nanite", part["disallow_nanite"])
        if part["disallow_nanite"]:
            nanite_disallowed += 1
        component.modify()
        component.set_num_custom_data_floats(LIGHTING_PAYLOAD_FLOATS)
        component.set_cast_shadow(False)
        component.set_affect_dynamic_indirect_lighting(False)
        component.set_affect_distance_field_lighting(False)

        transforms = [
            unreal.Transform(
                unreal.Vector(*placement["location"]),
                unreal.Rotator(roll=0.0, pitch=0.0, yaw=placement["yaw"]),
                unreal.Vector(1.0, 1.0, 1.0))
            for placement in parts
        ]
        added = list(
            component.add_instances(transforms, True, False))
        if (len(added) != len(parts)
                or added != list(range(len(parts)))):
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО: ISM instance ids {path}: "
                f"{added[:5]}...; ожидались последовательные 0..{len(parts) - 1}")
        for instance_index, placement in enumerate(parts):
            payload = placement["payload"]
            for custom_index, value in enumerate(payload):
                component.set_custom_data_value(
                    instance_index, custom_index, float(value), False)

        actor.set_actor_label(f"{ACTOR_PREFIX}ISM_{parts[0]['part']['stem']}")
        actor_count += 1
        instance_count += len(parts)

    report.line(
        f"STATIC ISM: actors={actor_count} instances={instance_count} "
        f"naniteDisallowed={nanite_disallowed}")
    return actor_count, instance_count


def place_skeletal(report, actors, placements):
    placed = 0
    for placement in placements:
        part = placement["part"]
        actor = actors.spawn_actor_from_class(
            unreal.SkeletalMeshActor,
            unreal.Vector(*placement["location"]),
            unreal.Rotator(roll=0.0, pitch=0.0, yaw=placement["yaw"]))
        if actor is None:
            raise RuntimeError(
                f"НЕВЕРНОЕ_КОЛИЧЕСТВО: skeletal actor {part['path']}")
        component = actor.skeletal_mesh_component
        component.set_skeletal_mesh(part["asset"])
        component.modify()
        component.override_animation_data(
            part["animation"], False, False, part["sample_seconds"], 1.0)
        component.set_editor_property("pause_anims", True)
        apply_component_lighting(component, placement["payload"])
        actor.set_actor_label(
            f"{ACTOR_PREFIX}SK_{part['stem']}_{placement['index']}")
        placed += 1
    report.line(f"SKELETAL ACTORS: {placed}")
    return placed


def place_player_start(report, actors):
    """Source-точка parity кадра как PlayerStart: земля из half-meter raster."""
    height_path = pathlib.Path(DEFAULT_HEIGHT_MAP)
    if not height_path.is_file():
        raise RuntimeError(f"ОТСУТСТВУЕТ_MESH/HEIGHT: {height_path}")
    width = height_grid_width(height_path)
    with height_path.open("rb") as handle:
        ground_z = sample_height_cm(
            handle, width, int(SPAWN_SOURCE_XY[0]), int(SPAWN_SOURCE_XY[1]))
    location = unreal.Vector(*source_location_to_ue(
        SPAWN_SOURCE_XY[0], SPAWN_SOURCE_XY[1], ground_z + 100.0))
    actor = actors.spawn_actor_from_class(
        unreal.PlayerStart, location,
        unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    if actor is None:
        raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: PlayerStart actor=0")
    actor.set_actor_label(f"{ACTOR_PREFIX}PlayerStart")
    report.line(
        f"PLAYER START: source={SPAWN_SOURCE_XY} groundZ={ground_z:.0f} "
        f"ue=({location.x:.0f},{location.y:.0f},{location.z:.0f})")


def clear_stale_actors(report, actors):
    removed = 0
    for actor in list(actors.get_all_level_actors()):
        label = actor.get_actor_label()
        if (label.startswith(ACTOR_PREFIX)
                or label.startswith(TERRAIN_ACTOR_PREFIX)
                or label.startswith("Inst_")
                or label.startswith("Obj_")):
            if actors.destroy_actor(actor):
                removed += 1
    if removed:
        report.line(f"убрано stale actors: {removed}")


def main(report, args):
    if RefuseIfEditorOpen(report):
        raise RuntimeError("редактор уже открыт")
    validate_args(args)

    service_ids = load_service_model_ids(args.database)
    records, missing_report, service_report = load_scene_records(
        os.path.abspath(args.manifest), service_ids)
    report.line(
        f"VISUAL FILTER: sceneModels={EXPECTED_SCENE_MODELS} "
        f"visual={len(records)} holes={sum(missing_report.values())} "
        f"byModel={ {k: v for k, v in sorted(missing_report.items())} }")
    report.line(
        f"SERVICE MARKERS: отложено={sum(service_report.values())} "
        f"видов={len(service_report)} "
        f"byModel={ {k: v for k, v in sorted(service_report.items())} }")
    lighting = compute_lighting(records, args.database)
    report.line(
        f"LIGHTING: payloads={len(lighting)}/{len(records)} "
        f"floats={LIGHTING_PAYLOAD_FLOATS}")
    models = load_model_map(os.path.abspath(args.model_map), args.content_root)
    resolved = resolve_parts(records, models, args.content_root)

    # Подготовка placement batch: каждая запись = часть × anchor.
    static_batch = []
    skeletal_batch = []
    record_index = 0
    for record, resolved_light in zip(records, lighting):
        location = source_location_to_ue(
            record["x"], record["y"], record["zCm"])
        yaw = source_scene_yaw_to_ue(record["sourceYawDegrees"])
        payload = resolved_light["payload"]
        if len(payload) != LIGHTING_PAYLOAD_FLOATS:
            raise RuntimeError("НЕВЕРНОЕ_КОЛИЧЕСТВО: lighting payload")
        for part in resolved[int(record["modelId"])]:
            placement = {
                "index": record_index,
                "location": location,
                "yaw": yaw,
                "payload": payload,
                "part": part,
            }
            if part["kind"] == "static":
                static_batch.append(placement)
            else:
                skeletal_batch.append(placement)
        record_index += 1

    report.line(
        f"PLACEMENT BATCH: static={len(static_batch)} "
        f"skeletal={len(skeletal_batch)} anchors={len(records)}")

    library = unreal.EditorAssetLibrary
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    had_target = recover_transaction(report, library)

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    temp = begin_build_empty_temp(report, levels)
    report.line(f"temp карта открыта: {temp}")
    try:
        clear_stale_actors(report, actors)
        place_terrain(report, actors)
        ism_actors, ism_instances = place_static_ism(
            report, actors, static_batch)
        skeletal_placed = place_skeletal(report, actors, skeletal_batch)
        place_player_start(report, actors)

        if not levels.save_current_level():
            raise RuntimeError(f"map transaction: temp карта не сохранена {temp}")
        if not library.does_asset_exist(temp):
            raise RuntimeError(
                f"map transaction: после save нет temp {temp}")
        report.line("MAP TRANSACTION BUILD PASS: сохранено и видно реестр")
    except Exception:
        # rollback: удалить temp и вернуть как было
        if library.does_asset_exist(temp):
            delete_owned_map(library, temp)
        raise

    publish_temp(report, library, levels, had_target)
    report.line(
        f"УСПЕХ: {OWNED_TARGET}; anchors={len(records)}; "
        f"staticInstances={ism_instances}; skeletalActors={skeletal_placed}; "
        f"terrainPages=64")


report = Reporter("place_garner_city")
try:
    main(report, parse_args(sys.argv[1:]))
except Exception as exc:
    report.exception(exc)
    raise
finally:
    report.close()
