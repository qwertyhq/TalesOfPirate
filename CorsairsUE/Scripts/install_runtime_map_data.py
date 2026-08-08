#!/usr/bin/env python3
"""Durably install the two Garner runtime terrain files from a Task 7 manifest."""

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
_LOCK_NAME = ".garner-runtime-install.lock"
_RETIRED_LOCK_NAME = _LOCK_NAME + ".retired"


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
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        raise InstallError(f"invalid manifest JSON: {exc}") from exc
    if type(value) is not dict:
        raise InstallError("invalid manifest JSON: root must be an object")
    return value, raw


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
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected finite number")
    return float(value)


def _string(value: Any, pointer: str) -> str:
    if type(value) is not str:
        raise InstallError(f"INVALID_SCHEMA {pointer}: expected string")
    return value


def _hash(value: Any, pointer: str) -> str:
    text = _string(value, pointer)
    if not _SHA256.fullmatch(text):
        raise InstallError(f"INVALID_HASH {pointer}: expected lowercase SHA-256")
    return text


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
    run_id: str
    raw: bytes


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
    aliases: set[str] = set()
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
    return ValidatedManifest(data, resolved, run_id, raw)


def _json_path(path: Path) -> str:
    return json.dumps(path.as_posix(), ensure_ascii=False, separators=(",", ":"))


