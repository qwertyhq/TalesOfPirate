#!/usr/bin/env python3
"""Build, import, package, and durably attest the Garner reference terrain.

The module is intentionally importable: pure tests inject probes, runners and
faults without starting Unreal.  Production keeps one persistent flock and a
strict durable journal around every mutable Task 7/8 output.
"""

from __future__ import annotations

from dataclasses import dataclass
import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
from typing import Any, Callable, Iterable, Sequence


_SCRIPT_ROOT = Path(__file__).resolve().parents[1]
_UE_SCRIPTS = _SCRIPT_ROOT / "CorsairsUE/Scripts"
if str(_UE_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_UE_SCRIPTS))

import reference_terrain_rules as rules  # noqa: E402


TOP_MANIFEST = "artifacts/maps/garner.reference-albedo.json"
BASE_BUNDLE = "artifacts/maps/reports/garner-terrain-base.json"
LOCK_RELATIVE = "artifacts/maps/reports/.garner-terrain-task8.lock"
JOURNAL_RELATIVE = "artifacts/maps/reports/.garner-terrain-task8.transaction.json"
RECOVERY_PARENT = "artifacts/maps/reports/.garner-terrain-task8.recovery"
EVIDENCE_PARENT = "artifacts/maps/reports/runs"
PACKAGE_RUN_PARENT = "artifacts/maps/package-run"
ARCHIVE_EXECUTABLE_TOKEN = "__TASK8_ARCHIVE_EXECUTABLE_FROM_RECEIPT__"
RUNTIME_TARGETS = (
    "CorsairsUE/Data/Heights/garner.block.raw",
    "CorsairsUE/Data/Heights/garner.terrain.json",
)
INNER_CONTROL_PATHS = {
    "publisher": (
        "artifacts/maps/.garner.reference-albedo.publish.lock",
        "artifacts/maps/.garner.reference-albedo.publish.lock.retired",
        "artifacts/maps/.garner.reference-albedo.publish.json",
        "artifacts/maps/.garner.reference-albedo.publish.json.retired",
    ),
    "installer": (
        "CorsairsUE/Data/Heights/.garner-runtime-install.lock",
        "CorsairsUE/Data/Heights/.garner-runtime-install.lock.retired",
        "CorsairsUE/Data/Heights/.garner-runtime-install.transaction.json",
        "CorsairsUE/Data/Heights/.garner-runtime-install.transaction.json.retired",
    ),
}
MANAGED_PACKAGE_STEMS = rules.PACKAGE_STEMS
LOCK_MARKER = b'{"schemaVersion":1,"lockName":"garner-terrain-task8"}\n'
RECOVERY_COMMAND = (
    'PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 '
    'scripts/build_garner_reference_terrain.py --repo-root "$PWD" '
    '--manifest artifacts/maps/garner.reference-albedo.json '
    '--map /Game/Maps/Garner --output artifacts/maps/reports --recover-only')

_TXN = re.compile(r"[0-9a-f]{32}\Z")
_HEAD = re.compile(r"[0-9a-f]{40}\Z")
_HEALTHY_THERMAL = {
    "No thermal warning level has been recorded",
    "No performance warning level has been recorded",
    "No CPU power status has been recorded",
}
_PROCESS_PATTERNS = (
    "unrealeditor", "corsairsue.app/contents/macos/corsairsue", "game.exe",
    "crossover", "wine", "build.sh", "runuat", "cmake --build",
    "unrealbuildtool",
)
_JOURNAL_PHASES = (
    "SNAPSHOT", "TASK7_PUBLISHED", "RUNTIME_INSTALLED", "UNREAL_RUNNING",
    "EDITOR_VERIFIED", "PACKAGE_VERIFIED", "BUNDLE_PREPARED",
    "BUNDLE_REPLACED", "COMMITTED", "ROLLED_BACK",
)
_COMPLETED_STEPS = (
    "snapshot", "terrain-reference", "installer-1", "installer-2",
    "editor-build", "level-build", "import-1", "import-2", "checker",
    "editor-automation", "game-build", "cook-package", "runtime-smoke",
    "base-validated", "base-published",
)
_ACTIVE_STEPS = set(_COMPLETED_STEPS) | {
    "nested-publisher-recovery", "nested-installer-recovery",
}


class Task8Error(RuntimeError):
    def __init__(self, detail: str, *, status: str = "FAILED") -> None:
        super().__init__(detail)
        self.detail = detail
        self.status = status

    def __str__(self) -> str:
        suffix = f" recovery={RECOVERY_COMMAND}" if self.status == "RECOVERY_REQUIRED" else ""
        return f"{self.status} {self.detail}{suffix}"


@dataclass(frozen=True)
class ProcessRecord:
    pid: int
    pgid: int
    start_token: str
    executable: str
    argv: tuple[str, ...]


@dataclass(frozen=True)
class CommandSpec:
    name: str
    argv: tuple[str, ...]
    env: tuple[tuple[str, str], ...] = ()
    timeout_seconds: int = 7200


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


def lock_path(repo_root: Path | str) -> Path:
    return Path(repo_root) / LOCK_RELATIVE


def journal_path(repo_root: Path | str) -> Path:
    return Path(repo_root) / JOURNAL_RELATIVE


def recovery_root(repo_root: Path | str, transaction_id: str) -> Path:
    return Path(repo_root) / RECOVERY_PARENT / transaction_id


def evidence_root(repo_root: Path | str, transaction_id: str) -> Path:
    return Path(repo_root) / EVIDENCE_PARENT / transaction_id


def package_run_root(repo_root: Path | str, transaction_id: str) -> Path:
    return Path(repo_root) / PACKAGE_RUN_PARENT / transaction_id


def inner_control_paths(repo_root: Path | str, owner: str) -> tuple[Path, ...]:
    if owner not in INNER_CONTROL_PATHS:
        raise Task8Error(f"unknown inner owner: {owner}")
    root = Path(repo_root)
    return tuple(root / relative for relative in INNER_CONTROL_PATHS[owner])


def _nice(*arguments: str) -> tuple[str, ...]:
    return ("nice", "-n", "10", *arguments)


def production_commands(
    repo_root: Path | str, transaction_id: str, source_head: str,
) -> list[CommandSpec]:
    root = Path(repo_root).resolve()
    if not _TXN.fullmatch(transaction_id) or not _HEAD.fullmatch(source_head):
        raise Task8Error("invalid transaction/source identity")
    report_root = root / EVIDENCE_PARENT / transaction_id
    package_root = root / PACKAGE_RUN_PARENT / transaction_id
    converter = root / "tools/AssetConverter/build/AssetConverter"
    installer = root / "CorsairsUE/Scripts/install_runtime_map_data.py"
    project = root / "CorsairsUE/CorsairsUE.uproject"
    manifest = root / TOP_MANIFEST
    runtime_root = root / "CorsairsUE/Data/Heights"
    editor_build = "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh"
    editor = "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
    uat = "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh"
    python_env = (("PYTHONDONTWRITEBYTECODE", "1"),)
    task7 = _nice(
        str(converter), "terrain-reference",
        "--map", "Client/map/garner.map",
        "--database", "databases/gamedata.sqlite",
        "--client-root", "Client",
        "--alpha", "Client/texture/terrain/alpha/total.png",
        "--output", "artifacts/maps",
        "--page", "17", "21",
        "--require-present-rect", "2193", "2756", "80", "47",
        "--max-rss-mib", "128", "--max-cache-mib", "32",
        "--max-png-mib", "96", "--max-height-error-cm", "5",
        "--max-rms-error-cm", "2")
    install = _nice(
        sys.executable, str(installer), str(manifest), str(runtime_root))
    identity = (transaction_id, source_head)
    editor_script = lambda name, *args: _nice(
        editor, str(project), "-run=pythonscript",
        "-script=" + " ".join(shlex.quote(str(item)) for item in (
            root / f"CorsairsUE/Scripts/{name}", *args, *identity)),
        "-unattended", "-nop4", "-NullRHI", "-NoSound")
    commands = [
        CommandSpec("terrain-reference", task7),
        CommandSpec("installer-1", install, python_env),
        CommandSpec("installer-2", install, python_env),
        CommandSpec("editor-build", _nice(
            editor_build, "CorsairsUEEditor", "Mac", "Development", str(project),
            "-WaitMutex", "-MaxParallelActions=2")),
        CommandSpec("level-build", editor_script(
            "build_reference_terrain_level.py", manifest, rules.REFERENCE_MAP_PACKAGE,
            report_root / "reference-terrain-level-build.json")),
        CommandSpec("import-1", editor_script(
            "import_reference_terrain.py", manifest, rules.REFERENCE_MAP_PACKAGE,
            report_root / "reference-terrain-import-pass1.json")),
        CommandSpec("import-2", editor_script(
            "import_reference_terrain.py", manifest, rules.REFERENCE_MAP_PACKAGE,
            report_root / "reference-terrain-import-pass2.json")),
        CommandSpec("checker", editor_script(
            "check_reference_terrain.py", manifest, rules.REFERENCE_MAP_PACKAGE,
            report_root / "reference-terrain-level-build.json",
            report_root / "reference-terrain-import-pass1.json",
            report_root / "reference-terrain-import-pass2.json",
            report_root / "reference-terrain-check.json")),
        CommandSpec("editor-automation", _nice(
            editor, str(project), "-unattended", "-nop4", "-NullRHI", "-NoSound",
            "-ExecCmds=Automation RunTests Corsairs.Terrain.ReferenceAssets",
            "-TestExit=Automation Test Queue Empty",
            f"-ReportExportPath={report_root / 'reference-terrain-editor-automation'}")),
        CommandSpec("game-build", _nice(
            editor_build, "CorsairsUE", "Mac", "Development", str(project),
            "-WaitMutex", "-MaxParallelActions=2")),
        CommandSpec("cook-package", _nice(
            uat, "BuildCookRun", f"-project={project}", "-noP4", "-unattended",
            "-utf8output", "-platform=Mac", "-clientconfig=Development",
            "-skipbuild", "-cook", "-stage", "-pak", "-archive",
            "-map=/Game/Maps/Garner",
            f"-CookOutputDir={package_root / 'cooked'}",
            f"-stagingdirectory={package_root / 'stage'}",
            f"-archivedirectory={package_root / 'archive'}",
            "-MaxParallelActions=2")),
        CommandSpec("runtime-smoke", _nice(
            ARCHIVE_EXECUTABLE_TOKEN,
            "-unattended", "-NullRHI", "-NoSound", "-stdout",
            "-FullStdOutLogOutput", f"-CorsairsTerrainTransaction={transaction_id}",
            f"-CorsairsTerrainSourceHead={source_head}",
            "-ExecCmds=Automation RunTests Corsairs.Terrain.ReferenceRuntime",
            "-TestExit=Automation Test Queue Empty",
            f"-ReportExportPath={report_root / 'reference-terrain-runtime'}")),
    ]
    return commands


