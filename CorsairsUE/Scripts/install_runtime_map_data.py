#!/usr/bin/env python3
"""Durably install Garner block/region/metadata while preserving height."""

from __future__ import annotations

import copy
from dataclasses import dataclass
import errno
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import stat
import sys
import time
from typing import Any, Callable, Iterable

if os.name != "nt":
    import fcntl


_LEAVES = {
    "height": "garner.height.r16",
    "block": "garner.block.raw",
    "region": "garner.region.raw",
    "terrainMetadata": "garner.terrain.json",
    "albedo": "garner.albedo_17_21.png",
    "meshGltf": "garner.terrain_17_21.gltf",
    "meshBin": "garner.terrain_17_21.bin",
}
_TOP_KEYS = {
    "schemaVersion", "algorithmVersion", "source", "page",
    "requiredPresentRect", "usedTextureIds", "sectionPresence", "files",
    "metrics",
}
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_MAX_RSS = 128 * 1024 * 1024
_MAX_TEXTURE_CACHE = 32 * 1024 * 1024
_MAX_RGBA_ROW = 16 * 1024
_MAX_PNG = 96 * 1024 * 1024
_MAX_HEIGHT_ERROR_CM = 5.0
_MAX_RMS_ERROR_CM = 2.0
_MAX_SHARED_BOUNDARY_CM = 0.0
_JOURNAL_NAME = ".garner-runtime-install.transaction.json"
_RETIRED_JOURNAL_NAME = _JOURNAL_NAME + ".retired"
_JOURNAL_AUTH_NAME = _JOURNAL_NAME + ".retired-auth"
_LOCK_NAME = ".garner-runtime-install.lock"
_RETIRED_LOCK_NAME = _LOCK_NAME + ".retired"
_RUNTIME_CONTRACT_NAME = "garner.runtime.json"
_INSTALL_KEYS = ("block", "region", "metadata")
_INSTALL_LEAVES = {
    "block": "garner.block.raw",
    "region": "garner.region.raw",
    "metadata": "garner.terrain.json",
}
_MANIFEST_FILE_KEYS = {
    "block": "block",
    "region": "region",
    "metadata": "terrainMetadata",
}


class InstallError(RuntimeError):
    def __init__(
        self,
        detail: str,
        *,
        status: str = "WRITE_FAILED",
        recovery_paths: Iterable[Path] = (),
        recovery_command: str = "",
    ) -> None:
        super().__init__(detail)
        self.detail = detail
        self.status = status
        self.recovery_paths = tuple(Path(path) for path in recovery_paths)
        self.recovery_command = recovery_command

    def __str__(self) -> str:
        suffix = ""
        if self.recovery_paths:
            suffix += " paths=" + ",".join(
                json.dumps(path.as_posix()) for path in self.recovery_paths)
        if self.recovery_command:
            suffix += " command=" + self.recovery_command
        return f"{self.status} {self.detail}{suffix}"


class DurableFsError(RuntimeError):
    pass


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member {key}")
        result[key] = value
    return result


def _load_strict_json(path: Path) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON number {token}")),
        )
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError,
            RecursionError) as exc:
        raise InstallError(f"invalid manifest JSON: {exc}") from exc
    if type(value) is not dict:
        raise InstallError("invalid manifest JSON: root must be an object")
    _validate_unicode_scalars(value, "")
    return value, raw


def _validate_unicode_scalars(
    value: Any,
    pointer: str,
    *,
    status: str = "WRITE_FAILED",
    label: str = "manifest JSON",
) -> None:
    def reject_surrogate(text: str, location: str) -> None:
        if any(0xD800 <= ord(character) <= 0xDFFF for character in text):
            raise InstallError(
                f"invalid {label}: non-Unicode scalar at " +
                (location or "/"), status=status)

    stack: list[tuple[Any, str, int]] = [(value, pointer, 0)]
    while stack:
        current, location, depth = stack.pop()
        if depth > 128:
            raise InstallError(
                f"invalid {label}: nesting exceeds 128 at " +
                (location or "/"), status=status)
        if type(current) is str:
            reject_surrogate(current, location)
        elif type(current) is list:
            stack.extend(
                (item, f"{location}/{index}", depth + 1)
                for index, item in enumerate(current))
        elif type(current) is dict:
            for key, item in current.items():
                escaped = key.replace("~", "~0").replace("/", "~1")
                child = f"{location}/{escaped}"
                reject_surrogate(key, child)
                stack.append((item, child, depth + 1))