class PosixDurableFs:
    def _error(self, operation: str, path: Path, native: int) -> DurableFsError:
        return DurableFsError(
            f"DURABLE_FS_ERROR op={operation} path={_json_path(path)} "
            f"native=errno:{native}:")

    def open_exclusive_temp(self, path: Path) -> None:
        flags = os.O_CREAT | os.O_EXCL | os.O_RDWR
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
            os.close(descriptor)
        except OSError as exc:
            raise self._error("OpenExclusiveTemp", path, exc.errno or errno.EIO) from exc

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

    def sync_directory_or_equivalent(self, directory: Path) -> None:
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

    def remove_owned(self, path: Path) -> None:
        try:
            info = path.lstat()
        except FileNotFoundError:
            return
        except OSError as exc:
            raise self._error("RemoveOwned", path, exc.errno or errno.EIO) from exc
        if not stat.S_ISREG(info.st_mode) or path.is_symlink():
            raise self._error("RemoveOwned", path, errno.EINVAL)
        try:
            os.unlink(path)
            self.sync_directory_or_equivalent(path.parent)
        except OSError as exc:
            raise self._error("RemoveOwned", path, exc.errno or errno.EIO) from exc
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

    def lock_exclusive(self, canonical: Path, retired: Path) -> "_PosixLock":
        marker = f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode()
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        for _ in range(32):
            try:
                descriptor = os.open(canonical, flags, 0o600)
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                handle_info = os.fstat(descriptor)
                if (not stat.S_ISREG(handle_info.st_mode) or
                        handle_info.st_nlink != 1):
                    raise OSError(errno.EMLINK, "lock must be a no-link regular file")
                if handle_info.st_size == 0:
                    os.ftruncate(descriptor, 0)
                    os.pwrite(descriptor, marker, 0)
                    os.fsync(descriptor)
                    handle_info = os.fstat(descriptor)
                if os.pread(descriptor, len(marker), 0) != marker:
                    raise OSError(errno.EINVAL, "lock marker mismatch")
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
                    self.remove_owned(retired)
                return _PosixLock(canonical, retired, descriptor, marker)
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

    def retire_lock(self, lock: "_PosixLock") -> None:
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
            self.replace_same_volume(lock.canonical, lock.retired)
            self.sync_directory_or_equivalent(lock.canonical.parent)
            self.remove_owned(lock.retired)
        except (OSError, DurableFsError):
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
        if (not self.kernel32.GetFileInformationByHandleEx(
                handle, 9, self.ctypes.byref(tag), self.ctypes.sizeof(tag)) or
                tag.FileAttributes & self.FILE_ATTRIBUTE_REPARSE_POINT):
            native = self.ctypes.get_last_error() or 4390
            self.kernel32.CloseHandle(handle)
            raise self._error(operation, path, native)
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
            original_identity = self._identity(handle, reserved)
            written = self.wintypes.DWORD()
            buffer = self.ctypes.create_string_buffer(payload)
            if (not self.kernel32.WriteFile(
                    handle, buffer, len(payload), self.ctypes.byref(written),
                    None) or written.value != len(payload)):
                native = self.ctypes.get_last_error()
                raise self._error("ReserveMoveTarget", reserved, native)
            if not self.kernel32.FlushFileBuffers(handle):
                native = self.ctypes.get_last_error()
                raise self._error("ReserveMoveTarget", reserved, native)
        finally:
            self.kernel32.CloseHandle(handle)

        reopened = self._open(
            reserved, self.OPEN_EXISTING, self.GENERIC_READ, 0,
            "ReserveMoveTarget")
        try:
            if self._identity(reopened, reserved) != original_identity:
                raise self._error("ReserveMoveTarget", reserved, 1168)
            read = self.wintypes.DWORD()
            readback = self.ctypes.create_string_buffer(len(payload))
            if (not self.kernel32.ReadFile(
                    reopened, readback, len(payload),
                    self.ctypes.byref(read), None) or
                    read.value != len(payload) or readback.raw != payload):
                raise self._error("ReserveMoveTarget", reserved, 13)
        finally:
            self.kernel32.CloseHandle(reopened)
        return payload

    def _identity(self, handle: Any, path: Path) -> tuple[int, int, int]:
        information = self.ByHandleFileInformation()
        if not self.kernel32.GetFileInformationByHandle(
                handle, self.ctypes.byref(information)):
            native = self.ctypes.get_last_error()
            raise self._error("LockExclusive", path, native)
        return (information.VolumeSerialNumber,
                information.FileIndexHigh, information.FileIndexLow)

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

    def open_exclusive_temp(self, path: Path) -> None:
        handle = self._open(
            path, self.CREATE_NEW, self.GENERIC_READ | self.GENERIC_WRITE, 0,
            "OpenExclusiveTemp")
        self.kernel32.CloseHandle(handle)

    def flush_file(self, path: Path) -> None:
        handle = self._open(
            path, self.OPEN_EXISTING, self.GENERIC_READ | self.GENERIC_WRITE,
            self.FILE_SHARE_READ)
        if not self.kernel32.FlushFileBuffers(handle):
            native = self.ctypes.get_last_error()
            self.kernel32.CloseHandle(handle)
            raise self._error("FlushFile", path, native)
        self.kernel32.CloseHandle(handle)

    def replace_same_volume(self, source: Path, destination: Path) -> None:
        if self._volume_serial(source) != self._volume_serial(destination.parent):
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

    def sync_directory_or_equivalent(self, directory: Path) -> None:
        # A preceding MOVEFILE_WRITE_THROUGH is the Windows entry barrier.
        del directory

    def remove_owned(self, path: Path) -> None:
        attributes = self.kernel32.GetFileAttributesW(str(path))
        if attributes == 0xFFFFFFFF:
            native = self.ctypes.get_last_error()
            if native in (2, 3):
                return
            raise self._error("RemoveOwned", path, native)
        if attributes & self.FILE_ATTRIBUTE_REPARSE_POINT:
            raise self._error("RemoveOwned", path, 4390)
        if attributes & self.FILE_ATTRIBUTE_READONLY:
            updated = attributes & ~self.FILE_ATTRIBUTE_READONLY
            if updated == 0:
                updated = self.FILE_ATTRIBUTE_NORMAL
            if not self.kernel32.SetFileAttributesW(str(path), updated):
                native = self.ctypes.get_last_error()
                raise self._error("RemoveOwned", path, native)
        if not self.kernel32.DeleteFileW(str(path)):
            native = self.ctypes.get_last_error()
            raise DurableFsError(
                f"DURABLE_FS_ERROR op=RemoveOwned path={_json_path(path)} "
                f"native=win32:{native}:")

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

    def lock_exclusive(self, canonical: Path, retired: Path) -> "_WindowsLock":
        marker = f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode()
        handle = self._open(
            canonical, self.OPEN_ALWAYS, self.GENERIC_READ | self.GENERIC_WRITE,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE | self.FILE_SHARE_DELETE)
        overlap = self.Overlapped()
        if not self.kernel32.LockFileEx(
                handle, self.LOCKFILE_EXCLUSIVE_LOCK |
                self.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                self.ctypes.byref(overlap)):
            native = self.ctypes.get_last_error()
            self.kernel32.CloseHandle(handle)
            raise self._error("LockExclusive", canonical, native)
        size = self.ctypes.c_longlong()
        if not self.kernel32.GetFileSizeEx(handle, self.ctypes.byref(size)):
            native = self.ctypes.get_last_error()
            self.kernel32.CloseHandle(handle)
            raise self._error("LockExclusive", canonical, native)
        if size.value == 0:
            written = self.wintypes.DWORD()
            marker_buffer = self.ctypes.create_string_buffer(marker)
            if (not self.kernel32.WriteFile(
                    handle, marker_buffer, len(marker), self.ctypes.byref(written),
                    None) or written.value != len(marker) or
                    not self.kernel32.FlushFileBuffers(handle)):
                native = self.ctypes.get_last_error()
                self.kernel32.CloseHandle(handle)
                raise self._error("LockExclusive", canonical, native)
        if not self.kernel32.SetFilePointerEx(handle, 0, None, 0):
            native = self.ctypes.get_last_error()
            self.kernel32.CloseHandle(handle)
            raise self._error("LockExclusive", canonical, native)
        read = self.wintypes.DWORD()
        readback = self.ctypes.create_string_buffer(len(marker))
        if (not self.kernel32.ReadFile(
                handle, readback, len(marker), self.ctypes.byref(read), None) or
                read.value != len(marker) or readback.raw != marker):
            self.kernel32.CloseHandle(handle)
            raise self._error("LockExclusive", canonical, 13)
        fresh = self._open(
            canonical, self.OPEN_EXISTING, self.GENERIC_READ,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE | self.FILE_SHARE_DELETE)
        try:
            if self._identity(handle, canonical) != self._identity(fresh, canonical):
                raise self._error("LockExclusive", canonical, 1168)
        finally:
            self.kernel32.CloseHandle(fresh)
        identity = self._identity(handle, canonical)
        attributes = self.kernel32.GetFileAttributesW(str(retired))
        if attributes != self.INVALID_FILE_ATTRIBUTES:
            if attributes & self.FILE_ATTRIBUTE_REPARSE_POINT:
                self.kernel32.CloseHandle(handle)
                raise self._error("LockExclusive", retired, 4390)
            retired_bytes = retired.read_bytes()
            generation = "-".join(f"{part:08x}" for part in identity)
            expected_reservation = self._reservation_bytes(
                canonical, retired, "retire-lock", generation,
                hashlib.sha256(marker).hexdigest())
            if retired_bytes == expected_reservation:
                self.remove_owned(retired)
            elif retired_bytes == marker:
                old = self._open(
                    retired, self.OPEN_EXISTING, self.GENERIC_READ,
                    self.FILE_SHARE_READ | self.FILE_SHARE_WRITE |
                    self.FILE_SHARE_DELETE, "LockExclusive")
                try:
                    if self._identity(old, retired) == identity:
                        raise self._error("LockExclusive", retired, 13)
                finally:
                    self.kernel32.CloseHandle(old)
                self.remove_owned(retired)
            else:
                self.kernel32.CloseHandle(handle)
                raise self._error("LockExclusive", retired, 13)
        else:
            native = self.ctypes.get_last_error()
            if native not in (2, 3):
                self.kernel32.CloseHandle(handle)
                raise self._error("LockExclusive", retired, native)
        return _WindowsLock(canonical, retired, handle, marker, identity)

    def abandon_lock(self, lock: "_WindowsLock") -> None:
        if lock.handle is not None:
            self.kernel32.CloseHandle(lock.handle)
            lock.handle = None

    def retire_lock(self, lock: "_WindowsLock") -> None:
        fresh = self._open(
            lock.canonical, self.OPEN_EXISTING, self.GENERIC_READ,
            self.FILE_SHARE_READ | self.FILE_SHARE_WRITE | self.FILE_SHARE_DELETE)
        try:
            if self._identity(fresh, lock.canonical) != lock.identity:
                raise self._error("RetireLock", lock.canonical, 1168)
        finally:
            self.kernel32.CloseHandle(fresh)
        generation = "-".join(f"{part:08x}" for part in lock.identity)
        self.reserve_move_target(
            lock.canonical, lock.retired, "retire-lock", generation,
            hashlib.sha256(lock.marker).hexdigest())
        self.replace_same_volume(lock.canonical, lock.retired)
        attributes = self.kernel32.GetFileAttributesW(str(lock.canonical))
        if attributes != self.INVALID_FILE_ATTRIBUTES:
            self.abandon_lock(lock)
            raise self._error("RetireLock", lock.canonical, 13)
        native = self.ctypes.get_last_error()
        if native not in (2, 3):
            self.abandon_lock(lock)
            raise self._error("RetireLock", lock.canonical, native)
        if self._identity(lock.handle, lock.retired) != lock.identity:
            self.abandon_lock(lock)
            raise self._error("RetireLock", lock.retired, 1168)
        if not self.kernel32.DeleteFileW(str(lock.retired)):
            native = self.ctypes.get_last_error()
            self.abandon_lock(lock)
            raise self._error("RetireLock", lock.retired, native)
        self.abandon_lock(lock)  # CloseHandle is the final filesystem call


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
    durable_fs.open_exclusive_temp(path)
    with path.open("r+b") as output:
        output.write(payload)
        output.flush()
    durable_fs.restore_mode(path, mode)
    durable_fs.flush_file(path)


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