def resolve_runtime_command(
    command: CommandSpec,
    repo_root: Path | str,
    executable: dict[str, Any],
) -> CommandSpec:
    """Bind runtime-smoke to the unique archive-inventory launch product."""
    if command.name != "runtime-smoke":
        return command
    if command.argv.count(ARCHIVE_EXECUTABLE_TOKEN) != 1:
        raise Task8Error("runtime command lacks its executable binding token")
    if (type(executable) is not dict or set(executable) != {
            "path", "sha256", "sizeBytes"} or
            not _normalized_relative(executable.get("path"))):
        raise Task8Error("packaged executable evidence is malformed")
    root = Path(repo_root).resolve()
    physical = root / PurePosixPath(executable["path"])
    expected_prefix = root / PACKAGE_RUN_PARENT
    try:
        physical.resolve(strict=False).relative_to(expected_prefix.resolve())
    except ValueError as exc:
        raise Task8Error("packaged executable is outside package-run root") from exc
    argv = tuple(
        str(physical) if item == ARCHIVE_EXECUTABLE_TOKEN else item
        for item in command.argv)
    return CommandSpec(command.name, argv, command.env, command.timeout_seconds)


def require_healthy_thermal(returncode: int, output: str) -> None:
    if returncode != 0:
        raise Task8Error("pmset -g therm failed before heavy command")
    observed: set[str] = set()
    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("Note: "):
            line = line[6:]
        if line not in _HEALTHY_THERMAL:
            raise Task8Error(f"unrecognized or unhealthy thermal status: {line}")
        observed.add(line)
    if observed != _HEALTHY_THERMAL:
        raise Task8Error("pmset -g therm omitted a required healthy line")


def competing_processes(records: Iterable[ProcessRecord]) -> list[ProcessRecord]:
    result = []
    for record in records:
        text = " ".join((record.executable, *record.argv)).lower()
        if (" rg " in f" {text} " or text.endswith("/rg") or
                " ps " in f" {text} "):
            continue
        if any(pattern in text for pattern in _PROCESS_PATTERNS):
            result.append(record)
    return result


def refuse_competing_processes(
    records: Iterable[ProcessRecord],
    _kill: Callable[[Any], Any] | None = None,
) -> None:
    found = competing_processes(records)
    if found:
        detail = ", ".join(f"pid={item.pid}:{item.executable}" for item in found)
        raise Task8Error(f"pre-existing heavyweight process refused: {detail}")


def preflight_plan(
    repo_root: Path | str,
    transaction_id: str,
    *,
    status_gate: Callable[[], tuple[str, str]],
    process_probe: Callable[[], Iterable[ProcessRecord]],
    thermal_probe: Callable[[], tuple[int, str]],
) -> list[CommandSpec]:
    head, status = status_gate()
    if not _HEAD.fullmatch(head):
        raise Task8Error("git HEAD is not a full lowercase commit ID")
    if status:
        raise Task8Error("clean checkout is required before mutation")
    refuse_competing_processes(process_probe())
    require_healthy_thermal(*thermal_probe())
    return production_commands(repo_root, transaction_id, head)


def _physical_directory(path: Path) -> os.stat_result:
    info = path.lstat()
    if path.is_symlink() or not stat.S_ISDIR(info.st_mode):
        raise Task8Error(f"expected physical directory: {path}")
    return info


def _physical_regular(path: Path, *, allow_empty: bool = False) -> os.stat_result:
    info = path.lstat()
    if (path.is_symlink() or not stat.S_ISREG(info.st_mode) or
            getattr(info, "st_nlink", 1) != 1):
        raise Task8Error(f"expected physical non-hard-linked regular file: {path}")
    if not allow_empty and info.st_size <= 0:
        raise Task8Error(f"expected nonempty file: {path}")
    return info


def _sync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_exclusive(path: Path, payload: bytes, mode: int = 0o600) -> None:
    descriptor = os.open(
        path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
        getattr(os, "O_NOFOLLOW", 0), mode)
    try:
        position = 0
        while position < len(payload):
            position += os.write(descriptor, payload[position:])
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    info = _physical_regular(path, allow_empty=not payload)
    if info.st_size != len(payload) or path.read_bytes() != payload:
        raise Task8Error(f"durable write readback differs: {path}")


def _atomic_write(path: Path, payload: bytes, mode: int = 0o600) -> None:
    _physical_directory(path.parent)
    temporary = path.parent / f".{path.name}.tmp.{os.getpid()}.{os.urandom(8).hex()}"
    try:
        _write_exclusive(temporary, payload, mode)
        os.replace(temporary, path)
        _sync_directory(path.parent)
        info = _physical_regular(path, allow_empty=not payload)
        if info.st_size != len(payload) or path.read_bytes() != payload:
            raise Task8Error(f"atomic publication readback differs: {path}")
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _strict_json(path: Path) -> dict[str, Any]:
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise Task8Error(f"duplicate JSON member {key}",
                                 status="RECOVERY_REQUIRED")
            result[key] = value
        return result
    try:
        raw = _physical_regular(path).st_size and path.read_bytes()
        value = json.loads(
            raw.decode("utf-8", errors="strict"), object_pairs_hook=unique,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON number {token}")))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        raise Task8Error(f"invalid recovery journal: {exc}",
                         status="RECOVERY_REQUIRED") from exc
    if type(value) is not dict:
        raise Task8Error("recovery journal root is not an object",
                         status="RECOVERY_REQUIRED")
    return value


def _normalized_relative(value: Any) -> bool:
    if type(value) is not str or not value or "\\" in value:
        return False
    path = PurePosixPath(value)
    return (not path.is_absolute() and path.as_posix() == value and
            all(part not in ("", ".", "..") for part in path.parts))


def _contained(root: Path, relative: str) -> Path:
    if not _normalized_relative(relative):
        raise Task8Error(f"invalid journal path: {relative}",
                         status="RECOVERY_REQUIRED")
    candidate = root / PurePosixPath(relative)
    try:
        candidate.resolve(strict=False).relative_to(root.resolve())
    except (OSError, ValueError) as exc:
        raise Task8Error(f"journal path escapes repository: {relative}",
                         status="RECOVERY_REQUIRED") from exc
    return candidate


def _sha256(path: Path) -> str:
    return rules.sha256_file(path)