def _require_keys(value: Any, expected: set[str], pointer: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected object")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        if missing:
            raise InstallError(
                f"INVALID_SCHEMA {pointer}/{missing[0]}: required field is missing")
        raise InstallError(f"INVALID_SCHEMA {pointer}/{unknown[0]}: unknown field")
    return value


def _integer(value: Any, pointer: str, maximum: int = (1 << 64) - 1) -> int:
    if type(value) is not int or value < 0 or value > maximum:
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected bounded unsigned integer")
    return value


def _number(value: Any, pointer: str) -> float:
    if type(value) not in (int, float):
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected finite number")
    try:
        converted = float(value)
    except (OverflowError, ValueError) as exc:
        raise InstallError(
            f"INVALID_SCHEMA {pointer}: expected finite number") from exc
    if not math.isfinite(converted):
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected finite number")
    return converted


def _string(value: Any, pointer: str) -> str:
    if type(value) is not str:
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected string")
    return value


def _hash(value: Any, pointer: str) -> str:
    text = _string(value, pointer)
    if not _SHA256.fullmatch(text):
        raise InstallError(f"INVALID_HASH {pointer}: expected lowercase SHA-256")
    return text


def _validate_runtime_contract(target_root: Path) -> dict[str, Any]:
    contract_path = target_root / _RUNTIME_CONTRACT_NAME
    _physical_regular(contract_path, "/runtimeContract")
    contract, _ = _load_strict_json(contract_path)
    _require_keys(
        contract,
        {"schemaVersion", "map", "gridWidth", "gridHeight", "files"},
        "/runtimeContract",
    )
    if (_integer(contract["schemaVersion"], "/runtimeContract/schemaVersion") != 1 or
            _string(contract["map"], "/runtimeContract/map") != "garner" or
            _integer(contract["gridWidth"], "/runtimeContract/gridWidth") != 4096 or
            _integer(contract["gridHeight"], "/runtimeContract/gridHeight") != 4096):
        raise InstallError("invalid runtime contract header")
    files = _require_keys(
        contract["files"],
        {"height", "block", "region", "terrainMetadata"},
        "/runtimeContract/files",
    )
    expected_names = {
        "height": "garner.height.r16",
        "block": "garner.block.raw",
        "region": "garner.region.raw",
        "terrainMetadata": "garner.terrain.json",
    }
    for key, expected_name in expected_names.items():
        item = _require_keys(
            files[key], {"name", "sha256", "sizeBytes"},
            f"/runtimeContract/files/{key}")
        if _string(item["name"], f"/runtimeContract/files/{key}/name") != expected_name:
            raise InstallError(f"invalid runtime contract file name for {key}")
        _hash(item["sha256"], f"/runtimeContract/files/{key}/sha256")
        if _integer(item["sizeBytes"], f"/runtimeContract/files/{key}/sizeBytes") == 0:
            raise InstallError(f"invalid runtime contract file size for {key}")
    height = target_root / expected_names["height"]
    info = _physical_regular(height, "/runtimeContract/files/height")
    expected_height = files["height"]
    if (info.st_size != expected_height["sizeBytes"] or
            _sha256_file(height) != expected_height["sha256"]):
        raise InstallError("runtime height does not match preserved contract")
    return contract


def _normalized_relative(text: Any, pointer: str) -> PurePosixPath:
    value = _string(text, pointer)
    if not value or "\\" in value:
        raise InstallError(f"INVALID_FILE {pointer}: invalid normalized path")
    path = PurePosixPath(value)
    if path.is_absolute() or value != path.as_posix() or any(
        part in ("", ".", "..") for part in path.parts
    ):
        raise InstallError(f"INVALID_FILE {pointer}: invalid normalized path")
    return path


def _contained(root: Path, candidate: Path) -> bool:
    try:
        candidate.relative_to(root)
        return candidate != root
    except ValueError:
        return False


def _physical_regular(path: Path, pointer: str) -> os.stat_result:
    try:
        info = path.lstat()
    except OSError as exc:
        raise InstallError(f"INVALID_FILE {pointer}: {exc}") from exc
    if not stat.S_ISREG(info.st_mode) or path.is_symlink():
        raise InstallError(f"INVALID_FILE {pointer}: expected non-symlink regular file")
    return info


def _physical_identity(path: Path) -> tuple[int, int, int]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise InstallError(f"physical identity unavailable: {path}: {exc}") from exc
    if (not stat.S_ISREG(info.st_mode) or path.is_symlink() or
            info.st_nlink != 1):
        raise InstallError(
            f"physical identity is not a no-link regular file: {path}")
    return int(info.st_dev), int(info.st_ino), int(info.st_nlink)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class ValidatedManifest:
    data: dict[str, Any]
    paths: dict[str, Path]
    identities: dict[str, tuple[int, int, int]]
    run_id: str
    raw: bytes


@dataclass(frozen=True)
class DurableEntryToFinalize:
    source: Path
    reservation: Path
    kind: str
    transaction_id: str
    source_hash: str
    source_identity: tuple[int, int, int]
    source_mode: int


def _finalize_windows_entries(
    durable_fs: Any,
    entries: Iterable[DurableEntryToFinalize],
) -> None:
    entries = tuple(entries)
    exists_hook = getattr(durable_fs, "entry_exists", None)
    bytes_hook = getattr(durable_fs, "entry_bytes", None)
    hash_hook = getattr(durable_fs, "entry_hash", None)
    identity_hook = getattr(durable_fs, "entry_identity", None)
    mode_hook = getattr(durable_fs, "entry_mode", None)
    reservation_builder = getattr(
        durable_fs, "_reservation_bytes",
        getattr(durable_fs, "reservation_bytes", None))
    if reservation_builder is None:
        raise DurableFsError("typed Windows finalization lacks reservation builder")

    def present(path: Path) -> bool:
        return (bool(exists_hook(path)) if exists_hook is not None
                else path.exists() or path.is_symlink())

    def payload(path: Path) -> bytes:
        return (bytes(bytes_hook(path)) if bytes_hook is not None
                else path.read_bytes())

    def digest(path: Path) -> str:
        return (str(hash_hook(path)) if hash_hook is not None
                else _sha256_file(path))

    def identity(path: Path) -> tuple[int, int, int]:
        return (tuple(identity_hook(path)) if identity_hook is not None
                else _physical_identity(path))

    def mode(path: Path) -> int:
        return (int(mode_hook(path)) if mode_hook is not None
                else _observable_mode(path.lstat()))

    def recorded(path: Path, entry: DurableEntryToFinalize) -> bool:
        return (identity(path) == entry.source_identity and
                mode(path) == entry.source_mode and
                digest(path) == entry.source_hash)

    for entry in entries:
        source_present = present(entry.source)
        reservation_present = present(entry.reservation)
        expected_reservation = reservation_builder(
            entry.source, entry.reservation, entry.kind,
            entry.transaction_id, entry.source_hash)
        if source_present and not recorded(entry.source, entry):
            raise DurableFsError(
                f"typed finalization source identity/mode changed: {entry.source}")
        if source_present and reservation_present:
            if payload(entry.reservation) != expected_reservation:
                raise DurableFsError(
                    f"invalid typed finalization reservation: {entry.reservation}")
            durable_fs.remove_owned(
                entry.reservation,
                source_hash=digest(entry.reservation),
                expected_mode=mode(entry.reservation),
                expected_identity=identity(entry.reservation))
            reservation_present = False
        elif not source_present and reservation_present:
            if not recorded(entry.reservation, entry):
                raise DurableFsError(
                    "typed finalization reservation is neither record nor payload: "
                    f"{entry.reservation}")
            durable_fs.replace_same_volume(entry.reservation, entry.source)
            if not recorded(entry.source, entry):
                raise DurableFsError(
                    f"restored Windows entry changed: {entry.source}")
            source_present = True
            reservation_present = False
        if not source_present or reservation_present:
            raise DurableFsError(
                f"invalid typed finalization state: {entry.source}")

    # The contract flushes every normalized entry before any new reservation
    # or there-and-back write-through move is begun.
    for entry in entries:
        durable_fs.flush_file(entry.source)

    for entry in entries:
        durable_fs.reserve_move_target(
            entry.source, entry.reservation, entry.kind,
            entry.transaction_id, entry.source_hash)
        durable_fs.replace_same_volume(entry.source, entry.reservation)
        if not recorded(entry.reservation, entry):
            raise DurableFsError(
                f"moved Windows entry changed: {entry.reservation}")
        durable_fs.replace_same_volume(entry.reservation, entry.source)
        if not recorded(entry.source, entry):
            raise DurableFsError(
                f"restored Windows entry changed: {entry.source}")
        verify = getattr(durable_fs, "verify_finalized_entry", None)
        verified = (verify(
                        entry.source, entry.source_hash,
                        entry.source_identity, entry.source_mode)
                    if verify is not None
                    else recorded(entry.source, entry))
        if not verified:
            raise DurableFsError(
                f"finalized Windows entry changed: {entry.source}")


def _validate_rect(value: Any, pointer: str, expected: tuple[int, int, int, int]) -> None:
    rect = _require_keys(value, {"x", "y", "width", "height"}, pointer)
    actual = tuple(_integer(rect[key], f"{pointer}/{key}", (1 << 32) - 1)
                   for key in ("x", "y", "width", "height"))
    if actual != expected:
        raise InstallError(f"INVALID_BOUNDS {pointer}: unexpected rectangle")


def validate_manifest(manifest_path: Path | str) -> ValidatedManifest:
    manifest_path = Path(manifest_path)
    _physical_regular(manifest_path, "/manifest")
    data, raw = _load_strict_json(manifest_path)
    _require_keys(data, _TOP_KEYS, "")
    if _integer(data["schemaVersion"], "/schemaVersion", (1 << 32) - 1) != 1:
        raise InstallError("INVALID_SCHEMA /schemaVersion: must be 1")
    if _string(data["algorithmVersion"], "/algorithmVersion") != (
        "legacy-fixed-pipeline-v1"
    ):
        raise InstallError("INVALID_ALGORITHM /algorithmVersion")

    page = _require_keys(
        data["page"],
        {"x", "y", "sourceCellBounds", "pixelsPerCell", "pixelWidth",
         "pixelHeight", "ambient", "dwTColor"},
        "/page",
    )
    if (_integer(page["x"], "/page/x") != 17 or
            _integer(page["y"], "/page/y") != 21 or
            _integer(page["pixelsPerCell"], "/page/pixelsPerCell") != 32 or
            _integer(page["pixelWidth"], "/page/pixelWidth") != 4096 or
            _integer(page["pixelHeight"], "/page/pixelHeight") != 4096 or
            _integer(page["dwTColor"], "/page/dwTColor") != 0):
        raise InstallError("INVALID_BOUNDS /page: unexpected fixed page values")
    _validate_rect(page["sourceCellBounds"], "/page/sourceCellBounds",
                   (2176, 2688, 128, 128))
    if type(page["ambient"]) is not list or len(page["ambient"]) != 3 or any(
        _number(item, f"/page/ambient/{index}") != 1.0
        for index, item in enumerate(page["ambient"])
    ):
        raise InstallError("INVALID_BOUNDS /page/ambient")
    _validate_rect(data["requiredPresentRect"], "/requiredPresentRect",
                   (2193, 2756, 80, 47))

    ids = data["usedTextureIds"]
    if type(ids) is not list:
        raise InstallError("INVALID_SCHEMA /usedTextureIds: expected array")
    ids = [_integer(value, f"/usedTextureIds/{index}", 255)
           for index, value in enumerate(ids)]
    if not ids or ids != sorted(set(ids)) or 0 in ids:
        raise InstallError("INVALID_TEXTURE_IDS /usedTextureIds")

    presence = _require_keys(
        data["sectionPresence"],
        {"originX", "originY", "width", "height", "rowMajorMask"},
        "/sectionPresence",
    )
    mask = presence["rowMajorMask"]
    if (tuple(_integer(presence[key], f"/sectionPresence/{key}")
              for key in ("originX", "originY", "width", "height")) !=
            (272, 336, 16, 16) or type(mask) is not list or len(mask) != 256 or
            any(type(value) is not int or value != 1 for value in mask)):
        raise InstallError("INVALID_SECTION_MASK /sectionPresence")

    files = _require_keys(data["files"], set(_LEAVES), "/files")
    manifest_root = manifest_path.parent.resolve(strict=True)
    run_id: str | None = None
    resolved: dict[str, Path] = {}
    resolved_identities: dict[str, tuple[int, int, int]] = {}
    aliases: set[str] = set()
    physical_identities: set[tuple[int, int, int]] = set()
    total = 0
    for key, leaf in _LEAVES.items():
        item = _require_keys(files[key], {"path", "sha256", "sizeBytes"},
                             f"/files/{key}")
        relative = _normalized_relative(item["path"], f"/files/{key}/path")
        if (len(relative.parts) != 3 or relative.parts[0] != "runs" or
                not relative.parts[1] or relative.parts[2] != leaf):
            raise InstallError(f"INVALID_FILE /files/{key}/path: invalid one-run path")
        if run_id is None:
            run_id = relative.parts[1]
        elif run_id != relative.parts[1]:
            raise InstallError(f"INVALID_FILE /files/{key}/path: mixed run IDs")
        alias = relative.as_posix().lower()
        if alias in aliases:
            raise InstallError(f"INVALID_FILE /files/{key}/path: duplicate path")
        aliases.add(alias)
        expected_hash = _hash(item["sha256"], f"/files/{key}/sha256")
        expected_size = _integer(item["sizeBytes"], f"/files/{key}/sizeBytes")
        if expected_size == 0:
            raise InstallError(f"INVALID_FILE /files/{key}/sizeBytes")
        candidate = manifest_root.joinpath(*relative.parts)
        info = _physical_regular(candidate, f"/files/{key}/path")
        try:
            identity = _physical_identity(candidate)
        except InstallError as exc:
            raise InstallError(
                f"INVALID_FILE /files/{key}/path: "
                "outputs must have seven distinct physical identities") from exc
        if identity in physical_identities:
            raise InstallError(
                f"INVALID_FILE /files/{key}/path: "
                "outputs must have seven distinct physical identities")
        physical_identities.add(identity)
        resolved_identities[key] = identity
        canonical = candidate.resolve(strict=True)
        if not _contained(manifest_root, canonical):
            raise InstallError(f"INVALID_FILE /files/{key}/path: escape")
        if info.st_size != expected_size:
            raise InstallError(f"INVALID_FILE /files/{key}/sizeBytes: disk mismatch")
        if _sha256_file(candidate) != expected_hash:
            raise InstallError(f"INVALID_HASH /files/{key}/sha256: disk mismatch")
        resolved[key] = candidate
        total += expected_size
    assert run_id is not None
    run_directory = resolved["height"].parent
    if len(list(run_directory.iterdir())) != 7:
        raise InstallError("INVALID_FILE /files: run must contain exactly seven products")

    source = _require_keys(
        data["source"],
        {"map", "database", "clientRoot", "alphaAtlas", "usedTextures"},
        "/source",
    )
    repo_root = Path.cwd().resolve(strict=True)
    source_aliases: set[str] = set()
    for key in ("map", "database", "alphaAtlas"):
        item = _require_keys(source[key], {"path", "sha256"}, f"/source/{key}")
        relative = _normalized_relative(item["path"], f"/source/{key}/path")
        alias = relative.as_posix().lower()
        if alias in source_aliases:
            raise InstallError(f"INVALID_PROVENANCE /source/{key}/path: duplicate")
        source_aliases.add(alias)
        expected_hash = _hash(item["sha256"], f"/source/{key}/sha256")
        candidate = repo_root.joinpath(*relative.parts)
        _physical_regular(candidate, f"/source/{key}/path")
        canonical = candidate.resolve(strict=True)
        if not _contained(repo_root, canonical) or _sha256_file(candidate) != expected_hash:
            raise InstallError(f"INVALID_PROVENANCE /source/{key}: disk mismatch")
    client_relative = _normalized_relative(source["clientRoot"], "/source/clientRoot")
    client = repo_root.joinpath(*client_relative.parts)
    try:
        client_canonical = client.resolve(strict=True)
    except OSError as exc:
        raise InstallError(f"INVALID_PROVENANCE /source/clientRoot: {exc}") from exc
    if not client_canonical.is_dir() or not _contained(repo_root, client_canonical):
        raise InstallError("INVALID_PROVENANCE /source/clientRoot")
    source_aliases.add(client_relative.as_posix().lower())
    textures = source["usedTextures"]
    if type(textures) is not list or len(textures) != len(ids):
        raise InstallError("INVALID_TEXTURE_IDS /source/usedTextures")
    for index, item_value in enumerate(textures):
        item = _require_keys(item_value, {"textureId", "path", "sha256"},
                             f"/source/usedTextures/{index}")
        texture_id = _integer(item["textureId"],
                              f"/source/usedTextures/{index}/textureId", 255)
        if texture_id != ids[index]:
            raise InstallError("INVALID_TEXTURE_IDS /source/usedTextures")
        relative = _normalized_relative(item["path"],
                                        f"/source/usedTextures/{index}/path")
        if (len(relative.parts) <= len(client_relative.parts) or
                relative.parts[:len(client_relative.parts)] !=
                client_relative.parts):
            raise InstallError(
                "INVALID_PROVENANCE "
                f"/source/usedTextures/{index}/path: "
                "texture path is not lexically beneath client root")
        alias = relative.as_posix().lower()
        if alias in source_aliases:
            raise InstallError(
                "INVALID_PROVENANCE "
                f"/source/usedTextures/{index}/path: "
                "duplicate or case-alias source path")
        source_aliases.add(alias)
        expected_hash = _hash(item["sha256"],
                              f"/source/usedTextures/{index}/sha256")
        candidate = repo_root.joinpath(*relative.parts)
        _physical_regular(candidate, f"/source/usedTextures/{index}/path")
        canonical = candidate.resolve(strict=True)
        if not _contained(client_canonical, canonical) or _sha256_file(candidate) != expected_hash:
            raise InstallError(f"INVALID_PROVENANCE /source/usedTextures/{index}")

    metrics = _require_keys(
        data["metrics"],
        {"peakRssBytes", "peakTextureCacheBytes", "peakRgbaRowBytes",
         "pngBytes", "totalOutputBytes", "maxHeightErrorCm",
         "rmsHeightErrorCm", "sharedBoundaryMaxCm", "absentSectionCount",
         "unresolvedLayerCount"},
        "/metrics",
    )
    rss = _integer(metrics["peakRssBytes"], "/metrics/peakRssBytes")
    if not 0 < rss <= _MAX_RSS:
        raise InstallError("BUDGET_EXCEEDED /metrics/peakRssBytes")
    for key in ("peakTextureCacheBytes", "peakRgbaRowBytes", "pngBytes",
                "totalOutputBytes", "absentSectionCount", "unresolvedLayerCount"):
        _integer(metrics[key], f"/metrics/{key}")
    integer_budgets = {
        "peakTextureCacheBytes": _MAX_TEXTURE_CACHE,
        "peakRgbaRowBytes": _MAX_RGBA_ROW,
        "pngBytes": _MAX_PNG,
    }
    for key, maximum in integer_budgets.items():
        if metrics[key] > maximum:
            raise InstallError(f"BUDGET_EXCEEDED /metrics/{key}")
    for key in ("maxHeightErrorCm", "rmsHeightErrorCm", "sharedBoundaryMaxCm"):
        if _number(metrics[key], f"/metrics/{key}") < 0:
            raise InstallError(f"INVALID_METRIC /metrics/{key}")
    geometry_budgets = {
        "maxHeightErrorCm": _MAX_HEIGHT_ERROR_CM,
        "rmsHeightErrorCm": _MAX_RMS_ERROR_CM,
        "sharedBoundaryMaxCm": _MAX_SHARED_BOUNDARY_CM,
    }
    for key, maximum in geometry_budgets.items():
        if float(metrics[key]) > maximum:
            raise InstallError(f"BUDGET_EXCEEDED /metrics/{key}")
    if metrics["totalOutputBytes"] != total or metrics["pngBytes"] != files["albedo"]["sizeBytes"]:
        raise InstallError("INVALID_METRIC /metrics/totalOutputBytes")
    if metrics["absentSectionCount"] != 0 or metrics["unresolvedLayerCount"] != 0:
        raise InstallError("INVALID_METRIC /metrics")
    return ValidatedManifest(data, resolved, resolved_identities, run_id, raw)


def _json_path(path: Path) -> str:
    return json.dumps(path.as_posix(), ensure_ascii=False, separators=(",", ":"))


class PosixDurableFs:
    def _error(self, operation: str, path: Path, native: int) -> DurableFsError:
        return DurableFsError(
            f"DURABLE_FS_ERROR op={operation} path={_json_path(path)} "
            f"native=errno:{native}:")

    def open_exclusive_temp(self, path: Path) -> int:
        flags = os.O_CREAT | os.O_EXCL | os.O_RDWR
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                os.close(descriptor)
                raise OSError(errno.EMLINK, "exclusive temp is not no-link regular")
            return descriptor
        except OSError as exc:
            raise self._error("OpenExclusiveTemp", path, exc.errno or errno.EIO) from exc

    def write_exclusive_temp(
        self, descriptor: int, path: Path, payload: bytes, mode: int
    ) -> None:
        try:
            opened = os.fstat(descriptor)
            os.ftruncate(descriptor, 0)
            os.lseek(descriptor, 0, os.SEEK_SET)
            offset = 0
            while offset < len(payload):
                written = os.write(descriptor, payload[offset:])
                if written <= 0:
                    raise OSError(errno.EIO, "short exclusive-temp write")
                offset += written
            os.fchmod(descriptor, mode)
            os.fsync(descriptor)
            finished = os.fstat(descriptor)
            path_info = path.lstat()
            if (not stat.S_ISREG(finished.st_mode) or finished.st_nlink != 1 or
                    not stat.S_ISREG(path_info.st_mode) or
                    path.is_symlink() or path_info.st_nlink != 1 or
                    (opened.st_dev, opened.st_ino) !=
                    (finished.st_dev, finished.st_ino) or
                    (finished.st_dev, finished.st_ino) !=
                    (path_info.st_dev, path_info.st_ino)):
                raise OSError(errno.ESTALE, "exclusive temp identity changed")
        except OSError as exc:
            raise self._error(
                "OpenExclusiveTemp", path, exc.errno or errno.EIO) from exc
        finally:
            os.close(descriptor)

    def flush_file(self, path: Path) -> None:
        flags = os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags)
            try:
                if not stat.S_ISREG(os.fstat(descriptor).st_mode):
                    raise OSError(errno.EINVAL, "not a regular file")
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError as exc:
            raise self._error("FlushFile", path, exc.errno or errno.EIO) from exc

    def replace_same_volume(self, source: Path, destination: Path) -> None:
        try:
            source_info = source.lstat()
            parent_info = destination.parent.stat()
            if source_info.st_dev != parent_info.st_dev:
                raise DurableFsError(
                    "DURABLE_FS_ERROR op=ReplaceSameVolume "
                    f"source={_json_path(source)} destination={_json_path(destination)} "
                    "native=contract:CROSS_VOLUME:")
            os.replace(source, destination)
        except DurableFsError:
            raise
        except OSError as exc:
            raise DurableFsError(
                "DURABLE_FS_ERROR op=ReplaceSameVolume "
                f"source={_json_path(source)} destination={_json_path(destination)} "
                f"native=errno:{exc.errno or errno.EIO}:") from exc

    def sync_directory_or_equivalent(
        self,
        directory: Path,
        entries_to_finalize: Iterable[DurableEntryToFinalize] = (),
    ) -> None:
        del entries_to_finalize
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        try:
            descriptor = os.open(directory, flags)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError as exc:
            raise self._error("SyncDirectoryOrEquivalent", directory,
                              exc.errno or errno.EIO) from exc

    def remove_owned(
        self,
        path: Path,
        tombstone: Path | None = None,
        *,
        transaction_id: str = "",
        kind: str = "remove-owned",
        source_hash: str = "",
        expected_mode: int | None = None,
        expected_identity: Iterable[int] = (),
        fault: Callable[[str], Any] | None = None,
        fault_point: str = "",
    ) -> None:
        del transaction_id, kind
        if tombstone is not None and (tombstone.exists() or tombstone.is_symlink()):
            raise self._error("RemoveOwned", tombstone, errno.EINVAL)
        if not source_hash or expected_mode is None or not tuple(expected_identity):
            raise self._error("RemoveOwned", path, errno.EINVAL)
        descriptor = -1
        try:
            descriptor = os.open(
                path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            info = os.fstat(descriptor)
        except FileNotFoundError:
            return
        except OSError as exc:
            raise self._error("RemoveOwned", path, exc.errno or errno.EIO) from exc
        try:
            retained_identity = (info.st_dev, info.st_ino, info.st_nlink)
            if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                    tuple(expected_identity) != retained_identity or
                    _observable_mode(info) != expected_mode):
                raise OSError(errno.ESTALE, "owned identity/mode changed")

            def retained_hash() -> str:
                digest = hashlib.sha256()
                offset = 0
                while True:
                    block = os.pread(descriptor, 1024 * 1024, offset)
                    if not block:
                        return digest.hexdigest()
                    digest.update(block)
                    offset += len(block)

            if retained_hash() != source_hash:
                raise OSError(errno.ESTALE, "owned payload changed")
            path_info = path.lstat()
            if ((path_info.st_dev, path_info.st_ino, path_info.st_nlink) !=
                    retained_identity or path.is_symlink()):
                raise OSError(errno.ESTALE, "owned path identity changed")
            if fault_point:
                _run_fault(fault, fault_point)
            refreshed = os.fstat(descriptor)
            path_info = path.lstat()
            if ((refreshed.st_dev, refreshed.st_ino, refreshed.st_nlink) !=
                    retained_identity or
                    (path_info.st_dev, path_info.st_ino, path_info.st_nlink) !=
                    retained_identity or path.is_symlink() or
                    _observable_mode(refreshed) != expected_mode or
                    retained_hash() != source_hash):
                raise OSError(errno.ESTALE, "owned path changed before unlink")
            os.unlink(path)
            self.sync_directory_or_equivalent(path.parent)
        except _InjectedFault:
            raise
        except OSError as exc:
            raise self._error("RemoveOwned", path, exc.errno or errno.EIO) from exc
        finally:
            if descriptor >= 0:
                os.close(descriptor)
        if path.exists() or path.is_symlink():
            raise self._error("RemoveOwned", path, errno.EIO)

    def restore_mode(self, path: Path, mode: int) -> None:
        flags = os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags)
            try:
                os.fchmod(descriptor, mode)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError as exc:
            raise self._error("RestoreMode", path, exc.errno or errno.EIO) from exc

    def lock_exclusive(
        self,
        canonical: Path,
        retired: Path,
        fault: Callable[[str], Any] | None = None,
    ) -> "_PosixLock":
        marker = f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode()
        nofollow = getattr(os, "O_NOFOLLOW", 0)
        for _ in range(32):
            created = False
            try:
                try:
                    descriptor = os.open(
                        canonical, os.O_CREAT | os.O_EXCL | os.O_RDWR | nofollow,
                        0o600)
                    created = True
                except FileExistsError:
                    descriptor = os.open(canonical, os.O_RDWR | nofollow)
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                handle_info = os.fstat(descriptor)
                if (not stat.S_ISREG(handle_info.st_mode) or
                        handle_info.st_nlink != 1):
                    raise OSError(errno.EMLINK, "lock must be a no-link regular file")
                if handle_info.st_size == 0:
                    if not created:
                        raise OSError(
                            errno.EEXIST,
                            "pre-existing empty durable lock is not owned")
                    os.ftruncate(descriptor, 0)
                    os.pwrite(descriptor, marker, 0)
                    _run_fault(
                        fault, "INSTALL_LOCK_MARKER_AFTER_WRITE_BEFORE_FLUSH")
                    os.fsync(descriptor)
                    _run_fault(
                        fault, "INSTALL_LOCK_MARKER_AFTER_FLUSH_BEFORE_READBACK")
                    handle_info = os.fstat(descriptor)
                if (handle_info.st_size != len(marker) or
                        os.pread(descriptor, len(marker), 0) != marker):
                    raise OSError(errno.EINVAL, "lock marker mismatch")
                _run_fault(fault, "INSTALL_LOCK_MARKER_AFTER_READBACK")
                path_info = canonical.lstat()
                if (not stat.S_ISREG(path_info.st_mode) or
                        path_info.st_nlink != 1 or
                        (handle_info.st_dev, handle_info.st_ino) !=
                        (path_info.st_dev, path_info.st_ino)):
                    os.close(descriptor)
                    continue
                if retired.exists() or retired.is_symlink():
                    retired_info = retired.lstat()
                    if (retired.is_symlink() or
                            not stat.S_ISREG(retired_info.st_mode) or
                            retired_info.st_nlink != 1 or
                            (retired_info.st_dev, retired_info.st_ino) ==
                            (handle_info.st_dev, handle_info.st_ino) or
                            retired.read_bytes() != marker):
                        raise OSError(errno.EINVAL, "foreign retired lock")
                    self.remove_owned(
                        retired,
                        source_hash=hashlib.sha256(marker).hexdigest(),
                        expected_mode=_observable_mode(retired_info),
                        expected_identity=(
                            retired_info.st_dev, retired_info.st_ino,
                            retired_info.st_nlink))
                return _PosixLock(canonical, retired, descriptor, marker)
            except _InjectedFault:
                try:
                    os.close(descriptor)
                except (OSError, UnboundLocalError):
                    pass
                raise
            except BlockingIOError as exc:
                try:
                    os.close(descriptor)
                except (OSError, UnboundLocalError):
                    pass
                raise self._error("LockExclusive", canonical,
                                  exc.errno or errno.EWOULDBLOCK) from exc
            except OSError as exc:
                try:
                    os.close(descriptor)
                except (OSError, UnboundLocalError):
                    pass
                raise self._error("LockExclusive", canonical,
                                  exc.errno or errno.EIO) from exc
        raise self._error("LockExclusive", canonical, errno.EAGAIN)

    def abandon_lock(self, lock: "_PosixLock") -> None:
        if lock.descriptor >= 0:
            os.close(lock.descriptor)
            lock.descriptor = -1

    def retire_lock(
        self,
        lock: "_PosixLock",
        fault: Callable[[str], Any] | None = None,
    ) -> None:
        try:
            handle_info = os.fstat(lock.descriptor)
            path_info = lock.canonical.lstat()
            if (not stat.S_ISREG(handle_info.st_mode) or
                    handle_info.st_nlink != 1 or
                    not stat.S_ISREG(path_info.st_mode) or
                    path_info.st_nlink != 1 or
                    (handle_info.st_dev, handle_info.st_ino) !=
                    (path_info.st_dev, path_info.st_ino)):
                raise OSError(errno.ESTALE, "lock identity changed")
            _run_fault(fault, "INSTALL_AFTER_LOCK_RESERVATION_DURABLE")
            refreshed_handle = os.fstat(lock.descriptor)
            refreshed_path = lock.canonical.lstat()
            if (not stat.S_ISREG(refreshed_path.st_mode) or
                    refreshed_path.st_nlink != 1 or
                    (refreshed_handle.st_dev, refreshed_handle.st_ino) !=
                    (refreshed_path.st_dev, refreshed_path.st_ino) or
                    lock.canonical.read_bytes() != lock.marker):
                raise OSError(errno.ESTALE, "lock identity changed after hook")
            self.replace_same_volume(lock.canonical, lock.retired)
            self.sync_directory_or_equivalent(lock.canonical.parent)
            _run_fault(fault, "INSTALL_AFTER_LOCK_MOVE_TO_RETIRED")
            retired_info = lock.retired.lstat()
            if ((retired_info.st_dev, retired_info.st_ino) !=
                    (handle_info.st_dev, handle_info.st_ino) or
                    lock.retired.read_bytes() != lock.marker):
                raise OSError(errno.ESTALE, "retired lock identity changed")
            os.unlink(lock.retired)
            self.sync_directory_or_equivalent(lock.retired.parent)
            _run_fault(fault, "INSTALL_AFTER_LOCK_DELETE_PENDING")
        except (OSError, DurableFsError, _InjectedFault):
            self.abandon_lock(lock)
            raise
        self.abandon_lock(lock)  # final filesystem operation is the close


