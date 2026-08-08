"""Pure, strict contracts for the generated Garner reference terrain.

This module deliberately has no ``unreal`` import.  Editor entry points and the
outer build orchestrator both use the same DTO/path rules, while independently
opening and hashing every physical file they attest.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any, TypedDict


class ValidationIssue(TypedDict):
    code: str
    field: str
    detail: str


REFERENCE_GAME_MODE = "/Script/CorsairsGame.CorsairsGameMode"
REFERENCE_MAP_PACKAGE = "/Game/Maps/Garner"
REFERENCE_WORLD_OBJECT = "/Game/Maps/Garner.Garner"
REFERENCE_BUILD_MARKER = (
    "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot")
REFERENCE_TAG = "CorsairsReferenceTerrain"
REFERENCE_BUILD_TAG = "CorsairsReferenceTerrainBuildRoot"

RUNTIME_INPUT_PATHS = (
    "Data/character_map.json",
    "Data/Heights/garner.block.raw",
    "Data/Heights/garner.terrain.json",
)
RUNTIME_STAGE_PATHS = tuple(f"CorsairsUE/{path}" for path in RUNTIME_INPUT_PATHS)
RUNTIME_CONTAINER_PATHS = tuple(
    f"../../../CorsairsUE/{path}" for path in RUNTIME_INPUT_PATHS)

PACKAGE_FAMILIES = ("mesh", "texture", "material", "instance", "map")
PACKAGE_PATHS = {
    "mesh": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
    "texture": "/Game/Terrain/Reference/Garner/T_Garner_17_21",
    "material": "/Game/Terrain/Reference/M_TerrainReference",
    "instance": "/Game/Terrain/Reference/Garner/MI_Garner_17_21",
    "map": REFERENCE_MAP_PACKAGE,
}
PACKAGE_STEMS = {
    "mesh": "CorsairsUE/Content/Terrain/Reference/Garner/SM_Garner_17_21",
    "texture": "CorsairsUE/Content/Terrain/Reference/Garner/T_Garner_17_21",
    "material": "CorsairsUE/Content/Terrain/Reference/M_TerrainReference",
    "instance": "CorsairsUE/Content/Terrain/Reference/Garner/MI_Garner_17_21",
    "map": "CorsairsUE/Content/Maps/Garner",
}
PACKAGE_SIDECARS = (".uexp", ".ubulk", ".uptnl")

_HASH = re.compile(r"[0-9a-f]{64}\Z")
_TXN = re.compile(r"[0-9a-f]{32}\Z")
_HEAD = re.compile(r"[0-9a-f]{40}\Z")
_RUN_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")


@dataclass(frozen=True)
class ReferenceTerrainLimits:
    max_rss_bytes: int = 128 * 1024 * 1024
    max_texture_cache_bytes: int = 32 * 1024 * 1024
    max_rgba_row_bytes: int = 16 * 1024
    max_png_bytes: int = 96 * 1024 * 1024
    max_height_error_cm: float = 5.0
    max_rms_error_cm: float = 2.0
    max_shared_boundary_cm: float = 0.0


def _issue(code: str, field: str, detail: str) -> ValidationIssue:
    return {"code": code, "field": field or "/", "detail": detail}


def _first(
    issues: list[ValidationIssue], code: str, field: str, detail: str,
) -> bool:
    if not issues:
        issues.append(_issue(code, field, detail))
    return False


def _pointer(parent: str, child: str | int) -> str:
    escaped = str(child).replace("~", "~0").replace("/", "~1")
    return f"{parent}/{escaped}" if parent else f"/{escaped}"


def _require_keys(
    value: Any,
    expected: set[str],
    pointer: str,
    issues: list[ValidationIssue],
) -> dict[str, Any] | None:
    if type(value) is not dict:
        _first(issues, "INVALID_SCHEMA", pointer, "expected object")
        return None
    actual = set(value)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if missing:
        _first(
            issues, "INVALID_SCHEMA", _pointer(pointer, missing[0]),
            "required field is missing")
        return None
    if unknown:
        _first(
            issues, "INVALID_SCHEMA", _pointer(pointer, unknown[0]),
            "unknown field")
        return None
    return value


def _is_int(value: Any, minimum: int = 0, maximum: int = (1 << 64) - 1) -> bool:
    return type(value) is int and minimum <= value <= maximum


def _is_number(value: Any) -> bool:
    return type(value) in (int, float) and math.isfinite(float(value))


def _normalized_relative(value: Any) -> bool:
    if type(value) is not str or not value or "\\" in value:
        return False
    path = PurePosixPath(value)
    return (
        not path.is_absolute()
        and path.as_posix() == value
        and all(part not in ("", ".", "..") for part in path.parts)
    )


def _physical_regular(path: Path, *, allow_empty: bool = False) -> os.stat_result:
    info = path.lstat()
    if (path.is_symlink() or not stat.S_ISREG(info.st_mode) or
            getattr(info, "st_nlink", 1) != 1):
        raise OSError("expected physical non-hard-linked regular file")
    if not allow_empty and info.st_size <= 0:
        raise OSError("expected nonempty regular file")
    return info


def sha256_file(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
        allow_nan=False).encode("utf-8")


def strict_json_load(path: Path | str) -> tuple[dict[str, Any], bytes]:
    raw = Path(path).read_bytes()

    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON member {key}")
            result[key] = value
        return result

    value = json.loads(
        raw.decode("utf-8", errors="strict"), object_pairs_hook=unique,
        parse_constant=lambda token: (_ for _ in ()).throw(
            ValueError(f"non-finite JSON number {token}")))
    if type(value) is not dict:
        raise ValueError("JSON root must be an object")
    return value, raw


def asset_paths(map_name: str, page_x: int, page_y: int) -> dict[str, str]:
    normalized = map_name.title()
    stem = f"{normalized}_{page_x:02}_{page_y:02}"
    root = f"/Game/Terrain/Reference/{normalized}"
    return {
        "mesh": f"{root}/SM_{stem}",
        "texture": f"{root}/T_{stem}",
        "material": "/Game/Terrain/Reference/M_TerrainReference",
        "instance": f"{root}/MI_{stem}",
    }


def reference_actor_object_path(
    map_package: str, map_name: str, page_x: int, page_y: int,
) -> str:
    world_name = map_package.rsplit("/", 1)[-1]
    normalized = map_name.title()
    return (
        f"{map_package}.{world_name}:PersistentLevel."
        f"ReferenceTerrain_{normalized}_{page_x:02}_{page_y:02}")


def overlapping_legacy_tiles(
    page_bounds: tuple[float, float, float, float],
    actor_bounds: list[tuple[str, float, float, float, float]],
) -> list[str]:
    page_min_x, page_min_y, page_max_x, page_max_y = page_bounds
    result = []
    for name, min_x, min_y, max_x, max_y in actor_bounds:
        if (max_x > page_min_x and min_x < page_max_x and
                max_y > page_min_y and min_y < page_max_y):
            result.append(name)
    return sorted(set(result))


def assert_legacy_target_allowed(entry_point: str, asset_path: str) -> None:
    if not entry_point or type(asset_path) is not str:
        raise ValueError("legacy terrain mutation target is invalid")
    if (asset_path == "/Game/Terrain/Reference" or
            asset_path.startswith("/Game/Terrain/Reference/")):
        raise ValueError(
            f"{entry_point} cannot mutate Task 8 reference namespace: "
            f"{asset_path}")


def _check_exact(
    value: Any, expected: Any, pointer: str, issues: list[ValidationIssue],
    code: str = "INVALID_SCHEMA",
) -> bool:
    if type(value) is not type(expected) or value != expected:
        return _first(issues, code, pointer, f"expected {expected!r}")
    return True


def _validate_hashed_input(
    value: Any, pointer: str, repo_root: Path, issues: list[ValidationIssue],
) -> Path | None:
    record = _require_keys(value, {"path", "sha256"}, pointer, issues)
    if record is None:
        return None
    if not _normalized_relative(record["path"]):
        _first(issues, "INVALID_FILE", _pointer(pointer, "path"),
               "expected normalized repository-relative path")
        return None
    if type(record["sha256"]) is not str or not _HASH.fullmatch(record["sha256"]):
        _first(issues, "INVALID_HASH", _pointer(pointer, "sha256"),
               "expected lowercase SHA-256")
        return None
    path = repo_root / PurePosixPath(record["path"])
    try:
        _physical_regular(path)
        if sha256_file(path) != record["sha256"]:
            _first(issues, "INVALID_HASH", _pointer(pointer, "sha256"),
                   "physical file hash differs")
            return None
    except OSError as exc:
        _first(issues, "INVALID_FILE", _pointer(pointer, "path"), str(exc))
        return None
    return path


def _validate_texture_input(
    record: dict[str, Any], pointer: str, repo_root: Path,
    issues: list[ValidationIssue],
) -> Path | None:
    return _validate_hashed_input(
        {"path": record["path"], "sha256": record["sha256"]},
        pointer, repo_root, issues)


def validate_manifest(
    data: dict,
    manifest_path: Path,
    limits: ReferenceTerrainLimits = ReferenceTerrainLimits(),
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    top = _require_keys(data, {
        "schemaVersion", "algorithmVersion", "source", "page",
        "requiredPresentRect", "usedTextureIds", "sectionPresence", "files",
        "metrics",
    }, "", issues)
    if top is None:
        return issues
    if not _check_exact(top["schemaVersion"], 1, "/schemaVersion", issues):
        return issues
    if not _check_exact(
            top["algorithmVersion"], "legacy-fixed-pipeline-v1",
            "/algorithmVersion", issues):
        return issues

    source = _require_keys(top["source"], {
        "map", "database", "clientRoot", "alphaAtlas", "usedTextures",
    }, "/source", issues)
    if source is None:
        return issues
    manifest_physical = Path(manifest_path).resolve()
    if (manifest_physical.parent.name == "maps" and
            manifest_physical.parent.parent.name == "artifacts"):
        repo_root = manifest_physical.parent.parent.parent
    else:
        repo_root = Path.cwd().resolve()
    for name in ("map", "database", "alphaAtlas"):
        _validate_hashed_input(source[name], f"/source/{name}", repo_root, issues)
        if issues:
            return issues
    if not _normalized_relative(source["clientRoot"]):
        return [_issue("INVALID_FILE", "/source/clientRoot",
                       "expected normalized repository-relative path")]
    client_root = repo_root / PurePosixPath(source["clientRoot"])
    try:
        info = client_root.lstat()
        if client_root.is_symlink() or not stat.S_ISDIR(info.st_mode):
            raise OSError("expected physical directory")
    except OSError as exc:
        return [_issue("INVALID_FILE", "/source/clientRoot", str(exc))]
    if type(source["usedTextures"]) is not list or not source["usedTextures"]:
        return [_issue("INVALID_SCHEMA", "/source/usedTextures",
                       "expected nonempty list")]
    texture_ids: list[int] = []
    texture_paths: list[str] = []
    for index, item in enumerate(source["usedTextures"]):
        pointer = f"/source/usedTextures/{index}"
        record = _require_keys(item, {"textureId", "path", "sha256"}, pointer, issues)
        if record is None:
            return issues
        if not _is_int(record["textureId"], 0, 255):
            return [_issue("INVALID_TEXTURE_IDS", pointer + "/textureId",
                           "expected uint8 texture ID")]
        _validate_texture_input(record, pointer, repo_root, issues)
        if issues:
            return issues
        if not (record["path"] == source["clientRoot"] or
                record["path"].startswith(source["clientRoot"] + "/")):
            return [_issue("INVALID_PROVENANCE", pointer + "/path",
                           "texture is outside clientRoot")]
        texture_ids.append(record["textureId"])
        texture_paths.append(record["path"])
    if texture_ids != sorted(set(texture_ids)):
        return [_issue("INVALID_TEXTURE_IDS", "/source/usedTextures",
                       "texture IDs must be sorted and unique")]
    if len(set(texture_paths)) != len(texture_paths):
        return [_issue("INVALID_PROVENANCE", "/source/usedTextures",
                       "texture paths must be distinct")]

    page = _require_keys(top["page"], {
        "x", "y", "sourceCellBounds", "pixelsPerCell", "pixelWidth",
        "pixelHeight", "ambient", "dwTColor",
    }, "/page", issues)
    if page is None:
        return issues
    expected_page = {
        "x": 17, "y": 21, "pixelsPerCell": 32, "pixelWidth": 4096,
        "pixelHeight": 4096, "ambient": [1, 1, 1], "dwTColor": 0,
    }
    for key, expected in expected_page.items():
        if not _check_exact(page[key], expected, f"/page/{key}", issues,
                            "INVALID_BOUNDS"):
            return issues
    for value, pointer, expected in (
        (page["sourceCellBounds"], "/page/sourceCellBounds",
         {"x": 2176, "y": 2688, "width": 128, "height": 128}),
        (top["requiredPresentRect"], "/requiredPresentRect",
         {"x": 2193, "y": 2756, "width": 80, "height": 47}),
    ):
        record = _require_keys(value, set(expected), pointer, issues)
        if record is None:
            return issues
        for key, wanted in expected.items():
            if not _check_exact(record[key], wanted, f"{pointer}/{key}", issues,
                                "INVALID_BOUNDS"):
                return issues
    if top["usedTextureIds"] != texture_ids:
        return [_issue("INVALID_TEXTURE_IDS", "/usedTextureIds",
                       "must equal sorted usedTextures IDs")]
    section = _require_keys(top["sectionPresence"], {
        "originX", "originY", "width", "height", "rowMajorMask",
    }, "/sectionPresence", issues)
    if section is None:
        return issues
    expected_section = {"originX": 272, "originY": 336, "width": 16, "height": 16}
    for key, expected in expected_section.items():
        if not _check_exact(section[key], expected, f"/sectionPresence/{key}",
                            issues, "INVALID_SECTION_MASK"):
            return issues
    if (type(section["rowMajorMask"]) is not list or
            section["rowMajorMask"] != [1] * 256):
        return [_issue("INVALID_SECTION_MASK", "/sectionPresence/rowMajorMask",
                       "expected exactly 256 present sections")]

    files = _require_keys(top["files"], set(_MANIFEST_LEAVES), "/files", issues)
    if files is None:
        return issues
    run_ids: set[str] = set()
    identities: set[tuple[int, int]] = set()
    total = 0
    manifest_root = Path(manifest_path).resolve().parent
    for key, leaf in _MANIFEST_LEAVES.items():
        pointer = f"/files/{key}"
        item = _require_keys(files[key], {"path", "sha256", "sizeBytes"}, pointer, issues)
        if item is None:
            return issues
        if not _normalized_relative(item["path"]):
            return [_issue("INVALID_FILE", pointer + "/path",
                           "expected normalized relative path")]
        parts = PurePosixPath(item["path"]).parts
        if (len(parts) != 3 or parts[0] != "runs" or
                not _RUN_ID.fullmatch(parts[1]) or parts[2] != leaf):
            return [_issue("INVALID_FILE", pointer + "/path",
                           f"expected runs/<run-id>/{leaf}")]
        if type(item["sha256"]) is not str or not _HASH.fullmatch(item["sha256"]):
            return [_issue("INVALID_HASH", pointer + "/sha256",
                           "expected lowercase SHA-256")]
        if not _is_int(item["sizeBytes"], 1):
            return [_issue("INVALID_FILE", pointer + "/sizeBytes",
                           "expected positive integer")]
        path = manifest_root / PurePosixPath(item["path"])
        try:
            info = _physical_regular(path)
        except OSError as exc:
            return [_issue("INVALID_FILE", pointer + "/path", str(exc))]
        if info.st_size != item["sizeBytes"]:
            return [_issue("INVALID_FILE", pointer + "/sizeBytes",
                           "physical size differs")]
        if sha256_file(path) != item["sha256"]:
            return [_issue("INVALID_HASH", pointer + "/sha256",
                           "physical hash differs")]
        identity = (int(info.st_dev), int(info.st_ino))
        if identity in identities:
            return [_issue("INVALID_FILE", pointer + "/path",
                           "run products must be distinct physical files")]
        identities.add(identity)
        run_ids.add(parts[1])
        total += item["sizeBytes"]
    if len(run_ids) != 1:
        return [_issue("INVALID_FILE", "/files", "all products must share one run ID")]

    metrics = _require_keys(top["metrics"], {
        "peakRssBytes", "peakTextureCacheBytes", "peakRgbaRowBytes", "pngBytes",
        "totalOutputBytes", "maxHeightErrorCm", "rmsHeightErrorCm",
        "sharedBoundaryMaxCm", "absentSectionCount", "unresolvedLayerCount",
    }, "/metrics", issues)
    if metrics is None:
        return issues
    integer_metrics = (
        ("peakRssBytes", 1, limits.max_rss_bytes),
        ("peakTextureCacheBytes", 0, limits.max_texture_cache_bytes),
        ("peakRgbaRowBytes", 0, limits.max_rgba_row_bytes),
        ("pngBytes", 0, limits.max_png_bytes),
    )
    for key, minimum, maximum in integer_metrics:
        if not _is_int(metrics[key], minimum):
            return [_issue("INVALID_METRIC", f"/metrics/{key}",
                           "expected bounded integer metric")]
        if metrics[key] > maximum:
            return [_issue("BUDGET_EXCEEDED", f"/metrics/{key}",
                           "metric exceeds budget")]
    if metrics["totalOutputBytes"] != total:
        return [_issue("INVALID_METRIC", "/metrics/totalOutputBytes",
                       "must equal sum of seven disk sizes")]
    for key, maximum in (
        ("maxHeightErrorCm", limits.max_height_error_cm),
        ("rmsHeightErrorCm", limits.max_rms_error_cm),
        ("sharedBoundaryMaxCm", limits.max_shared_boundary_cm),
    ):
        if not _is_number(metrics[key]) or metrics[key] < 0:
            return [_issue("INVALID_METRIC", f"/metrics/{key}",
                           "expected finite nonnegative metric")]
        if float(metrics[key]) > maximum:
            return [_issue("BUDGET_EXCEEDED", f"/metrics/{key}",
                           "metric exceeds budget")]
    for key in ("absentSectionCount", "unresolvedLayerCount"):
        if not _check_exact(metrics[key], 0, f"/metrics/{key}", issues,
                            "INVALID_METRIC"):
            return issues
    return issues


_MANIFEST_LEAVES = {
    "height": "garner.height.r16",
    "block": "garner.block.raw",
    "region": "garner.region.raw",
    "terrainMetadata": "garner.terrain.json",
    "albedo": "garner.albedo_17_21.png",
    "meshGltf": "garner.terrain_17_21.gltf",
    "meshBin": "garner.terrain_17_21.bin",
}


def file_evidence(
    path: Path | str, repo_root: Path, *, allow_empty: bool = False,
) -> dict[str, Any]:
    root = Path(repo_root).resolve()
    physical = Path(path).resolve(strict=True)
    physical.relative_to(root)
    info = _physical_regular(physical, allow_empty=allow_empty)
    return {
        "path": physical.relative_to(root).as_posix(),
        "sha256": sha256_file(physical),
        "sizeBytes": int(info.st_size),
    }


def package_family_evidence(
    family: str, package: str, stem: Path | str, repo_root: Path,
) -> dict[str, Any]:
    if family not in PACKAGE_FAMILIES or package != PACKAGE_PATHS[family]:
        raise ValueError("noncanonical package family")
    stem = Path(stem)
    primary = ".umap" if family == "map" else ".uasset"
    allowed = (primary,) + PACKAGE_SIDECARS
    paths = [stem.with_suffix(suffix) for suffix in allowed
             if stem.with_suffix(suffix).exists() or stem.with_suffix(suffix).is_symlink()]
    if stem.with_suffix(primary) not in paths:
        raise ValueError(f"missing package primary {stem.with_suffix(primary)}")
    try:
        for child in stem.parent.iterdir():
            if child.name.startswith(stem.name + ".") and child.suffix not in allowed:
                raise ValueError(f"unexpected same-stem suffix {child}")
    except OSError as exc:
        raise ValueError(f"cannot enumerate package family {stem}: {exc}") from exc
    files = [file_evidence(path, repo_root) for path in sorted(paths)]
    digest = hashlib.sha256(canonical_json_bytes(files)).hexdigest()
    return {
        "family": family,
        "package": package,
        "files": files,
        "familySha256": digest,
    }


def enumerate_package_families(repo_root: Path) -> list[dict[str, Any]]:
    root = Path(repo_root)
    return [
        package_family_evidence(
            family, PACKAGE_PATHS[family], root / PACKAGE_STEMS[family], root)
        for family in PACKAGE_FAMILIES
    ]


def _validate_file_evidence(
    value: Any,
    pointer: str,
    repo_root: Path,
    issues: list[ValidationIssue],
    *,
    allow_empty: bool = False,
    rehash: bool = True,
) -> Path | None:
    record = _require_keys(value, {"path", "sha256", "sizeBytes"}, pointer, issues)
    if record is None:
        return None
    if not _normalized_relative(record["path"]):
        _first(issues, "INVALID_FILE", pointer + "/path",
               "expected normalized repository-relative path")
        return None
    if type(record["sha256"]) is not str or not _HASH.fullmatch(record["sha256"]):
        _first(issues, "INVALID_HASH", pointer + "/sha256",
               "expected lowercase SHA-256")
        return None
    minimum = 0 if allow_empty else 1
    if not _is_int(record["sizeBytes"], minimum):
        _first(issues, "INVALID_FILE", pointer + "/sizeBytes",
               "expected valid file size")
        return None
    if not rehash:
        return None
    path = Path(repo_root).resolve() / PurePosixPath(record["path"])
    try:
        info = _physical_regular(path, allow_empty=allow_empty)
        if info.st_size != record["sizeBytes"]:
            _first(issues, "INVALID_FILE", pointer + "/sizeBytes",
                   "physical size differs")
            return None
        if sha256_file(path) != record["sha256"]:
            _first(issues, "INVALID_HASH", pointer + "/sha256",
                   "physical file hash differs")
            return None
    except OSError as exc:
        _first(issues, "INVALID_FILE", pointer + "/path", str(exc))
        return None
    return path


def _validate_issue_list(
    status: Any, value: Any, pointer: str, issues: list[ValidationIssue],
) -> bool:
    if type(value) is not list:
        return _first(issues, "INVALID_SCHEMA", pointer, "expected issue list")
    for index, item in enumerate(value):
        item_pointer = f"{pointer}/{index}"
        record = _require_keys(item, {"code", "field", "detail"}, item_pointer, issues)
        if record is None:
            return False
        if (type(record["code"]) is not str or not record["code"] or
                type(record["field"]) is not str or
                not record["field"].startswith("/") or
                type(record["detail"]) is not str or not record["detail"]):
            return _first(issues, "INVALID_SCHEMA", item_pointer,
                          "invalid Issue record")
    if status == "PASS" and value:
        return _first(issues, "INVALID_STATUS", pointer,
                      "PASS requires an empty issue list")
    if status == "FAIL" and not value:
        return _first(issues, "INVALID_STATUS", pointer,
                      "FAIL requires at least one issue")
    if status not in ("PASS", "FAIL"):
        return _first(issues, "INVALID_STATUS", "/status",
                      "status must be PASS or FAIL")
    return True


def _validate_identity(
    record: dict[str, Any], issues: list[ValidationIssue], pointer: str = "",
) -> bool:
    if type(record.get("transactionId")) is not str or not _TXN.fullmatch(
            record["transactionId"]):
        return _first(issues, "INVALID_IDENTITY", pointer + "/transactionId",
                      "expected 32 lowercase hex characters")
    if type(record.get("sourceHead")) is not str or not _HEAD.fullmatch(
            record["sourceHead"]):
        return _first(issues, "INVALID_IDENTITY", pointer + "/sourceHead",
                      "expected 40 lowercase hex characters")
    return True


def _validate_package_family(
    value: Any,
    pointer: str,
    repo_root: Path,
    issues: list[ValidationIssue],
    *,
    expected_family: str,
    rehash: bool,
) -> bool:
    record = _require_keys(
        value, {"family", "package", "files", "familySha256"}, pointer, issues)
    if record is None:
        return False
    if record["family"] != expected_family:
        return _first(issues, "INVALID_PACKAGE", pointer + "/family",
                      f"expected {expected_family}")
    if record["package"] != PACKAGE_PATHS[expected_family]:
        return _first(issues, "INVALID_PACKAGE", pointer + "/package",
                      "noncanonical package path")
    if type(record["files"]) is not list or not record["files"]:
        return _first(issues, "INVALID_PACKAGE", pointer + "/files",
                      "expected nonempty package file list")
    paths: list[str] = []
    primary = ".umap" if expected_family == "map" else ".uasset"
    expected_stem = PACKAGE_STEMS[expected_family]
    suffixes: set[str] = set()
    for index, item in enumerate(record["files"]):
        item_pointer = f"{pointer}/files/{index}"
        _validate_file_evidence(
            item, item_pointer, repo_root, issues, rehash=rehash)
        if issues:
            return False
        path = item["path"]
        pure = PurePosixPath(path)
        suffix = pure.suffix
        if pure.with_suffix("").as_posix() != expected_stem:
            return _first(issues, "INVALID_PACKAGE", item_pointer + "/path",
                          "file is outside exact package stem")
        if suffix not in ((primary,) + PACKAGE_SIDECARS):
            return _first(issues, "INVALID_PACKAGE", item_pointer + "/path",
                          "unexpected package suffix")
        paths.append(path)
        suffixes.add(suffix)
    if paths != sorted(set(paths)):
        return _first(issues, "INVALID_PACKAGE", pointer + "/files",
                      "package files must be sorted and unique")
    if primary not in suffixes:
        return _first(issues, "INVALID_PACKAGE", pointer + "/files",
                      "required package primary is missing")
    digest = hashlib.sha256(canonical_json_bytes(record["files"])).hexdigest()
    if type(record["familySha256"]) is not str or record["familySha256"] != digest:
        return _first(issues, "INVALID_HASH", pointer + "/familySha256",
                      "package family digest differs")
    if rehash:
        stem = Path(repo_root).resolve() / expected_stem
        try:
            current = package_family_evidence(
                expected_family, PACKAGE_PATHS[expected_family], stem, repo_root)
        except (OSError, ValueError) as exc:
            return _first(issues, "INVALID_PACKAGE", pointer, str(exc))
        if current != record:
            return _first(issues, "INVALID_PACKAGE", pointer,
                          "physical package family differs")
    return True


def validate_reference_state(
    value: Any, pointer: str = "/referenceState",
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    root = _require_keys(
        value, {"mesh", "texture", "material", "instance", "actor"},
        pointer, issues)
    if root is None:
        return issues
    paths = asset_paths("garner", 17, 21)
    expected_shapes = {
        "mesh": {"objectPath", "sourceGltfSha256", "sourceBinSha256",
                 "naniteEnabled", "materialSlot0"},
        "texture": {"objectPath", "sourceWidth", "sourceHeight", "sourceFormat",
                    "sourceSha256", "srgb", "compression", "filter", "addressX",
                    "addressY", "mipGenSettings", "lodGroup", "lodBias",
                    "neverStream"},
        "material": {"objectPath", "blendMode", "shadingModel", "parameterName",
                     "samplerType", "samplerSource", "rgbOutput", "alphaOutput",
                     "usageFlags"},
        "instance": {"objectPath", "parent", "baseColorTexture"},
        "actor": {"objectPath", "className", "label", "tag", "locationCm",
                  "staticMesh", "boundsMinCm", "boundsMaxCm",
                  "componentMaterialSlot0"},
    }
    for key, shape in expected_shapes.items():
        if _require_keys(root[key], shape, f"{pointer}/{key}", issues) is None:
            return issues
    exact = {
        "mesh": {
            "objectPath": paths["mesh"], "naniteEnabled": True,
            "materialSlot0": paths["instance"],
        },
        "texture": {
            "objectPath": paths["texture"], "sourceWidth": 4096,
            "sourceHeight": 4096, "sourceFormat": "RGBA8", "srgb": True,
            "compression": "TC_DEFAULT", "filter": "TF_BILINEAR",
            "addressX": "TA_CLAMP", "addressY": "TA_CLAMP",
            "mipGenSettings": "TMGS_FROM_TEXTURE_GROUP",
            "lodGroup": "TEXTUREGROUP_WORLD", "lodBias": 0,
            "neverStream": False,
        },
        "material": {
            "objectPath": paths["material"], "blendMode": "BLEND_MASKED",
            "shadingModel": "MSM_UNLIT", "parameterName": "BaseColorTexture",
            "samplerType": "SAMPLERTYPE_COLOR",
            "samplerSource": "SSM_FROM_TEXTURE_ASSET",
            "rgbOutput": "MP_EMISSIVE_COLOR", "alphaOutput": "MP_OPACITY_MASK",
            "usageFlags": ["MATUSAGE_NANITE", "MATUSAGE_STATIC_MESH"],
        },
        "instance": {
            "objectPath": paths["instance"], "parent": paths["material"],
            "baseColorTexture": paths["texture"],
        },
        "actor": {
            "objectPath": reference_actor_object_path(
                REFERENCE_MAP_PACKAGE, "garner", 17, 21),
            "className": "/Script/Engine.StaticMeshActor",
            "label": "ReferenceTerrain_Garner_17_21", "tag": REFERENCE_TAG,
            "locationCm": [217600.0, -268800.0, 0.0],
            "staticMesh": paths["mesh"],
            "componentMaterialSlot0": paths["instance"],
        },
    }
    for section, members in exact.items():
        for key, expected in members.items():
            if not _check_exact(
                    root[section][key], expected, f"{pointer}/{section}/{key}",
                    issues, "INVALID_REFERENCE_STATE"):
                return issues
    for section, key in (
        ("mesh", "sourceGltfSha256"), ("mesh", "sourceBinSha256"),
        ("texture", "sourceSha256"),
    ):
        value_hash = root[section][key]
        if type(value_hash) is not str or not _HASH.fullmatch(value_hash):
            return [_issue("INVALID_HASH", f"{pointer}/{section}/{key}",
                           "expected lowercase SHA-256")]
    for key, expected in (
        ("boundsMinCm", (217600.0, -281600.0)),
        ("boundsMaxCm", (230400.0, -268800.0)),
    ):
        observed = root["actor"][key]
        if (type(observed) is not list or len(observed) != 2 or
                any(not _is_number(item) for item in observed) or
                any(abs(float(item) - wanted) > 1.0
                    for item, wanted in zip(observed, expected))):
            return [_issue("INVALID_REFERENCE_STATE", f"{pointer}/actor/{key}",
                           "bounds exceed 1 cm tolerance")]
    return issues


def _validate_manifest_evidence(
    value: Any, pointer: str, repo_root: Path, issues: list[ValidationIssue],
) -> bool:
    path = _validate_file_evidence(value, pointer, repo_root, issues)
    if issues or path is None:
        return False
    try:
        manifest, _ = strict_json_load(path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        return _first(issues, "INVALID_MANIFEST", pointer + "/path", str(exc))
    nested = validate_manifest(manifest, path)
    if nested:
        item = nested[0]
        return _first(issues, item["code"], pointer + item["field"], item["detail"])
    return True


def validate_level_build_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    expected = {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "manifest", "mapPackage", "worldObject", "mapPackageHash",
        "markerObject", "gameModeClass", "replacedExistingMap", "issues",
    }
    root = _require_keys(data, expected, "", issues)
    if root is None:
        return issues
    for key, value in (
        ("schemaVersion", 1),
        ("reportType", "garner-reference-terrain-level-build"),
        ("mapPackage", REFERENCE_MAP_PACKAGE),
        ("worldObject", REFERENCE_WORLD_OBJECT),
        ("markerObject", REFERENCE_BUILD_MARKER),
        ("gameModeClass", REFERENCE_GAME_MODE),
    ):
        if not _check_exact(root[key], value, f"/{key}", issues):
            return issues
    if type(root["replacedExistingMap"]) is not bool:
        return [_issue("INVALID_SCHEMA", "/replacedExistingMap", "expected boolean")]
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    if root["status"] != "PASS":
        return issues
    _validate_manifest_evidence(root["manifest"], "/manifest", repo_root, issues)
    if issues:
        return issues
    _validate_package_family(
        root["mapPackageHash"], "/mapPackageHash", repo_root, issues,
        expected_family="map", rehash=False)
    return issues


def _validate_object_changes(
    value: Any, pointer: str, issues: list[ValidationIssue],
) -> bool:
    if type(value) is not list:
        return _first(issues, "INVALID_SCHEMA", pointer, "expected list")
    records: list[tuple[str, str, str]] = []
    for index, item in enumerate(value):
        record = _require_keys(
            item, {"objectPath", "className", "reason"}, f"{pointer}/{index}", issues)
        if record is None:
            return False
        if (type(record["objectPath"]) is not str or
                not record["objectPath"].startswith("/Game/") or
                type(record["className"]) is not str or
                not record["className"].startswith("/Script/") or
                type(record["reason"]) is not str or not record["reason"]):
            return _first(issues, "INVALID_SCHEMA", f"{pointer}/{index}",
                          "invalid ObjectChange")
        records.append((record["objectPath"], record["className"], record["reason"]))
    if records != sorted(set(records)):
        return _first(issues, "INVALID_SCHEMA", pointer,
                      "ObjectChange list must be sorted and unique")
    return True


def _validate_family_list(
    value: Any, pointer: str, repo_root: Path, issues: list[ValidationIssue],
    *, rehash: bool,
) -> bool:
    if type(value) is not list or len(value) != len(PACKAGE_FAMILIES):
        return _first(issues, "INVALID_PACKAGE", pointer,
                      "expected five package families")
    for index, family in enumerate(PACKAGE_FAMILIES):
        if not _validate_package_family(
                value[index], f"{pointer}/{index}", repo_root, issues,
                expected_family=family, rehash=rehash):
            return False
    return True


def validate_import_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    expected = {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "manifest", "mapPackage", "worldObject", "markerObject", "gameModeClass",
        "beforeMapPackageHash", "referenceState", "created", "updated", "deleted",
        "savedPackages", "finalPackageHashes", "issues",
    }
    root = _require_keys(data, expected, "", issues)
    if root is None:
        return issues
    for key, value in (
        ("schemaVersion", 1), ("reportType", "garner-reference-terrain-import"),
        ("mapPackage", REFERENCE_MAP_PACKAGE), ("worldObject", REFERENCE_WORLD_OBJECT),
        ("markerObject", REFERENCE_BUILD_MARKER), ("gameModeClass", REFERENCE_GAME_MODE),
    ):
        if not _check_exact(root[key], value, f"/{key}", issues):
            return issues
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    if root["status"] != "PASS":
        return issues
    _validate_manifest_evidence(root["manifest"], "/manifest", repo_root, issues)
    if issues:
        return issues
    manifest_path = Path(repo_root).resolve() / PurePosixPath(root["manifest"]["path"])
    try:
        manifest_data, _ = strict_json_load(manifest_path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        return [_issue("INVALID_MANIFEST", "/manifest/path", str(exc))]
    _validate_package_family(
        root["beforeMapPackageHash"], "/beforeMapPackageHash", repo_root, issues,
        expected_family="map", rehash=False)
    if issues:
        return issues
    state_issues = validate_reference_state(root["referenceState"])
    if state_issues:
        return state_issues
    source_links = (
        ("mesh", "sourceGltfSha256", "meshGltf"),
        ("mesh", "sourceBinSha256", "meshBin"),
        ("texture", "sourceSha256", "albedo"),
    )
    for section, field, manifest_key in source_links:
        if (root["referenceState"][section][field] !=
                manifest_data["files"][manifest_key]["sha256"]):
            return [_issue(
                "INVALID_HASH", f"/referenceState/{section}/{field}",
                f"must equal manifest files.{manifest_key}.sha256")]
    for key in ("created", "updated", "deleted"):
        if not _validate_object_changes(root[key], f"/{key}", issues):
            return issues
    if type(root["savedPackages"]) is not list or any(
            type(item) is not str or not item.startswith("/Game/")
            for item in root["savedPackages"]):
        return [_issue("INVALID_SCHEMA", "/savedPackages",
                       "expected absolute Unreal package paths")]
    if root["savedPackages"] != sorted(set(root["savedPackages"])):
        return [_issue("INVALID_SCHEMA", "/savedPackages",
                       "saved packages must be sorted and unique")]
    _validate_family_list(
        root["finalPackageHashes"], "/finalPackageHashes", repo_root, issues,
        rehash=True)
    return issues


def validate_idempotent_import_reports(
    first: dict, second: dict,
) -> list[ValidationIssue]:
    for key in ("created", "updated", "deleted", "savedPackages"):
        if second.get(key) != []:
            return [_issue("NOT_IDEMPOTENT", f"/second/{key}",
                           "second import must make no mutation")]
    if first.get("referenceState") != second.get("referenceState"):
        return [_issue("NOT_IDEMPOTENT", "/second/referenceState",
                       "reference state differs between passes")]
    if first.get("finalPackageHashes") != second.get("finalPackageHashes"):
        return [_issue("NOT_IDEMPOTENT", "/second/finalPackageHashes",
                       "physical package families differ between passes")]
    for key in (
        "transactionId", "sourceHead", "manifest", "mapPackage", "worldObject",
        "markerObject", "gameModeClass",
    ):
        if key in first or key in second:
            if first.get(key) != second.get(key):
                return [_issue("NOT_IDEMPOTENT", f"/second/{key}",
                               "import identity differs between passes")]
    if ("beforeMapPackageHash" in second and
            type(first.get("finalPackageHashes")) is list and
            len(first["finalPackageHashes"]) == 5 and
            second["beforeMapPackageHash"] != first["finalPackageHashes"][4]):
        return [_issue("NOT_IDEMPOTENT", "/second/beforeMapPackageHash",
                       "pass 2 must observe pass 1 map family")]
    return []


def validate_check_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    expected = {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "manifest", "levelBuildReport", "importPass1Report", "importPass2Report",
        "mapPackage", "worldObject", "markerObject", "gameModeClass",
        "referenceState", "finalPackageHashes", "overlappingLegacyActors",
        "visibleLegacyOverlaps", "grassOverrides", "materialFallbackCount",
        "materialUsageErrorCount", "translucentNaniteCount", "issues",
    }
    root = _require_keys(data, expected, "", issues)
    if root is None:
        return issues
    for key, value in (
        ("schemaVersion", 1), ("reportType", "garner-reference-terrain-check"),
        ("mapPackage", REFERENCE_MAP_PACKAGE), ("worldObject", REFERENCE_WORLD_OBJECT),
        ("markerObject", REFERENCE_BUILD_MARKER), ("gameModeClass", REFERENCE_GAME_MODE),
    ):
        if not _check_exact(root[key], value, f"/{key}", issues):
            return issues
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    if root["status"] != "PASS":
        return issues
    _validate_manifest_evidence(root["manifest"], "/manifest", repo_root, issues)
    if issues:
        return issues
    linked = (
        ("levelBuildReport", validate_level_build_report),
        ("importPass1Report", validate_import_report),
        ("importPass2Report", validate_import_report),
    )
    parsed: dict[str, dict[str, Any]] = {}
    for key, validator in linked:
        path = _validate_file_evidence(root[key], f"/{key}", repo_root, issues)
        if issues or path is None:
            return issues
        try:
            linked_data, _ = strict_json_load(path)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            return [_issue("INVALID_REPORT", f"/{key}/path", str(exc))]
        nested = validator(linked_data, repo_root)
        if nested:
            item = nested[0]
            return [_issue(item["code"], f"/{key}{item['field']}", item["detail"])]
        if (linked_data["transactionId"] != root["transactionId"] or
                linked_data["sourceHead"] != root["sourceHead"]):
            return [_issue("INVALID_IDENTITY", f"/{key}",
                           "linked report identity differs")]
        parsed[key] = linked_data
    relation = validate_idempotent_import_reports(
        parsed["importPass1Report"], parsed["importPass2Report"])
    if relation:
        return relation
    state_issues = validate_reference_state(root["referenceState"])
    if state_issues:
        return state_issues
    if root["referenceState"] != parsed["importPass2Report"]["referenceState"]:
        return [_issue("INVALID_REFERENCE_STATE", "/referenceState",
                       "checker differs from import pass 2")]
    _validate_family_list(
        root["finalPackageHashes"], "/finalPackageHashes", repo_root, issues,
        rehash=True)
    if issues:
        return issues
    if root["finalPackageHashes"] != parsed["importPass2Report"]["finalPackageHashes"]:
        return [_issue("INVALID_PACKAGE", "/finalPackageHashes",
                       "checker differs from import pass 2")]
    for key in ("overlappingLegacyActors", "visibleLegacyOverlaps", "grassOverrides"):
        value = root[key]
        if (type(value) is not list or any(type(item) is not str for item in value) or
                value != sorted(set(value))):
            return [_issue("INVALID_SCHEMA", f"/{key}",
                           "expected sorted unique string list")]
    if root["visibleLegacyOverlaps"]:
        return [_issue("VISIBLE_OVERLAP", "/visibleLegacyOverlaps",
                       "overlapping legacy terrain must be hidden")]
    if root["grassOverrides"]:
        return [_issue("GRASS_OVERRIDE", "/grassOverrides",
                       "reference actor must not use MI_grass05")]
    for key in (
        "materialFallbackCount", "materialUsageErrorCount", "translucentNaniteCount",
    ):
        if root[key] != 0 or type(root[key]) is not int:
            return [_issue("INVALID_REFERENCE_STATE", f"/{key}",
                           "expected zero")]
    return []


def validate_module_contract(repo_root: Path) -> list[ValidationIssue]:
    root = Path(repo_root)
    game_path = root / "CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs"
    import_path = root / "CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs"
    project_path = root / "CorsairsUE/CorsairsUE.uproject"
    try:
        game = game_path.read_text(encoding="utf-8")
        editor = import_path.read_text(encoding="utf-8")
        project, _ = strict_json_load(project_path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        return [_issue("INVALID_MODULE_GRAPH", "/modules", str(exc))]
    if re.search(r'"CorsairsImport"', game):
        return [_issue("INVALID_MODULE_GRAPH", "/CorsairsGame/dependencies",
                       "runtime module must not depend on CorsairsImport")]
    if re.search(r'"CorsairsGame"', editor):
        return [_issue("INVALID_MODULE_GRAPH", "/CorsairsImport/dependencies",
                       "Editor module must not depend on CorsairsGame")]
    public_match = re.search(
        r"PublicDependencyModuleNames\.AddRange\s*\(.*?\);", editor, re.S)
    private_match = re.search(
        r"PrivateDependencyModuleNames\.AddRange\s*\(.*?\);", editor, re.S)
    if public_match and '"UnrealEd"' in public_match.group(0):
        return [_issue("INVALID_MODULE_GRAPH", "/CorsairsImport/UnrealEd",
                       "UnrealEd must be a private dependency")]
    if private_match is None or '"UnrealEd"' not in private_match.group(0):
        return [_issue("INVALID_MODULE_GRAPH", "/CorsairsImport/UnrealEd",
                       "private UnrealEd dependency is missing")]
    modules = project.get("Modules")
    if type(modules) is not list:
        return [_issue("INVALID_MODULE_GRAPH", "/uproject/Modules",
                       "expected module list")]
    matching = [item for item in modules if type(item) is dict and
                item.get("Name") == "CorsairsImport"]
    if len(matching) != 1 or matching[0].get("Type") != "Editor":
        return [_issue("INVALID_MODULE_GRAPH", "/uproject/CorsairsImport/Type",
                       "CorsairsImport must remain Editor-only")]
    for relative in RUNTIME_INPUT_PATHS:
        literal = f'RuntimeDependencies.Add("$(ProjectDir)/{relative}", '
        if game.count(literal) != 1:
            return [_issue("INVALID_RUNTIME_STAGING", "/runtimeDependencies",
                           f"missing exact UFS staging for {relative}")]
    if game.count("StagedFileType.UFS") != 3:
        return [_issue("INVALID_RUNTIME_STAGING", "/runtimeDependencies",
                       "expected exactly three UFS dependencies")]
    return []


def atomic_write_json(path: Path | str, value: dict[str, Any]) -> None:
    """Durably write one canonical JSON report in its existing directory."""
    destination = Path(path)
    parent = destination.parent
    parent_info = parent.lstat()
    if parent.is_symlink() or not stat.S_ISDIR(parent_info.st_mode):
        raise OSError("report parent must be a physical directory")
    payload = canonical_json_bytes(value) + b"\n"
    temporary = parent / f".{destination.name}.tmp.{os.getpid()}.{os.urandom(8).hex()}"
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
        0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.close(descriptor)
        descriptor = -1
        info = _physical_regular(temporary)
        if info.st_size != len(payload) or temporary.read_bytes() != payload:
            raise OSError("report temp readback differs")
        os.replace(temporary, destination)
        directory = os.open(parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def validate_editor_identity_target(
    report_path: Path | str,
    transaction_id: str,
    source_head: str,
    repo_root: Path | str,
) -> list[ValidationIssue]:
    if type(transaction_id) is not str or not _TXN.fullmatch(transaction_id):
        return [_issue("INVALID_IDENTITY", "/transactionId",
                       "expected 32 lowercase hex characters")]
    if type(source_head) is not str or not _HEAD.fullmatch(source_head):
        return [_issue("INVALID_IDENTITY", "/sourceHead",
                       "expected 40 lowercase hex characters")]
    root = Path(repo_root).resolve()
    try:
        root_info = root.lstat()
        if root.is_symlink() or not stat.S_ISDIR(root_info.st_mode):
            raise OSError("repo root is not a physical directory")
        expected = root / "artifacts/maps/reports/runs" / transaction_id
        expected_info = expected.lstat()
        if expected.is_symlink() or not stat.S_ISDIR(expected_info.st_mode):
            raise OSError("transaction evidence root is not a physical directory")
        candidate = Path(report_path)
        if not candidate.is_absolute():
            candidate = root / candidate
        physical_parent = candidate.parent.resolve(strict=True)
        physical_parent.relative_to(expected.resolve(strict=True))
        if candidate.resolve(strict=False) == expected.resolve(strict=True):
            raise ValueError("report target cannot be the evidence root")
    except (OSError, ValueError) as exc:
        return [_issue("INVALID_FILE", "/reportPath",
                       f"report must be below its transaction root: {exc}")]
    return []


def validate_runtime_observation(data: dict) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    expected = {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "mapPackage", "worldObject", "gameModeClass", "actorObject",
        "runtimeInputs", "issues",
    }
    root = _require_keys(data, expected, "", issues)
    if root is None:
        return issues
    exact = {
        "schemaVersion": 1,
        "reportType": "garner-terrain-packaged-runtime",
        "status": "PASS",
        "mapPackage": REFERENCE_MAP_PACKAGE,
        "worldObject": REFERENCE_WORLD_OBJECT,
        "gameModeClass": REFERENCE_GAME_MODE,
        "actorObject": reference_actor_object_path(
            REFERENCE_MAP_PACKAGE, "garner", 17, 21),
    }
    for key, value in exact.items():
        if not _check_exact(root[key], value, f"/{key}", issues):
            return issues
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    if type(root["runtimeInputs"]) is not list or len(root["runtimeInputs"]) != 3:
        return [_issue("INVALID_SCHEMA", "/runtimeInputs",
                       "expected three runtime observations")]
    for index, relative in enumerate(RUNTIME_INPUT_PATHS):
        pointer = f"/runtimeInputs/{index}"
        record = _require_keys(
            root["runtimeInputs"][index],
            {"projectRelativePath", "sha256", "sizeBytes"}, pointer, issues)
        if record is None:
            return issues
        if record["projectRelativePath"] != relative:
            return [_issue("INVALID_FILE", pointer + "/projectRelativePath",
                           f"expected {relative}")]
        if type(record["sha256"]) is not str or not _HASH.fullmatch(record["sha256"]):
            return [_issue("INVALID_HASH", pointer + "/sha256",
                           "expected lowercase SHA-256")]
        if not _is_int(record["sizeBytes"], 1):
            return [_issue("INVALID_FILE", pointer + "/sizeBytes",
                           "expected positive size")]
    return []


def _transaction_relative_prefix(transaction_id: str, kind: str) -> str:
    if kind == "reports":
        return f"artifacts/maps/reports/runs/{transaction_id}"
    if kind == "package":
        return f"artifacts/maps/package-run/{transaction_id}"
    raise ValueError(f"unknown transaction root kind {kind}")


def _require_file_below(
    record: dict[str, Any], pointer: str, prefix: str,
    issues: list[ValidationIssue],
) -> bool:
    expected = prefix.rstrip("/") + "/"
    if not record["path"].startswith(expected):
        return _first(
            issues, "INVALID_FILE", pointer + "/path",
            f"file must be below immutable transaction root {prefix}")
    return True


def validate_evidence_set(
    data: Any,
    pointer: str,
    repo_root: Path,
    transaction_id: str,
    source_head: str,
) -> list[ValidationIssue]:
    """Validate one immutable, exhaustive engine-generated evidence tree."""
    issues: list[ValidationIssue] = []
    root = _require_keys(
        data, {"transactionId", "sourceHead", "root", "files"}, pointer, issues)
    if root is None:
        return issues
    if not _validate_identity(root, issues, pointer):
        return issues
    if (root["transactionId"] != transaction_id or
            root["sourceHead"] != source_head):
        return [_issue("INVALID_IDENTITY", pointer,
                       "evidence-set identity differs from bundle")]
    prefix = _transaction_relative_prefix(transaction_id, "reports")
    if (not _normalized_relative(root["root"]) or
            not root["root"].startswith(prefix + "/")):
        return [_issue("INVALID_FILE", pointer + "/root",
                       "evidence root must be transaction-private")]
    physical_root = Path(repo_root).resolve() / PurePosixPath(root["root"])
    try:
        info = physical_root.lstat()
        if physical_root.is_symlink() or not stat.S_ISDIR(info.st_mode):
            raise OSError("expected physical evidence directory")
    except OSError as exc:
        return [_issue("INVALID_FILE", pointer + "/root", str(exc))]
    if type(root["files"]) is not list or not root["files"]:
        return [_issue("INVALID_SCHEMA", pointer + "/files",
                       "expected nonempty evidence file list")]
    paths: list[str] = []
    for index, item in enumerate(root["files"]):
        item_pointer = f"{pointer}/files/{index}"
        _validate_file_evidence(
            item, item_pointer, repo_root, issues, allow_empty=True)
        if issues:
            return issues
        if not _require_file_below(item, item_pointer, root["root"], issues):
            return issues
        paths.append(item["path"])
    if paths != sorted(set(paths)):
        return [_issue("INVALID_SCHEMA", pointer + "/files",
                       "evidence files must be sorted unique")]
    observed: list[dict[str, Any]] = []
    for directory, subdirs, names in os.walk(physical_root, followlinks=False):
        current = Path(directory)
        for name in subdirs:
            child = current / name
            child_info = child.lstat()
            if child.is_symlink() or not stat.S_ISDIR(child_info.st_mode):
                return [_issue("INVALID_FILE", pointer + "/files",
                               "nonphysical evidence directory")]
        for name in names:
            try:
                observed.append(file_evidence(
                    current / name, repo_root, allow_empty=True))
            except OSError as exc:
                return [_issue("INVALID_FILE", pointer + "/files", str(exc))]
    observed.sort(key=lambda item: item["path"])
    if observed != root["files"]:
        return [_issue("INVALID_FILE", pointer + "/files",
                       "evidence tree differs from disk")]
    return []


def validate_build_evidence(
    data: Any,
    pointer: str,
    repo_root: Path,
    transaction_id: str,
    source_head: str,
    target: str,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    root = _require_keys(data, {
        "transactionId", "target", "platform", "configuration", "sourceHead",
        "receipt", "products",
    }, pointer, issues)
    if root is None:
        return issues
    if target not in ("CorsairsUEEditor", "CorsairsUE"):
        return [_issue("INVALID_BUILD", pointer + "/target",
                       "unknown canonical target")]
    if not _validate_identity(root, issues, pointer):
        return issues
    exact = {
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "target": target,
        "platform": "Mac",
        "configuration": "Development",
    }
    for key, value in exact.items():
        if root[key] != value:
            return [_issue("INVALID_BUILD", f"{pointer}/{key}",
                           f"expected {value}")]
    prefix = (
        _transaction_relative_prefix(transaction_id, "reports") +
        f"/builds/{target}")
    receipt_path = _validate_file_evidence(
        root["receipt"], pointer + "/receipt", repo_root, issues)
    if issues or receipt_path is None:
        return issues
    if not _require_file_below(
            root["receipt"], pointer + "/receipt", prefix, issues):
        return issues
    if receipt_path.name != f"{target}.target":
        return [_issue("INVALID_BUILD", pointer + "/receipt/path",
                       "receipt leaf differs from target")]
    try:
        receipt_text = receipt_path.read_text(encoding="utf-8", errors="strict")
        receipt_value = json.loads(receipt_text)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return [_issue("INVALID_BUILD", pointer + "/receipt/path", str(exc))]
    if type(receipt_value) is not dict:
        return [_issue("INVALID_BUILD", pointer + "/receipt/path",
                       "receipt root must be an object")]
    receipt_target = receipt_value.get("TargetName")
    if receipt_target is not None and receipt_target != target:
        return [_issue("INVALID_BUILD", pointer + "/receipt/path",
                       "receipt target identity differs")]
    if "corsairsimport" in receipt_text.lower():
        return [_issue("EDITOR_MODULE_LEAK", pointer + "/receipt/path",
                       "CorsairsImport leaked into target receipt")]
    if type(root["products"]) is not list or not root["products"]:
        return [_issue("INVALID_SCHEMA", pointer + "/products",
                       "expected nonempty build products")]
    paths: list[str] = []
    for index, item in enumerate(root["products"]):
        item_pointer = f"{pointer}/products/{index}"
        _validate_file_evidence(item, item_pointer, repo_root, issues)
        if issues:
            return issues
        if not _require_file_below(item, item_pointer, prefix + "/products", issues):
            return issues
        if "corsairsimport" in item["path"].lower():
            return [_issue("EDITOR_MODULE_LEAK", item_pointer + "/path",
                           "CorsairsImport leaked into build products")]
        paths.append(item["path"])
    if paths != sorted(set(paths)):
        return [_issue("INVALID_SCHEMA", pointer + "/products",
                       "build products must be sorted unique")]
    return []


def validate_inventory_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    root = _require_keys(data, {
        "schemaVersion", "reportType", "transactionId", "sourceHead", "root",
        "files", "issues",
    }, "", issues)
    if root is None:
        return issues
    if not _check_exact(root["schemaVersion"], 1, "/schemaVersion", issues):
        return issues
    if root["reportType"] not in (
            "garner-terrain-stage-inventory", "garner-terrain-archive-inventory"):
        return [_issue("INVALID_SCHEMA", "/reportType", "unknown inventory type")]
    if not _validate_identity(root, issues):
        return issues
    if root["issues"] != []:
        return [_issue("INVALID_STATUS", "/issues", "inventory issues must be empty")]
    if not _normalized_relative(root["root"]):
        return [_issue("INVALID_FILE", "/root", "expected normalized relative root")]
    expected_prefix = f"artifacts/maps/package-run/{root['transactionId']}/"
    if not root["root"].startswith(expected_prefix):
        return [_issue("INVALID_FILE", "/root", "inventory root is outside package run")]
    physical_root = Path(repo_root).resolve() / PurePosixPath(root["root"])
    try:
        info = physical_root.lstat()
        if physical_root.is_symlink() or not stat.S_ISDIR(info.st_mode):
            raise OSError("expected physical inventory directory")
    except OSError as exc:
        return [_issue("INVALID_FILE", "/root", str(exc))]
    if type(root["files"]) is not list or not root["files"]:
        return [_issue("INVALID_SCHEMA", "/files", "expected nonempty inventory")]
    paths = []
    for index, item in enumerate(root["files"]):
        _validate_file_evidence(
            item, f"/files/{index}", repo_root, issues, allow_empty=True)
        if issues:
            return issues
        path = Path(repo_root).resolve() / PurePosixPath(item["path"])
        try:
            path.relative_to(physical_root)
        except ValueError:
            return [_issue("INVALID_FILE", f"/files/{index}/path",
                           "inventory member is outside root")]
        paths.append(item["path"])
    if paths != sorted(set(paths)):
        return [_issue("INVALID_SCHEMA", "/files", "files must be sorted unique")]
    observed = []
    for directory, subdirs, names in os.walk(physical_root, followlinks=False):
        current = Path(directory)
        for name in subdirs:
            child = current / name
            child_info = child.lstat()
            if child.is_symlink() or not stat.S_ISDIR(child_info.st_mode):
                return [_issue("INVALID_FILE", "/files", "nonphysical directory")]
        for name in names:
            child = current / name
            try:
                _physical_regular(child, allow_empty=True)
            except OSError as exc:
                return [_issue("INVALID_FILE", "/files", str(exc))]
            observed.append(file_evidence(child, repo_root, allow_empty=True))
    observed.sort(key=lambda item: item["path"])
    if observed != root["files"]:
        return [_issue("INVALID_FILE", "/files", "inventory differs from disk")]
    return []


def validate_container_list_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    root = _require_keys(data, {
        "schemaVersion", "reportType", "transactionId", "sourceHead", "container",
        "members", "issues",
    }, "", issues)
    if root is None:
        return issues
    if not _check_exact(root["schemaVersion"], 1, "/schemaVersion", issues):
        return issues
    if not _check_exact(root["reportType"], "garner-terrain-container-list",
                        "/reportType", issues):
        return issues
    if not _validate_identity(root, issues):
        return issues
    if root["issues"] != []:
        return [_issue("INVALID_STATUS", "/issues", "container issues must be empty")]
    _validate_file_evidence(root["container"], "/container", repo_root, issues)
    if issues:
        return issues
    if type(root["members"]) is not list or not root["members"]:
        return [_issue("INVALID_SCHEMA", "/members", "expected nonempty member list")]
    paths = []
    for index, item in enumerate(root["members"]):
        pointer = f"/members/{index}"
        record = _require_keys(item, {"path", "sizeBytes"}, pointer, issues)
        if record is None:
            return issues
        if (type(record["path"]) is not str or
                not record["path"].startswith("../../../")):
            return [_issue("INVALID_FILE", pointer + "/path",
                           "expected ../../../ container member")]
        suffix = record["path"][9:]
        if not _normalized_relative(suffix):
            return [_issue("INVALID_FILE", pointer + "/path",
                           "invalid normalized container member")]
        if not _is_int(record["sizeBytes"], 0):
            return [_issue("INVALID_FILE", pointer + "/sizeBytes",
                           "expected nonnegative size")]
        paths.append(record["path"])
    if paths != sorted(set(paths)):
        return [_issue("INVALID_SCHEMA", "/members", "members must be sorted unique")]
    return []


def validate_cook_package_report(
    data: dict, repo_root: Path,
) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    root = _require_keys(data, {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "targetReceipt", "stageManifest", "archiveManifest", "containers",
        "containerLists", "packagedExecutable", "runtimeFiles",
        "corsairsImportLeaks", "issues",
    }, "", issues)
    if root is None:
        return issues
    for key, expected in (
        ("schemaVersion", 1), ("reportType", "garner-terrain-cook-package"),
        ("status", "PASS"),
    ):
        if not _check_exact(root[key], expected, f"/{key}", issues):
            return issues
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    if root["corsairsImportLeaks"] != []:
        return [_issue("EDITOR_MODULE_LEAK", "/corsairsImportLeaks",
                       "CorsairsImport must be absent")]
    for key in ("targetReceipt", "stageManifest", "archiveManifest",
                "packagedExecutable"):
        _validate_file_evidence(root[key], f"/{key}", repo_root, issues)
        if issues:
            return issues
    for key, expected_type in (
        ("stageManifest", "garner-terrain-stage-inventory"),
        ("archiveManifest", "garner-terrain-archive-inventory"),
    ):
        path = Path(repo_root).resolve() / PurePosixPath(root[key]["path"])
        try:
            nested, _ = strict_json_load(path)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            return [_issue("INVALID_REPORT", f"/{key}/path", str(exc))]
        nested_issues = validate_inventory_report(nested, repo_root)
        if nested_issues:
            item = nested_issues[0]
            return [_issue(item["code"], f"/{key}{item['field']}", item["detail"])]
        if (nested["reportType"] != expected_type or
                nested["transactionId"] != root["transactionId"] or
                nested["sourceHead"] != root["sourceHead"]):
            return [_issue("INVALID_IDENTITY", f"/{key}",
                           "nested inventory identity/type differs")]
    for key in ("containers", "containerLists"):
        if type(root[key]) is not list or not root[key]:
            return [_issue("INVALID_SCHEMA", f"/{key}", "expected nonempty list")]
        paths = []
        for index, item in enumerate(root[key]):
            _validate_file_evidence(item, f"/{key}/{index}", repo_root, issues)
            if issues:
                return issues
            paths.append(item["path"])
        if paths != sorted(set(paths)):
            return [_issue("INVALID_SCHEMA", f"/{key}",
                           "evidence list must be sorted unique")]
    parsed_lists = []
    for index, evidence in enumerate(root["containerLists"]):
        path = Path(repo_root).resolve() / PurePosixPath(evidence["path"])
        nested, _ = strict_json_load(path)
        nested_issues = validate_container_list_report(nested, repo_root)
        if nested_issues:
            item = nested_issues[0]
            return [_issue(item["code"], f"/containerLists/{index}{item['field']}",
                           item["detail"])]
        if (nested["transactionId"] != root["transactionId"] or
                nested["sourceHead"] != root["sourceHead"]):
            return [_issue("INVALID_IDENTITY", f"/containerLists/{index}",
                           "container-list identity differs")]
        parsed_lists.append(nested)
    if sorted(
            (item["container"] for item in parsed_lists),
            key=lambda item: item["path"]) != root["containers"]:
        return [_issue("INVALID_SCHEMA", "/containerLists",
                       "every container requires exactly one list")]
    if type(root["runtimeFiles"]) is not list or len(root["runtimeFiles"]) != 3:
        return [_issue("INVALID_SCHEMA", "/runtimeFiles",
                       "expected three runtime files")]
    container_paths = {item["path"] for item in root["containers"]}
    members = {item["path"] for report in parsed_lists for item in report["members"]}
    for index, (item, relative, member) in enumerate(zip(
            root["runtimeFiles"], RUNTIME_INPUT_PATHS, RUNTIME_CONTAINER_PATHS)):
        pointer = f"/runtimeFiles/{index}"
        record = _require_keys(item, {
            "projectRelativePath", "containerPath", "containerMemberPath",
            "extractedEvidence", "sourceSha256", "sourceSizeBytes",
        }, pointer, issues)
        if record is None:
            return issues
        if record["projectRelativePath"] != relative:
            return [_issue("INVALID_FILE", pointer + "/projectRelativePath",
                           f"expected {relative}")]
        if record["containerPath"] not in container_paths:
            return [_issue("INVALID_FILE", pointer + "/containerPath",
                           "container is not listed")]
        if record["containerMemberPath"] != member or member not in members:
            return [_issue("INVALID_FILE", pointer + "/containerMemberPath",
                           "required UFS member is not listed")]
        _validate_file_evidence(
            record["extractedEvidence"], pointer + "/extractedEvidence",
            repo_root, issues)
        if issues:
            return issues
        if (type(record["sourceSha256"]) is not str or
                not _HASH.fullmatch(record["sourceSha256"]) or
                not _is_int(record["sourceSizeBytes"], 1)):
            return [_issue("INVALID_FILE", pointer + "/sourceSha256",
                           "invalid source evidence")]
        if (record["extractedEvidence"]["sha256"] != record["sourceSha256"] or
                record["extractedEvidence"]["sizeBytes"] != record["sourceSizeBytes"]):
            return [_issue("INVALID_HASH", pointer + "/extractedEvidence",
                           "extracted/source identity differs")]
    return []


def _load_inventory_evidence(
    evidence: Any,
    pointer: str,
    expected_type: str,
    expected_root: str,
    repo_root: Path,
    transaction_id: str,
    source_head: str,
    issues: list[ValidationIssue],
) -> tuple[dict[str, Any] | None, Path | None]:
    path = _validate_file_evidence(evidence, pointer, repo_root, issues)
    if issues or path is None:
        return None, None
    report_prefix = _transaction_relative_prefix(transaction_id, "reports")
    if not _require_file_below(evidence, pointer, report_prefix, issues):
        return None, None
    try:
        value, _ = strict_json_load(path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        _first(issues, "INVALID_REPORT", pointer + "/path", str(exc))
        return None, None
    nested = validate_inventory_report(value, repo_root)
    if nested:
        item = nested[0]
        _first(issues, item["code"], pointer + item["field"], item["detail"])
        return None, None
    if (value["reportType"] != expected_type or
            value["transactionId"] != transaction_id or
            value["sourceHead"] != source_head):
        _first(issues, "INVALID_IDENTITY", pointer,
               "inventory type or identity differs")
        return None, None
    if value["root"] != expected_root:
        _first(issues, "INVALID_FILE", pointer + "/root",
               "inventory names the wrong transaction directory")
        return None, None
    return value, path


def validate_package_evidence(
    data: Any,
    pointer: str,
    repo_root: Path,
    transaction_id: str,
    source_head: str,
) -> list[ValidationIssue]:
    """Validate the immutable package wrapper and all duplicated identities."""
    issues: list[ValidationIssue] = []
    root = _require_keys(data, {
        "transactionId", "sourceHead", "targetReceipt", "stageManifest",
        "archiveManifest", "containers", "containerLists",
        "packagedExecutable", "runtimeFiles",
    }, pointer, issues)
    if root is None:
        return issues
    if not _validate_identity(root, issues, pointer):
        return issues
    if (root["transactionId"] != transaction_id or
            root["sourceHead"] != source_head):
        return [_issue("INVALID_IDENTITY", pointer,
                       "package identity differs from bundle")]
    report_prefix = _transaction_relative_prefix(transaction_id, "reports")
    package_prefix = _transaction_relative_prefix(transaction_id, "package")
    receipt_path = _validate_file_evidence(
        root["targetReceipt"], pointer + "/targetReceipt", repo_root, issues)
    if issues or receipt_path is None:
        return issues
    if not _require_file_below(
            root["targetReceipt"], pointer + "/targetReceipt",
            report_prefix + "/builds/CorsairsUE", issues):
        return issues
    if receipt_path.name != "CorsairsUE.target":
        return [_issue("INVALID_BUILD", pointer + "/targetReceipt/path",
                       "package receipt must be the Game target receipt")]
    stage_root = package_prefix + "/stage"
    archive_root = package_prefix + "/archive"
    stage, _ = _load_inventory_evidence(
        root["stageManifest"], pointer + "/stageManifest",
        "garner-terrain-stage-inventory", stage_root, repo_root,
        transaction_id, source_head, issues)
    if issues or stage is None:
        return issues
    archive, _ = _load_inventory_evidence(
        root["archiveManifest"], pointer + "/archiveManifest",
        "garner-terrain-archive-inventory", archive_root, repo_root,
        transaction_id, source_head, issues)
    if issues or archive is None:
        return issues
    archive_records = {item["path"]: item for item in archive["files"]}
    if type(root["containers"]) is not list or not root["containers"]:
        return [_issue("INVALID_SCHEMA", pointer + "/containers",
                       "expected nonempty container evidence")]
    container_paths: list[str] = []
    for index, item in enumerate(root["containers"]):
        item_pointer = f"{pointer}/containers/{index}"
        _validate_file_evidence(item, item_pointer, repo_root, issues)
        if issues:
            return issues
        if (not _require_file_below(item, item_pointer, archive_root, issues) or
                PurePosixPath(item["path"]).suffix.lower() != ".pak"):
            if not issues:
                _first(issues, "INVALID_FILE", item_pointer + "/path",
                       "container must be a transaction archive .pak")
            return issues
        if archive_records.get(item["path"]) != item:
            return [_issue("INVALID_FILE", item_pointer,
                           "container differs from archive inventory")]
        container_paths.append(item["path"])
    if container_paths != sorted(set(container_paths)):
        return [_issue("INVALID_SCHEMA", pointer + "/containers",
                       "containers must be sorted unique")]
    if type(root["containerLists"]) is not list or not root["containerLists"]:
        return [_issue("INVALID_SCHEMA", pointer + "/containerLists",
                       "expected nonempty container-list evidence")]
    parsed_lists: list[dict[str, Any]] = []
    list_paths: list[str] = []
    for index, item in enumerate(root["containerLists"]):
        item_pointer = f"{pointer}/containerLists/{index}"
        path = _validate_file_evidence(item, item_pointer, repo_root, issues)
        if issues or path is None:
            return issues
        if not _require_file_below(
                item, item_pointer, report_prefix + "/package", issues):
            return issues
        try:
            value, _ = strict_json_load(path)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            return [_issue("INVALID_REPORT", item_pointer + "/path", str(exc))]
        nested = validate_container_list_report(value, repo_root)
        if nested:
            problem = nested[0]
            return [_issue(problem["code"], item_pointer + problem["field"],
                           problem["detail"])]
        if (value["transactionId"] != transaction_id or
                value["sourceHead"] != source_head):
            return [_issue("INVALID_IDENTITY", item_pointer,
                           "container-list identity differs")]
        if any("corsairsimport" in member["path"].lower()
               for member in value["members"]):
            return [_issue("EDITOR_MODULE_LEAK", item_pointer,
                           "CorsairsImport leaked into container members")]
        parsed_lists.append(value)
        list_paths.append(item["path"])
    if list_paths != sorted(set(list_paths)):
        return [_issue("INVALID_SCHEMA", pointer + "/containerLists",
                       "container-list evidence must be sorted unique")]
    listed_containers = sorted(
        (item["container"] for item in parsed_lists),
        key=lambda item: item["path"])
    if listed_containers != root["containers"]:
        return [_issue("INVALID_SCHEMA", pointer + "/containerLists",
                       "every container requires exactly one matching list")]
    executable = _validate_file_evidence(
        root["packagedExecutable"], pointer + "/packagedExecutable",
        repo_root, issues)
    if issues or executable is None:
        return issues
    if not _require_file_below(
            root["packagedExecutable"], pointer + "/packagedExecutable",
            archive_root, issues):
        return issues
    if archive_records.get(root["packagedExecutable"]["path"]) != root[
            "packagedExecutable"]:
        return [_issue("INVALID_FILE", pointer + "/packagedExecutable",
                       "executable differs from archive inventory")]
    if type(root["runtimeFiles"]) is not list or len(root["runtimeFiles"]) != 3:
        return [_issue("INVALID_SCHEMA", pointer + "/runtimeFiles",
                       "expected exactly three packaged runtime records")]
    member_sets = {
        report["container"]["path"]: {member["path"] for member in report["members"]}
        for report in parsed_lists
    }
    for index, (item, relative, member) in enumerate(zip(
            root["runtimeFiles"], RUNTIME_INPUT_PATHS, RUNTIME_CONTAINER_PATHS)):
        item_pointer = f"{pointer}/runtimeFiles/{index}"
        record = _require_keys(item, {
            "projectRelativePath", "containerPath", "containerMemberPath",
            "extractedEvidence", "sourceSha256", "sourceSizeBytes",
            "runtimeReportedSha256", "runtimeReportedSizeBytes",
        }, item_pointer, issues)
        if record is None:
            return issues
        if record["projectRelativePath"] != relative:
            return [_issue("INVALID_FILE", item_pointer + "/projectRelativePath",
                           f"expected {relative}")]
        if (record["containerPath"] not in container_paths or
                record["containerMemberPath"] != member or
                member not in member_sets.get(record["containerPath"], set())):
            return [_issue("INVALID_FILE", item_pointer + "/containerMemberPath",
                           "runtime input is not in its listed container")]
        _validate_file_evidence(
            record["extractedEvidence"], item_pointer + "/extractedEvidence",
            repo_root, issues)
        if issues:
            return issues
        if not _require_file_below(
                record["extractedEvidence"], item_pointer + "/extractedEvidence",
                report_prefix + "/package/extracted", issues):
            return issues
        for key in ("sourceSha256", "runtimeReportedSha256"):
            if type(record[key]) is not str or not _HASH.fullmatch(record[key]):
                return [_issue("INVALID_HASH", item_pointer + f"/{key}",
                               "expected lowercase SHA-256")]
        for key in ("sourceSizeBytes", "runtimeReportedSizeBytes"):
            if not _is_int(record[key], 1):
                return [_issue("INVALID_FILE", item_pointer + f"/{key}",
                               "expected positive byte size")]
        evidence = record["extractedEvidence"]
        identities = {
            (evidence["sha256"], evidence["sizeBytes"]),
            (record["sourceSha256"], record["sourceSizeBytes"]),
            (record["runtimeReportedSha256"],
             record["runtimeReportedSizeBytes"]),
        }
        if len(identities) != 1:
            return [_issue("INVALID_HASH", item_pointer,
                           "source, extracted, and runtime identities differ")]
    return []


def validate_base_bundle(
    data: dict, bundle_path: Path, repo_root: Path,
) -> list[ValidationIssue]:
    """Strictly validate the terrain-base graph and rehash transitive inputs.

    Nested cook/container DTO validation is intentionally performed here, not
    delegated to a producer.  This keeps the published bundle as the sole
    downstream authority.
    """
    issues: list[ValidationIssue] = []
    expected = {
        "schemaVersion", "reportType", "status", "transactionId", "sourceHead",
        "terrainManifest", "runtimeInputs", "reports", "finalPackageHashes",
        "builds", "package", "issues",
    }
    root = _require_keys(data, expected, "", issues)
    if root is None:
        return issues
    for key, value in (
        ("schemaVersion", 1), ("reportType", "garner-terrain-base"),
        ("status", "PASS"),
    ):
        if not _check_exact(root[key], value, f"/{key}", issues):
            return issues
    if not _validate_identity(root, issues):
        return issues
    if not _validate_issue_list(root["status"], root["issues"], "/issues", issues):
        return issues
    root_path = Path(repo_root).resolve()
    expected_bundle = root_path / "artifacts/maps/reports/garner-terrain-base.json"
    candidate = Path(bundle_path).resolve(strict=False)
    if candidate != expected_bundle and candidate.parent != expected_bundle.parent:
        return [_issue("INVALID_FILE", "/", "bundle path is outside reports root")]
    terrain = _require_keys(
        root["terrainManifest"], {"top", "runId", "runFiles"},
        "/terrainManifest", issues)
    if terrain is None:
        return issues
    top_path = _validate_file_evidence(
        terrain["top"], "/terrainManifest/top", root_path, issues)
    if issues or top_path is None:
        return issues
    if terrain["top"]["path"] != "artifacts/maps/garner.reference-albedo.json":
        return [_issue("INVALID_MANIFEST", "/terrainManifest/top/path",
                       "expected the fixed Task 7 top manifest")]
    try:
        manifest, _ = strict_json_load(top_path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        return [_issue("INVALID_MANIFEST", "/terrainManifest/top/path", str(exc))]
    nested = validate_manifest(manifest, top_path)
    if nested:
        return nested
    manifest_run = PurePosixPath(manifest["files"]["height"]["path"]).parts[1]
    if terrain["runId"] != manifest_run:
        return [_issue("INVALID_IDENTITY", "/terrainManifest/runId",
                       "run ID differs from top manifest")]
    if type(terrain["runFiles"]) is not list or len(terrain["runFiles"]) != 7:
        return [_issue("INVALID_SCHEMA", "/terrainManifest/runFiles",
                       "expected seven run files")]
    for index, (key, _) in enumerate(_MANIFEST_LEAVES.items()):
        _validate_file_evidence(
            terrain["runFiles"][index], f"/terrainManifest/runFiles/{index}",
            root_path, issues)
        if issues:
            return issues
        expected_record = {
            "path": (PurePosixPath("artifacts/maps") /
                     PurePosixPath(manifest["files"][key]["path"])).as_posix(),
            "sha256": manifest["files"][key]["sha256"],
            "sizeBytes": manifest["files"][key]["sizeBytes"],
        }
        if terrain["runFiles"][index] != expected_record:
            return [_issue("INVALID_MANIFEST", f"/terrainManifest/runFiles/{index}",
                           "run evidence differs from manifest")]
    if type(root["runtimeInputs"]) is not list or len(root["runtimeInputs"]) != 3:
        return [_issue("INVALID_SCHEMA", "/runtimeInputs",
                       "expected three runtime inputs")]
    for index, relative in enumerate(RUNTIME_INPUT_PATHS):
        _validate_file_evidence(
            root["runtimeInputs"][index], f"/runtimeInputs/{index}", root_path, issues)
        if issues:
            return issues
        if root["runtimeInputs"][index]["path"] != f"CorsairsUE/{relative}":
            return [_issue("INVALID_FILE", f"/runtimeInputs/{index}/path",
                           "noncanonical runtime input path")]
    _validate_family_list(
        root["finalPackageHashes"], "/finalPackageHashes", root_path, issues,
        rehash=True)
    if issues:
        return issues
    reports = _require_keys(root["reports"], {
        "levelBuild", "importPass1", "importPass2", "terrainCheck",
        "editorAutomation", "cookPackage", "runtimeAutomation",
        "runtimeObservation",
    }, "/reports", issues)
    if reports is None:
        return issues
    transaction_id = root["transactionId"]
    source_head = root["sourceHead"]
    report_prefix = _transaction_relative_prefix(transaction_id, "reports")
    parsed_reports: dict[str, dict[str, Any]] = {}
    report_specs = {
        "levelBuild": validate_level_build_report,
        "importPass1": validate_import_report,
        "importPass2": validate_import_report,
        "terrainCheck": validate_check_report,
        "cookPackage": validate_cook_package_report,
        "runtimeObservation": validate_runtime_observation,
    }
    for key, validator in report_specs.items():
        pointer = f"/reports/{key}"
        path = _validate_file_evidence(reports[key], pointer, root_path, issues)
        if issues or path is None:
            return issues
        if not _require_file_below(reports[key], pointer, report_prefix, issues):
            return issues
        try:
            nested_data, _ = strict_json_load(path)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            return [_issue("INVALID_REPORT", pointer + "/path", str(exc))]
        nested_issues = (validator(nested_data) if key == "runtimeObservation"
                         else validator(nested_data, root_path))
        if nested_issues:
            item = nested_issues[0]
            return [_issue(item["code"], pointer + item["field"], item["detail"])]
        if (nested_data["transactionId"] != transaction_id or
                nested_data["sourceHead"] != source_head):
            return [_issue("INVALID_IDENTITY", pointer,
                           "nested report identity differs from bundle")]
        parsed_reports[key] = nested_data
    for key in ("editorAutomation", "runtimeAutomation"):
        nested_issues = validate_evidence_set(
            reports[key], f"/reports/{key}", root_path,
            transaction_id, source_head)
        if nested_issues:
            return nested_issues

    level = parsed_reports["levelBuild"]
    first = parsed_reports["importPass1"]
    second = parsed_reports["importPass2"]
    check = parsed_reports["terrainCheck"]
    cook = parsed_reports["cookPackage"]
    runtime = parsed_reports["runtimeObservation"]
    for key, report in (
        ("levelBuild", level), ("importPass1", first),
        ("importPass2", second), ("terrainCheck", check),
    ):
        if report["manifest"] != terrain["top"]:
            return [_issue("INVALID_MANIFEST", f"/reports/{key}",
                           "report manifest evidence differs from bundle")]
    if level["mapPackageHash"] != first["beforeMapPackageHash"]:
        return [_issue("INVALID_PACKAGE", "/reports/importPass1",
                       "pass 1 did not start from the builder map family")]
    relation = validate_idempotent_import_reports(first, second)
    if relation:
        item = relation[0]
        return [_issue(item["code"], "/reports/importPass2" + item["field"],
                       item["detail"])]
    if (check["levelBuildReport"] != reports["levelBuild"] or
            check["importPass1Report"] != reports["importPass1"] or
            check["importPass2Report"] != reports["importPass2"]):
        return [_issue("INVALID_REPORT", "/reports/terrainCheck",
                       "checker report links differ from bundle")]
    if (check["finalPackageHashes"] != second["finalPackageHashes"] or
            root["finalPackageHashes"] != check["finalPackageHashes"]):
        return [_issue("INVALID_PACKAGE", "/finalPackageHashes",
                       "final physical families differ across reports")]

    builds = _require_keys(root["builds"], {"editor", "game"}, "/builds", issues)
    if builds is None:
        return issues
    for key, target in (("editor", "CorsairsUEEditor"), ("game", "CorsairsUE")):
        nested_issues = validate_build_evidence(
            builds[key], f"/builds/{key}", root_path,
            transaction_id, source_head, target)
        if nested_issues:
            return nested_issues

    package_issues = validate_package_evidence(
        root["package"], "/package", root_path, transaction_id, source_head)
    if package_issues:
        return package_issues
    package = root["package"]
    if (builds["game"]["receipt"] != cook["targetReceipt"] or
            builds["game"]["receipt"] != package["targetReceipt"]):
        return [_issue("INVALID_BUILD", "/package/targetReceipt",
                       "Game build/package receipts are not the same evidence")]
    for key in (
        "targetReceipt", "stageManifest", "archiveManifest", "containers",
        "containerLists", "packagedExecutable",
    ):
        if package[key] != cook[key]:
            return [_issue("INVALID_PACKAGE", f"/package/{key}",
                           "package wrapper differs from cook report")]
    for index, (packaged, cooked) in enumerate(zip(
            package["runtimeFiles"], cook["runtimeFiles"])):
        cook_projection = {
            key: value for key, value in packaged.items()
            if key not in ("runtimeReportedSha256", "runtimeReportedSizeBytes")
        }
        if cook_projection != cooked:
            return [_issue("INVALID_PACKAGE", f"/package/runtimeFiles/{index}",
                           "package wrapper differs from cook report")]
        source = root["runtimeInputs"][index]
        observed = runtime["runtimeInputs"][index]
        if (packaged["projectRelativePath"] != RUNTIME_INPUT_PATHS[index] or
                source["sha256"] != packaged["sourceSha256"] or
                source["sizeBytes"] != packaged["sourceSizeBytes"] or
                observed["projectRelativePath"] != RUNTIME_INPUT_PATHS[index] or
                observed["sha256"] != packaged["runtimeReportedSha256"] or
                observed["sizeBytes"] != packaged["runtimeReportedSizeBytes"]):
            return [_issue("INVALID_HASH", f"/package/runtimeFiles/{index}",
                           "source, package, and runtime identities differ")]
    return []