def _copy_file_durable(source: Path, destination: Path, mode: int) -> None:
    _physical_regular(source)
    descriptor = os.open(
        destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
        getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        with source.open("rb") as input_file:
            while True:
                block = input_file.read(1024 * 1024)
                if not block:
                    break
                offset = 0
                while offset < len(block):
                    offset += os.write(descriptor, block[offset:])
        os.fchmod(descriptor, mode)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    _physical_regular(destination)


def _mkdir_exclusive(path: Path, mode: int = 0o700) -> None:
    try:
        os.mkdir(path, mode)
    except FileExistsError as exc:
        raise Task8Error(f"transaction path collision: {path}") from exc
    _physical_directory(path)
    if stat.S_IMODE(path.stat().st_mode) != mode:
        path.chmod(mode)
    _sync_directory(path.parent)


class OuterTransaction:
    def __init__(
        self, repo_root: Path, lock_descriptor: int, journal: dict[str, Any] | None,
    ) -> None:
        self.repo_root = repo_root.resolve()
        self.lock_descriptor = lock_descriptor
        self.journal: dict[str, Any] = journal or {}

    @classmethod
    def _lock(cls, repo_root: Path | str) -> "OuterTransaction":
        root = Path(repo_root).resolve()
        reports = root / "artifacts/maps/reports"
        reports.mkdir(parents=True, exist_ok=True, mode=0o700)
        _physical_directory(reports)
        path = lock_path(root)
        created = False
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags, 0o600)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            os.close(descriptor)
            if exc.errno in (errno.EACCES, errno.EAGAIN):
                raise Task8Error("another Task 8 transaction owns the lock") from exc
            raise
        try:
            info = os.fstat(descriptor)
            path_info = path.lstat()
            if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                    (info.st_dev, info.st_ino) != (path_info.st_dev, path_info.st_ino) or
                    path.is_symlink()):
                raise Task8Error("Task 8 lock is not one physical file")
            current = os.pread(descriptor, info.st_size, 0)
            if info.st_size == 0:
                created = True
                os.ftruncate(descriptor, 0)
                os.pwrite(descriptor, LOCK_MARKER, 0)
                os.fsync(descriptor)
            elif current != LOCK_MARKER:
                raise Task8Error("Task 8 lock marker differs",
                                 status="RECOVERY_REQUIRED")
            if os.pread(descriptor, len(LOCK_MARKER) + 1, 0) != LOCK_MARKER:
                raise Task8Error("Task 8 lock marker readback differs",
                                 status="RECOVERY_REQUIRED")
            if created:
                _sync_directory(path.parent)
            return cls(root, descriptor, None)
        except BaseException:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
            raise

    @classmethod
    def acquire(cls, repo_root: Path | str) -> "OuterTransaction":
        transaction = cls._lock(repo_root)
        path = journal_path(transaction.repo_root)
        if path.exists() or path.is_symlink():
            transaction.journal = _strict_json(path)
            transaction._validate_journal()
        return transaction

    @classmethod
    def begin(
        cls, repo_root: Path | str, transaction_id: str, source_head: str,
    ) -> "OuterTransaction":
        if not _TXN.fullmatch(transaction_id) or not _HEAD.fullmatch(source_head):
            raise Task8Error("invalid transaction/source identity")
        transaction = cls.acquire(repo_root)
        if transaction.journal:
            transaction.close()
            raise Task8Error("unfinished Task 8 journal requires recovery",
                             status="RECOVERY_REQUIRED")
        root = transaction.repo_root
        try:
            for parent in (root / RECOVERY_PARENT, root / EVIDENCE_PARENT,
                           root / PACKAGE_RUN_PARENT):
                parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                _physical_directory(parent)
            recovery = recovery_root(root, transaction_id)
            evidence = evidence_root(root, transaction_id)
            package = package_run_root(root, transaction_id)
            _mkdir_exclusive(recovery)
            _mkdir_exclusive(evidence)
            _mkdir_exclusive(package)
            snapshots, listings = transaction._capture_snapshots(recovery)
            transaction.journal = {
                "schemaVersion": 1,
                "transactionId": transaction_id,
                "sourceHead": source_head,
                "phase": "SNAPSHOT",
                "completedStep": "snapshot",
                "evidenceRoot": evidence.relative_to(root).as_posix(),
                "packageRunRoot": package.relative_to(root).as_posix(),
                "recoveryRoot": recovery.relative_to(root).as_posix(),
                "snapshots": snapshots,
                "managedFamilyListings": listings,
                "activeProcess": None,
                "intendedBundleSha256": "",
                "issues": [],
            }
            transaction.persist()
            return transaction
        except BaseException:
            transaction.close()
            raise

    def _snapshot_candidates(self) -> tuple[list[str], dict[str, list[str]]]:
        candidates = [TOP_MANIFEST, *RUNTIME_TARGETS, BASE_BUNDLE]
        listings: dict[str, list[str]] = {}
        for family, stem_text in MANAGED_PACKAGE_STEMS.items():
            stem = self.repo_root / stem_text
            primary = ".umap" if family == "map" else ".uasset"
            allowed = (primary, *rules.PACKAGE_SIDECARS)
            family_paths = []
            if stem.parent.exists():
                _physical_directory(stem.parent)
                for child in stem.parent.iterdir():
                    if child.name.startswith(stem.name + ".") and child.suffix not in allowed:
                        raise Task8Error(f"unexpected managed family suffix: {child}")
            for suffix in allowed:
                relative = (PurePosixPath(stem_text).with_suffix(suffix)).as_posix()
                candidates.append(relative)
                path = self.repo_root / relative
                if path.exists() or path.is_symlink():
                    family_paths.append(relative)
            listings[family] = sorted(family_paths)
        return sorted(set(candidates)), listings

    def _capture_snapshots(
        self, recovery: Path,
    ) -> tuple[list[dict[str, Any]], dict[str, list[str]]]:
        snapshot_dir = recovery / "snapshots"
        _mkdir_exclusive(snapshot_dir)
        candidates, listings = self._snapshot_candidates()
        snapshots = []
        for index, relative in enumerate(candidates):
            target = self.repo_root / PurePosixPath(relative)
            if target.exists() or target.is_symlink():
                info = _physical_regular(target)
                backup = snapshot_dir / f"{index:03}.bin"
                _copy_file_durable(target, backup, 0o600)
                if (_sha256(backup) != _sha256(target) or
                        backup.stat().st_size != info.st_size):
                    raise Task8Error(f"snapshot readback differs: {target}")
                snapshots.append({
                    "path": relative,
                    "priorType": "regular",
                    "priorMode": stat.S_IMODE(info.st_mode),
                    "priorSha256": _sha256(target),
                    "priorSizeBytes": int(info.st_size),
                    "backupPath": backup.relative_to(self.repo_root).as_posix(),
                })
            else:
                snapshots.append({
                    "path": relative,
                    "priorType": "absent",
                    "priorMode": 0,
                    "priorSha256": "",
                    "priorSizeBytes": 0,
                    "backupPath": "",
                })
        _sync_directory(snapshot_dir)
        _sync_directory(recovery)
        return snapshots, listings

    def _validate_journal(self) -> None:
        expected = {
            "schemaVersion", "transactionId", "sourceHead", "phase", "completedStep",
            "evidenceRoot", "packageRunRoot", "recoveryRoot", "snapshots",
            "managedFamilyListings", "activeProcess", "intendedBundleSha256", "issues",
        }
        if set(self.journal) != expected:
            raise Task8Error("recovery journal key set differs",
                             status="RECOVERY_REQUIRED")
        if (self.journal["schemaVersion"] != 1 or
                type(self.journal["schemaVersion"]) is not int or
                not _TXN.fullmatch(self.journal["transactionId"]) or
                not _HEAD.fullmatch(self.journal["sourceHead"]) or
                self.journal["phase"] not in _JOURNAL_PHASES or
                self.journal["completedStep"] not in _COMPLETED_STEPS or
                self.journal["issues"] != []):
            raise Task8Error("recovery journal identity/state is invalid",
                             status="RECOVERY_REQUIRED")
        transaction_id = self.journal["transactionId"]
        expected_paths = {
            "evidenceRoot": f"{EVIDENCE_PARENT}/{transaction_id}",
            "packageRunRoot": f"{PACKAGE_RUN_PARENT}/{transaction_id}",
            "recoveryRoot": f"{RECOVERY_PARENT}/{transaction_id}",
        }
        for key, expected_path in expected_paths.items():
            if self.journal[key] != expected_path:
                raise Task8Error(f"recovery journal {key} differs",
                                 status="RECOVERY_REQUIRED")
        if type(self.journal["snapshots"]) is not list:
            raise Task8Error("recovery snapshots are not a list",
                             status="RECOVERY_REQUIRED")
        paths = []
        for record in self.journal["snapshots"]:
            if type(record) is not dict or set(record) != {
                    "path", "priorType", "priorMode", "priorSha256",
                    "priorSizeBytes", "backupPath"}:
                raise Task8Error("invalid Snapshot DTO", status="RECOVERY_REQUIRED")
            _contained(self.repo_root, record["path"])
            paths.append(record["path"])
            if record["priorType"] == "regular":
                if (type(record["priorMode"]) is not int or
                        not 0 <= record["priorMode"] <= 0o7777 or
                        type(record["priorSha256"]) is not str or
                        not re.fullmatch(r"[0-9a-f]{64}", record["priorSha256"]) or
                        type(record["priorSizeBytes"]) is not int or
                        record["priorSizeBytes"] <= 0 or
                        not _normalized_relative(record["backupPath"])):
                    raise Task8Error("invalid regular Snapshot",
                                     status="RECOVERY_REQUIRED")
            elif record["priorType"] == "absent":
                if (record["priorMode"], record["priorSha256"],
                        record["priorSizeBytes"], record["backupPath"]) != (0, "", 0, ""):
                    raise Task8Error("invalid absent Snapshot",
                                     status="RECOVERY_REQUIRED")
            else:
                raise Task8Error("invalid Snapshot priorType",
                                 status="RECOVERY_REQUIRED")
        if paths != sorted(set(paths)):
            raise Task8Error("snapshot paths are not sorted unique",
                             status="RECOVERY_REQUIRED")
        active = self.journal["activeProcess"]
        if active is not None:
            if type(active) is not dict or set(active) != {
                    "step", "pid", "pgid", "startToken", "executable",
                    "argvSha256"}:
                raise Task8Error("invalid ActiveProcess DTO",
                                 status="RECOVERY_REQUIRED")
            if (active["step"] not in _ACTIVE_STEPS or
                    type(active["pid"]) is not int or active["pid"] <= 0 or
                    type(active["pgid"]) is not int or active["pgid"] <= 0 or
                    type(active["startToken"]) is not str or not active["startToken"] or
                    type(active["executable"]) is not str or
                    not Path(active["executable"]).is_absolute() or
                    type(active["argvSha256"]) is not str or
                    not re.fullmatch(r"[0-9a-f]{64}", active["argvSha256"])):
                raise Task8Error("invalid ActiveProcess identity",
                                 status="RECOVERY_REQUIRED")

    def persist(self) -> None:
        self._validate_journal()
        payload = rules.canonical_json_bytes(self.journal) + b"\n"
        _atomic_write(journal_path(self.repo_root), payload, 0o600)
        if _strict_json(journal_path(self.repo_root)) != self.journal:
            raise Task8Error("journal strict readback differs",
                             status="RECOVERY_REQUIRED")

    def _restore_regular(self, record: dict[str, Any]) -> None:
        target = _contained(self.repo_root, record["path"])
        backup = _contained(self.repo_root, record["backupPath"])
        info = _physical_regular(backup)
        if (info.st_size != record["priorSizeBytes"] or
                _sha256(backup) != record["priorSha256"]):
            raise Task8Error(f"snapshot backup differs: {backup}",
                             status="RECOVERY_REQUIRED")
        _physical_directory(target.parent)
        temporary = target.parent / (
            f".{target.name}.task8-restore.{self.journal['transactionId']}")
        if temporary.exists() or temporary.is_symlink():
            raise Task8Error(f"foreign restore temp exists: {temporary}",
                             status="RECOVERY_REQUIRED")
        try:
            _copy_file_durable(backup, temporary, record["priorMode"])
            if (_sha256(temporary) != record["priorSha256"] or
                    temporary.stat().st_size != record["priorSizeBytes"] or
                    stat.S_IMODE(temporary.stat().st_mode) != record["priorMode"]):
                raise Task8Error(f"restore temp differs: {temporary}",
                                 status="RECOVERY_REQUIRED")
            if target.exists() or target.is_symlink():
                _physical_regular(target)
            os.replace(temporary, target)
            _sync_directory(target.parent)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass

    def _restore_absent(self, record: dict[str, Any]) -> None:
        target = _contained(self.repo_root, record["path"])
        if not target.exists() and not target.is_symlink():
            return
        _physical_regular(target)
        target.unlink()
        _sync_directory(target.parent)

    def _verify_snapshot(self, record: dict[str, Any]) -> None:
        target = _contained(self.repo_root, record["path"])
        if record["priorType"] == "absent":
            if target.exists() or target.is_symlink():
                raise Task8Error(f"rollback did not remove {target}",
                                 status="RECOVERY_REQUIRED")
            return
        info = _physical_regular(target)
        if (info.st_size != record["priorSizeBytes"] or
                stat.S_IMODE(info.st_mode) != record["priorMode"] or
                _sha256(target) != record["priorSha256"]):
            raise Task8Error(f"rollback verification differs: {target}",
                             status="RECOVERY_REQUIRED")

    def rollback(self) -> None:
        self._validate_journal()
        if self.journal["phase"] == "COMMITTED":
            raise Task8Error("committed transaction cannot roll back",
                             status="RECOVERY_REQUIRED")
        for record in self.journal["snapshots"]:
            if record["priorType"] == "regular":
                self._restore_regular(record)
            else:
                self._restore_absent(record)
        for record in self.journal["snapshots"]:
            self._verify_snapshot(record)
        self.journal["phase"] = "ROLLED_BACK"
        self.journal["activeProcess"] = None
        self.persist()

    def recover(
        self,
        source_head: str,
        *,
        process_probe: Callable[[int], ProcessRecord | None] | None = None,
        kill_group: Callable[[int], None] | None = None,
        nested_retry: Callable[[str, tuple[Path, ...]], None] | None = None,
        nested_validate: Callable[[str], None] | None = None,
    ) -> None:
        if not self.journal:
            return
        self._validate_journal()
        if source_head != self.journal["sourceHead"]:
            raise Task8Error(
                f"recovery requires commit {self.journal['sourceHead']}",
                status="RECOVERY_REQUIRED")
        if self.journal["phase"] in ("ROLLED_BACK", "COMMITTED"):
            return
        if self.journal["activeProcess"] is not None:
            active = self.journal["activeProcess"]
            probe = process_probe or _probe_process
            observed = probe(active["pid"])
            if observed is None or observed.start_token != active["startToken"]:
                self.journal["activeProcess"] = None
                self.persist()
            else:
                argv_hash = hashlib.sha256(
                    b"\0".join(os.fsencode(item) for item in observed.argv)).hexdigest()
                try:
                    observed_executable = str(Path(observed.executable).resolve(strict=True))
                except OSError as exc:
                    raise Task8Error(
                        f"recorded process executable is unreadable: {exc}",
                        status="RECOVERY_REQUIRED") from exc
                if (observed.pid != active["pid"] or
                        observed.pgid != active["pgid"] or
                        observed_executable != active["executable"] or
                        argv_hash != active["argvSha256"]):
                    raise Task8Error("live process only partially matches journal",
                                     status="RECOVERY_REQUIRED")
                terminator = kill_group or _terminate_process_group
                terminator(active["pgid"])
                if probe(active["pid"]) is not None:
                    raise Task8Error("owned process group did not terminate",
                                     status="RECOVERY_REQUIRED")
                self.journal["activeProcess"] = None
                self.persist()
        self._resolve_inner_owners(nested_retry, nested_validate)
        self.rollback()

    def _resolve_inner_owners(
        self,
        retry: Callable[[str, tuple[Path, ...]], None] | None,
        validate: Callable[[str], None] | None,
    ) -> None:
        for owner in ("publisher", "installer"):
            paths = inner_control_paths(self.repo_root, owner)
            present = tuple(
                path for path in paths if path.exists() or path.is_symlink())
            if not present:
                continue
            if retry is None or validate is None:
                raise Task8Error(
                    f"inner {owner} recovery is required before outer rollback",
                    status="RECOVERY_REQUIRED")
            retry(owner, paths)
            remaining = tuple(
                path for path in paths if path.exists() or path.is_symlink())
            if remaining:
                raise Task8Error(
                    f"inner {owner} recovery left control paths: "
                    + ",".join(path.as_posix() for path in remaining),
                    status="RECOVERY_REQUIRED")
            validate(owner)

    def run_owned_command(
        self,
        command: CommandSpec,
        *,
        fault: Callable[[str], Any] | None = None,
    ) -> CommandResult:
        if command.name not in _ACTIVE_STEPS:
            raise Task8Error(f"unrecognized owned step: {command.name}")
        if self.journal.get("activeProcess") is not None:
            raise Task8Error("journal already has an active process",
                             status="RECOVERY_REQUIRED")
        if not command.argv:
            raise Task8Error("owned command argv is empty")
        executable = shutil.which(command.argv[0])
        if executable is None:
            raise Task8Error(f"owned executable not found: {command.argv[0]}")
        executable_path = str(Path(executable).resolve(strict=True))
        command_dir = _contained(self.repo_root, self.journal["evidenceRoot"]) / "commands"
        if not command_dir.exists():
            _mkdir_exclusive(command_dir)
        else:
            _physical_directory(command_dir)
        attempt = 1
        while True:
            suffix = "" if attempt == 1 else f".attempt-{attempt}"
            stdout_path = command_dir / f"{command.name}{suffix}.stdout.log"
            stderr_path = command_dir / f"{command.name}{suffix}.stderr.log"
            if (not stdout_path.exists() and not stdout_path.is_symlink() and
                    not stderr_path.exists() and not stderr_path.is_symlink()):
                break
            attempt += 1
        stdout_descriptor = os.open(
            stdout_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
            getattr(os, "O_NOFOLLOW", 0), 0o600)
        try:
            stderr_descriptor = os.open(
                stderr_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                getattr(os, "O_NOFOLLOW", 0), 0o600)
        except BaseException:
            os.close(stdout_descriptor)
            raise
        release_read, release_write = os.pipe()
        pid = os.fork()
        if pid == 0:  # pragma: no cover - exercised through parent observations
            try:
                os.close(release_write)
                os.setsid()
                os.dup2(stdout_descriptor, 1)
                os.dup2(stderr_descriptor, 2)
                os.close(stdout_descriptor)
                os.close(stderr_descriptor)
                if self.lock_descriptor >= 0:
                    os.close(self.lock_descriptor)
                release = os.read(release_read, 1)
                os.close(release_read)
                if release != b"1":
                    os._exit(125)
                environment = os.environ.copy()
                environment.update(dict(command.env))
                os.execve(executable_path, list(command.argv), environment)
            except BaseException as exc:
                try:
                    os.write(2, f"owned exec failed: {exc}\n".encode("utf-8"))
                except OSError:
                    pass
                os._exit(126)
        os.close(release_read)
        os.close(stdout_descriptor)
        os.close(stderr_descriptor)
        try:
            deadline = time.monotonic() + 5.0
            while True:
                try:
                    pgid = os.getpgid(pid)
                except ProcessLookupError as exc:
                    raise Task8Error("owned launcher exited before handshake") from exc
                if pgid == pid:
                    break
                if time.monotonic() >= deadline:
                    raise Task8Error("owned launcher did not create a process group")
                time.sleep(0.005)
            start_token = _process_start_token(pid)
            argv_hash = hashlib.sha256(
                b"\0".join(os.fsencode(item) for item in command.argv)).hexdigest()
            self.journal["activeProcess"] = {
                "step": command.name,
                "pid": pid,
                "pgid": pgid,
                "startToken": start_token,
                "executable": executable_path,
                "argvSha256": argv_hash,
            }
            self.journal["phase"] = "UNREAL_RUNNING"
            self.persist()
            try:
                self._fault(fault, "AFTER_ACTIVE_PROCESS_DURABLE")
            except Task8Error:
                os.close(release_write)
                release_write = -1
                os.waitpid(pid, 0)
                raise
            os.write(release_write, b"1")
            os.close(release_write)
            release_write = -1
            deadline = time.monotonic() + command.timeout_seconds
            status_value: int | None = None
            while status_value is None:
                waited, status_candidate = os.waitpid(pid, os.WNOHANG)
                if waited == pid:
                    status_value = status_candidate
                    break
                if time.monotonic() >= deadline:
                    _terminate_process_group(pgid)
                    os.waitpid(pid, 0)
                    raise Task8Error(f"owned step timed out: {command.name}")
                time.sleep(0.02)
            self._fault(fault, "AFTER_OWNED_PROCESS_WAIT")
            returncode = os.waitstatus_to_exitcode(status_value)
            self.journal["activeProcess"] = None
            if command.name in _COMPLETED_STEPS:
                self.journal["completedStep"] = command.name
            self.persist()
            stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            return CommandResult(returncode, stdout, stderr)
        except BaseException:
            if release_write >= 0:
                os.close(release_write)
            raise

    def _fault(self, fault: Callable[[str], Any] | None, point: str) -> None:
        if fault is None:
            return
        action = fault(point)
        if action in ("fail", "crash"):
            raise Task8Error(f"injected {action} at {point}")

    def publish_bundle(
        self,
        bundle: dict[str, Any],
        *,
        validate: Callable[[dict[str, Any], Path], list[dict[str, Any]]],
        fault: Callable[[str], Any] | None = None,
    ) -> None:
        destination = self.repo_root / BASE_BUNDLE
        temporary = destination.parent / (
            f".{destination.name}.prepared.{self.journal['transactionId']}")
        payload = rules.canonical_json_bytes(bundle) + b"\n"
        try:
            if temporary.exists() or temporary.is_symlink():
                raise Task8Error(f"foreign bundle temp exists: {temporary}")
            _write_exclusive(temporary, payload, 0o600)
            if temporary.read_bytes() != payload:
                raise Task8Error("bundle temp readback differs")
            issues = validate(bundle, temporary)
            if issues:
                raise Task8Error(f"bundle validation failed: {issues[0]}")
            self.journal["phase"] = "BUNDLE_PREPARED"
            self.journal["completedStep"] = "base-validated"
            self.journal["intendedBundleSha256"] = hashlib.sha256(payload).hexdigest()
            self.persist()
            self._fault(fault, "BEFORE_BUNDLE_REPLACE")
            if destination.exists() or destination.is_symlink():
                _physical_regular(destination)
            os.replace(temporary, destination)
            self._fault(fault, "AFTER_BUNDLE_REPLACE")
            _sync_directory(destination.parent)
            self._fault(fault, "AFTER_REPORTS_PARENT_FSYNC")
            if destination.read_bytes() != payload:
                raise Task8Error("published bundle readback differs")
            self.journal["phase"] = "BUNDLE_REPLACED"
            self.journal["completedStep"] = "base-published"
            self.persist()
        except Task8Error:
            try:
                self.rollback()
            except Task8Error as recovery_error:
                raise Task8Error(
                    f"bundle publication and rollback failed: {recovery_error}",
                    status="RECOVERY_REQUIRED") from recovery_error
            raise
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass

    def _remove_recovery_material(self) -> None:
        recovery = _contained(self.repo_root, self.journal["recoveryRoot"])
        if not recovery.exists() and not recovery.is_symlink():
            return
        _physical_directory(recovery)
        for directory, subdirs, files in os.walk(recovery, topdown=False,
                                                  followlinks=False):
            current = Path(directory)
            for name in files:
                child = current / name
                _physical_regular(child, allow_empty=True)
                child.unlink()
            for name in subdirs:
                child = current / name
                _physical_directory(child)
                child.rmdir()
        recovery.rmdir()
        _sync_directory(recovery.parent)

    def finish(self) -> None:
        if self.journal.get("phase") not in ("COMMITTED", "ROLLED_BACK"):
            raise Task8Error("only terminal transaction may be cleaned",
                             status="RECOVERY_REQUIRED")
        self._remove_recovery_material()
        path = journal_path(self.repo_root)
        if path.exists() or path.is_symlink():
            _physical_regular(path)
            path.unlink()
            _sync_directory(path.parent)
        self.close()

    def abandon_for_test(self) -> None:
        self.close()

    def close(self) -> None:
        if self.lock_descriptor >= 0:
            fcntl.flock(self.lock_descriptor, fcntl.LOCK_UN)
            os.close(self.lock_descriptor)
            self.lock_descriptor = -1