@dataclass
class _PosixLock:
    canonical: Path
    retired: Path
    descriptor: int
    marker: bytes


class WindowsDurableFs:
    """Direct Win32 durable_fs binding; instantiated only on Windows."""

    MOVEFILE_REPLACE_EXISTING = 0x1
    MOVEFILE_WRITE_THROUGH = 0x8
    FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
    FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
    FILE_ATTRIBUTE_READONLY = 0x00000001
    FILE_ATTRIBUTE_NORMAL = 0x00000080
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    DELETE = 0x00010000
    FILE_WRITE_ATTRIBUTES = 0x00000100
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    FILE_SHARE_DELETE = 0x00000004
    LOCKFILE_EXCLUSIVE_LOCK = 0x00000002
    LOCKFILE_FAIL_IMMEDIATELY = 0x00000001
    CREATE_NEW = 1
    OPEN_EXISTING = 3
    OPEN_ALWAYS = 4
    INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF

    def __init__(self) -> None:
        if os.name != "nt":
            raise OSError("WindowsDurableFs is available only on Windows")
        import ctypes
        from ctypes import wintypes

        self.ctypes = ctypes
        self.wintypes = wintypes
        class FileAttributeTagInfo(ctypes.Structure):
            _fields_ = [("FileAttributes", wintypes.DWORD),
                        ("ReparseTag", wintypes.DWORD)]

        class ByHandleFileInformation(ctypes.Structure):
            _fields_ = [
                ("FileAttributes", wintypes.DWORD),
                ("CreationTime", wintypes.FILETIME),
                ("LastAccessTime", wintypes.FILETIME),
                ("LastWriteTime", wintypes.FILETIME),
                ("VolumeSerialNumber", wintypes.DWORD),
                ("FileSizeHigh", wintypes.DWORD),
                ("FileSizeLow", wintypes.DWORD),
                ("NumberOfLinks", wintypes.DWORD),
                ("FileIndexHigh", wintypes.DWORD),
                ("FileIndexLow", wintypes.DWORD),
            ]

        class FileBasicInfo(ctypes.Structure):
            _fields_ = [
                ("CreationTime", ctypes.c_longlong),
                ("LastAccessTime", ctypes.c_longlong),
                ("LastWriteTime", ctypes.c_longlong),
                ("ChangeTime", ctypes.c_longlong),
                ("FileAttributes", wintypes.DWORD),
            ]

        class FileDispositionInfo(ctypes.Structure):
            _fields_ = [("DeleteFile", ctypes.c_ubyte)]

        class Overlapped(ctypes.Structure):
            _fields_ = [
                ("Internal", ctypes.c_size_t),
                ("InternalHigh", ctypes.c_size_t),
                ("Offset", wintypes.DWORD),
                ("OffsetHigh", wintypes.DWORD),
                ("Event", wintypes.HANDLE),
            ]

        self.FileAttributeTagInfo = FileAttributeTagInfo
        self.ByHandleFileInformation = ByHandleFileInformation
        self.FileBasicInfo = FileBasicInfo
        self.FileDispositionInfo = FileDispositionInfo
        self.Overlapped = Overlapped
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel32.CreateFileW.restype = wintypes.HANDLE
        self.kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
        self.kernel32.FlushFileBuffers.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.GetFileInformationByHandleEx.argtypes = [
            wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        self.kernel32.GetFileInformationByHandleEx.restype = wintypes.BOOL
        self.kernel32.SetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        self.kernel32.SetFileInformationByHandle.restype = wintypes.BOOL
        self.kernel32.GetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(ByHandleFileInformation)]
        self.kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
        self.kernel32.GetFileSizeEx.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(ctypes.c_longlong)]
        self.kernel32.GetFileSizeEx.restype = wintypes.BOOL
        self.kernel32.SetFilePointerEx.argtypes = [
            wintypes.HANDLE, ctypes.c_longlong,
            ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD]
        self.kernel32.SetFilePointerEx.restype = wintypes.BOOL
        self.kernel32.SetEndOfFile.argtypes = [wintypes.HANDLE]
        self.kernel32.SetEndOfFile.restype = wintypes.BOOL
        self.kernel32.ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        self.kernel32.ReadFile.restype = wintypes.BOOL
        self.kernel32.WriteFile.argtypes = [
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        self.kernel32.WriteFile.restype = wintypes.BOOL
        self.kernel32.LockFileEx.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
        self.kernel32.LockFileEx.restype = wintypes.BOOL
        self.kernel32.MoveFileExW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
        self.kernel32.MoveFileExW.restype = wintypes.BOOL
        self.kernel32.DeleteFileW.argtypes = [wintypes.LPCWSTR]
        self.kernel32.DeleteFileW.restype = wintypes.BOOL
        self.kernel32.SetFileAttributesW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
        self.kernel32.SetFileAttributesW.restype = wintypes.BOOL
        self.kernel32.GetFileAttributesW.argtypes = [wintypes.LPCWSTR]
        self.kernel32.GetFileAttributesW.restype = wintypes.DWORD
        self.kernel32.GetVolumePathNameW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
        self.kernel32.GetVolumePathNameW.restype = wintypes.BOOL
        self.kernel32.GetVolumeInformationW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD]
        self.kernel32.GetVolumeInformationW.restype = wintypes.BOOL

    def _error(self, operation: str, path: Path, native: int) -> DurableFsError:
        return DurableFsError(
            f"DURABLE_FS_ERROR op={operation} path={_json_path(path)} "
            f"native=win32:{native}:")

    def _open(
        self,
        path: Path,
        disposition: int,
        access: int,
        share: int,
        operation: str = "CreateFileW",
    ) -> Any:
        handle = self.kernel32.CreateFileW(
            str(path), access, share, None, disposition,
            self.FILE_ATTRIBUTE_NORMAL | self.FILE_FLAG_OPEN_REPARSE_POINT,
            None)
        invalid = self.ctypes.c_void_p(-1).value
        if handle == invalid:
            native = self.ctypes.get_last_error()
            raise self._error(operation, path, native)
        tag = self.FileAttributeTagInfo()
        if not self.kernel32.GetFileInformationByHandleEx(
                handle, 9, self.ctypes.byref(tag), self.ctypes.sizeof(tag)):
            native = self.ctypes.get_last_error()
            self.kernel32.CloseHandle(handle)
            raise self._error(operation, path, native)
        if tag.FileAttributes & self.FILE_ATTRIBUTE_REPARSE_POINT:
            self.kernel32.CloseHandle(handle)
            raise self._error(operation, path, 4390)
        return handle

    @staticmethod
    def _reservation_bytes(
        source: Path,
        reserved: Path,
        kind: str,
        transaction_id: str,
        source_hash: str,
    ) -> bytes:
        return _canonical_json({
            "kind": kind,
            "reserved": Path(os.path.abspath(reserved)).as_posix(),
            "source": Path(os.path.abspath(source)).as_posix(),
            "sourceHash": source_hash,
            "transactionId": transaction_id,
            "version": 1,
        })

    def reserve_move_target(
        self,
        source: Path,
        reserved: Path,
        kind: str,
        transaction_id: str,
        source_hash: str,
    ) -> bytes:
        """Create, flush, reopen, and identity-verify one typed move target."""
        payload = self._reservation_bytes(
            source, reserved, kind, transaction_id, source_hash)
        handle = self._open(
            reserved, self.CREATE_NEW,
            self.GENERIC_READ | self.GENERIC_WRITE, 0,
            "ReserveMoveTarget")
        try:
            original_identity = self._identity(
                handle, reserved, "ReserveMoveTarget")
            written = self.wintypes.DWORD()
            buffer = self.ctypes.create_string_buffer(payload)
            if not self.kernel32.WriteFile(
                    handle, buffer, len(payload), self.ctypes.byref(written),
                    None):
                native = self.ctypes.get_last_error()
                raise self._error("ReserveMoveTarget", reserved, native)
            if written.value != len(payload):
                raise self._error("ReserveMoveTarget", reserved, 29)
            if not self.kernel32.FlushFileBuffers(handle):
                native = self.ctypes.get_last_error()
                raise self._error("ReserveMoveTarget", reserved, native)
        finally:
            self.kernel32.CloseHandle(handle)

        reopened = self._open(
            reserved, self.OPEN_EXISTING, self.GENERIC_READ, 0,
            "ReserveMoveTarget")
        try:
            if self._identity(
                    reopened, reserved, "ReserveMoveTarget") != original_identity:
                raise self._error("ReserveMoveTarget", reserved, 1168)
            read = self.wintypes.DWORD()
            readback = self.ctypes.create_string_buffer(len(payload))
            if not self.kernel32.ReadFile(
                    reopened, readback, len(payload),
                    self.ctypes.byref(read), None):
                native = self.ctypes.get_last_error()
                raise self._error("ReserveMoveTarget", reserved, native)
            if read.value != len(payload) or readback.raw != payload:
                raise self._error("ReserveMoveTarget", reserved, 13)
        finally:
            self.kernel32.CloseHandle(reopened)
        return payload

    def _identity(
        self, handle: Any, path: Path, operation: str
    ) -> tuple[int, int, int]:
        information = self.ByHandleFileInformation()
        if not self.kernel32.GetFileInformationByHandle(
                handle, self.ctypes.byref(information)):
            native = self.ctypes.get_last_error()
            raise self._error(operation, path, native)
        if information.NumberOfLinks != 1:
            raise self._error(operation, path, 1142)
        return (information.VolumeSerialNumber,
                information.FileIndexHigh, information.FileIndexLow)

    @staticmethod
    def physical_identity(path: Path) -> tuple[int, int, int]:
        return _physical_identity(path)

    @staticmethod
    def physical_bytes(path: Path) -> bytes:
        return path.read_bytes()

    @staticmethod
    def physical_hash(path: Path) -> str:
        return _sha256_file(path)

    @staticmethod
    def physical_mode(path: Path) -> int:
        return _observable_mode(path.lstat())

    def _volume_serial(self, path: Path) -> int:
        root = self.ctypes.create_unicode_buffer(32768)
        if not self.kernel32.GetVolumePathNameW(
                os.path.abspath(path), root, len(root)):
            native = self.ctypes.get_last_error()
            raise self._error("ReplaceSameVolume", path, native)
        serial = self.wintypes.DWORD()
        if not self.kernel32.GetVolumeInformationW(
                root.value, None, 0, self.ctypes.byref(serial), None, None,
                None, 0):
            native = self.ctypes.get_last_error()
            raise self._error("ReplaceSameVolume", path, native)
        return int(serial.value)

    def open_exclusive_temp(self, path: Path) -> Any:
        handle = self._open(
            path, self.CREATE_NEW, self.GENERIC_READ | self.GENERIC_WRITE, 0,
            "OpenExclusiveTemp")
        try:
            identity = self._identity(handle, path, "OpenExclusiveTemp")
        except BaseException:
            self.kernel32.CloseHandle(handle)
            raise
        return handle, identity

    def write_exclusive_temp(
        self,
        opened: tuple[Any, tuple[int, int, int]],
        path: Path,
        payload: bytes,
        mode: int,
    ) -> None:
        handle, original_identity = opened
        try:
            if not self.kernel32.SetFilePointerEx(handle, 0, None, 0):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            if not self.kernel32.SetEndOfFile(handle):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            written = self.wintypes.DWORD()
            buffer = self.ctypes.create_string_buffer(payload)
            if not self.kernel32.WriteFile(
                    handle, buffer, len(payload), self.ctypes.byref(written),
                    None):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            if written.value != len(payload):
                raise self._error("OpenExclusiveTemp", path, 29)
            attributes = mode or self.FILE_ATTRIBUTE_NORMAL
            basic = self.FileBasicInfo()
            if not self.kernel32.GetFileInformationByHandleEx(
                    handle, 0, self.ctypes.byref(basic),
                    self.ctypes.sizeof(basic)):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            basic.FileAttributes = attributes
            if not self.kernel32.SetFileInformationByHandle(
                    handle, 0, self.ctypes.byref(basic),
                    self.ctypes.sizeof(basic)):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            readback = self.FileBasicInfo()
            if not self.kernel32.GetFileInformationByHandleEx(
                    handle, 0, self.ctypes.byref(readback),
                    self.ctypes.sizeof(readback)):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            if readback.FileAttributes != attributes:
                raise self._error("OpenExclusiveTemp", path, 13)
            if not self.kernel32.FlushFileBuffers(handle):
                native = self.ctypes.get_last_error()
                raise self._error("OpenExclusiveTemp", path, native)
            if self._identity(
                    handle, path, "OpenExclusiveTemp") != original_identity:
                raise self._error("OpenExclusiveTemp", path, 1168)
        finally:
            self.kernel32.CloseHandle(handle)

    def flush_file(self, path: Path) -> None:
        metadata = self._open(
            path, self.OPEN_EXISTING,
            self.GENERIC_READ | self.FILE_WRITE_ATTRIBUTES,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
            "FlushFile")
        original_basic = self.FileBasicInfo()
        cleared_readonly = False
        try:
            original_identity = self._identity(metadata, path, "FlushFile")
            if not self.kernel32.GetFileInformationByHandleEx(
                    metadata, 0, self.ctypes.byref(original_basic),
                    self.ctypes.sizeof(original_basic)):
                raise self._error(
                    "FlushFile", path, self.ctypes.get_last_error())
            if original_basic.FileAttributes & self.FILE_ATTRIBUTE_READONLY:
                writable_basic = self.FileBasicInfo()
                self.ctypes.memmove(
                    self.ctypes.byref(writable_basic),
                    self.ctypes.byref(original_basic),
                    self.ctypes.sizeof(original_basic))
                writable_basic.FileAttributes &= ~self.FILE_ATTRIBUTE_READONLY
                if writable_basic.FileAttributes == 0:
                    writable_basic.FileAttributes = self.FILE_ATTRIBUTE_NORMAL
                if not self.kernel32.SetFileInformationByHandle(
                        metadata, 0, self.ctypes.byref(writable_basic),
                        self.ctypes.sizeof(writable_basic)):
                    raise self._error(
                        "FlushFile", path, self.ctypes.get_last_error())
                cleared_readonly = True
                readback = self.FileBasicInfo()
                if not self.kernel32.GetFileInformationByHandleEx(
                        metadata, 0, self.ctypes.byref(readback),
                        self.ctypes.sizeof(readback)):
                    raise self._error(
                        "FlushFile", path, self.ctypes.get_last_error())
                if (readback.FileAttributes !=
                        writable_basic.FileAttributes):
                    raise self._error("FlushFile", path, 13)

            try:
                handle = self._open(
                    path, self.OPEN_EXISTING,
                    self.GENERIC_READ | self.GENERIC_WRITE,
                    self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
                    "FlushFile")
            except BaseException:
                if cleared_readonly:
                    self.kernel32.SetFileInformationByHandle(
                        metadata, 0, self.ctypes.byref(original_basic),
                        self.ctypes.sizeof(original_basic))
                raise
            try:
                if self._identity(handle, path, "FlushFile") != original_identity:
                    raise self._error("FlushFile", path, 1168)
                if not self.kernel32.FlushFileBuffers(handle):
                    raise self._error(
                        "FlushFile", path, self.ctypes.get_last_error())
                if cleared_readonly:
                    if not self.kernel32.SetFileInformationByHandle(
                            handle, 0, self.ctypes.byref(original_basic),
                            self.ctypes.sizeof(original_basic)):
                        raise self._error(
                            "FlushFile", path, self.ctypes.get_last_error())
                    readback = self.FileBasicInfo()
                    if not self.kernel32.GetFileInformationByHandleEx(
                            handle, 0, self.ctypes.byref(readback),
                            self.ctypes.sizeof(readback)):
                        raise self._error(
                            "FlushFile", path, self.ctypes.get_last_error())
                    if (readback.FileAttributes !=
                            original_basic.FileAttributes):
                        raise self._error("FlushFile", path, 13)
                    if not self.kernel32.FlushFileBuffers(handle):
                        raise self._error(
                            "FlushFile", path, self.ctypes.get_last_error())
                if self._identity(
                        handle, path, "FlushFile") != original_identity:
                    raise self._error("FlushFile", path, 1168)
            except BaseException:
                if cleared_readonly:
                    self.kernel32.SetFileInformationByHandle(
                        metadata, 0, self.ctypes.byref(original_basic),
                        self.ctypes.sizeof(original_basic))
                raise
            finally:
                self.kernel32.CloseHandle(handle)
        except BaseException:
            if cleared_readonly:
                self.kernel32.SetFileInformationByHandle(
                    metadata, 0, self.ctypes.byref(original_basic),
                    self.ctypes.sizeof(original_basic))
            raise
        finally:
            self.kernel32.CloseHandle(metadata)

    def replace_same_volume(self, source: Path, destination: Path) -> None:
        try:
            source_volume = self._volume_serial(source)
            destination_volume = self._volume_serial(destination.parent)
        except DurableFsError as exc:
            rendered = str(exc)
            native = (rendered[rendered.index("native="):]
                      if "native=" in rendered else "native=win32:13:")
            raise DurableFsError(
                "DURABLE_FS_ERROR op=ReplaceSameVolume "
                f"source={_json_path(source)} "
                f"destination={_json_path(destination)} {native}") from exc
        if source_volume != destination_volume:
            raise DurableFsError(
                "DURABLE_FS_ERROR op=ReplaceSameVolume "
                f"source={_json_path(source)} destination={_json_path(destination)} "
                "native=contract:CROSS_VOLUME:")
        flags = self.MOVEFILE_REPLACE_EXISTING | self.MOVEFILE_WRITE_THROUGH
        if not self.kernel32.MoveFileExW(str(source), str(destination), flags):
            native = self.ctypes.get_last_error()
            raise DurableFsError(
                "DURABLE_FS_ERROR op=ReplaceSameVolume "
                f"source={_json_path(source)} destination={_json_path(destination)} "
                f"native=win32:{native}:")

    def sync_directory_or_equivalent(
        self,
        directory: Path,
        entries_to_finalize: Iterable[DurableEntryToFinalize] = (),
    ) -> None:
        # Typed there-and-back write-through moves are the Windows entry
        # durability barrier; an empty set records the preceding replacement.
        del directory
        _finalize_windows_entries(self, entries_to_finalize)

    def remove_owned(
        self,
        path: Path,
        tombstone: Path | None = None,
        *,
        transaction_id: str = "",
        kind: str = "remove-owned",
        source_hash: str = "",
        expected_mode: int | None = None,
        expected_identity: Iterable[int] = (),
        fault: Callable[[str], Any] | None = None,
        fault_point: str = "",
    ) -> None:
        expected_identity = tuple(expected_identity)
        if tombstone is not None:
            path_present = path.exists() or path.is_symlink()
            tombstone_present = tombstone.exists() or tombstone.is_symlink()
            reservation = self._reservation_bytes(
                path, tombstone, kind, transaction_id, source_hash)
            if path_present and tombstone_present:
                reservation_identity = self.physical_identity(tombstone)
                if (self.physical_bytes(tombstone) != reservation or
                        self.physical_identity(tombstone) !=
                        reservation_identity):
                    raise self._error("RemoveOwned", tombstone, 13)
                if not self.kernel32.DeleteFileW(str(tombstone)):
                    native = self.ctypes.get_last_error()
                    raise self._error("RemoveOwned", tombstone, native)
                tombstone_present = False
            if path_present and not tombstone_present:
                if (expected_identity and
                        self.physical_identity(path) != expected_identity):
                    raise self._error("RemoveOwned", path, 1168)
                if source_hash and self.physical_hash(path) != source_hash:
                    raise self._error("RemoveOwned", path, 13)
                if (expected_mode is not None and
                        self.physical_mode(path) != expected_mode):
                    raise self._error("RemoveOwned", path, 13)
                self.reserve_move_target(
                    path, tombstone, kind, transaction_id, source_hash)
                if fault_point:
                    _run_fault(fault, fault_point)
                if (expected_identity and
                        self.physical_identity(path) != expected_identity):
                    raise self._error("RemoveOwned", path, 1168)
                if source_hash and self.physical_hash(path) != source_hash:
                    raise self._error("RemoveOwned", path, 13)
                self.replace_same_volume(path, tombstone)
                if path.exists() or path.is_symlink():
                    raise self._error("RemoveOwned", path, 13)
                path = tombstone
            elif not path_present and tombstone_present:
                path = tombstone
            elif not path_present:
                return
            if (expected_identity and
                    self.physical_identity(path) != expected_identity):
                raise self._error("RemoveOwned", path, 1168)
            if source_hash and self.physical_hash(path) != source_hash:
                raise self._error("RemoveOwned", path, 13)
            if (expected_mode is not None and
                    self.physical_mode(path) != expected_mode):
                raise self._error("RemoveOwned", path, 13)
        elif path.exists() or path.is_symlink():
            if (expected_identity and
                    self.physical_identity(path) != expected_identity):
                raise self._error("RemoveOwned", path, 1168)
            if source_hash and self.physical_hash(path) != source_hash:
                raise self._error("RemoveOwned", path, 13)
            if (expected_mode is not None and
                    self.physical_mode(path) != expected_mode):
                raise self._error("RemoveOwned", path, 13)
        if not expected_identity or not source_hash or expected_mode is None:
            raise self._error("RemoveOwned", path, 87)
        attributes = self.kernel32.GetFileAttributesW(str(path))
        if attributes == self.INVALID_FILE_ATTRIBUTES:
            native = self.ctypes.get_last_error()
            if native in (2, 3):
                return
            raise self._error("RemoveOwned", path, native)
        handle = self._open(
            path, self.OPEN_EXISTING,
            self.DELETE | self.GENERIC_READ | self.FILE_WRITE_ATTRIBUTES,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
            "RemoveOwned")
        original_basic = self.FileBasicInfo()
        current_basic = self.FileBasicInfo()
        readonly_cleared = False

        def handle_bytes() -> bytes:
            size = self.ctypes.c_longlong()
            if not self.kernel32.GetFileSizeEx(handle, self.ctypes.byref(size)):
                raise self._error(
                    "RemoveOwned", path, self.ctypes.get_last_error())
            beginning = self.ctypes.c_longlong()
            if not self.kernel32.SetFilePointerEx(
                    handle, beginning, None, 0):
                raise self._error(
                    "RemoveOwned", path, self.ctypes.get_last_error())
            remaining = size.value
            chunks: list[bytes] = []
            while remaining:
                count = min(remaining, 1024 * 1024)
                buffer = self.ctypes.create_string_buffer(count)
                read = self.wintypes.DWORD()
                if not self.kernel32.ReadFile(
                        handle, buffer, count, self.ctypes.byref(read), None):
                    raise self._error(
                        "RemoveOwned", path, self.ctypes.get_last_error())
                if read.value <= 0 or read.value > count:
                    raise self._error("RemoveOwned", path, 13)
                chunks.append(buffer.raw[:read.value])
                remaining -= read.value
            return b"".join(chunks)

        try:
            if self._identity(handle, path, "RemoveOwned") != expected_identity:
                raise self._error("RemoveOwned", path, 1168)
            if not self.kernel32.GetFileInformationByHandleEx(
                    handle, 0, self.ctypes.byref(original_basic),
                    self.ctypes.sizeof(original_basic)):
                raise self._error(
                    "RemoveOwned", path, self.ctypes.get_last_error())
            if (original_basic.FileAttributes != expected_mode or
                    hashlib.sha256(handle_bytes()).hexdigest() != source_hash):
                raise self._error("RemoveOwned", path, 1168)
            if fault_point:
                _run_fault(fault, fault_point)
            if original_basic.FileAttributes & self.FILE_ATTRIBUTE_READONLY:
                self.ctypes.memmove(
                    current_basic, original_basic,
                    self.ctypes.sizeof(original_basic))
                current_basic.FileAttributes &= ~self.FILE_ATTRIBUTE_READONLY
                if current_basic.FileAttributes == 0:
                    current_basic.FileAttributes = self.FILE_ATTRIBUTE_NORMAL
                if not self.kernel32.SetFileInformationByHandle(
                        handle, 0, self.ctypes.byref(current_basic),
                        self.ctypes.sizeof(current_basic)):
                    raise self._error(
                        "RemoveOwned", path, self.ctypes.get_last_error())
                readonly_cleared = True
            if (self._identity(handle, path, "RemoveOwned") !=
                    expected_identity or
                    hashlib.sha256(handle_bytes()).hexdigest() != source_hash):
                raise self._error("RemoveOwned", path, 1168)
            disposition = self.FileDispositionInfo()
            disposition.DeleteFile = 1
            if not self.kernel32.SetFileInformationByHandle(
                    handle, 4, self.ctypes.byref(disposition),
                    self.ctypes.sizeof(disposition)):
                raise self._error(
                    "RemoveOwned", path, self.ctypes.get_last_error())
        except BaseException:
            if readonly_cleared:
                self.kernel32.SetFileInformationByHandle(
                    handle, 0, self.ctypes.byref(original_basic),
                    self.ctypes.sizeof(original_basic))
                self.kernel32.FlushFileBuffers(handle)
            self.kernel32.CloseHandle(handle)
            raise
        self.kernel32.CloseHandle(handle)
        remaining_attributes = self.kernel32.GetFileAttributesW(str(path))
        if remaining_attributes != self.INVALID_FILE_ATTRIBUTES:
            raise self._error("RemoveOwned", path, 1168)
        native = self.ctypes.get_last_error()
        if native not in (2, 3):
            raise self._error("RemoveOwned", path, native)

    def restore_mode(self, path: Path, mode: int) -> None:
        if mode == 0:
            mode = self.FILE_ATTRIBUTE_NORMAL
        if not self.kernel32.SetFileAttributesW(str(path), mode):
            native = self.ctypes.get_last_error()
            raise DurableFsError(
                f"DURABLE_FS_ERROR op=RestoreMode path={_json_path(path)} "
                f"native=win32:{native}:")
        if self.kernel32.GetFileAttributesW(str(path)) != mode:
            raise self._error("RestoreMode", path, 13)
        self.flush_file(path)

    def lock_exclusive(
        self,
        canonical: Path,
        retired: Path,
        fault: Callable[[str], Any] | None = None,
    ) -> "_WindowsLock":
        marker = f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode()
        for _ in range(32):
            created = False
            try:
                handle = self._open(
                    canonical, self.CREATE_NEW,
                    self.DELETE | self.GENERIC_READ | self.GENERIC_WRITE,
                    self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                    self.FILE_SHARE_DELETE, "LockExclusive")
                created = True
            except DurableFsError as exc:
                if ("native=win32:80:" not in str(exc) and
                        "native=win32:183:" not in str(exc)):
                    raise
                handle = self._open(
                    canonical, self.OPEN_EXISTING,
                    self.DELETE | self.GENERIC_READ | self.GENERIC_WRITE,
                    self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                    self.FILE_SHARE_DELETE, "LockExclusive")
            try:
                overlap = self.Overlapped()
                if not self.kernel32.LockFileEx(
                        handle, self.LOCKFILE_EXCLUSIVE_LOCK |
                        self.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                        self.ctypes.byref(overlap)):
                    native = self.ctypes.get_last_error()
                    raise self._error("LockExclusive", canonical, native)
                identity = self._identity(handle, canonical, "LockExclusive")
                size = self.ctypes.c_longlong()
                if not self.kernel32.GetFileSizeEx(
                        handle, self.ctypes.byref(size)):
                    native = self.ctypes.get_last_error()
                    raise self._error("LockExclusive", canonical, native)
                if size.value == 0:
                    if not created:
                        raise self._error("LockExclusive", canonical, 183)
                    written = self.wintypes.DWORD()
                    marker_buffer = self.ctypes.create_string_buffer(marker)
                    if not self.kernel32.WriteFile(
                            handle, marker_buffer, len(marker),
                            self.ctypes.byref(written), None):
                        native = self.ctypes.get_last_error()
                        raise self._error("LockExclusive", canonical, native)
                    if written.value != len(marker):
                        raise self._error("LockExclusive", canonical, 29)
                    _run_fault(
                        fault, "INSTALL_LOCK_MARKER_AFTER_WRITE_BEFORE_FLUSH")
                    if not self.kernel32.FlushFileBuffers(handle):
                        native = self.ctypes.get_last_error()
                        raise self._error("LockExclusive", canonical, native)
                    _run_fault(
                        fault, "INSTALL_LOCK_MARKER_AFTER_FLUSH_BEFORE_READBACK")
                    if not self.kernel32.GetFileSizeEx(
                            handle, self.ctypes.byref(size)):
                        native = self.ctypes.get_last_error()
                        raise self._error("LockExclusive", canonical, native)
                if size.value != len(marker):
                    raise self._error("LockExclusive", canonical, 13)
                if not self.kernel32.SetFilePointerEx(handle, 0, None, 0):
                    native = self.ctypes.get_last_error()
                    raise self._error("LockExclusive", canonical, native)
                read = self.wintypes.DWORD()
                readback = self.ctypes.create_string_buffer(len(marker))
                if not self.kernel32.ReadFile(
                        handle, readback, len(marker), self.ctypes.byref(read),
                        None):
                    native = self.ctypes.get_last_error()
                    raise self._error("LockExclusive", canonical, native)
                if read.value != len(marker) or readback.raw != marker:
                    raise self._error("LockExclusive", canonical, 13)
                _run_fault(fault, "INSTALL_LOCK_MARKER_AFTER_READBACK")
                try:
                    fresh = self._open(
                        canonical, self.OPEN_EXISTING, self.GENERIC_READ,
                        self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                        self.FILE_SHARE_DELETE, "LockExclusive")
                except DurableFsError as exc:
                    if "native=win32:2:" in str(exc) or \
                            "native=win32:3:" in str(exc):
                        self.kernel32.CloseHandle(handle)
                        continue
                    raise
                try:
                    fresh_identity = self._identity(
                        fresh, canonical, "LockExclusive")
                finally:
                    self.kernel32.CloseHandle(fresh)
                if fresh_identity != identity:
                    self.kernel32.CloseHandle(handle)
                    continue

                attributes = self.kernel32.GetFileAttributesW(str(retired))
                if attributes != self.INVALID_FILE_ATTRIBUTES:
                    if attributes & self.FILE_ATTRIBUTE_REPARSE_POINT:
                        raise self._error("LockExclusive", retired, 4390)
                    retired_identity = self.physical_identity(retired)
                    retired_bytes = self.physical_bytes(retired)
                    if self.physical_identity(retired) != retired_identity:
                        raise self._error("LockExclusive", retired, 1168)
                    generation = "-".join(
                        f"{part:08x}" for part in identity)
                    expected_reservation = self._reservation_bytes(
                        canonical, retired, "retire-lock", generation,
                        hashlib.sha256(marker).hexdigest())
                    if retired_bytes == expected_reservation:
                        self.remove_owned(
                            retired,
                            source_hash=hashlib.sha256(
                                expected_reservation).hexdigest(),
                            expected_mode=attributes,
                            expected_identity=retired_identity)
                    elif retired_bytes == marker:
                        old = self._open(
                            retired, self.OPEN_EXISTING, self.GENERIC_READ,
                            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                            self.FILE_SHARE_DELETE, "LockExclusive")
                        try:
                            old_identity = self._identity(
                                old, retired, "LockExclusive")
                        finally:
                            self.kernel32.CloseHandle(old)
                        if old_identity == identity:
                            raise self._error("LockExclusive", retired, 13)
                        self.remove_owned(
                            retired,
                            source_hash=hashlib.sha256(marker).hexdigest(),
                            expected_mode=attributes,
                            expected_identity=retired_identity)
                    else:
                        raise self._error("LockExclusive", retired, 13)
                else:
                    native = self.ctypes.get_last_error()
                    if native not in (2, 3):
                        raise self._error("LockExclusive", retired, native)
                return _WindowsLock(
                    canonical, retired, handle, marker, identity)
            except BaseException:
                self.kernel32.CloseHandle(handle)
                raise
        raise self._error("LockExclusive", canonical, 1237)

    def abandon_lock(self, lock: "_WindowsLock") -> None:
        if lock.handle is not None:
            self.kernel32.CloseHandle(lock.handle)
            lock.handle = None

    def retire_lock(
        self,
        lock: "_WindowsLock",
        fault: Callable[[str], Any] | None = None,
    ) -> None:
        guard = None
        try:
            fresh = self._open(
                lock.canonical, self.OPEN_EXISTING, self.GENERIC_READ,
                self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                self.FILE_SHARE_DELETE, "RetireLock")
            try:
                fresh_identity = self._identity(
                    fresh, lock.canonical, "RetireLock")
            finally:
                self.kernel32.CloseHandle(fresh)
            if fresh_identity != lock.identity or self._identity(
                    lock.handle, lock.canonical, "RetireLock") != lock.identity:
                raise self._error("RetireLock", lock.canonical, 1168)
            generation = "-".join(f"{part:08x}" for part in lock.identity)
            self.reserve_move_target(
                lock.canonical, lock.retired, "retire-lock", generation,
                hashlib.sha256(lock.marker).hexdigest())
            _run_fault(fault, "INSTALL_AFTER_LOCK_RESERVATION_DURABLE")
            self.replace_same_volume(lock.canonical, lock.retired)
            attributes = self.kernel32.GetFileAttributesW(str(lock.canonical))
            if attributes != self.INVALID_FILE_ATTRIBUTES:
                raise self._error("RetireLock", lock.canonical, 13)
            native = self.ctypes.get_last_error()
            if native not in (2, 3):
                raise self._error("RetireLock", lock.canonical, native)
            if self._identity(
                    lock.handle, lock.retired, "RetireLock") != lock.identity:
                raise self._error("RetireLock", lock.retired, 1168)
            _run_fault(fault, "INSTALL_AFTER_LOCK_MOVE_TO_RETIRED")
            guard = self._open(
                lock.retired, self.OPEN_EXISTING,
                self.DELETE | self.GENERIC_READ,
                self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
                "RetireLock")
            if self._identity(
                    guard, lock.retired, "RetireLock") != lock.identity:
                raise self._error("RetireLock", lock.retired, 1168)
            disposition = self.FileDispositionInfo()
            disposition.DeleteFile = 1
            if not self.kernel32.SetFileInformationByHandle(
                    guard, 4, self.ctypes.byref(disposition),
                    self.ctypes.sizeof(disposition)):
                raise self._error(
                    "RetireLock", lock.retired,
                    self.ctypes.get_last_error())
            _run_fault(fault, "INSTALL_AFTER_LOCK_DELETE_PENDING")
        except BaseException:
            if guard is not None:
                self.kernel32.CloseHandle(guard)
            self.abandon_lock(lock)
            raise
        self.abandon_lock(lock)
        self.kernel32.CloseHandle(guard)  # final filesystem operation


