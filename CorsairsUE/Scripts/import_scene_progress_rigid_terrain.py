"""Импортирует один rigid-Q terrain mesh для карты визуального прогресса."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import sys

sys.dont_write_bytecode = True
_SCRIPT_DIR = str(Path(__file__).resolve().parent)
if not sys.path or sys.path[0] != _SCRIPT_DIR:
    sys.path.insert(0, _SCRIPT_DIR)

import unreal

from report import Reporter, RefuseIfEditorOpen


TARGET_MESH = "/Game/Terrain/Reference/GarnerRigid/SM_Garner_17_21"
TERRAIN_INSTANCE = "/Game/Terrain/Reference/Garner/MI_Garner_17_21"
ATTEMPT_ROOT = "/Game/Terrain/Reference/GarnerRigid/__ImportAttempt"
SOURCE_GLTF_METADATA = "Corsairs.SourceGltfSha256"
SOURCE_BIN_METADATA = "Corsairs.SourceBinSha256"
SOURCE_PROFILE_METADATA = "Corsairs.TerrainCoordinateProfile"
RIGID_COORDINATE_PROFILE = "RigidQ"
EXPECTED_VERTEX_COUNT = 16641
EXPECTED_INDEX_COUNT = 98304
UE_BOUNDS_TOLERANCE_CM = 0.1
EXPECTED_GLTF_BOUNDS_MIN = (-128.0, -2.7, 0.0)
EXPECTED_GLTF_BOUNDS_MAX = (0.0, 3.0, 128.0)
EXPECTED_UE_BOUNDS_MIN_CM = (-12800.0, 0.0, -270.0)
EXPECTED_UE_BOUNDS_MAX_CM = (0.0, 12800.0, 300.0)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Импортировать один rigid-Q Garner terrain glTF")
    parser.add_argument("source_gltf")
    return parser.parse_args(argv)


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def validate_source_contract(source_gltf):
    source = Path(source_gltf).resolve()
    if (not source.is_file() or source.is_symlink()
            or source.suffix.lower() != ".gltf"):
        raise RuntimeError(f"нет rigid terrain glTF: {source}")
    try:
        document = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"не читается rigid terrain glTF {source}: {error}") from error
    asset = document.get("asset") if isinstance(document, dict) else None
    if not isinstance(asset, dict) or asset.get("version") != "2.0":
        raise RuntimeError("rigid terrain glTF: asset.version должен быть 2.0")
    profile = asset.get("extras", {}).get(
        "corsairsTerrainCoordinateProfile")
    if profile != RIGID_COORDINATE_PROFILE:
        raise RuntimeError(
            "rigid terrain glTF: coordinate profile должен быть "
            f"{RIGID_COORDINATE_PROFILE}, получено {profile!r}")
    buffers = document.get("buffers")
    if not isinstance(buffers, list) or len(buffers) != 1:
        raise RuntimeError("rigid terrain glTF: должен быть ровно один bin buffer")
    buffer = buffers[0]
    uri = buffer.get("uri") if isinstance(buffer, dict) else None
    relative = PurePosixPath(uri) if isinstance(uri, str) else None
    if (relative is None or relative.is_absolute() or len(relative.parts) != 1
            or relative.parts[0] in ("", ".", "..") or "\\" in uri):
        raise RuntimeError(f"rigid terrain glTF: неверный bin uri {uri!r}")
    binary = (source.parent / uri).resolve()
    if not binary.is_file() or binary.is_symlink():
        raise RuntimeError(f"нет rigid terrain bin: {binary}")
    byte_length = buffer.get("byteLength")
    if (not isinstance(byte_length, int) or isinstance(byte_length, bool)
            or byte_length != binary.stat().st_size):
        raise RuntimeError(
            f"rigid terrain bin byteLength: {byte_length!r}/"
            f"{binary.stat().st_size}")
    meshes = document.get("meshes")
    if (not isinstance(meshes, list) or len(meshes) != 1
            or document.get("nodes") != [{"mesh": 0, "name": "mesh"}]
            or document.get("scenes") != [{"nodes": [0]}]
            or document.get("scene") != 0):
        raise RuntimeError("rigid terrain glTF: ожидается одна mesh/root scene")
    primitives = meshes[0].get("primitives") if isinstance(meshes[0], dict) else None
    if not isinstance(primitives, list) or len(primitives) != 1:
        raise RuntimeError("rigid terrain glTF: ожидается ровно один primitive")
    primitive = primitives[0]
    expected_attributes = {
        "POSITION": 0,
        "NORMAL": 1,
        "TEXCOORD_0": 2,
    }
    if (not isinstance(primitive, dict)
            or primitive.get("attributes") != expected_attributes
            or primitive.get("indices") != 3
            or primitive.get("mode") != 4):
        raise RuntimeError(
            "rigid terrain glTF: primitive contract должен быть "
            "POSITION/NORMAL/TEXCOORD_0 + uint indices TRIANGLES")

    accessors = document.get("accessors")
    expected_accessors = (
        (5126, EXPECTED_VERTEX_COUNT, "VEC3"),
        (5126, EXPECTED_VERTEX_COUNT, "VEC3"),
        (5126, EXPECTED_VERTEX_COUNT, "VEC2"),
        (5125, EXPECTED_INDEX_COUNT, "SCALAR"),
    )
    if not isinstance(accessors, list) or len(accessors) != len(expected_accessors):
        raise RuntimeError("rigid terrain glTF: ожидается ровно четыре accessor")
    for index, (component_type, count, value_type) in enumerate(expected_accessors):
        accessor = accessors[index]
        if (not isinstance(accessor, dict)
                or accessor.get("componentType") != component_type
                or accessor.get("count") != count
                or accessor.get("type") != value_type):
            raise RuntimeError(
                f"rigid terrain glTF: неверный accessor {index}")

    position_min = accessors[0].get("min")
    position_max = accessors[0].get("max")
    if (not _finite_vec3(position_min) or not _finite_vec3(position_max)
            or any(not _close(position_min[index], expected)
                   for index, expected in enumerate(EXPECTED_GLTF_BOUNDS_MIN))
            or any(not _close(position_max[index], expected)
                   for index, expected in enumerate(EXPECTED_GLTF_BOUNDS_MAX))):
        raise RuntimeError(
            "rigid terrain glTF: POSITION bounds должны быть "
            f"min={EXPECTED_GLTF_BOUNDS_MIN}, max={EXPECTED_GLTF_BOUNDS_MAX}")
    return {
        "gltfPath": source,
        "binPath": binary,
        "gltfSha256": sha256_file(source),
        "binSha256": sha256_file(binary),
        "coordinateProfile": profile,
        "vertexCount": EXPECTED_VERTEX_COUNT,
        "indexCount": EXPECTED_INDEX_COUNT,
        "ueBoundsMinCm": EXPECTED_UE_BOUNDS_MIN_CM,
        "ueBoundsMaxCm": EXPECTED_UE_BOUNDS_MAX_CM,
    }


def _finite_vec3(value):
    return (isinstance(value, list) and len(value) == 3
            and all(isinstance(item, (int, float))
                    and not isinstance(item, bool)
                    and math.isfinite(float(item))
                    for item in value))


def _close(actual, expected):
    return math.isclose(
        float(actual), float(expected), rel_tol=0.0, abs_tol=1.0e-6)


def imported_package_path(object_path):
    package, separator, object_name = str(object_path).rpartition(".")
    if separator and package.rsplit("/", 1)[-1] == object_name:
        return package
    return str(object_path)


def is_attempt_owned_path(object_path):
    package = imported_package_path(object_path)
    return package == ATTEMPT_ROOT or package.startswith(ATTEMPT_ROOT + "/")


def select_exact_static_mesh(imported_paths, load_asset):
    unique = list(dict.fromkeys(str(path) for path in imported_paths))
    outside = [path for path in unique if not is_attempt_owned_path(path)]
    if outside:
        raise RuntimeError(
            f"import returned paths outside owned attempt: {outside}")
    candidates = [
        (path, asset)
        for path in unique
        if isinstance((asset := load_asset(path)), unreal.StaticMesh)
    ]
    if len(candidates) != 1:
        raise RuntimeError(
            f"импорт должен вернуть ровно один StaticMesh: "
            f"получено {len(candidates)} из {unique}")
    return candidates[0]


def asset_package_path(asset):
    object_path = str(asset.get_path_name())
    package, separator, object_name = object_path.rpartition(".")
    if separator and package.rsplit("/", 1)[-1] == object_name:
        return package
    return object_path


def metadata(asset, key):
    return str(unreal.EditorAssetLibrary.get_metadata_tag(asset, key) or "")


def configure_mesh(mesh, instance, contract):
    if mesh.get_material(0) != instance:
        mesh.set_material(0, instance)
    nanite = mesh.get_editor_property("nanite_settings")
    if not nanite.enabled:
        nanite.enabled = True
        mesh.set_editor_property("nanite_settings", nanite)
    unreal.EditorAssetLibrary.set_metadata_tag(
        mesh, SOURCE_GLTF_METADATA, contract["gltfSha256"])
    unreal.EditorAssetLibrary.set_metadata_tag(
        mesh, SOURCE_BIN_METADATA, contract["binSha256"])
    unreal.EditorAssetLibrary.set_metadata_tag(
        mesh, SOURCE_PROFILE_METADATA, contract["coordinateProfile"])


def mesh_geometry(mesh):
    bounds = mesh.get_bounding_box()
    minimum = bounds.min
    maximum = bounds.max
    return {
        "boundsValid": bool(bounds.is_valid),
        "lods": mesh.get_num_lods(),
        "boundsMinCm": (minimum.x, minimum.y, minimum.z),
        "boundsMaxCm": (maximum.x, maximum.y, maximum.z),
    }


def geometry_matches_contract(geometry, contract):
    if not geometry["boundsValid"] or geometry["lods"] != 1:
        return False
    for actual_key, expected_key in (
            ("boundsMinCm", "ueBoundsMinCm"),
            ("boundsMaxCm", "ueBoundsMaxCm")):
        actual = geometry[actual_key]
        expected = contract[expected_key]
        if (len(actual) != 3 or len(expected) != 3
                or not all(math.isfinite(float(value)) for value in actual)
                or any(
                    abs(float(actual[index]) - float(expected[index])) >
                    UE_BOUNDS_TOLERANCE_CM + 1.0e-9
                    for index in range(3))):
            return False
    return True


def require_geometry(mesh, contract, label):
    geometry = mesh_geometry(mesh)
    if not geometry_matches_contract(geometry, contract):
        raise RuntimeError(
            f"{label}: geometry={geometry}, expected="
            f"boundsValid=True lods=1 "
            f"boundsMinCm={contract['ueBoundsMinCm']} "
            f"boundsMaxCm={contract['ueBoundsMaxCm']}")


def validate_mesh(mesh, instance, contract):
    issues = []
    if not isinstance(mesh, unreal.StaticMesh):
        issues.append("type")
    else:
        if asset_package_path(mesh) != TARGET_MESH:
            issues.append("path")
        if mesh.get_material(0) != instance:
            issues.append("material0")
        if not mesh.get_editor_property("nanite_settings").enabled:
            issues.append("nanite")
        if metadata(mesh, SOURCE_GLTF_METADATA) != contract["gltfSha256"]:
            issues.append("gltfSha256")
        if metadata(mesh, SOURCE_BIN_METADATA) != contract["binSha256"]:
            issues.append("binSha256")
        if (metadata(mesh, SOURCE_PROFILE_METADATA) !=
                contract["coordinateProfile"]):
            issues.append("coordinateProfile")
        if not geometry_matches_contract(mesh_geometry(mesh), contract):
            issues.append("geometry")
    if issues:
        raise RuntimeError(f"rigid terrain StaticMesh readback: {issues}")


def delete_attempt_root():
    library = unreal.EditorAssetLibrary
    if library.does_directory_exist(ATTEMPT_ROOT):
        if not library.delete_directory(ATTEMPT_ROOT):
            raise RuntimeError(f"не удалён owned import attempt: {ATTEMPT_ROOT}")
    if library.does_directory_exist(ATTEMPT_ROOT):
        raise RuntimeError(f"owned import attempt остался: {ATTEMPT_ROOT}")


def cleanup_failed_first_publication():
    issues = []
    if unreal.load_asset(TARGET_MESH) is not None:
        if not unreal.EditorAssetLibrary.delete_asset(TARGET_MESH):
            issues.append(f"не удалён созданный target {TARGET_MESH}")
    if unreal.EditorAssetLibrary.does_directory_exist(ATTEMPT_ROOT):
        if not unreal.EditorAssetLibrary.delete_directory(ATTEMPT_ROOT):
            issues.append(f"не удалён owned import attempt {ATTEMPT_ROOT}")
    if issues:
        raise RuntimeError("RECOVERY_REQUIRED: " + "; ".join(issues))


def import_candidate(contract):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", str(contract["gltfPath"]))
    task.set_editor_property("destination_path", ATTEMPT_ROOT)
    task.set_editor_property("destination_name", "SM_Garner_17_21__Candidate")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", False)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    # В UE 5.8 get_objects() является completion barrier для Interchange.
    list(task.get_objects())
    finish_asset_compilation()
    return [str(path) for path in task.get_editor_property("imported_object_paths")]


def finish_asset_compilation():
    # StaticMesh render data компилируются отдельно от Interchange task.
    unreal.AutomationUtilsBlueprintLibrary.finish_all_asset_compilation()


def main(report, args):
    if RefuseIfEditorOpen(report):
        raise RuntimeError("редактор уже открыт")
    contract = validate_source_contract(args.source_gltf)
    instance = unreal.load_asset(TERRAIN_INSTANCE)
    if not isinstance(instance, unreal.MaterialInstanceConstant):
        raise RuntimeError(f"нет terrain material instance: {TERRAIN_INSTANCE}")

    current = unreal.load_asset(TARGET_MESH)
    if isinstance(current, unreal.StaticMesh):
        validate_mesh(current, instance, contract)
        report.line(
            f"RIGID TERRAIN ALREADY CURRENT: {TARGET_MESH} "
            f"gltf={contract['gltfSha256']}")
        return
    if current is not None:
        raise RuntimeError(f"rigid terrain target занят чужим типом: {TARGET_MESH}")

    delete_attempt_root()
    completed = False
    try:
        imported = import_candidate(contract)
        _candidate_path, candidate = select_exact_static_mesh(
            imported, unreal.load_asset)
        configure_mesh(candidate, instance, contract)
        # Включение Nanite заново инвалидирует render data.
        finish_asset_compilation()
        require_geometry(candidate, contract, "candidate rigid terrain")
        if not unreal.EditorAssetLibrary.save_loaded_asset(
                candidate, only_if_is_dirty=False):
            raise RuntimeError("не сохранён candidate rigid terrain StaticMesh")
        if not unreal.EditorAssetLibrary.rename_loaded_asset(
                candidate, TARGET_MESH):
            raise RuntimeError(f"candidate не опубликован в {TARGET_MESH}")
        target = unreal.load_asset(TARGET_MESH)
        validate_mesh(target, instance, contract)
        if not unreal.EditorAssetLibrary.save_loaded_asset(
                target, only_if_is_dirty=False):
            raise RuntimeError(f"не сохранён rigid terrain target: {TARGET_MESH}")
        delete_attempt_root()
        validate_mesh(unreal.load_asset(TARGET_MESH), instance, contract)
        completed = True
    finally:
        if not completed:
            cleanup_failed_first_publication()
    report.line(
        f"RIGID TERRAIN PASS: {TARGET_MESH} gltf={contract['gltfSha256']} "
        f"bin={contract['binSha256']}")


report = Reporter("import_scene_progress_rigid_terrain")
try:
    main(report, parse_args(sys.argv[1:]))
finally:
    report.close()