def _parse_journal(raw: bytes, target_root: Path) -> dict[str, Any]:
    try:
        journal = json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_pairs)
    except (UnicodeError, ValueError, json.JSONDecodeError) as exc:
        raise InstallError(f"invalid install journal: {exc}", status="RECOVERY_REQUIRED") from exc
    expected = {"version", "phase", "transactionId", "manifestPath",
                "manifestSha256", "entries"}
    _require_keys(journal, expected, "/journal")
    if _integer(journal["version"], "/journal/version") != 1:
        raise InstallError("invalid install journal version", status="RECOVERY_REQUIRED")
    if journal["phase"] not in (
        "SNAPSHOT", "PREPARED", "BLOCK_REPLACED", "PAIR_REPLACED", "COMMITTED"
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
    entries = _require_keys(journal["entries"], {"block", "metadata"},
                            "/journal/entries")
    for key, entry_value in entries.items():
        entry = _require_keys(
            entry_value,
            {"target", "stage", "backup", "priorExists", "priorSha256",
             "priorMode", "intendedSha256", "intendedMode"},
            f"/journal/entries/{key}",
        )
        for path_key in ("target", "stage"):
            path = Path(_string(entry[path_key], f"/journal/entries/{key}/{path_key}"))
            if not _direct_child(target_root, path):
                raise InstallError("install journal path escape", status="RECOVERY_REQUIRED")
        expected_target = target_root / (
            "garner.block.raw" if key == "block" else "garner.terrain.json")
        expected_stage = target_root / (
            f".garner-runtime-install.stage.{transaction_id}.{key}")
        if (Path(entry["target"]) != expected_target or
                Path(entry["stage"]) != expected_stage):
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
        elif backup_text or entry["priorSha256"] or entry["priorMode"] != 0:
            raise InstallError("invalid absent-prior journal", status="RECOVERY_REQUIRED")
        _hash(entry["intendedSha256"], f"/journal/entries/{key}/intendedSha256")
        _integer(entry["intendedMode"], f"/journal/entries/{key}/intendedMode")
    return journal


def _write_journal(durable_fs: Any, canonical: Path, journal: dict[str, Any]) -> None:
    update = canonical.parent / (
        f".garner-runtime-install.journal.{journal['transactionId']}.{time.monotonic_ns()}")
    payload = _journal_bytes(journal)
    _write_owned(durable_fs, update, payload, _private_mode())
    if _parse_journal(update.read_bytes(), canonical.parent) != journal:
        raise DurableFsError("journal readback mismatch")
    durable_fs.replace_same_volume(update, canonical)
    durable_fs.sync_directory_or_equivalent(canonical.parent)


def _verify_entry(entry: dict[str, Any], intended: bool) -> bool:
    target = Path(entry["target"])
    exists, digest, mode, _ = _path_state(target)
    if not exists:
        return False
    prefix = "intended" if intended else "prior"
    return digest == entry[f"{prefix}Sha256"] and mode == entry[f"{prefix}Mode"]


def _recover_transaction(
    durable_fs: Any,
    target_root: Path,
    journal_path: Path,
    retired_journal: Path,
    fault: Callable[[str], Any] | None = None,
) -> None:
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
    if not canonical_exists and not retired_exists:
        return
    if canonical_exists and retired_exists:
        raise InstallError(
            "both canonical and retired install journals exist",
            status="RECOVERY_REQUIRED", recovery_paths=(journal_path, retired_journal))
    source = journal_path if canonical_exists else retired_journal
    _physical_regular(source, "/journal")
    journal = _parse_journal(source.read_bytes(), target_root)
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
        if not all(_verify_entry(entry, True) for entry in entries.values()):
            raise InstallError(
                "committed runtime pair does not match journal",
                status="RECOVERY_REQUIRED", recovery_paths=(source,))
    else:
        for key in ("block", "metadata"):
            entry = entries[key]
            target = Path(entry["target"])
            exists, digest, mode, _ = _path_state(target)
            if entry["priorExists"]:
                if exists and digest == entry["priorSha256"] and mode == entry["priorMode"]:
                    continue
                if not exists or digest != entry["intendedSha256"]:
                    raise InstallError(
                        f"uncertain runtime target {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                backup = Path(entry["backup"])
                backup_exists, backup_hash, backup_mode, _ = _path_state(backup)
                if (not backup_exists or backup_hash != entry["priorSha256"] or
                        backup_mode != entry["priorMode"]):
                    raise InstallError(
                        f"invalid runtime backup {backup}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source, backup))
                rollback = target_root / (
                    f".garner-runtime-install.rollback.{journal['transactionId']}.{key}")
                _copy_owned(durable_fs, backup, rollback, entry["priorMode"])
                recovery_fault(
                    "INSTALL_RECOVERY_AFTER_MODE_STAGE_FLUSH", (source, backup))
                recovery_fault(
                    "INSTALL_RECOVERY_BLOCK_REPLACE" if key == "block"
                    else "INSTALL_RECOVERY_METADATA_REPLACE",
                    (source, backup),
                )
                durable_fs.replace_same_volume(rollback, target)
                durable_fs.sync_directory_or_equivalent(target_root)
                recovery_fault(
                    "INSTALL_RECOVERY_DURABILITY_BARRIER", (source, backup))
                if not _verify_entry(entry, False):
                    raise InstallError(
                        f"runtime rollback verification failed {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source, backup))
                recovery_fault("INSTALL_RECOVERY_VERIFY", (source, backup))
            elif exists:
                if digest != entry["intendedSha256"]:
                    raise InstallError(
                        f"unexpected absent-prior target {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                recovery_fault(
                    "INSTALL_RECOVERY_BLOCK_REPLACE" if key == "block"
                    else "INSTALL_RECOVERY_METADATA_REPLACE",
                    (source,),
                )
                durable_fs.remove_owned(target)
                durable_fs.sync_directory_or_equivalent(target_root)
                recovery_fault("INSTALL_RECOVERY_DURABILITY_BARRIER", (source,))
                if target.exists() or target.is_symlink():
                    raise InstallError(
                        f"runtime absent-prior rollback failed {target}",
                        status="RECOVERY_REQUIRED", recovery_paths=(source,))
                recovery_fault("INSTALL_RECOVERY_VERIFY", (source,))
    for entry in entries.values():
        for key in ("stage", "backup"):
            text = entry[key]
            if text:
                path = Path(text)
                if path.exists() or path.is_symlink():
                    durable_fs.remove_owned(path)
    if source == journal_path:
        durable_fs.replace_same_volume(journal_path, retired_journal)
        durable_fs.sync_directory_or_equivalent(target_root)
        source = retired_journal
    durable_fs.remove_owned(source)


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
    try:
        lock = durable_fs.lock_exclusive(lock_path, retired_lock)
    except (OSError, DurableFsError) as exc:
        raise InstallError(str(exc)) from exc
    try:
        _recover_transaction(
            durable_fs, target_root, journal_path, retired_journal, fault)
        validated = validate_manifest(manifest_path)
        sources = {
            "block": validated.paths["block"],
            "metadata": validated.paths["terrainMetadata"],
        }
        targets = {
            "block": target_root / "garner.block.raw",
            "metadata": target_root / "garner.terrain.json",
        }
        intended: dict[str, tuple[str, int]] = {}
        for key, source in sources.items():
            intended[key] = (_sha256_file(source), _observable_mode(source.stat()))
        if all(
            _path_state(targets[key])[:3] ==
            (True, intended[key][0], intended[key][1])
            for key in ("block", "metadata")
        ):
            durable_fs.retire_lock(lock)
            return "NOOP"

        transaction_id = f"{time.monotonic_ns():016x}-{os.getpid():08x}"
        journal: dict[str, Any] = {
            "version": 1,
            "phase": "SNAPSHOT",
            "transactionId": transaction_id,
            "manifestPath": manifest_path.as_posix(),
            "manifestSha256": hashlib.sha256(validated.raw).hexdigest(),
            "entries": {},
        }
        for key in ("block", "metadata"):
            target = targets[key]
            exists, digest, mode, _ = _path_state(target)
            journal["entries"][key] = {
                "target": target.as_posix(),
                "stage": (target_root /
                          f".garner-runtime-install.stage.{transaction_id}.{key}").as_posix(),
                "backup": ((target_root /
                            f".garner-runtime-install.backup.{transaction_id}.{key}").as_posix()
                           if exists else ""),
                "priorExists": exists,
                "priorSha256": digest,
                "priorMode": mode,
                "intendedSha256": intended[key][0],
                "intendedMode": intended[key][1],
            }
        _write_journal(durable_fs, journal_path, journal)
        committed = False
        try:
            for key, point in (
                ("block", "INSTALL_AFTER_BLOCK_STAGE_FLUSH"),
                ("metadata", "INSTALL_AFTER_METADATA_STAGE_FLUSH"),
            ):
                entry = journal["entries"][key]
                stage = Path(entry["stage"])
                _copy_owned(durable_fs, sources[key], stage, entry["intendedMode"])
                if (_sha256_file(stage) != entry["intendedSha256"] or
                        _observable_mode(stage.stat()) != entry["intendedMode"]):
                    raise DurableFsError(f"stage verification failed {stage}")
                _run_fault(fault, point)
            for key in ("block", "metadata"):
                entry = journal["entries"][key]
                if entry["priorExists"]:
                    backup = Path(entry["backup"])
                    _copy_owned(durable_fs, Path(entry["target"]), backup,
                                entry["priorMode"])
                    if not _verify_entry({**entry, "target": backup.as_posix()}, False):
                        raise DurableFsError(f"backup verification failed {backup}")
            durable_fs.sync_directory_or_equivalent(target_root)
            _run_fault(fault, "INSTALL_AFTER_BACKUPS_DURABILITY_BARRIER")
            journal["phase"] = "PREPARED"
            _write_journal(durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE")

            _run_fault(fault, "INSTALL_BEFORE_BLOCK_REPLACE")
            durable_fs.replace_same_volume(
                Path(journal["entries"]["block"]["stage"]), targets["block"])
            _run_fault(fault, "INSTALL_AFTER_BLOCK_REPLACE")
            durable_fs.sync_directory_or_equivalent(target_root)
            _run_fault(fault, "INSTALL_AFTER_BLOCK_REPLACE_DURABILITY_BARRIER")
            journal["phase"] = "BLOCK_REPLACED"
            _write_journal(durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE")

            _run_fault(fault, "INSTALL_BEFORE_METADATA_REPLACE")
            durable_fs.replace_same_volume(
                Path(journal["entries"]["metadata"]["stage"]), targets["metadata"])
            _run_fault(fault, "INSTALL_AFTER_METADATA_REPLACE")
            durable_fs.sync_directory_or_equivalent(target_root)
            _run_fault(fault, "INSTALL_AFTER_METADATA_REPLACE_DURABILITY_BARRIER")
            journal["phase"] = "PAIR_REPLACED"
            _write_journal(durable_fs, journal_path, journal)
            _run_fault(fault, "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE")
            if not all(_verify_entry(journal["entries"][key], True)
                       for key in ("block", "metadata")):
                raise DurableFsError("installed pair verification failed")
            _run_fault(fault, "INSTALL_AFTER_PAIR_VERIFY")
            journal["phase"] = "COMMITTED"
            _write_journal(durable_fs, journal_path, journal)
            committed = True
            _run_fault(fault, "INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE")
            _run_fault(fault, "INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE")
            for entry in journal["entries"].values():
                backup = entry["backup"]
                if backup and Path(backup).exists():
                    durable_fs.remove_owned(Path(backup))
            durable_fs.replace_same_volume(journal_path, retired_journal)
            durable_fs.sync_directory_or_equivalent(target_root)
            _run_fault(fault, "INSTALL_AFTER_JOURNAL_RETIRE")
            durable_fs.remove_owned(retired_journal)
            durable_fs.retire_lock(lock)
            return "OK"
        except _InjectedFault as exc:
            if exc.action == "crash" or committed:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    f"injected fault {exc.point}",
                    status="RECOVERY_REQUIRED" if committed else "WRITE_FAILED",
                    recovery_paths=(journal_path,),
                    recovery_command=command,
                ) from exc
            try:
                _recover_transaction(
                    durable_fs, target_root, journal_path, retired_journal, fault)
                durable_fs.retire_lock(lock)
            except (InstallError, DurableFsError, OSError) as recovery_exc:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    f"injected fault {exc.point}; rollback failed: {recovery_exc}",
                    status="RECOVERY_REQUIRED",
                    recovery_paths=(journal_path,),
                    recovery_command=command,
                ) from recovery_exc
            raise InstallError(f"injected fault {exc.point}") from exc
        except (DurableFsError, OSError) as exc:
            if committed:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    str(exc), status="RECOVERY_REQUIRED",
                    recovery_paths=(journal_path,), recovery_command=command) from exc
            try:
                _recover_transaction(
                    durable_fs, target_root, journal_path, retired_journal, fault)
                durable_fs.retire_lock(lock)
            except (InstallError, DurableFsError, OSError) as recovery_exc:
                durable_fs.abandon_lock(lock)
                raise InstallError(
                    f"{exc}; rollback failed: {recovery_exc}",
                    status="RECOVERY_REQUIRED", recovery_paths=(journal_path,),
                    recovery_command=command) from recovery_exc
            raise InstallError(str(exc)) from exc
    except InstallError:
        lock_is_open = (
            getattr(lock, "descriptor", -1) >= 0 or
            getattr(lock, "handle", None) is not None
        )
        if lock_is_open:
            try:
                durable_fs.retire_lock(lock)
            except (OSError, DurableFsError):
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