@dataclass
class _WindowsLock:
    canonical: Path
    retired: Path
    handle: Any
    marker: bytes
    identity: tuple[int, int, int]


def _default_durable_fs() -> PosixDurableFs | WindowsDurableFs:
    return WindowsDurableFs() if os.name == "nt" else PosixDurableFs()


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _write_owned(
    durable_fs: Any, path: Path, payload: bytes, mode: int
) -> None:
    opened = durable_fs.open_exclusive_temp(path)
    durable_fs.write_exclusive_temp(opened, path, payload, mode)


def _copy_owned(durable_fs: Any, source: Path, destination: Path, mode: int) -> None:
    _write_owned(durable_fs, destination, source.read_bytes(), mode)


def _observable_mode(info: os.stat_result) -> int:
    if os.name == "nt":
        return int(getattr(info, "st_file_attributes"))
    return stat.S_IMODE(info.st_mode)


def _private_mode() -> int:
    return (WindowsDurableFs.FILE_ATTRIBUTE_NORMAL
            if os.name == "nt" else 0o600)


def _path_state(path: Path) -> tuple[bool, str, int, bytes]:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False, "", 0, b""
    if not stat.S_ISREG(info.st_mode) or path.is_symlink():
        raise InstallError(f"target is a symlink or non-regular path: {path}")
    payload = path.read_bytes()
    return True, hashlib.sha256(payload).hexdigest(), _observable_mode(info), payload