def _process_start_token(pid: int) -> str:
    result = subprocess.run(
        ("ps", "-p", str(pid), "-o", "lstart="),
        capture_output=True, text=True, check=False, timeout=5)
    token = result.stdout.strip()
    if result.returncode != 0 or not token:
        raise Task8Error(f"cannot read start token for pid {pid}",
                         status="RECOVERY_REQUIRED")
    return token


def _probe_process(pid: int) -> ProcessRecord | None:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    except PermissionError as exc:
        raise Task8Error(f"cannot inspect pid {pid}: {exc}",
                         status="RECOVERY_REQUIRED") from exc
    try:
        pgid = os.getpgid(pid)
        token = _process_start_token(pid)
    except ProcessLookupError:
        return None
    result = subprocess.run(
        ("ps", "-p", str(pid), "-o", "comm=", "-o", "command="),
        capture_output=True, text=True, check=False, timeout=5)
    if result.returncode != 0 or not result.stdout.strip():
        raise Task8Error(f"cannot inspect live pid {pid}",
                         status="RECOVERY_REQUIRED")
    line = result.stdout.strip()
    parts = line.split(None, 1)
    if len(parts) != 2:
        raise Task8Error(f"partial process identity for pid {pid}",
                         status="RECOVERY_REQUIRED")
    executable, command_text = parts
    try:
        argv = tuple(shlex.split(command_text))
    except ValueError as exc:
        raise Task8Error(f"cannot parse process argv for pid {pid}",
                         status="RECOVERY_REQUIRED") from exc
    return ProcessRecord(pid, pgid, token, executable, argv)


