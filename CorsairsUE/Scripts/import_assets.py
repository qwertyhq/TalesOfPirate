"""Импорт конвертированных ассетов в UE и проверка результата.

Запускается редактором в headless-режиме:

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/import_assets.py <каталог-с-gltf> <путь-в-Content>"

Скрипт импортирует каждый .gltf через Interchange и печатает, что реально
получилось: тип созданного ассета, число вершин, костей, материалов. Это
проверка того, что конвертер отдаёт файлы, пригодные именно для UE, —
корректность самого glTF уже подтверждена импортом в Blender.
"""

import argparse
import json
import math
import os
import sys

import unreal


CAPTURE_TICK = 120
CAPTURE_HZ = 30.0


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Импортировать glTF и проверить созданные UE-ассеты")
    parser.add_argument("source_dir")
    parser.add_argument("destination")
    parser.add_argument(
        "--expected-preserve-animated",
        type=int,
        default=None,
        help="точное ожидаемое число glTF с BONE policy=preserveAnimated",
    )
    args = parser.parse_args(argv)
    if (args.expected_preserve_animated is not None
            and args.expected_preserve_animated < 0):
        parser.error("--expected-preserve-animated не может быть отрицательным")
    return args


def gather_gltf(source_dir):
    found = []
    for root, _dirs, files in os.walk(source_dir):
        for name in files:
            if name.lower().endswith(".gltf"):
                found.append(os.path.join(root, name))
    return sorted(found)