def _direct_child(parent: Path, path: Path) -> bool:
    return path.parent == parent and path.name not in ("", ".", "..")


def _journal_bytes(journal: dict[str, Any]) -> bytes:
    return _canonical_json(journal)


_JOURNAL_AUTH_PREFIX = b"corsairs-journal-retired-auth-v1\nsha256="


def _journal_auth_bytes(journal_bytes: bytes) -> bytes:
    return (_JOURNAL_AUTH_PREFIX + hashlib.sha256(journal_bytes).hexdigest().encode()
            + b"\n" + journal_bytes)


def _read_journal_auth(path: Path) -> bytes:
    _physical_regular(path, "/journalRetiredAuth")
    if _observable_mode(path.lstat()) != _private_mode():
        raise InstallError(
            "install journal retirement authentication mode mismatch",
            status="RECOVERY_REQUIRED", recovery_paths=(path,))
    payload = path.read_bytes()
    if not payload.startswith(_JOURNAL_AUTH_PREFIX):
        raise InstallError(
            "invalid install journal retirement authentication",
            status="RECOVERY_REQUIRED", recovery_paths=(path,))
    separator = payload.find(b"\n", len(_JOURNAL_AUTH_PREFIX))
    if separator < 0:
        raise InstallError(
            "invalid install journal retirement authentication",
            status="RECOVERY_REQUIRED", recovery_paths=(path,))
    recorded_hash = payload[len(_JOURNAL_AUTH_PREFIX):separator]
    journal_bytes = payload[separator + 1:]
    if (len(recorded_hash) != 64 or
            re.fullmatch(rb"[0-9a-f]{64}", recorded_hash) is None or
            hashlib.sha256(journal_bytes).hexdigest().encode() != recorded_hash):
        raise InstallError(
            "install journal retirement authentication mismatch",
            status="RECOVERY_REQUIRED", recovery_paths=(path,))
    return journal_bytes


def _ensure_journal_auth(
    durable_fs: Any, canonical: Path, journal_bytes: bytes
) -> Path:
    auth_path = canonical.parent / _JOURNAL_AUTH_NAME
    expected = _journal_auth_bytes(journal_bytes)
    if not (auth_path.exists() or auth_path.is_symlink()):
        _write_owned(durable_fs, auth_path, expected, _private_mode())
        durable_fs.sync_directory_or_equivalent(auth_path.parent)
    if _read_journal_auth(auth_path) != journal_bytes:
        raise InstallError(
            "install journal retirement authentication does not match payload",
            status="RECOVERY_REQUIRED", recovery_paths=(auth_path,))
    return auth_path


def _remove_journal_auth(
    durable_fs: Any, canonical: Path, journal_bytes: bytes
) -> None:
    auth_path = canonical.parent / _JOURNAL_AUTH_NAME
    if not (auth_path.exists() or auth_path.is_symlink()):
        raise InstallError(
            "install journal retirement authentication is missing",
            status="RECOVERY_REQUIRED", recovery_paths=(auth_path,))
    if _read_journal_auth(auth_path) != journal_bytes:
        raise InstallError(
            "install journal retirement authentication does not match payload",
            status="RECOVERY_REQUIRED", recovery_paths=(auth_path,))
    identity = _physical_identity(auth_path)
    payload = _journal_auth_bytes(journal_bytes)
    durable_fs.remove_owned(
        auth_path, source_hash=hashlib.sha256(payload).hexdigest(),
        expected_mode=_private_mode(), expected_identity=identity)