def _terminate_process_group(pgid: int) -> None:
    if pgid <= 0:
        raise Task8Error("refusing invalid process group",
                         status="RECOVERY_REQUIRED")
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        return


def git_status_gate(repo_root: Path | str) -> tuple[str, str]:
    root = Path(repo_root).resolve()
    top = subprocess.run(
        ("git", "rev-parse", "--show-toplevel"), cwd=root,
        capture_output=True, text=True, check=False, timeout=30)
    if top.returncode != 0:
        raise Task8Error(f"git top-level probe failed: {top.stderr.strip()}")
    try:
        discovered = Path(top.stdout.strip()).resolve(strict=True)
    except OSError as exc:
        raise Task8Error(f"git top-level is invalid: {exc}") from exc
    if discovered != root:
        raise Task8Error("--repo-root is not the physical git top-level")
    head_result = subprocess.run(
        ("git", "rev-parse", "HEAD"), cwd=root,
        capture_output=True, text=True, check=False, timeout=30)
    status_result = subprocess.run(
        ("git", "status", "--porcelain=v1", "--untracked-files=all"), cwd=root,
        capture_output=True, text=True, check=False, timeout=30)
    if head_result.returncode != 0 or status_result.returncode != 0:
        raise Task8Error("git identity/status probe failed")
    return head_result.stdout.strip(), status_result.stdout


def probe_thermal() -> tuple[int, str]:
    result = subprocess.run(
        ("pmset", "-g", "therm"), capture_output=True, text=True,
        check=False, timeout=30)
    return result.returncode, result.stdout + result.stderr


def probe_process_table() -> list[ProcessRecord]:
    result = subprocess.run(
        ("ps", "-axo", "pid=,pgid=,lstart=,comm=,command="),
        capture_output=True, text=True, check=False, timeout=30)
    if result.returncode != 0:
        raise Task8Error("process table probe failed")
    records = []
    for raw in result.stdout.splitlines():
        parts = raw.split(None, 8)
        if len(parts) < 9:
            continue
        try:
            pid = int(parts[0])
            pgid = int(parts[1])
        except ValueError:
            continue
        start_token = " ".join(parts[2:7])
        executable = parts[7]
        try:
            argv = tuple(shlex.split(parts[8]))
        except ValueError:
            argv = (parts[8],)
        records.append(ProcessRecord(pid, pgid, start_token, executable, argv))
    return records