def read_preserve_animated_metadata(path):
    """Читает и fail-closed проверяет converter BONE metadata."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"не удалось прочитать glTF metadata {path}: {error}") from error

    captures = []
    for node in document.get("nodes", []):
        if not isinstance(node, dict):
            continue
        capture = (node.get("extras", {})
                   .get("corsairsLegacyCapture"))
        if not isinstance(capture, dict):
            continue
        bone = capture.get("bone")
        if isinstance(bone, dict) and bone.get("policy") == "preserveAnimated":
            captures.append((capture, bone))

    if not captures:
        return None
    if len(captures) != 1:
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: part_root records={len(captures)}")

    capture, bone = captures[0]
    capture_tick = capture.get("captureTick")
    frame_count = bone.get("frameCount")
    sample_frame = bone.get("sampleFrame")
    if capture.get("schemaVersion") != 1 or capture_tick != CAPTURE_TICK:
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: captureTick={capture_tick}, "
            f"ожидался {CAPTURE_TICK}")
    if (not isinstance(frame_count, int) or isinstance(frame_count, bool)
            or frame_count <= 0):
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: frameCount={frame_count!r}")
    expected_sample = (CAPTURE_TICK - 1) % frame_count
    if sample_frame != expected_sample:
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: sampleFrame={sample_frame!r}, "
            f"ожидался {expected_sample}")
    if len(document.get("skins", [])) != 1:
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: ожидался ровно один skin")
    animations = document.get("animations", [])
    if (len(animations) != 1
            or animations[0].get("name") != "legacy_bone"):
        raise RuntimeError(
            f"PreserveAnimated metadata {path}: ожидалась одна legacy_bone animation")
    return {
        "captureTick": capture_tick,
        "frameCount": frame_count,
        "sampleFrame": sample_frame,
    }


def require_preserve_animated_count(actual, expected):
    if expected is not None and actual != expected:
        raise RuntimeError(
            f"PreserveAnimated glTF: ожидалось {expected}, получено {actual}")


def expected_interchange_paths(path, destination):
    stem = os.path.splitext(os.path.basename(path))[0]
    base = (
        f"{destination.rstrip('/')}/{stem}/SkeletalMeshes/{stem}")
    return {
        "animation": base + "_Anim",
        "physics_asset": base + "_PhysicsAsset",
        "skeletal_mesh": base,
        "skeleton": base + "_Skeleton",
        "static_mesh": (
            f"{destination.rstrip('/')}/{stem}/StaticMeshes/{stem}"),
    }


def validate_preserve_animated_import(path, destination, metadata):
    """Проверяет точный UE Interchange результат для animated SceneMap glTF."""
    paths = expected_interchange_paths(path, destination)
    mesh = unreal.load_asset(paths["skeletal_mesh"])
    skeleton = unreal.load_asset(paths["skeleton"])
    physics_asset = unreal.load_asset(paths["physics_asset"])
    animation = unreal.load_asset(paths["animation"])
    static_mesh = unreal.load_asset(paths["static_mesh"])

    expected_types = (
        (mesh, unreal.SkeletalMesh, paths["skeletal_mesh"]),
        (skeleton, unreal.Skeleton, paths["skeleton"]),
        (physics_asset, unreal.PhysicsAsset, paths["physics_asset"]),
        (animation, unreal.AnimSequence, paths["animation"]),
    )
    for asset, expected_type, asset_path in expected_types:
        if not isinstance(asset, expected_type):
            raise RuntimeError(
                f"PreserveAnimated asset {asset_path}: ожидался "
                f"{expected_type.__name__}")
    if isinstance(static_mesh, unreal.StaticMesh):
        raise RuntimeError(
            f"PreserveAnimated создал неожиданный StaticMesh {paths['static_mesh']}")

    mesh_skeleton = mesh.get_editor_property("skeleton")
    animation_skeleton = animation.get_editor_property("skeleton")
    if mesh_skeleton != skeleton or animation_skeleton != skeleton:
        raise RuntimeError(
            f"PreserveAnimated skeleton mismatch для {os.path.basename(path)}")

    data_model = animation.get_editor_property("data_model_interface")
    if data_model is None:
        raise RuntimeError(
            f"PreserveAnimated animation без data model: {paths['animation']}")
    frame_count = int(data_model.get_number_of_keys())
    frame_intervals = int(data_model.get_number_of_frames())
    if frame_count != metadata["frameCount"]:
        raise RuntimeError(
            f"PreserveAnimated key count {paths['animation']}: "
            f"{frame_count}/{metadata['frameCount']}")
    if frame_intervals != frame_count - 1:
        raise RuntimeError(
            f"PreserveAnimated frame intervals {paths['animation']}: "
            f"{frame_intervals}/{frame_count - 1}")

    frame_rate = data_model.get_frame_rate()
    numerator = int(frame_rate.numerator)
    denominator = int(frame_rate.denominator)
    if numerator != int(CAPTURE_HZ) or denominator != 1:
        raise RuntimeError(
            f"PreserveAnimated frame rate {paths['animation']}: "
            f"{numerator}/{denominator}")
    expected_length = (frame_count - 1) / CAPTURE_HZ
    if not math.isclose(
            float(animation.get_play_length()), expected_length,
            rel_tol=0.0, abs_tol=1.0e-4):
        raise RuntimeError(
            f"PreserveAnimated duration {paths['animation']}: "
            f"{animation.get_play_length()}/{expected_length}")
    return paths


def build_task(path, destination):
    task = unreal.AssetImportTask()
    task.filename = path
    task.destination_path = destination
    task.automated = True
    task.replace_existing = True
    task.save = True
    return task


def describe(asset):
    """Краткое описание импортированного ассета для отчёта."""
    if isinstance(asset, unreal.StaticMesh):
        lod = asset.get_num_vertices(0) if asset.get_num_lods() > 0 else 0
        return f"StaticMesh вершин={lod} материалов={len(asset.static_materials)}"

    if isinstance(asset, unreal.SkeletalMesh):
        skeleton = asset.skeleton
        bones = len(skeleton.get_editor_property("bone_tree")) if skeleton else 0
        return f"SkeletalMesh материалов={len(asset.materials)} костей={bones}"

    if isinstance(asset, unreal.Skeleton):
        return f"Skeleton костей={len(asset.get_editor_property('bone_tree'))}"

    if isinstance(asset, unreal.AnimSequence):
        return (f"AnimSequence кадров={asset.get_editor_property('number_of_sampled_frames')} "
                f"длительность={asset.get_play_length():.3f}с")

    if isinstance(asset, unreal.Texture2D):
        return f"Texture2D {asset.blueprint_get_size_x()}x{asset.blueprint_get_size_y()}"

    return type(asset).__name__


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    files = gather_gltf(args.source_dir)
    if not files:
        raise RuntimeError(f"в {args.source_dir} нет .gltf")

    animation_metadata = {
        path: metadata
        for path in files
        if (metadata := read_preserve_animated_metadata(path)) is not None
    }
    require_preserve_animated_count(
        len(animation_metadata), args.expected_preserve_animated)

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    imported_total = 0
    failed = []

    for path in files:
        task = build_task(path, args.destination)
        tools.import_asset_tasks([task])
        # В UE 5.8 get_objects() дожидается завершения AssetImportTask.
        list(task.get_objects())

        created = list(task.get_editor_property("imported_object_paths"))
        if not created:
            failed.append(path)
            unreal.log_warning(f"ИМПОРТ_ПУСТ {path}")
            continue

        imported_total += len(created)
        for object_path in created:
            asset = unreal.load_asset(object_path)
            if asset is None:
                unreal.log_warning(f"НЕ_ЗАГРУЗИЛСЯ {object_path}")
                continue
            unreal.log(f"АССЕТ {os.path.basename(path)} -> {object_path} :: {describe(asset)}")

        metadata = animation_metadata.get(path)
        if metadata is not None:
            paths = validate_preserve_animated_import(
                path, args.destination, metadata)
            unreal.log(
                f"PRESERVE_ANIMATED {os.path.basename(path)} -> "
                f"{paths['skeletal_mesh']} + {paths['animation']} "
                f"sampleFrame={metadata['sampleFrame']}/"
                f"{metadata['frameCount']}")

    unreal.log(
        f"ИТОГО файлов={len(files)} ассетов={imported_total} "
        f"preserveAnimated={len(animation_metadata)} "
        f"без_результата={len(failed)}")
    for path in failed:
        unreal.log(f"  БЕЗ_РЕЗУЛЬТАТА {path}")
    if failed:
        raise RuntimeError(f"без результата glTF={len(failed)}")


main()