def _parse_journal_impl(raw: bytes, target_root: Path) -> dict[str, Any]:
    try:
        journal = json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_pairs)
    except (UnicodeError, ValueError, json.JSONDecodeError,
            RecursionError) as exc:
        raise InstallError(f"invalid install journal: {exc}", status="RECOVERY_REQUIRED") from exc
    _validate_unicode_scalars(
        journal, "/journal", status="RECOVERY_REQUIRED",
        label="install journal")
    expected = {"version", "phase", "transactionId", "manifestPath",
                "manifestSha256", "recoveryCommand", "journalRetired",
                "entries"}
    _require_keys(journal, expected, "/journal")
    if _integer(journal["version"], "/journal/version") != 1:
        raise InstallError("invalid install journal version", status="RECOVERY_REQUIRED")
    if journal["phase"] not in (
        "SNAPSHOT", "PREPARED", "BLOCK_REPLACED", "REGION_REPLACED",
        "METADATA_REPLACED", "TRIPLE_REPLACED", "PAIR_REPLACED", "COMMITTED"
    ):
        raise InstallError("invalid install journal phase", status="RECOVERY_REQUIRED")
    transaction_id = _string(
        journal["transactionId"], "/journal/transactionId")
    if re.fullmatch(r"[0-9a-f]{16,}-[0-9a-f]{8}", transaction_id) is None:
        raise InstallError(
            "invalid install journal transactionId",
            status="RECOVERY_REQUIRED")
    manifest_path = Path(_string(
        journal["manifestPath"], "/journal/manifestPath"))
    if (manifest_path.as_posix() in ("", ".") or
            ".." in manifest_path.parts):
        raise InstallError(
            "invalid install journal manifestPath",
            status="RECOVERY_REQUIRED")
    _hash(journal["manifestSha256"], "/journal/manifestSha256")
    recovery_command = _string(
        journal["recoveryCommand"], "/journal/recoveryCommand")
    if (not recovery_command or
            recovery_command != _recovery_command(manifest_path, target_root)):
        raise InstallError(
            "invalid install journal recoveryCommand",
            status="RECOVERY_REQUIRED")
    journal_retired = Path(_string(
        journal["journalRetired"], "/journal/journalRetired"))
    if journal_retired != target_root / _RETIRED_JOURNAL_NAME:
        raise InstallError(
            "install journal retired mapping mismatch",
            status="RECOVERY_REQUIRED")
    entries = _require_keys(journal["entries"], set(_INSTALL_KEYS),
                            "/journal/entries")
    for key, entry_value in entries.items():
        entry = _require_keys(
            entry_value,
            {"target", "stage", "backup", "stageReservation",
             "backupReservation", "rollbackStage", "rollbackReservation",
             "targetTombstone", "stageTombstone", "backupTombstone",
             "rollbackTombstone", "priorExists", "priorSha256",
             "priorMode", "priorIdentity", "intendedSha256",
             "intendedSize", "intendedMode", "stageIdentity",
             "backupIdentity", "rollbackIdentity"},
            f"/journal/entries/{key}",
        )
        for path_key in (
            "target", "stage", "stageReservation", "backupReservation",
            "rollbackStage", "rollbackReservation", "targetTombstone",
            "stageTombstone", "backupTombstone", "rollbackTombstone",
        ):
            path = Path(_string(entry[path_key], f"/journal/entries/{key}/{path_key}"))
            if not _direct_child(target_root, path):
                raise InstallError("install journal path escape", status="RECOVERY_REQUIRED")
        expected_target = target_root / _INSTALL_LEAVES[key]
        expected_stage = target_root / (
            f".garner-runtime-install.stage.{transaction_id}.{key}")
        expected_mappings = {
            "stageReservation": target_root / (
                f".garner-runtime-install.finalize.{transaction_id}.{key}.stage"),
            "backupReservation": target_root / (
                f".garner-runtime-install.finalize.{transaction_id}.{key}.backup"),
            "rollbackStage": target_root / (
                f".garner-runtime-install.rollback.{transaction_id}.{key}"),
            "rollbackReservation": target_root / (
                f".garner-runtime-install.finalize.{transaction_id}.{key}.rollback"),
            "targetTombstone": target_root / (
                f".garner-runtime-install.tombstone.{transaction_id}.{key}.target"),
            "stageTombstone": target_root / (
                f".garner-runtime-install.tombstone.{transaction_id}.{key}.stage"),
            "backupTombstone": target_root / (
                f".garner-runtime-install.tombstone.{transaction_id}.{key}.backup"),
            "rollbackTombstone": target_root / (
                f".garner-runtime-install.tombstone.{transaction_id}.{key}.rollback"),
        }
        if (Path(entry["target"]) != expected_target or
                Path(entry["stage"]) != expected_stage or any(
                    Path(entry[field]) != expected_path
                    for field, expected_path in expected_mappings.items())):
            raise InstallError(
                "install journal path does not match transaction",
                status="RECOVERY_REQUIRED")
        if type(entry["priorExists"]) is not bool:
            raise InstallError("invalid journal priorExists", status="RECOVERY_REQUIRED")
        backup_text = _string(entry["backup"], f"/journal/entries/{key}/backup")
        if entry["priorExists"]:
            backup = Path(backup_text)
            expected_backup = target_root / (
                f".garner-runtime-install.backup.{transaction_id}.{key}")
            if (not _direct_child(target_root, backup) or
                    backup != expected_backup):
                raise InstallError("install journal backup escape", status="RECOVERY_REQUIRED")
            _hash(entry["priorSha256"], f"/journal/entries/{key}/priorSha256")
            _integer(entry["priorMode"], f"/journal/entries/{key}/priorMode")
        elif (backup_text or entry["priorSha256"] or entry["priorMode"] != 0 or
              entry["priorIdentity"] != []):
            raise InstallError("invalid absent-prior journal", status="RECOVERY_REQUIRED")
        _hash(entry["intendedSha256"], f"/journal/entries/{key}/intendedSha256")
        if _integer(entry["intendedSize"],
                    f"/journal/entries/{key}/intendedSize") == 0:
            raise InstallError("invalid intended size", status="RECOVERY_REQUIRED")
        _integer(entry["intendedMode"], f"/journal/entries/{key}/intendedMode")
        for identity_key in (
            "priorIdentity", "stageIdentity", "backupIdentity",
            "rollbackIdentity",
        ):
            identity = entry[identity_key]
            if type(identity) is not list or len(identity) not in (0, 3) or any(
                    type(component) is not int or component < 0
                    for component in identity):
                raise InstallError(
                    f"invalid journal {identity_key}",
                    status="RECOVERY_REQUIRED")
        if entry["priorExists"] and len(entry["priorIdentity"]) != 3:
            raise InstallError("missing prior identity", status="RECOVERY_REQUIRED")
        if journal["phase"] != "SNAPSHOT" and (
                len(entry["stageIdentity"]) != 3 or
                (entry["priorExists"] and len(entry["backupIdentity"]) != 3)):
            raise InstallError(
                "missing prepared artifact identity", status="RECOVERY_REQUIRED")
    return journal


def _parse_journal(raw: bytes, target_root: Path) -> dict[str, Any]:
    try:
        return _parse_journal_impl(raw, target_root)
    except InstallError as exc:
        if exc.status == "RECOVERY_REQUIRED" and exc.recovery_paths:
            raise
        raise InstallError(
            exc.detail,
            status="RECOVERY_REQUIRED",
            recovery_paths=(
                target_root / _JOURNAL_NAME,
                target_root / _RETIRED_JOURNAL_NAME,
            ),
        ) from exc


def _write_journal(
    durable_fs: Any, canonical: Path, journal: dict[str, Any]
) -> tuple[tuple[int, int, int], bytes]:
    update = canonical.parent / (
        f".garner-runtime-install.journal.{journal['transactionId']}.{time.monotonic_ns()}")
    payload = _journal_bytes(journal)
    update_identity: tuple[int, int, int] = ()
    try:
        _write_owned(durable_fs, update, payload, _private_mode())
        update_identity = _physical_identity(update)
        if (_observable_mode(update.lstat()) != _private_mode() or
                _parse_journal(update.read_bytes(), canonical.parent) != journal):
            raise DurableFsError("journal readback mismatch")
        durable_fs.replace_same_volume(update, canonical)
        try:
            durable_fs.sync_directory_or_equivalent(canonical.parent)
        except (DurableFsError, OSError) as exc:
            raise InstallError(
                "install journal rename durability uncertain "
                f"path={canonical.as_posix()} cause={exc}",
                status="RECOVERY_REQUIRED",
                recovery_paths=(canonical,),
                recovery_command=journal["recoveryCommand"],
            ) from exc
    except BaseException as exc:
        if update.exists() or update.is_symlink():
            if not update_identity:
                raise DurableFsError(
                    "RECOVERY_REQUIRED install journal update retained "
                    f"path={update.as_posix()} cause={exc}") from exc
            try:
                durable_fs.remove_owned(
                    update, source_hash=hashlib.sha256(payload).hexdigest(),
                    expected_mode=_private_mode(),
                    expected_identity=update_identity)
            except BaseException as cleanup_exc:
                raise DurableFsError(
                    "RECOVERY_REQUIRED install journal update retained "
                    f"path={update.as_posix()} cause={exc} "
                    f"cleanup={cleanup_exc}") from cleanup_exc
        raise
    if canonical.read_bytes() != payload:
        raise DurableFsError("journal publication readback mismatch")
    return _physical_identity(canonical), payload


def _verify_entry(
    entry: dict[str, Any],
    intended: bool,
    expected_identity: Iterable[int],
) -> bool:
    target = Path(entry["target"])
    try:
        exists, digest, mode, _ = _path_state(target)
    except (InstallError, OSError):
        return False
    if not exists:
        return False
    prefix = "intended" if intended else "prior"
    identity = tuple(expected_identity)
    try:
        return (len(identity) == 3 and
                _physical_identity(target) == identity and
                digest == entry[f"{prefix}Sha256"] and
                mode == entry[f"{prefix}Mode"])
    except (InstallError, OSError):
        return False


def _matches_prior_snapshot(entry: dict[str, Any]) -> bool:
    target = Path(entry["target"])
    try:
        exists, digest, mode, _ = _path_state(target)
        if not entry["priorExists"]:
            return not exists
        return (exists and digest == entry["priorSha256"] and
                mode == entry["priorMode"] and
                _physical_identity(target) ==
                tuple(entry["priorIdentity"]))
    except (InstallError, OSError):
        return False


def _remove_recorded_owned(
    durable_fs: Any,
    path: Path,
    tombstone: Path,
    *,
    transaction_id: str,
    kind: str,
    expected_hash: str,
    expected_mode: int,
    expected_identity: Iterable[int],
    fault: Callable[[str], Any] | None = None,
    fault_point: str = "",
) -> None:
    identity = tuple(expected_identity)
    path_present = path.exists() or path.is_symlink()
    tombstone_present = tombstone.exists() or tombstone.is_symlink()
    candidate = path if path_present else tombstone
    if (path_present or (tombstone_present and not path_present)):
        # A Windows pre-move reservation is validated by the adapter against
        # its exact typed bytes. Every actual owned payload is checked here.
        is_reservation = False
        if tombstone_present and path_present:
            reservation_builder = getattr(durable_fs, "_reservation_bytes", None)
            if reservation_builder is not None:
                try:
                    is_reservation = tombstone.read_bytes() == reservation_builder(
                        path, tombstone, kind, transaction_id, expected_hash)
                except OSError:
                    is_reservation = False
        if not is_reservation:
            try:
                actual_identity = _physical_identity(candidate)
                actual_hash = _sha256_file(candidate)
                actual_mode = _observable_mode(candidate.lstat())
            except (InstallError, OSError) as exc:
                raise DurableFsError(
                    f"owned artifact identity/hash/mode mismatch: {candidate}") from exc
            if (not identity or actual_identity != identity or
                    actual_hash != expected_hash or actual_mode != expected_mode):
                raise DurableFsError(
                    f"owned artifact identity/hash/mode mismatch: {candidate}")
    durable_fs.remove_owned(
        path, tombstone,
        transaction_id=transaction_id,
        kind=kind,
        source_hash=expected_hash,
        expected_mode=expected_mode,
        expected_identity=identity,
        fault=fault,
        fault_point=fault_point,
    )