def execute_command_plan(
    transaction: OuterTransaction,
    commands: Sequence[CommandSpec],
    *,
    process_probe: Callable[[], Iterable[ProcessRecord]] = probe_process_table,
    thermal_probe: Callable[[], tuple[int, str]] = probe_thermal,
    runner: Callable[[CommandSpec], CommandResult] | None = None,
    after_step: Callable[[CommandSpec, CommandResult], None] | None = None,
    resolve_command: Callable[[CommandSpec], CommandSpec] | None = None,
) -> list[CommandResult]:
    results = []
    invoke = runner or transaction.run_owned_command
    for planned_command in commands:
        command = (resolve_command(planned_command)
                   if resolve_command is not None else planned_command)
        refuse_competing_processes(process_probe())
        require_healthy_thermal(*thermal_probe())
        result = invoke(command)
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise Task8Error(
                f"owned step {command.name} exited {result.returncode}: {detail}")
        if after_step is not None:
            after_step(command, result)
        results.append(result)
    return results


def _load_report(path: Path) -> dict[str, Any]:
    try:
        value, _ = rules.strict_json_load(path)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        raise Task8Error(f"invalid Task 8 report {path}: {exc}") from exc
    return value


def _require_valid_manifest(repo_root: Path) -> tuple[dict[str, Any], str]:
    path = repo_root / TOP_MANIFEST
    data = _load_report(path)
    issues = rules.validate_manifest(data, path)
    if issues:
        raise Task8Error(f"Task 7 manifest validation failed: {issues[0]}")
    run_id = PurePosixPath(data["files"]["height"]["path"]).parts[1]
    return data, run_id


def _validate_installed_pair(repo_root: Path, manifest: dict[str, Any]) -> None:
    for relative, key in zip(RUNTIME_TARGETS, ("block", "terrainMetadata")):
        path = repo_root / relative
        info = _physical_regular(path)
        expected = manifest["files"][key]
        if info.st_size != expected["sizeBytes"] or _sha256(path) != expected["sha256"]:
            raise Task8Error(f"installed runtime file differs from manifest: {path}")


def _evidence(path: Path, repo_root: Path, *, allow_empty: bool = False) -> dict[str, Any]:
    info = _physical_regular(path, allow_empty=allow_empty)
    return {
        "path": path.resolve().relative_to(repo_root.resolve()).as_posix(),
        "sha256": _sha256(path),
        "sizeBytes": int(info.st_size),
    }


def _evidence_set(
    root: Path, repo_root: Path, transaction_id: str, source_head: str,
) -> dict[str, Any]:
    _physical_directory(root)
    files = []
    for directory, subdirs, names in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in subdirs:
            _physical_directory(current / name)
        for name in names:
            files.append(_evidence(current / name, repo_root, allow_empty=True))
    files.sort(key=lambda item: item["path"])
    if not files:
        raise Task8Error(f"automation evidence is empty: {root}")
    return {
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "root": root.resolve().relative_to(repo_root.resolve()).as_posix(),
        "files": files,
    }


def _resolve_receipt_path(repo_root: Path, target: str) -> Path:
    return repo_root / f"CorsairsUE/Binaries/Mac/{target}.target"


def _receipt_project_path(value: str, project_root: Path) -> Path | None:
    prefix = "$(ProjectDir)/"
    if value.startswith(prefix):
        relative = value[len(prefix):]
        if not _normalized_relative(relative):
            raise Task8Error(f"receipt has invalid project path: {value}")
        return project_root / PurePosixPath(relative)
    candidate = Path(value)
    if not candidate.is_absolute():
        return None
    try:
        candidate.resolve(strict=True).relative_to(project_root.resolve(strict=True))
    except (OSError, ValueError):
        return None
    return candidate


def copy_build_evidence(
    repo_root: Path,
    transaction_id: str,
    source_head: str,
    target: str,
) -> dict[str, Any]:
    if target not in ("CorsairsUEEditor", "CorsairsUE"):
        raise Task8Error("unexpected build target")
    source_receipt = _resolve_receipt_path(repo_root, target)
    receipt_data = _load_report(source_receipt)
    if type(receipt_data.get("BuildProducts")) is not list:
        raise Task8Error(f"receipt lacks BuildProducts: {source_receipt}")
    destination_root = evidence_root(repo_root, transaction_id) / "builds" / target
    destination_root.mkdir(parents=True, exist_ok=False, mode=0o700)
    _physical_directory(destination_root)
    receipt_copy = destination_root / source_receipt.name
    _copy_file_durable(source_receipt, receipt_copy, 0o600)
    products = []
    project_root = repo_root / "CorsairsUE"
    for item in receipt_data["BuildProducts"]:
        if type(item) is not dict or type(item.get("Path")) is not str:
            raise Task8Error("receipt BuildProduct is malformed")
        source = _receipt_project_path(item["Path"], project_root)
        if source is None:
            continue
        relative = source.resolve(strict=True).relative_to(project_root.resolve())
        destination = destination_root / "products" / relative
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if destination.exists() or destination.is_symlink():
            raise Task8Error(f"duplicate receipt product: {destination}")
        _copy_file_durable(source, destination, 0o600)
        products.append(_evidence(destination, repo_root))
    products.sort(key=lambda item: item["path"])
    if not products:
        raise Task8Error(f"receipt has no project-owned products: {source_receipt}")
    return {
        "transactionId": transaction_id,
        "target": target,
        "platform": "Mac",
        "configuration": "Development",
        "sourceHead": source_head,
        "receipt": _evidence(receipt_copy, repo_root),
        "products": products,
    }


def _inventory_files(root: Path, repo_root: Path) -> list[dict[str, Any]]:
    _physical_directory(root)
    result = []
    for directory, subdirs, names in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in subdirs:
            _physical_directory(current / name)
        for name in names:
            result.append(_evidence(current / name, repo_root, allow_empty=True))
    result.sort(key=lambda item: item["path"])
    if not result:
        raise Task8Error(f"package inventory is empty: {root}")
    return result


def write_inventory_report(
    physical_root: Path,
    report_path: Path,
    report_type: str,
    repo_root: Path,
    transaction_id: str,
    source_head: str,
) -> dict[str, Any]:
    report = {
        "schemaVersion": 1,
        "reportType": report_type,
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "root": physical_root.resolve().relative_to(repo_root.resolve()).as_posix(),
        "files": _inventory_files(physical_root, repo_root),
        "issues": [],
    }
    rules.atomic_write_json(report_path, report)
    issues = rules.validate_inventory_report(report, repo_root)
    if issues:
        raise Task8Error(f"inventory report self-validation failed: {issues[0]}")
    return report


_UNREALPAK_MEMBER = re.compile(
    r'^\s*"(?P<path>\.\./\.\./\.\./[^\"]+)".*?\bsize:\s*(?P<size>[0-9]+)',
    re.IGNORECASE)


def parse_unrealpak_list(output: str) -> list[dict[str, Any]]:
    members = []
    for line in output.splitlines():
        match = _UNREALPAK_MEMBER.search(line)
        if match is None:
            continue
        member = match.group("path")
        suffix = member[9:]
        if not _normalized_relative(suffix):
            raise Task8Error(f"UnrealPak emitted invalid member path: {member}")
        members.append({"path": member, "sizeBytes": int(match.group("size"))})
    members.sort(key=lambda item: item["path"])
    if not members or len({item["path"] for item in members}) != len(members):
        raise Task8Error("UnrealPak member list is empty or duplicated")
    return members


def parse_runtime_observation_event(
    output: str, transaction_id: str, source_head: str,
) -> dict[str, Any]:
    prefix = "CORSAIRS_TERRAIN_RUNTIME_JSON="
    payloads = []
    for line in output.splitlines():
        position = line.find(prefix)
        if position >= 0:
            payloads.append(line[position + len(prefix):].strip())
    if len(payloads) != 1:
        raise Task8Error("packaged runtime emitted zero or duplicate JSON events")
    payload = payloads[0]
    try:
        value = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise Task8Error(f"packaged runtime JSON is truncated/invalid: {exc}") from exc
    if type(value) is not dict or rules.canonical_json_bytes(value).decode("utf-8") != payload:
        raise Task8Error("packaged runtime event is not canonical JSON")
    issues = rules.validate_runtime_observation(value)
    if issues:
        raise Task8Error(f"packaged runtime observation failed: {issues[0]}")
    if (value["transactionId"] != transaction_id or
            value["sourceHead"] != source_head):
        raise Task8Error("packaged runtime identity differs")
    return value


def _receipt_launch_leaf(repo_root: Path) -> str:
    receipt = _load_report(_resolve_receipt_path(repo_root, "CorsairsUE"))
    launch = receipt.get("Launch")
    if type(launch) is not str:
        raise Task8Error("Game target receipt lacks Launch")
    source = _receipt_project_path(launch, repo_root / "CorsairsUE")
    if source is None:
        raise Task8Error("Game receipt Launch is not project-owned")
    leaf = source.name
    if not leaf or "/" in leaf or "\\" in leaf:
        raise Task8Error("Game receipt Launch leaf is invalid")
    return leaf