def _recover_transaction(
    durable_fs: Any,
    target_root: Path,
    journal_path: Path,
    retired_journal: Path,
    fault: Callable[[str], Any] | None = None,
    recovery_context: dict[str, str] | None = None,
) -> None:
    auth_path = target_root / _JOURNAL_AUTH_NAME
    uncertain_updates = sorted(
        path for path in target_root.iterdir()
        if path.name.startswith(".garner-runtime-install.journal."))
    if uncertain_updates:
        raise InstallError(
            f"uncertain install journal update retained: "
            f"{uncertain_updates[0].as_posix()}",
            status="RECOVERY_REQUIRED",
            recovery_paths=tuple(uncertain_updates))
    unexpected = sorted(
        path for path in target_root.iterdir()
        if path.name.startswith(".garner-runtime-install") and
        path.name.endswith(".retired") and
        path not in (retired_journal, target_root / _RETIRED_LOCK_NAME)
    )
    if unexpected:
        raise InstallError(
            f"unexpected retired runtime artifact: {unexpected[0].as_posix()}",
            status="RECOVERY_REQUIRED", recovery_paths=(unexpected[0],))
    canonical_exists = journal_path.exists() or journal_path.is_symlink()
    retired_exists = retired_journal.exists() or retired_journal.is_symlink()
    auth_exists = auth_path.exists() or auth_path.is_symlink()
    authenticated_bytes = _read_journal_auth(auth_path) if auth_exists else None
    if not canonical_exists and not retired_exists:
        if authenticated_bytes is not None:
            journal = _parse_journal(authenticated_bytes, target_root)
            if recovery_context is not None:
                recovery_context["command"] = journal["recoveryCommand"]
            state_matches = True
            if journal["phase"] == "COMMITTED":
                state_matches = all(
                    _verify_entry(entry, True, entry["stageIdentity"])
                    for entry in journal["entries"].values())
            else:
                for entry in journal["entries"].values():
                    target = Path(entry["target"])
                    if entry["priorExists"]:
                        identities = {tuple(entry["priorIdentity"])}
                        if entry["rollbackIdentity"]:
                            identities.add(tuple(entry["rollbackIdentity"]))
                        try:
                            exists, digest, mode, _ = _path_state(target)
                            state_matches = (
                                exists and digest == entry["priorSha256"] and
                                mode == entry["priorMode"] and
                                _physical_identity(target) in identities)
                        except (InstallError, OSError):
                            state_matches = False
                    else:
                        state_matches = not (
                            target.exists() or target.is_symlink())
                    if not state_matches:
                        break
            if not state_matches:
                raise InstallError(
                    "authentication-only recovery cannot verify runtime pair",
                    status="RECOVERY_REQUIRED", recovery_paths=(auth_path,))
            _remove_journal_auth(durable_fs, journal_path, authenticated_bytes)
        return
    if canonical_exists and retired_exists:
        try:
            _physical_regular(journal_path, "/journal")
            canonical_bytes = journal_path.read_bytes()
        except (InstallError, OSError) as exc:
            raise InstallError(
                "cannot validate canonical install journal",
                status="RECOVERY_REQUIRED",
                recovery_paths=(journal_path, retired_journal)) from exc
        canonical_journal = _parse_journal(canonical_bytes, target_root)
        if recovery_context is not None:
            recovery_context["command"] = canonical_journal["recoveryCommand"]
        if (authenticated_bytes is not None and
                authenticated_bytes != canonical_bytes):
            raise InstallError(
                "install journal retirement authentication does not match payload",
                status="RECOVERY_REQUIRED",
                recovery_paths=(journal_path, retired_journal, auth_path))
        reservation_builder = getattr(durable_fs, "_reservation_bytes", None)
        expected_reservation = (
            reservation_builder(
                journal_path, retired_journal, "retire-install-journal",
                canonical_journal["transactionId"],
                hashlib.sha256(canonical_bytes).hexdigest())
            if reservation_builder is not None else None)
        try:
            retired_bytes = retired_journal.read_bytes()
        except OSError as exc:
            raise InstallError(
                "invalid install journal retirement reservation",
                status="RECOVERY_REQUIRED",
                recovery_paths=(journal_path, retired_journal)) from exc
        if expected_reservation is None or retired_bytes != expected_reservation:
            raise InstallError(
                "both canonical and retired install journals exist",
                status="RECOVERY_REQUIRED",
                recovery_paths=(journal_path, retired_journal))
        reservation_identity = _physical_identity(retired_journal)
        reservation_mode = _observable_mode(retired_journal.lstat())
        durable_fs.remove_owned(
            retired_journal,
            source_hash=hashlib.sha256(retired_bytes).hexdigest(),
            expected_mode=reservation_mode,
            expected_identity=reservation_identity)
        retired_exists = False
    source = journal_path if canonical_exists else retired_journal
    try:
        _physical_regular(source, "/journal")
        journal_identity = _physical_identity(source)
        journal_bytes = source.read_bytes()
    except (InstallError, OSError) as exc:
        raise InstallError(
            f"cannot validate physical install journal {source}",
            status="RECOVERY_REQUIRED", recovery_paths=(source,)) from exc
    journal = _parse_journal(journal_bytes, target_root)
    if recovery_context is not None:
        recovery_context["command"] = journal["recoveryCommand"]
    if source == retired_journal and authenticated_bytes is None:
        raise InstallError(
            "retired install journal lacks independent authentication",
            status="RECOVERY_REQUIRED", recovery_paths=(retired_journal,))
    if (authenticated_bytes is not None and
            authenticated_bytes != journal_bytes):
        raise InstallError(
            "install journal retirement authentication does not match payload",
            status="RECOVERY_REQUIRED",
            recovery_paths=(source, auth_path))
    normalization_entries: list[DurableEntryToFinalize] = []
    for key, entry in journal["entries"].items():
        stage = Path(entry["stage"])
        stage_reservation = Path(entry["stageReservation"])
        if entry["stageIdentity"] and (
                stage.exists() or stage.is_symlink() or
                stage_reservation.exists() or stage_reservation.is_symlink()):
            normalization_entries.append(DurableEntryToFinalize(
                stage, stage_reservation, "finalize-install-stage",
                journal["transactionId"], entry["intendedSha256"],
                tuple(entry["stageIdentity"]), entry["intendedMode"]))
        if entry["priorExists"] and entry["backupIdentity"]:
            backup = Path(entry["backup"])
            backup_reservation = Path(entry["backupReservation"])
            if (backup.exists() or backup.is_symlink() or
                    backup_reservation.exists() or
                    backup_reservation.is_symlink()):
                normalization_entries.append(DurableEntryToFinalize(
                    backup, backup_reservation, "finalize-install-backup",
                    journal["transactionId"], entry["priorSha256"],
                    tuple(entry["backupIdentity"]), entry["priorMode"]))
    if normalization_entries:
        durable_fs.sync_directory_or_equivalent(
            target_root, normalization_entries)
        lingering = next((entry.reservation for entry in normalization_entries
                          if entry.reservation.exists() or
                          entry.reservation.is_symlink()), None)
        if lingering is not None:
            raise InstallError(
                f"finalization reservation remains after recovery: {lingering}",
                status="RECOVERY_REQUIRED", recovery_paths=(source, lingering))

    def recovery_fault(point: str, evidence: tuple[Path, ...]) -> None:
        try:
            _run_fault(fault, point)
        except _InjectedFault as exc:
            raise InstallError(
                f"injected fault {point} during recovery",
                status="RECOVERY_REQUIRED",
                recovery_paths=evidence,
            ) from exc

    committed = journal["phase"] == "COMMITTED"
    entries = journal["entries"]
    if committed:
        if not all(_verify_entry(entry, True, entry["stageIdentity"])
                   for entry in entries.values()):
            raise InstallError(
                "committed runtime pair does not match journal",
                status="RECOVERY_REQUIRED", recovery_paths=(source,))
    else:
        for key in _INSTALL_KEYS:
            entry = entries[key]
            target = Path(entry["target"])
            exists, digest, mode, _ = _path_state(target)
            target_identity = _physical_identity(target) if exists else ()
            if entry["priorExists"]:
                prior_identities = {tuple(entry["priorIdentity"])}
                if entry["rollbackIdentity"]:
                    prior_identities.add(tuple(entry["rollbackIdentity"]))
                if (exists and digest == entry["priorSha256"] and
                        mode == entry["priorMode"] and
                        target_identity in prior_identities):
                    continue
                if (not exists or digest != entry["intendedSha256"] or
                        mode != entry["intendedMode"] or
                        target_identity != tuple(entry["stageIdentity"])):
                    raise InstallError(
                        f"uncertain runtime target {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                backup = Path(entry["backup"])
                backup_exists, backup_hash, backup_mode, _ = _path_state(backup)
                if (not backup_exists or backup_hash != entry["priorSha256"] or
                        backup_mode != entry["priorMode"] or
                        _physical_identity(backup) !=
                        tuple(entry["backupIdentity"])):
                    raise InstallError(
                        f"invalid runtime backup {backup}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source, backup))
                rollback = Path(entry["rollbackStage"])
                rollback_reservation = Path(entry["rollbackReservation"])
                if (not (rollback.exists() or rollback.is_symlink()) and
                        (rollback_reservation.exists() or
                         rollback_reservation.is_symlink())):
                    if not entry["rollbackIdentity"]:
                        raise InstallError(
                            f"rollback reservation lacks recorded identity "
                            f"{rollback_reservation}",
                            status="RECOVERY_REQUIRED",
                            recovery_paths=(source, rollback_reservation))
                    rollback_finalization = DurableEntryToFinalize(
                        rollback, rollback_reservation,
                        "finalize-install-rollback", journal["transactionId"],
                        entry["priorSha256"],
                        tuple(entry["rollbackIdentity"]), entry["priorMode"])
                    durable_fs.sync_directory_or_equivalent(
                        target_root, [rollback_finalization])
                    if (rollback_finalization.reservation.exists() or
                            rollback_finalization.reservation.is_symlink()):
                        raise InstallError(
                            f"invalid runtime rollback reservation "
                            f"{rollback_finalization.reservation}",
                            status="RECOVERY_REQUIRED",
                            recovery_paths=(source,
                                            rollback_finalization.reservation))
                if rollback.exists() or rollback.is_symlink():
                    if (not entry["rollbackIdentity"] or
                            _physical_identity(rollback) !=
                            tuple(entry["rollbackIdentity"]) or
                            _sha256_file(rollback) != entry["priorSha256"] or
                            _observable_mode(rollback.lstat()) !=
                            entry["priorMode"]):
                        raise InstallError(
                            f"invalid runtime rollback stage {rollback}",
                            status="RECOVERY_REQUIRED",
                            recovery_paths=(source, backup, rollback))
                else:
                    _copy_owned(durable_fs, backup, rollback, entry["priorMode"])
                    entry["rollbackIdentity"] = list(
                        _physical_identity(rollback))
                    journal_identity, journal_bytes = _write_journal(
                        durable_fs, source, journal)
                rollback_finalization = DurableEntryToFinalize(
                    rollback, rollback_reservation,
                    "finalize-install-rollback", journal["transactionId"],
                    entry["priorSha256"], tuple(entry["rollbackIdentity"]),
                    entry["priorMode"])
                durable_fs.sync_directory_or_equivalent(
                    target_root, [rollback_finalization],
                )
                recovery_fault(
                    "INSTALL_RECOVERY_AFTER_MODE_STAGE_FLUSH", (source, backup))
                recovery_fault(
                    "INSTALL_RECOVERY_" + key.upper() + "_REPLACE",
                    (source, backup),
                )
                durable_fs.replace_same_volume(rollback, target)
                durable_fs.sync_directory_or_equivalent(target_root)
                recovery_fault(
                    "INSTALL_RECOVERY_DURABILITY_BARRIER", (source, backup))
                if not _verify_entry(
                        entry, False, entry["rollbackIdentity"]):
                    raise InstallError(
                        f"runtime rollback verification failed {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source, backup))
                recovery_fault("INSTALL_RECOVERY_VERIFY", (source, backup))
            elif exists:
                if (digest != entry["intendedSha256"] or
                        mode != entry["intendedMode"] or
                        target_identity != tuple(entry["stageIdentity"])):
                    raise InstallError(
                        f"unexpected absent-prior target {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                recovery_fault(
                    "INSTALL_RECOVERY_" + key.upper() + "_REPLACE",
                    (source,),
                )
                _remove_recorded_owned(
                    durable_fs, target, Path(entry["targetTombstone"]),
                    transaction_id=journal["transactionId"],
                    kind="remove-owned-target",
                    expected_hash=entry["intendedSha256"],
                    expected_mode=entry["intendedMode"],
                    expected_identity=entry["stageIdentity"],
                    fault=fault,
                    fault_point="INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE",
                )
                durable_fs.sync_directory_or_equivalent(target_root)
                recovery_fault("INSTALL_RECOVERY_DURABILITY_BARRIER", (source,))
                if target.exists() or target.is_symlink():
                    raise InstallError(
                        f"runtime absent-prior rollback failed {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                recovery_fault("INSTALL_RECOVERY_VERIFY", (source,))
    for entry in entries.values():
        cleanup = (
            ("stage", "stageTombstone", "intendedSha256", "intendedMode",
             "stageIdentity", "remove-owned-stage"),
            ("backup", "backupTombstone", "priorSha256", "priorMode",
             "backupIdentity", "remove-owned-backup"),
            ("rollbackStage", "rollbackTombstone", "priorSha256", "priorMode",
             "rollbackIdentity", "remove-owned-rollback"),
        )
        for (path_key, tombstone_key, hash_key, mode_key,
             identity_key, kind) in cleanup:
            text = entry[path_key]
            if not text:
                continue
            path = Path(text)
            tombstone = Path(entry[tombstone_key])
            if (path.exists() or path.is_symlink() or
                    tombstone.exists() or tombstone.is_symlink()):
                _remove_recorded_owned(
                    durable_fs, path, tombstone,
                    transaction_id=journal["transactionId"], kind=kind,
                    expected_hash=entry[hash_key],
                    expected_mode=entry[mode_key],
                    expected_identity=entry[identity_key], fault=fault,
                    fault_point="INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE")
    if source == journal_path:
        _ensure_journal_auth(durable_fs, journal_path, journal_bytes)
        reservation_builder = getattr(durable_fs, "reserve_move_target", None)
        if reservation_builder is not None:
            reservation_builder(
                journal_path, retired_journal, "retire-install-journal",
                journal["transactionId"],
                hashlib.sha256(journal_bytes).hexdigest())
        durable_fs.replace_same_volume(journal_path, retired_journal)
        durable_fs.sync_directory_or_equivalent(target_root)
        source = retired_journal
    if (_physical_identity(source) != journal_identity or
            source.read_bytes() != journal_bytes or
            _observable_mode(source.lstat()) != _private_mode()):
        raise InstallError(
            "owned journal identity/hash/mode mismatch",
            status="RECOVERY_REQUIRED", recovery_paths=(source,))
    durable_fs.remove_owned(
        source, source_hash=hashlib.sha256(journal_bytes).hexdigest(),
        expected_mode=_private_mode(), expected_identity=journal_identity)
    _remove_journal_auth(durable_fs, journal_path, journal_bytes)


class _InjectedFault(RuntimeError):
    def __init__(self, point: str, action: str) -> None:
        super().__init__(point)
        self.point = point
        self.action = action


def _run_fault(fault: Callable[[str], Any] | None, point: str) -> None:
    if fault is None:
        return
    action = fault(point)
    if action in ("fail", "crash"):
        raise _InjectedFault(point, action)
    if action not in (None, False):
        raise ValueError(f"invalid fault action {action!r}")


def _recovery_command(manifest_path: Path, target_root: Path) -> str:
    return shlex.join((sys.executable, str(Path(__file__).resolve()),
                       str(manifest_path), str(target_root)))


def install_runtime_map_data(
    manifest_path: Path | str,
    target_root: Path | str,
    *,
    durable_fs: Any | None = None,
    fault: Callable[[str], Any] | None = None,
) -> str:
    manifest_path = Path(manifest_path)
    target_root = Path(target_root)
    if not target_root.exists() or target_root.is_symlink() or not target_root.is_dir():
        raise InstallError("runtime target must be an existing physical directory")
    durable_fs = durable_fs or _default_durable_fs()
    lock_path = target_root / _LOCK_NAME
    retired_lock = target_root / _RETIRED_LOCK_NAME
    journal_path = target_root / _JOURNAL_NAME
    retired_journal = target_root / _RETIRED_JOURNAL_NAME
    command = _recovery_command(manifest_path, target_root)
    recovery_context = {"command": command}

    def recovery_evidence() -> tuple[Path, ...]:
        candidates = {journal_path, retired_journal, lock_path, retired_lock}
        try:
            candidates.update(
                path for path in target_root.iterdir()
                if path.name.startswith(".garner-runtime-install."))
        except OSError:
            pass
        return tuple(sorted(
            (path for path in candidates
             if path.exists() or path.is_symlink()),
            key=lambda path: path.as_posix()))
    try:
        lock = durable_fs.lock_exclusive(lock_path, retired_lock, fault=fault)
    except (OSError, DurableFsError) as exc:
        raise InstallError(str(exc)) from exc
    try:
        _recover_transaction(
            durable_fs, target_root, journal_path, retired_journal, fault,
            recovery_context)
        runtime_contract = _validate_runtime_contract(target_root)
        validated = validate_manifest(manifest_path)
        _run_fault(fault, "INSTALL_AFTER_MANIFEST_VALIDATE")
        post_validate_identities = {
            key: _physical_identity(path)
            for key, path in validated.paths.items()
        }
        if (post_validate_identities != validated.identities or
                len(set(post_validate_identities.values())) != len(_LEAVES)):
            raise InstallError(
                "run output physical identity changed after manifest validation")
        sources = {
            "block": validated.paths["block"],
            "region": validated.paths["region"],
            "metadata": validated.paths["terrainMetadata"],
        }
        targets = {
            "block": target_root / "garner.block.raw",
            "region": target_root / "garner.region.raw",
            "metadata": target_root / "garner.terrain.json",
        }
        intended: dict[str, tuple[str, int, int]] = {}
        for key, source in sources.items():
            manifest_key = _MANIFEST_FILE_KEYS[key]
            manifest_file = validated.data["files"][manifest_key]
            runtime_file = runtime_contract["files"][manifest_key]
            expected_hash = manifest_file["sha256"]
            expected_size = manifest_file["sizeBytes"]
            if (runtime_file["sha256"] != expected_hash or
                    runtime_file["sizeBytes"] != expected_size):
                raise InstallError(
                    f"runtime contract disagrees with retained {manifest_key}")
            info = _physical_regular(source, f"/files/{manifest_key}/path")
            if info.st_size != expected_size or _sha256_file(source) != expected_hash:
                raise InstallError(
                    f"source changed after manifest validation: {source}")
            intended[key] = (
                expected_hash, _observable_mode(info), expected_size)
        if all(
            _path_state(targets[key])[:3] ==
            (True, intended[key][0], intended[key][1])
            for key in _INSTALL_KEYS
        ):
            durable_fs.retire_lock(lock, fault)
            return "NOOP"

        transaction_id = f"{time.monotonic_ns():016x}-{os.getpid():08x}"
        journal: dict[str, Any] = {
            "version": 1,
            "phase": "SNAPSHOT",
            "transactionId": transaction_id,
            "manifestPath": manifest_path.as_posix(),
            "manifestSha256": hashlib.sha256(validated.raw).hexdigest(),
            "recoveryCommand": command,
            "journalRetired": retired_journal.as_posix(),
            "entries": {},
        }
        for key in _INSTALL_KEYS:
            target = targets[key]
            exists, digest, mode, _ = _path_state(target)
            journal["entries"][key] = {
                "target": target.as_posix(),
                "stage": (target_root /
                          f".garner-runtime-install.stage.{transaction_id}.{key}").as_posix(),
                "backup": ((target_root /
                            f".garner-runtime-install.backup.{transaction_id}.{key}").as_posix()
                           if exists else ""),
                "stageReservation": (target_root /
                    f".garner-runtime-install.finalize.{transaction_id}.{key}.stage").as_posix(),
                "backupReservation": (target_root /
                    f".garner-runtime-install.finalize.{transaction_id}.{key}.backup").as_posix(),
                "rollbackStage": (target_root /
                    f".garner-runtime-install.rollback.{transaction_id}.{key}").as_posix(),
                "rollbackReservation": (target_root /
                    f".garner-runtime-install.finalize.{transaction_id}.{key}.rollback").as_posix(),
                "targetTombstone": (target_root /
                    f".garner-runtime-install.tombstone.{transaction_id}.{key}.target").as_posix(),
                "stageTombstone": (target_root /
                    f".garner-runtime-install.tombstone.{transaction_id}.{key}.stage").as_posix(),
                "backupTombstone": (target_root /
                    f".garner-runtime-install.tombstone.{transaction_id}.{key}.backup").as_posix(),
                "rollbackTombstone": (target_root /
                    f".garner-runtime-install.tombstone.{transaction_id}.{key}.rollback").as_posix(),
                "priorExists": exists,
                "priorSha256": digest,
                "priorMode": mode,
                "priorIdentity": list(_physical_identity(target)) if exists else [],
                "intendedSha256": intended[key][0],
                "intendedMode": intended[key][1],
                "intendedSize": intended[key][2],
                "stageIdentity": [],
                "backupIdentity": [],
                "rollbackIdentity": [],
            }
        journal_identity, journal_bytes = _write_journal(
            durable_fs, journal_path, journal)
        committed = False
        try:
            for key, point in (
                ("block", "INSTALL_AFTER_BLOCK_STAGE_FLUSH"),
                ("region", "INSTALL_AFTER_REGION_STAGE_FLUSH"),
                ("metadata", "INSTALL_AFTER_METADATA_STAGE_FLUSH"),
            ):
                entry = journal["entries"][key]
                stage = Path(entry["stage"])
                source_info = _physical_regular(
                    sources[key], f"/files/{_MANIFEST_FILE_KEYS[key]}/path")
                if (source_info.st_size != entry["intendedSize"] or
                        _sha256_file(sources[key]) != entry["intendedSha256"]):
                    raise InstallError(
                        f"source changed after manifest validation: {sources[key]}")
                _copy_owned(durable_fs, sources[key], stage, entry["intendedMode"])
                if (_physical_identity(sources[key]) !=
                        validated.identities[_MANIFEST_FILE_KEYS[key]] or
                        _sha256_file(stage) != entry["intendedSha256"] or
                        _observable_mode(stage.stat()) != entry["intendedMode"]):
                    raise DurableFsError(f"stage verification failed {stage}")
                entry["stageIdentity"] = list(_physical_identity(stage))
                journal_identity, journal_bytes = _write_journal(
                    durable_fs, journal_path, journal)
                durable_fs.flush_file(stage)
                _run_fault(fault, point)
            for key in _INSTALL_KEYS:
                entry = journal["entries"][key]
                if entry["priorExists"]:
                    backup = Path(entry["backup"])
                    if not _matches_prior_snapshot(entry):
                        raise DurableFsError(
                            f"runtime target changed before backup "
                            f"{entry['target']}")
                    _copy_owned(durable_fs, Path(entry["target"]), backup,
                                entry["priorMode"])
                    if not _matches_prior_snapshot(entry):
                        raise DurableFsError(
                            f"runtime target changed while copying backup "
                            f"{entry['target']}")
                    backup_identity = _physical_identity(backup)
                    if not _verify_entry(
                            {**entry, "target": backup.as_posix()}, False,
                            backup_identity):
                        raise DurableFsError(f"backup verification failed {backup}")
                    entry["backupIdentity"] = list(backup_identity)
                    journal_identity, journal_bytes = _write_journal(
                        durable_fs, journal_path, journal)
                    durable_fs.flush_file(backup)
            entries_to_finalize = []
            for key, entry in journal["entries"].items():
                entries_to_finalize.append(DurableEntryToFinalize(
                    Path(entry["stage"]), Path(entry["stageReservation"]),
                    "finalize-install-stage", transaction_id,
                    entry["intendedSha256"], tuple(entry["stageIdentity"]),
                    entry["intendedMode"]))
                if entry["priorExists"]:
                    entries_to_finalize.append(DurableEntryToFinalize(
                        Path(entry["backup"]), Path(entry["backupReservation"]),
                        "finalize-install-backup", transaction_id,
                        entry["priorSha256"], tuple(entry["backupIdentity"]),
                        entry["priorMode"]))
            durable_fs.sync_directory_or_equivalent(
                target_root, entries_to_finalize)
            for entry in journal["entries"].values():
                if not _verify_entry(
                        {**entry, "target": entry["stage"]}, True,
                        entry["stageIdentity"]):
                    raise DurableFsError(
                        f"stage verification failed after durability barrier "
                        f"{entry['stage']}")
                if (entry["priorExists"] and not _verify_entry(
                        {**entry, "target": entry["backup"]}, False,
                        entry["backupIdentity"])):
                    raise DurableFsError(
                        f"backup verification failed after durability barrier "
                        f"{entry['backup']}")
            journal_identity, journal_bytes = _write_journal(
                durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_BACKUPS_DURABILITY_BARRIER")
            journal["phase"] = "PREPARED"
            journal_identity, journal_bytes = _write_journal(
                durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE")

            for key in _INSTALL_KEYS:
                upper_key = key.upper()
                _run_fault(fault, f"INSTALL_BEFORE_{upper_key}_REPLACE")
                if not _matches_prior_snapshot(journal["entries"][key]):
                    raise DurableFsError(
                        f"runtime {key} target changed before replacement")
                durable_fs.replace_same_volume(
                    Path(journal["entries"][key]["stage"]), targets[key])
                _run_fault(fault, f"INSTALL_AFTER_{upper_key}_REPLACE")
                durable_fs.sync_directory_or_equivalent(target_root)
                _run_fault(
                    fault,
                    f"INSTALL_AFTER_{upper_key}_REPLACE_DURABILITY_BARRIER")
                journal["phase"] = f"{upper_key}_REPLACED"
                journal_identity, journal_bytes = _write_journal(
                    durable_fs, journal_path, journal)
                _run_fault(
                    fault,
                    f"INSTALL_AFTER_{upper_key}_REPLACED_JOURNAL_DURABLE")

            journal["phase"] = "TRIPLE_REPLACED"
            journal_identity, journal_bytes = _write_journal(
                durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE")
            if not all(_verify_entry(
                    journal["entries"][key], True,
                    journal["entries"][key]["stageIdentity"])
                       for key in _INSTALL_KEYS):
                raise DurableFsError("installed navigation triple verification failed")
            _run_fault(fault, "INSTALL_AFTER_PAIR_VERIFY")
            journal["phase"] = "COMMITTED"
            journal_identity, journal_bytes = _write_journal(
                durable_fs, journal_path, journal)
            committed = True
            _run_fault(fault, "INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE")
            for entry in journal["entries"].values():
                backup = entry["backup"]
                if backup and (
                        Path(backup).exists() or Path(backup).is_symlink() or
                        Path(entry["backupTombstone"]).exists() or
                        Path(entry["backupTombstone"]).is_symlink()):
                    _remove_recorded_owned(
                        durable_fs, Path(backup), Path(entry["backupTombstone"]),
                        transaction_id=transaction_id,
                        kind="remove-owned-backup",
                        expected_hash=entry["priorSha256"],
                        expected_mode=entry["priorMode"],
                        expected_identity=entry["backupIdentity"],
                        fault=fault,
                        fault_point="INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE",
                    )
            reserve_journal = getattr(durable_fs, "reserve_move_target", None)
            _ensure_journal_auth(durable_fs, journal_path, journal_bytes)
            if reserve_journal is not None:
                reserve_journal(
                    journal_path, retired_journal, "retire-install-journal",
                    transaction_id, hashlib.sha256(journal_bytes).hexdigest())
            durable_fs.replace_same_volume(journal_path, retired_journal)
            durable_fs.sync_directory_or_equivalent(target_root)
            _run_fault(fault, "INSTALL_AFTER_JOURNAL_RETIRE")
            if (_physical_identity(retired_journal) != journal_identity or
                    retired_journal.read_bytes() != journal_bytes or
                    _observable_mode(retired_journal.lstat()) != _private_mode()):
                raise DurableFsError(
                    f"owned journal identity/hash/mode mismatch: {retired_journal}")
            durable_fs.remove_owned(
                retired_journal,
                source_hash=hashlib.sha256(journal_bytes).hexdigest(),
                expected_mode=_private_mode(),
                expected_identity=journal_identity)
            _remove_journal_auth(durable_fs, journal_path, journal_bytes)
            durable_fs.retire_lock(lock, fault)
            return "OK"
        except _InjectedFault as exc:
            if exc.action == "crash" or committed:
                durable_fs.abandon_lock(lock)
                evidence = ((lock_path, retired_lock)
                            if exc.point.startswith("INSTALL_AFTER_LOCK_")
                            else (journal_path, retired_journal))
                evidence = tuple(path for path in evidence
                                 if path.exists() or path.is_symlink())
                raise InstallError(
                    f"injected fault {exc.point}",
                    status="RECOVERY_REQUIRED" if committed else "WRITE_FAILED",
                    recovery_paths=evidence,
                    recovery_command=command,
                ) from exc
            try:
                _recover_transaction(
                    durable_fs, target_root, journal_path, retired_journal, fault)
                durable_fs.retire_lock(lock, fault)
            except (InstallError, DurableFsError, OSError) as recovery_exc:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    f"injected fault {exc.point}; rollback failed: {recovery_exc}",
                    status="RECOVERY_REQUIRED",
                    recovery_paths=recovery_evidence(),
                    recovery_command=command,
                ) from recovery_exc
            raise InstallError(f"injected fault {exc.point}") from exc
        except (InstallError, DurableFsError, OSError) as exc:
            if (isinstance(exc, InstallError) and
                    exc.status == "RECOVERY_REQUIRED"):
                durable_fs.abandon_lock(lock)
                raise
            if committed:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    str(exc), status="RECOVERY_REQUIRED",
                    recovery_paths=recovery_evidence(),
                    recovery_command=command) from exc
            try:
                _recover_transaction(
                    durable_fs, target_root, journal_path, retired_journal, fault)
                durable_fs.retire_lock(lock, fault)
            except (InstallError, DurableFsError, OSError) as recovery_exc:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    f"{exc}; rollback failed: {recovery_exc}",
                    status="RECOVERY_REQUIRED",
                    recovery_paths=recovery_evidence(),
                    recovery_command=command) from recovery_exc
            raise InstallError(str(exc)) from exc
    except InstallError as original:
        if original.status == "RECOVERY_REQUIRED":
            if not original.recovery_command:
                original.recovery_command = recovery_context["command"]
            original.recovery_paths = tuple(sorted(
                set(original.recovery_paths) | set(recovery_evidence()),
                key=lambda path: path.as_posix()))
        lock_is_open = (
            getattr(lock, "descriptor", -1) >= 0 or
            getattr(lock, "handle", None) is not None
        )
        if lock_is_open:
            try:
                durable_fs.retire_lock(lock, fault)
            except _InjectedFault as exc:
                durable_fs.abandon_lock(lock)
                evidence = recovery_evidence()
                raise InstallError(
                    f"injected fault {exc.point} during lock retirement",
                    status="RECOVERY_REQUIRED", recovery_paths=evidence,
                    recovery_command=command) from exc
            except (OSError, DurableFsError) as exc:
                durable_fs.abandon_lock(lock)
                evidence = recovery_evidence()
                raise InstallError(
                    f"{original.detail}; lock retirement failed: {exc}",
                    status="RECOVERY_REQUIRED", recovery_paths=evidence,
                    recovery_command=command) from exc
        raise
    except _InjectedFault as exc:
        durable_fs.abandon_lock(lock)
        evidence = recovery_evidence()
        raise InstallError(
            f"injected fault {exc.point}", status="RECOVERY_REQUIRED",
            recovery_paths=evidence,
            recovery_command=recovery_context["command"]) from exc
    except (DurableFsError, OSError) as exc:
        durable_fs.abandon_lock(lock)
        evidence = recovery_evidence()
        raise InstallError(
            str(exc), status="RECOVERY_REQUIRED", recovery_paths=evidence,
            recovery_command=recovery_context["command"]) from exc
    except Exception as exc:
        durable_fs.abandon_lock(lock)
        evidence = recovery_evidence()
        raise InstallError(
            f"unexpected post-lock exception: {type(exc).__name__}: {exc}",
            status="RECOVERY_REQUIRED", recovery_paths=evidence,
            recovery_command=recovery_context["command"]) from exc
    except BaseException:
        durable_fs.abandon_lock(lock)
        raise


def compare_deterministic_manifests(
    first: Path | str, second: Path | str
) -> list[str]:
    try:
        first_valid = validate_manifest(first)
        second_valid = validate_manifest(second)
    except InstallError as exc:
        return [str(exc)]
    if first_valid.run_id == second_valid.run_id:
        return ["determinism comparison requires distinct run IDs"]
    for key in _LEAVES:
        first_file = first_valid.data["files"][key]
        second_file = second_valid.data["files"][key]
        if ((first_file["sha256"], first_file["sizeBytes"]) !=
                (second_file["sha256"], second_file["sizeBytes"]) or
                first_valid.paths[key].read_bytes() !=
                second_valid.paths[key].read_bytes()):
            return [f"corresponding run product differs: {key}"]
    first_projection = copy.deepcopy(first_valid.data)
    second_projection = copy.deepcopy(second_valid.data)
    for projection in (first_projection, second_projection):
        for item in projection["files"].values():
            parts = PurePosixPath(item["path"]).parts
            item["path"] = PurePosixPath("runs", "<run-id>", parts[2]).as_posix()
        projection["metrics"]["peakRssBytes"] = 0
    if _canonical_json(first_projection) != _canonical_json(second_projection):
        return ["manifests differ outside run ID and peak RSS"]
    return []


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if len(arguments) != 2:
        print(
            "usage: install_runtime_map_data.py <manifest> <runtime-height-dir>",
            file=sys.stderr,
        )
        return 2
    try:
        result = install_runtime_map_data(Path(arguments[0]), Path(arguments[1]))
    except InstallError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