def _archived_launch_evidence(
    archive_report: dict[str, Any],
    archive_root: Path,
    repo_root: Path,
    launch_leaf: str,
) -> dict[str, Any]:
    """Map receipt Launch through the complete archive inventory."""
    archive_prefix = archive_root.resolve().relative_to(
        repo_root.resolve()).as_posix()
    suffix = f"/Contents/MacOS/{launch_leaf}"
    candidates = []
    for item in archive_report["files"]:
        path = item["path"]
        if not path.startswith(archive_prefix + "/") or not path.endswith(suffix):
            continue
        app_relative = path[len(archive_prefix) + 1:-len(suffix)]
        if (not app_relative or
                not PurePosixPath(app_relative).name.endswith(".app")):
            continue
        candidates.append(item)
    if len(candidates) != 1:
        raise Task8Error(
            "archive inventory must map receipt Launch to exactly one .app "
            f"executable, observed={len(candidates)}")
    physical = repo_root / PurePosixPath(candidates[0]["path"])
    info = _physical_regular(physical)
    if not os.access(physical, os.X_OK) or info.st_size != candidates[0]["sizeBytes"]:
        raise Task8Error("archive launch product is not an executable regular file")
    if _sha256(physical) != candidates[0]["sha256"]:
        raise Task8Error("archive launch product changed after inventory")
    return candidates[0]


def prepare_package_evidence(
    transaction: OuterTransaction,
    source_head: str,
    game_build: dict[str, Any],
    *,
    process_probe: Callable[[], Iterable[ProcessRecord]] = probe_process_table,
    thermal_probe: Callable[[], tuple[int, str]] = probe_thermal,
) -> dict[str, Any]:
    repo_root = transaction.repo_root
    transaction_id = transaction.journal["transactionId"]
    package_root = package_run_root(repo_root, transaction_id)
    stage_root = package_root / "stage"
    archive_root = package_root / "archive"
    report_root = evidence_root(repo_root, transaction_id) / "package"
    report_root.mkdir(parents=True, exist_ok=False, mode=0o700)
    _physical_directory(report_root)
    stage_report_path = report_root / "stage-inventory.json"
    archive_report_path = report_root / "archive-inventory.json"
    stage_report = write_inventory_report(
        stage_root, stage_report_path, "garner-terrain-stage-inventory",
        repo_root, transaction_id, source_head)
    archive_report = write_inventory_report(
        archive_root, archive_report_path, "garner-terrain-archive-inventory",
        repo_root, transaction_id, source_head)
    leaks = sorted({item["path"] for item in (
        stage_report["files"] + archive_report["files"])
        if "corsairsimport" in item["path"].lower()})
    if leaks:
        raise Task8Error(f"CorsairsImport leaked into Game package: {leaks}")
    containers = [
        item for item in archive_report["files"]
        if PurePosixPath(item["path"]).suffix.lower() == ".pak"
    ]
    if not containers:
        raise Task8Error("archive inventory contains no .pak container")
    unrealpak = Path(
        "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealPak")
    _physical_regular(unrealpak)
    container_lists = []
    parsed_lists = []
    for index, container in enumerate(containers):
        refuse_competing_processes(process_probe())
        require_healthy_thermal(*thermal_probe())
        physical = repo_root / PurePosixPath(container["path"])
        result = transaction.run_owned_command(CommandSpec(
            "cook-package", _nice(str(unrealpak), str(physical), "-List")))
        if result.returncode != 0:
            raise Task8Error(f"UnrealPak -List failed: {result.stderr.strip()}")
        members = parse_unrealpak_list(result.stdout + "\n" + result.stderr)
        if any("corsairsimport" in item["path"].lower() for item in members):
            raise Task8Error("CorsairsImport member leaked into packaged container")
        list_report = {
            "schemaVersion": 1,
            "reportType": "garner-terrain-container-list",
            "transactionId": transaction_id,
            "sourceHead": source_head,
            "container": container,
            "members": members,
            "issues": [],
        }
        list_path = report_root / f"container-{index:02}.json"
        rules.atomic_write_json(list_path, list_report)
        issues = rules.validate_container_list_report(list_report, repo_root)
        if issues:
            raise Task8Error(f"container-list self-validation failed: {issues[0]}")
        container_lists.append(_evidence(list_path, repo_root))
        parsed_lists.append(list_report)
    executable_evidence = _archived_launch_evidence(
        archive_report, archive_root, repo_root, _receipt_launch_leaf(repo_root))
    extraction_root = report_root / "extracted"
    _mkdir_exclusive(extraction_root)
    runtime_files = []
    for relative, member_path in zip(rules.RUNTIME_INPUT_PATHS,
                                     rules.RUNTIME_CONTAINER_PATHS):
        matches = [(report, container) for report, container in zip(parsed_lists, containers)
                   if any(item["path"] == member_path for item in report["members"])]
        if len(matches) != 1:
            raise Task8Error(
                f"runtime UFS member must occur in exactly one container: {member_path}")
        _, container = matches[0]
        physical = repo_root / PurePosixPath(container["path"])
        member_root = extraction_root / hashlib.sha256(
            member_path.encode("utf-8")).hexdigest()[:16]
        _mkdir_exclusive(member_root)
        refuse_competing_processes(process_probe())
        require_healthy_thermal(*thermal_probe())
        result = transaction.run_owned_command(CommandSpec(
            "cook-package", _nice(
                str(unrealpak), str(physical), f"-Extract={member_root}",
                f"-Filter={member_path}")))
        if result.returncode != 0:
            raise Task8Error(f"UnrealPak extraction failed: {result.stderr.strip()}")
        extracted = member_root / PurePosixPath(member_path[9:])
        extracted_evidence = _evidence(extracted, repo_root)
        source = repo_root / "CorsairsUE" / PurePosixPath(relative)
        source_evidence = _evidence(source, repo_root)
        if (extracted_evidence["sha256"] != source_evidence["sha256"] or
                extracted_evidence["sizeBytes"] != source_evidence["sizeBytes"]):
            raise Task8Error(f"packaged runtime member differs: {relative}")
        runtime_files.append({
            "projectRelativePath": relative,
            "containerPath": container["path"],
            "containerMemberPath": member_path,
            "extractedEvidence": extracted_evidence,
            "sourceSha256": source_evidence["sha256"],
            "sourceSizeBytes": source_evidence["sizeBytes"],
        })
    cook_report = {
        "schemaVersion": 1,
        "reportType": "garner-terrain-cook-package",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "targetReceipt": game_build["receipt"],
        "stageManifest": _evidence(stage_report_path, repo_root),
        "archiveManifest": _evidence(archive_report_path, repo_root),
        "containers": containers,
        "containerLists": container_lists,
        "packagedExecutable": executable_evidence,
        "runtimeFiles": runtime_files,
        "corsairsImportLeaks": [],
        "issues": [],
    }
    cook_report_path = report_root / "cook-package.json"
    rules.atomic_write_json(cook_report_path, cook_report)
    cook_issues = rules.validate_cook_package_report(cook_report, repo_root)
    if cook_issues:
        raise Task8Error(f"cook-package report self-validation failed: {cook_issues[0]}")
    return {
        "cookReport": cook_report,
        "cookReportEvidence": _evidence(cook_report_path, repo_root),
        "package": {
            "transactionId": transaction_id,
            "sourceHead": source_head,
            "targetReceipt": game_build["receipt"],
            "stageManifest": _evidence(stage_report_path, repo_root),
            "archiveManifest": _evidence(archive_report_path, repo_root),
            "containers": containers,
            "containerLists": container_lists,
            "packagedExecutable": executable_evidence,
            "runtimeFiles": runtime_files,
        },
    }


def finalize_package_evidence(
    prepared: dict[str, Any], observation: dict[str, Any],
) -> dict[str, Any]:
    package = json.loads(json.dumps(prepared["package"]))
    for packaged, runtime in zip(package["runtimeFiles"], observation["runtimeInputs"]):
        if (packaged["projectRelativePath"] != runtime["projectRelativePath"] or
                packaged["sourceSha256"] != runtime["sha256"] or
                packaged["sourceSizeBytes"] != runtime["sizeBytes"]):
            raise Task8Error("packaged runtime/source observation differs")
        packaged["runtimeReportedSha256"] = runtime["sha256"]
        packaged["runtimeReportedSizeBytes"] = runtime["sizeBytes"]
    return package


def build_base_bundle(
    transaction: OuterTransaction, context: dict[str, Any], source_head: str,
) -> dict[str, Any]:
    root = transaction.repo_root
    transaction_id = transaction.journal["transactionId"]
    manifest = context["manifest"]
    run_id = context["runId"]
    run_files = []
    for key in rules._MANIFEST_LEAVES:  # exact insertion order is canonical leaf order
        physical = root / "artifacts/maps" / PurePosixPath(manifest["files"][key]["path"])
        run_files.append(_evidence(physical, root))
    bundle = {
        "schemaVersion": 1,
        "reportType": "garner-terrain-base",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "terrainManifest": {
            "top": _evidence(root / TOP_MANIFEST, root),
            "runId": run_id,
            "runFiles": run_files,
        },
        "runtimeInputs": [
            _evidence(root / "CorsairsUE" / PurePosixPath(relative), root)
            for relative in rules.RUNTIME_INPUT_PATHS
        ],
        "reports": {
            "levelBuild": context["levelEvidence"],
            "importPass1": context["pass1Evidence"],
            "importPass2": context["pass2Evidence"],
            "terrainCheck": context["checkEvidence"],
            "editorAutomation": context["editorAutomation"],
            "cookPackage": context["preparedPackage"]["cookReportEvidence"],
            "runtimeAutomation": context["runtimeAutomation"],
            "runtimeObservation": context["runtimeObservationEvidence"],
        },
        "finalPackageHashes": context["checkReport"]["finalPackageHashes"],
        "builds": {
            "editor": context["editorBuild"],
            "game": context["gameBuild"],
        },
        "package": context["package"],
        "issues": [],
    }
    return bundle


def _set_phase(
    transaction: OuterTransaction, phase: str, completed_step: str,
) -> None:
    transaction.journal["phase"] = phase
    transaction.journal["completedStep"] = completed_step
    transaction.persist()


def _production_after_step(
    transaction: OuterTransaction,
    context: dict[str, Any],
    source_head: str,
    command: CommandSpec,
    result: CommandResult,
) -> None:
    root = transaction.repo_root
    transaction_id = transaction.journal["transactionId"]
    reports = evidence_root(root, transaction_id)
    if command.name == "terrain-reference":
        manifest, run_id = _require_valid_manifest(root)
        context["manifest"] = manifest
        context["runId"] = run_id
        _set_phase(transaction, "TASK7_PUBLISHED", "terrain-reference")
    elif command.name == "installer-1":
        status_value = result.stdout.strip()
        if status_value not in ("OK", "NOOP"):
            raise Task8Error(f"installer 1 returned noncanonical result: {status_value}")
        context["installer1"] = status_value
    elif command.name == "installer-2":
        if result.stdout.strip() != "NOOP":
            raise Task8Error("installer 2 must be an exact NOOP")
        _validate_installed_pair(root, context["manifest"])
        _set_phase(transaction, "RUNTIME_INSTALLED", "installer-2")
    elif command.name == "editor-build":
        context["editorBuild"] = copy_build_evidence(
            root, transaction_id, source_head, "CorsairsUEEditor")
    elif command.name == "level-build":
        path = reports / "reference-terrain-level-build.json"
        report = _load_report(path)
        issues = rules.validate_level_build_report(report, root)
        if issues:
            raise Task8Error(f"level-build report failed: {issues[0]}")
        context["levelReport"] = report
        context["levelEvidence"] = _evidence(path, root)
    elif command.name in ("import-1", "import-2"):
        suffix = "pass1" if command.name == "import-1" else "pass2"
        path = reports / f"reference-terrain-import-{suffix}.json"
        report = _load_report(path)
        issues = rules.validate_import_report(report, root)
        if issues:
            raise Task8Error(f"{command.name} report failed: {issues[0]}")
        context[f"{suffix}Report"] = report
        context[f"{suffix}Evidence"] = _evidence(path, root)
        if command.name == "import-2":
            relation = rules.validate_idempotent_import_reports(
                context["pass1Report"], report)
            if relation:
                raise Task8Error(f"second import is not idempotent: {relation[0]}")
    elif command.name == "checker":
        path = reports / "reference-terrain-check.json"
        report = _load_report(path)
        issues = rules.validate_check_report(report, root)
        if issues:
            raise Task8Error(f"terrain checker report failed: {issues[0]}")
        context["checkReport"] = report
        context["checkEvidence"] = _evidence(path, root)
    elif command.name == "editor-automation":
        context["editorAutomation"] = _evidence_set(
            reports / "reference-terrain-editor-automation",
            root, transaction_id, source_head)
        _set_phase(transaction, "EDITOR_VERIFIED", "editor-automation")
    elif command.name == "game-build":
        context["gameBuild"] = copy_build_evidence(
            root, transaction_id, source_head, "CorsairsUE")
    elif command.name == "cook-package":
        context["preparedPackage"] = prepare_package_evidence(
            transaction, source_head, context["gameBuild"])
    elif command.name == "runtime-smoke":
        observation = parse_runtime_observation_event(
            result.stdout + "\n" + result.stderr, transaction_id, source_head)
        observation_path = reports / "reference-terrain-runtime-observation.json"
        rules.atomic_write_json(observation_path, observation)
        runtime_issues = rules.validate_runtime_observation(observation)
        if runtime_issues:
            raise Task8Error(f"runtime observation failed: {runtime_issues[0]}")
        context["runtimeObservation"] = observation
        context["runtimeObservationEvidence"] = _evidence(observation_path, root)
        context["runtimeAutomation"] = _evidence_set(
            reports / "reference-terrain-runtime",
            root, transaction_id, source_head)
        context["package"] = finalize_package_evidence(
            context["preparedPackage"], observation)
        _set_phase(transaction, "PACKAGE_VERIFIED", "runtime-smoke")


def _production_nested_callbacks(
    transaction: OuterTransaction,
    source_head: str,
) -> tuple[Callable[[str, tuple[Path, ...]], None], Callable[[str], None]]:
    commands = production_commands(
        transaction.repo_root, transaction.journal["transactionId"], source_head)
    originals = {"publisher": commands[0], "installer": commands[1]}

    def retry(owner: str, _paths: tuple[Path, ...]) -> None:
        refuse_competing_processes(probe_process_table())
        require_healthy_thermal(*probe_thermal())
        original = originals[owner]
        name = ("nested-publisher-recovery" if owner == "publisher" else
                "nested-installer-recovery")
        result = transaction.run_owned_command(CommandSpec(
            name, original.argv, original.env, original.timeout_seconds))
        if result.returncode != 0:
            raise Task8Error(f"nested {owner} retry failed: {result.stderr.strip()}",
                             status="RECOVERY_REQUIRED")

    def validate(owner: str) -> None:
        manifest, _ = _require_valid_manifest(transaction.repo_root)
        if owner == "installer":
            _validate_installed_pair(transaction.repo_root, manifest)

    return retry, validate


def run_task8(
    repo_root: Path | str,
    manifest_path: Path | str,
    map_package: str,
    output_root: Path | str,
    *,
    recover_only: bool = False,
) -> str:
    root = Path(repo_root).resolve(strict=True)
    _physical_directory(root)
    manifest = Path(manifest_path)
    if not manifest.is_absolute():
        manifest = root / manifest
    output = Path(output_root)
    if not output.is_absolute():
        output = root / output
    if (manifest.resolve(strict=False) != (root / TOP_MANIFEST).resolve(strict=False) or
            output.resolve(strict=False) !=
            (root / "artifacts/maps/reports").resolve(strict=False) or
            map_package != rules.REFERENCE_MAP_PACKAGE):
        raise Task8Error("manifest/map/output must equal the canonical Task 8 paths")

    acquired = OuterTransaction.acquire(root)
    try:
        head, status_value = git_status_gate(root)
        if acquired.journal:
            if status_value:
                raise Task8Error("recovery requires the journal's clean checkout",
                                 status="RECOVERY_REQUIRED")
            nested_retry, nested_validate = _production_nested_callbacks(acquired, head)
            acquired.recover(
                head, nested_retry=nested_retry, nested_validate=nested_validate)
            acquired.finish()
            acquired = None
        else:
            acquired.close()
            acquired = None
        if recover_only:
            return "RECOVERED"
    finally:
        if acquired is not None:
            acquired.close()

    transaction_id = os.urandom(16).hex()
    commands = preflight_plan(
        root, transaction_id, status_gate=lambda: git_status_gate(root),
        process_probe=probe_process_table, thermal_probe=probe_thermal)
    source_head, _ = git_status_gate(root)
    transaction = OuterTransaction.begin(root, transaction_id, source_head)
    context: dict[str, Any] = {}
    try:
        execute_command_plan(
            transaction, commands,
            after_step=lambda command, result: _production_after_step(
                transaction, context, source_head, command, result),
            resolve_command=lambda command: resolve_runtime_command(
                command, root,
                context["preparedPackage"]["package"]["packagedExecutable"])
            if command.name == "runtime-smoke" else command)
        bundle = build_base_bundle(transaction, context, source_head)
        transaction.publish_bundle(
            bundle,
            validate=lambda value, path: rules.validate_base_bundle(
                value, path, root))
        final_head, final_status = git_status_gate(root)
        if final_head != source_head or final_status:
            raise Task8Error("clean source HEAD changed before COMMITTED")
        published = _load_report(root / BASE_BUNDLE)
        issues = rules.validate_base_bundle(published, root / BASE_BUNDLE, root)
        if issues or published != bundle:
            raise Task8Error(f"published base validation failed: {issues[:1]}")
        transaction.journal["phase"] = "COMMITTED"
        transaction.journal["completedStep"] = "base-published"
        transaction.persist()
        transaction.finish()
        return transaction_id
    except BaseException as exc:
        try:
            if transaction.lock_descriptor >= 0 and transaction.journal:
                nested_retry, nested_validate = _production_nested_callbacks(
                    transaction, source_head)
                transaction._resolve_inner_owners(nested_retry, nested_validate)
                if transaction.journal["phase"] != "ROLLED_BACK":
                    transaction.rollback()
        except BaseException as rollback_error:
            transaction.close()
            raise Task8Error(
                f"Task 8 failed ({exc}); recovery failed ({rollback_error})",
                status="RECOVERY_REQUIRED") from rollback_error
        transaction.close()
        if isinstance(exc, Task8Error):
            raise
        raise Task8Error(f"Task 8 failed: {exc}") from exc


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--map", dest="map_package", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--recover-only", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        result = run_task8(
            arguments.repo_root, arguments.manifest, arguments.map_package,
            arguments.output, recover_only=arguments.recover_only)
    except Task8Error as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
