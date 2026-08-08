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
import plistlib
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
SANDBOX_REPORT_TOKEN = "__TASK8_SANDBOX_REPORT_ROOT__"
SANDBOX_EVENT_PREFIX = "CORSAIRS_TERRAIN_SANDBOX_JSON="
RUNTIME_EVENT_PREFIX = "CORSAIRS_TERRAIN_RUNTIME_JSON="
RUNTIME_AUTOMATION_DIRECTORY = "reference-terrain-runtime"
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
_BUNDLE_IDENTIFIER = re.compile(
    r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\Z")
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
    "editor-automation", "game-build", "cook-package", "runtime-report-probe",
    "runtime-smoke", "base-validated", "base-published",
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


class SimulatedCrash(Task8Error):
    """Test-only abrupt-stop seam; callers must not perform in-process cleanup."""


class _ProcessIdentityMismatch(Task8Error):
    """A recorded PGID may now belong to a different process; never signal it."""


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


@dataclass(frozen=True)
class SandboxReportScratch:
    reservation_path: Path
    transaction_root: Path
    report_root: Path
    destination_root: Path


@dataclass(frozen=True)
class VerifiedSandboxApp:
    app_bundle_path: str
    bundle_identifier: str
    signing_identifier: str
    packaged_executable: dict[str, Any]
    info_plist: dict[str, Any]
    canonical_entitlements: bytes


@dataclass(frozen=True)
class AttestedDirectoryComponent:
    path: Path
    identity: tuple[int, int, int, int]


@dataclass(frozen=True)
class AttestedSandboxEnvironment:
    transaction_id: str
    source_head: str
    bundle_identifier: str
    container_root: Path
    container_root_identity: tuple[int, int, int, int]
    container_data_root: Path
    container_data_identity: tuple[int, int, int, int]
    automation_reports_root: Path
    automation_reports_identity: tuple[int, int, int, int]
    directory_chain: tuple[AttestedDirectoryComponent, ...]
    metadata_path: Path
    metadata_fingerprint: tuple[int, ...]
    metadata_sha256: str
    attestation_evidence: dict[str, Any]


@dataclass(frozen=True)
class SandboxTreeRecord:
    kind: str
    path: str
    mode: int
    sha256: str
    size_bytes: int
    identity: tuple[int, ...]


@dataclass(frozen=True)
class SandboxTreeSnapshot:
    root_identity: tuple[int, ...]
    records: tuple[SandboxTreeRecord, ...]


@dataclass(frozen=True)
class OwnedDirectory:
    path: Path
    device: int
    inode: int
    owner_uid: int
    tree_snapshot: SandboxTreeSnapshot | None = None


@dataclass
class _OpenTreeDirectory:
    descriptor: int
    parent_descriptor: int | None
    name: str
    relative: str
    fingerprint: tuple[int, ...]
    names: set[str]


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
            "-skipbuild", "-cook", "-stage", "-pak", "-package", "-archive",
            "-map=/Game/Maps/Garner",
            "-AdditionalCookerOptions=-SkipZenStore",
            f"-CookOutputDir={package_root / 'cooked' / 'Mac'}",
            f"-stagingdirectory={package_root / 'stage'}",
            f"-archivedirectory={package_root / 'archive'}",
            "-MaxParallelActions=2")),
        CommandSpec("runtime-report-probe", _nice(
            ARCHIVE_EXECUTABLE_TOKEN,
            "-unattended", "-NullRHI", "-NoSound", "-stdout",
            "-FullStdOutLogOutput", f"-CorsairsTerrainTransaction={transaction_id}",
            f"-CorsairsTerrainSourceHead={source_head}",
            "-CorsairsTerrainSandboxProbe",
            "-ExecCmds=Automation RunTests Corsairs.Terrain.ReferenceRuntime",
            "-TestExit=Automation Test Queue Empty")),
        CommandSpec("runtime-smoke", _nice(
            ARCHIVE_EXECUTABLE_TOKEN,
            "-unattended", "-NullRHI", "-NoSound", "-stdout",
            "-FullStdOutLogOutput", f"-CorsairsTerrainTransaction={transaction_id}",
            f"-CorsairsTerrainSourceHead={source_head}",
            "-ExecCmds=Automation RunTests Corsairs.Terrain.ReferenceRuntime",
            "-TestExit=Automation Test Queue Empty",
            SANDBOX_REPORT_TOKEN)),
    ]
    return commands


def resolve_runtime_command(
    command: CommandSpec,
    repo_root: Path | str,
    verified_app: VerifiedSandboxApp,
    sandbox_scratch: SandboxReportScratch | None = None,
    *,
    transaction: "OuterTransaction | None" = None,
) -> CommandSpec:
    """Bind packaged probe/smoke to the inventory launch and owned scratch."""
    if command.name not in ("runtime-report-probe", "runtime-smoke"):
        return command
    if command.argv.count(ARCHIVE_EXECUTABLE_TOKEN) != 1:
        raise Task8Error("runtime command lacks its executable binding token")
    if transaction is None:
        raise Task8Error("packaged command lacks signed-app journal binding")
    root = Path(repo_root).resolve()
    if root != transaction.repo_root:
        raise Task8Error("packaged command repository differs from journal")
    _, physical, _, _, _ = _validate_verified_sandbox_app(
        transaction, verified_app)
    argv = tuple(
        str(physical) if item == ARCHIVE_EXECUTABLE_TOKEN else item
        for item in command.argv)
    if command.name == "runtime-report-probe":
        if (SANDBOX_REPORT_TOKEN in argv or any(
                item.startswith("-ReportExportPath=") for item in argv) or
                argv.count("-CorsairsTerrainSandboxProbe") != 1):
            raise Task8Error("sandbox probe command arguments differ")
        return CommandSpec(command.name, argv, command.env, command.timeout_seconds)
    if type(sandbox_scratch) is not SandboxReportScratch:
        raise Task8Error("runtime command lacks owned sandbox scratch")
    _validate_prepared_sandbox_scratch(transaction, sandbox_scratch)
    if argv.count(SANDBOX_REPORT_TOKEN) != 1:
        raise Task8Error("runtime command lacks sandbox report binding token")
    report_info = _physical_directory(sandbox_scratch.report_root)
    if (report_info.st_uid != os.geteuid() or
            stat.S_IMODE(report_info.st_mode) != 0o700 or
            os.listdir(sandbox_scratch.report_root)):
        raise Task8Error("sandbox runtime report directory ownership/state differs")
    if (sandbox_scratch.destination_root.exists() or
            sandbox_scratch.destination_root.is_symlink()):
        raise Task8Error("runtime report destination exists before sandbox copy")
    report_argument = f"-ReportExportPath={sandbox_scratch.report_root}"
    argv = tuple(
        report_argument if item == SANDBOX_REPORT_TOKEN else item for item in argv)
    if argv.count(report_argument) != 1 or "-CorsairsTerrainSandboxProbe" in argv:
        raise Task8Error("runtime command sandbox report argument differs")
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


def _open_physical_regular(
    path: Path, *, allow_empty: bool = False,
    status: str = "FAILED",
) -> tuple[int, os.stat_result]:
    """Open one stable physical file without following its final component."""
    flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
             getattr(os, "O_NOFOLLOW", 0))
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        path_info = path.lstat()
        if (stat.S_ISLNK(path_info.st_mode) or
                not stat.S_ISREG(info.st_mode) or
                getattr(info, "st_nlink", 1) != 1 or
                (path_info.st_dev, path_info.st_ino) !=
                (info.st_dev, info.st_ino) or
                (not allow_empty and info.st_size <= 0)):
            raise OSError("expected stable physical non-hard-linked regular file")
    except OSError as exc:
        if descriptor >= 0:
            os.close(descriptor)
        raise Task8Error(
            f"cannot open physical regular file without following links: {path}",
            status=status) from exc
    return descriptor, info


def _regular_fingerprint(info: os.stat_result) -> tuple[int, ...]:
    return (
        int(info.st_dev), int(info.st_ino), int(info.st_uid),
        stat.S_IMODE(info.st_mode), int(getattr(info, "st_nlink", 1)),
        int(info.st_size), int(info.st_mtime_ns), int(info.st_ctime_ns),
    )


def _directory_fingerprint(info: os.stat_result) -> tuple[int, ...]:
    return (
        int(info.st_dev), int(info.st_ino), int(info.st_uid),
        stat.S_IMODE(info.st_mode), int(getattr(info, "st_nlink", 1)),
        int(info.st_mtime_ns), int(info.st_ctime_ns),
    )


def _finish_physical_regular(
    path: Path, descriptor: int, before: os.stat_result, *, status: str,
) -> os.stat_result:
    try:
        after = os.fstat(descriptor)
        path_after = path.lstat()
    except OSError as exc:
        raise Task8Error(f"physical file changed while open: {path}",
                         status=status) from exc
    if (stat.S_ISLNK(path_after.st_mode) or
            (path_after.st_dev, path_after.st_ino) !=
            (after.st_dev, after.st_ino) or
            _regular_fingerprint(after) != _regular_fingerprint(before)):
        raise Task8Error(f"physical file changed while open: {path}",
                         status=status)
    return after


def _hash_physical_regular(
    path: Path, *, allow_empty: bool = False, status: str = "FAILED",
) -> tuple[os.stat_result, str]:
    descriptor, before = _open_physical_regular(
        path, allow_empty=allow_empty, status=status)
    digest = hashlib.sha256()
    size = 0
    try:
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            digest.update(block)
            size += len(block)
        _finish_physical_regular(path, descriptor, before, status=status)
    finally:
        os.close(descriptor)
    if size != before.st_size:
        raise Task8Error(f"physical file size changed while hashing: {path}",
                         status=status)
    return before, digest.hexdigest()


def _read_physical_regular(
    path: Path, *, allow_empty: bool = False, status: str = "FAILED",
) -> tuple[os.stat_result, bytes]:
    descriptor, before = _open_physical_regular(
        path, allow_empty=allow_empty, status=status)
    payload = bytearray()
    try:
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            payload.extend(block)
        _finish_physical_regular(path, descriptor, before, status=status)
    finally:
        os.close(descriptor)
    if len(payload) != before.st_size:
        raise Task8Error(f"physical file size changed while reading: {path}",
                         status=status)
    return before, bytes(payload)


def _sync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _directory_descriptor_flags() -> int:
    required = ("O_DIRECTORY", "O_NOFOLLOW", "O_CLOEXEC")
    if any(not hasattr(os, name) for name in required):
        raise Task8Error(
            "descriptor-relative no-follow directory operations are unsupported",
            status="RECOVERY_REQUIRED")
    return os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def _regular_descriptor_flags() -> int:
    required = ("O_NOFOLLOW", "O_CLOEXEC")
    if any(not hasattr(os, name) for name in required):
        raise Task8Error(
            "descriptor-relative no-follow file operations are unsupported",
            status="RECOVERY_REQUIRED")
    return os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC


def _same_entry(left: os.stat_result, right: os.stat_result) -> bool:
    return (left.st_dev, left.st_ino) == (right.st_dev, right.st_ino)


def _open_pinned_directory(
    path: Path, *, status: str = "RECOVERY_REQUIRED",
) -> tuple[int, os.stat_result]:
    """Open every absolute component without following a renamed symlink."""
    path_text = os.fspath(path)
    if (not path.is_absolute() or path_text != os.path.normpath(path_text) or
            any(part in ("", ".", "..") for part in path.parts[1:])):
        raise Task8Error(f"directory path is not normalized absolute: {path}",
                         status=status)
    flags = _directory_descriptor_flags()
    descriptor = -1
    try:
        descriptor = os.open(os.sep, flags)
        for component in path.parts[1:]:
            before = os.stat(
                component, dir_fd=descriptor, follow_symlinks=False)
            if not stat.S_ISDIR(before.st_mode):
                raise OSError(f"non-directory component {component}")
            child = os.open(component, flags, dir_fd=descriptor)
            try:
                opened = os.fstat(child)
                after = os.stat(
                    component, dir_fd=descriptor, follow_symlinks=False)
                if (not stat.S_ISDIR(opened.st_mode) or
                        not _same_entry(before, opened) or
                        not _same_entry(opened, after)):
                    raise OSError(f"directory component changed: {component}")
            except BaseException:
                os.close(child)
                raise
            os.close(descriptor)
            descriptor = child
        return descriptor, os.fstat(descriptor)
    except (OSError, ValueError) as exc:
        if descriptor >= 0:
            os.close(descriptor)
        raise Task8Error(f"cannot pin physical directory: {path}",
                         status=status) from exc


def _entry_stat_at(
    parent_descriptor: int, name: str, *, status: str = "RECOVERY_REQUIRED",
) -> os.stat_result | None:
    if (not name or name in (".", "..") or "/" in name or "\\" in name):
        raise Task8Error(f"invalid descriptor-relative leaf: {name}",
                         status=status)
    try:
        return os.stat(name, dir_fd=parent_descriptor, follow_symlinks=False)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise Task8Error(f"cannot inspect descriptor-relative leaf: {name}",
                         status=status) from exc


def _open_directory_at(
    parent_descriptor: int,
    name: str,
    *,
    expected_identity: list[int] | None = None,
    status: str = "RECOVERY_REQUIRED",
) -> tuple[int, os.stat_result]:
    before = _entry_stat_at(parent_descriptor, name, status=status)
    if before is None or not stat.S_ISDIR(before.st_mode):
        raise Task8Error(f"expected physical directory leaf: {name}", status=status)
    descriptor = -1
    try:
        descriptor = os.open(
            name, _directory_descriptor_flags(), dir_fd=parent_descriptor)
        opened = os.fstat(descriptor)
        after = _entry_stat_at(parent_descriptor, name, status=status)
        if (after is None or not stat.S_ISDIR(opened.st_mode) or
                not _same_entry(before, opened) or
                not _same_entry(opened, after)):
            raise Task8Error(f"directory leaf changed while opening: {name}",
                             status=status)
        observed = [
            int(opened.st_dev), int(opened.st_ino), int(opened.st_uid),
            stat.S_IMODE(opened.st_mode),
        ]
        if (opened.st_uid != os.geteuid() or
                (expected_identity is not None and observed != expected_identity)):
            raise Task8Error(f"directory leaf identity differs: {name}",
                             status=status)
        return descriptor, opened
    except BaseException:
        if descriptor >= 0:
            os.close(descriptor)
        raise


def _open_regular_at(
    parent_descriptor: int,
    name: str,
    *,
    expected_identity: list[int] | None = None,
    status: str = "RECOVERY_REQUIRED",
) -> tuple[int, os.stat_result]:
    before = _entry_stat_at(parent_descriptor, name, status=status)
    if (before is None or not stat.S_ISREG(before.st_mode) or
            getattr(before, "st_nlink", 1) != 1):
        raise Task8Error(f"expected physical regular leaf: {name}", status=status)
    descriptor = -1
    try:
        descriptor = os.open(
            name, _regular_descriptor_flags(), dir_fd=parent_descriptor)
        opened = os.fstat(descriptor)
        after = _entry_stat_at(parent_descriptor, name, status=status)
        if (after is None or not stat.S_ISREG(opened.st_mode) or
                getattr(opened, "st_nlink", 1) != 1 or
                not _same_entry(before, opened) or
                not _same_entry(opened, after)):
            raise Task8Error(f"regular leaf changed while opening: {name}",
                             status=status)
        observed = [
            int(opened.st_dev), int(opened.st_ino), int(opened.st_uid),
            stat.S_IMODE(opened.st_mode), int(opened.st_size),
        ]
        if (opened.st_uid != os.geteuid() or
                (expected_identity is not None and observed != expected_identity)):
            raise Task8Error(f"regular leaf identity differs: {name}",
                             status=status)
        return descriptor, opened
    except BaseException:
        if descriptor >= 0:
            os.close(descriptor)
        raise


def _read_descriptor(descriptor: int) -> bytes:
    os.lseek(descriptor, 0, os.SEEK_SET)
    payload = bytearray()
    while True:
        block = os.read(descriptor, 1024 * 1024)
        if not block:
            return bytes(payload)
        payload.extend(block)


def _verify_regular_at(
    parent_descriptor: int,
    name: str,
    *,
    expected_identity: list[int] | None = None,
    expected_payload: bytes | None = None,
    expected_sha256: str | None = None,
    expected_size: int | None = None,
    expected_mode: int | None = None,
    status: str = "RECOVERY_REQUIRED",
) -> tuple[int, os.stat_result]:
    descriptor, before = _open_regular_at(
        parent_descriptor, name, expected_identity=expected_identity, status=status)
    try:
        payload = _read_descriptor(descriptor)
        after = os.fstat(descriptor)
        entry_after = _entry_stat_at(parent_descriptor, name, status=status)
        digest = hashlib.sha256(payload).hexdigest()
        if (entry_after is None or not _same_entry(before, after) or
                not _same_entry(after, entry_after) or
                len(payload) != after.st_size or
                (expected_payload is not None and payload != expected_payload) or
                (expected_sha256 is not None and digest != expected_sha256) or
                (expected_size is not None and after.st_size != expected_size) or
                (expected_mode is not None and
                 stat.S_IMODE(after.st_mode) != expected_mode)):
            raise Task8Error(f"regular leaf verification differs: {name}",
                             status=status)
        return descriptor, after
    except BaseException:
        os.close(descriptor)
        raise


def _unlink_open_regular_at(
    parent_descriptor: int,
    name: str,
    descriptor: int,
    opened: os.stat_result,
    transaction: "OuterTransaction" | None,
    fault: Callable[[str], Any] | None,
) -> None:
    try:
        expected_fingerprint = _regular_fingerprint(opened)
        current = _entry_stat_at(parent_descriptor, name)
        held = os.fstat(descriptor)
        if (current is None or expected_fingerprint[4] != 1 or
                _regular_fingerprint(current) != expected_fingerprint or
                _regular_fingerprint(held) != expected_fingerprint):
            raise Task8Error(f"regular leaf changed before unlink: {name}",
                             status="RECOVERY_REQUIRED")
        if transaction is not None:
            transaction._fault(fault, "BEFORE_SANDBOX_DESCENDANT_REMOVE")
        current = _entry_stat_at(parent_descriptor, name)
        held = os.fstat(descriptor)
        if (current is None or
                _regular_fingerprint(current) != expected_fingerprint or
                _regular_fingerprint(held) != expected_fingerprint):
            raise Task8Error(f"regular leaf changed after unlink hook: {name}",
                             status="RECOVERY_REQUIRED")
        os.unlink(name, dir_fd=parent_descriptor)
        held_after = os.fstat(descriptor)
        if (held_after.st_nlink != 0 or
                not _same_entry(opened, held_after) or
                _entry_stat_at(parent_descriptor, name) is not None):
            raise Task8Error(f"regular leaf unlink was not exact: {name}",
                             status="RECOVERY_REQUIRED")
        if transaction is not None:
            transaction._fault(fault, "AFTER_SANDBOX_DESCENDANT_REMOVE")
    finally:
        os.close(descriptor)


def _remove_open_directory_at(
    parent_descriptor: int,
    name: str,
    descriptor: int,
    opened: os.stat_result,
    transaction: "OuterTransaction" | None,
    fault: Callable[[str], Any] | None,
) -> None:
    try:
        if os.listdir(descriptor):
            raise Task8Error(f"directory leaf is not empty: {name}",
                             status="RECOVERY_REQUIRED")
        held_before = os.fstat(descriptor)
        expected_fingerprint = _directory_fingerprint(held_before)
        current = _entry_stat_at(parent_descriptor, name)
        if (current is None or not _same_entry(opened, held_before) or
                _directory_fingerprint(current) != expected_fingerprint):
            raise Task8Error(f"directory leaf changed before rmdir: {name}",
                             status="RECOVERY_REQUIRED")
        if transaction is not None:
            transaction._fault(fault, "BEFORE_SANDBOX_DESCENDANT_REMOVE")
        current = _entry_stat_at(parent_descriptor, name)
        held = os.fstat(descriptor)
        if (current is None or
                _directory_fingerprint(current) != expected_fingerprint or
                _directory_fingerprint(held) != expected_fingerprint):
            raise Task8Error(f"directory leaf changed after rmdir hook: {name}",
                             status="RECOVERY_REQUIRED")
        os.rmdir(name, dir_fd=parent_descriptor)
        held_after = os.fstat(descriptor)
        if (_entry_stat_at(parent_descriptor, name) is not None or
                _directory_fingerprint(held_after) != expected_fingerprint):
            raise Task8Error(f"directory leaf rmdir was not exact: {name}",
                             status="RECOVERY_REQUIRED")
        if transaction is not None:
            transaction._fault(fault, "AFTER_SANDBOX_DESCENDANT_REMOVE")
    finally:
        os.close(descriptor)


def _remove_owned_directory_contents_at(
    descriptor: int,
    device: int,
    transaction: "OuterTransaction" | None,
    fault: Callable[[str], Any] | None,
) -> None:
    try:
        names = sorted(os.listdir(descriptor))
    except OSError as exc:
        raise Task8Error("descriptor-relative directory is unreadable",
                         status="RECOVERY_REQUIRED") from exc
    if (len(names) != len(set(names)) or
            any(not name or name in (".", "..") or "/" in name or "\\" in name
                for name in names)):
        raise Task8Error("descriptor-relative directory names are invalid",
                         status="RECOVERY_REQUIRED")
    for name in names:
        entry = _entry_stat_at(descriptor, name)
        if entry is None or entry.st_uid != os.geteuid() or entry.st_dev != device:
            raise Task8Error(f"owned cleanup entry identity differs: {name}",
                             status="RECOVERY_REQUIRED")
        if stat.S_ISDIR(entry.st_mode):
            child_descriptor, child_info = _open_directory_at(descriptor, name)
            try:
                _remove_owned_directory_contents_at(
                    child_descriptor, device, transaction, fault)
            except BaseException:
                os.close(child_descriptor)
                raise
            _remove_open_directory_at(
                descriptor, name, child_descriptor, child_info,
                transaction, fault)
        elif stat.S_ISREG(entry.st_mode):
            child_descriptor, child_info = _open_regular_at(descriptor, name)
            _unlink_open_regular_at(
                descriptor, name, child_descriptor, child_info,
                transaction, fault)
        else:
            raise Task8Error(f"owned cleanup found special leaf: {name}",
                             status="RECOVERY_REQUIRED")
        if name in os.listdir(descriptor):
            raise Task8Error(f"owned cleanup leaf reappeared: {name}",
                             status="RECOVERY_REQUIRED")
    os.fsync(descriptor)


def _write_exclusive(
    path: Path,
    payload: bytes,
    mode: int = 0o600,
    *,
    after_write: Callable[[], None] | None = None,
    after_fsync: Callable[[], None] | None = None,
) -> None:
    descriptor = os.open(
        path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
        getattr(os, "O_NOFOLLOW", 0), mode)
    try:
        position = 0
        while position < len(payload):
            position += os.write(descriptor, payload[position:])
        if after_write is not None:
            after_write()
        os.fsync(descriptor)
        if after_fsync is not None:
            after_fsync()
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
            all(part not in ("", ".", "..") for part in value.split("/")))


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


def _mkdir_exclusive(
    path: Path,
    mode: int = 0o700,
    *,
    parent_descriptor: int | None = None,
    retain_descriptor: bool = False,
) -> tuple[int, tuple[int, ...], tuple[int, ...]] | None:
    """Create one directory relative to a pinned parent and attest its inode.

    The retained form is used by the sandbox copier so no later path lookup can
    redirect child creation.  Other callers receive the historical ``None`` and
    the temporary descriptors are closed here.
    """
    owned_parent = -1
    child_descriptor = -1
    try:
        if parent_descriptor is None:
            owned_parent, parent_info = _open_pinned_directory(
                path.parent, status="FAILED")
            parent_descriptor = owned_parent
            parent_identity = [
                int(parent_info.st_dev), int(parent_info.st_ino),
                int(parent_info.st_uid), stat.S_IMODE(parent_info.st_mode),
            ]
        else:
            parent_info = os.fstat(parent_descriptor)
            parent_identity = [
                int(parent_info.st_dev), int(parent_info.st_ino),
                int(parent_info.st_uid), stat.S_IMODE(parent_info.st_mode),
            ]
        names_before = sorted(os.listdir(parent_descriptor))
        if (len(names_before) != len(set(names_before)) or
                path.name in names_before or
                _entry_stat_at(
                    parent_descriptor, path.name, status="FAILED") is not None):
            raise Task8Error(f"transaction path collision: {path}")
        try:
            os.mkdir(path.name, mode, dir_fd=parent_descriptor)
        except FileExistsError as exc:
            raise Task8Error(f"transaction path collision: {path}") from exc
        parent_created = os.fstat(parent_descriptor)
        names_created = sorted(os.listdir(parent_descriptor))
        if (names_created != sorted((*names_before, path.name)) or
                (parent_created.st_dev, parent_created.st_ino,
                 parent_created.st_uid, stat.S_IMODE(parent_created.st_mode)) !=
                (parent_info.st_dev, parent_info.st_ino,
                 parent_info.st_uid, stat.S_IMODE(parent_info.st_mode))):
            raise Task8Error(f"created directory parent changed: {path}")
        child_descriptor, opened = _open_directory_at(
            parent_descriptor, path.name, status="FAILED")
        if stat.S_IMODE(opened.st_mode) != mode:
            os.fchmod(child_descriptor, mode)
        child_created = os.fstat(child_descriptor)
        entry_created = _entry_stat_at(
            parent_descriptor, path.name, status="FAILED")
        if (entry_created is None or
                _directory_fingerprint(entry_created) !=
                _directory_fingerprint(child_created) or
                child_created.st_dev != parent_info.st_dev or
                child_created.st_uid != os.geteuid() or
                stat.S_IMODE(child_created.st_mode) != mode):
            raise Task8Error(f"created directory identity differs: {path}")
        child_fingerprint = _directory_fingerprint(child_created)
        os.fsync(child_descriptor)
        final = os.fstat(child_descriptor)
        entry = _entry_stat_at(parent_descriptor, path.name, status="FAILED")
        if (entry is None or
                _directory_fingerprint(final) != child_fingerprint or
                _directory_fingerprint(entry) != child_fingerprint):
            raise Task8Error(f"created directory identity differs: {path}")
        os.fsync(parent_descriptor)
        parent_after = os.fstat(parent_descriptor)
        names_after = sorted(os.listdir(parent_descriptor))
        if (names_after != names_created or
                _directory_fingerprint(parent_after) !=
                _directory_fingerprint(parent_created)):
            raise Task8Error(f"created directory parent changed: {path}")
        if owned_parent >= 0:
            _revalidate_pinned_directory_path(
                path.parent, owned_parent, parent_identity)
        result = (
            child_descriptor,
            _directory_fingerprint(final),
            _directory_fingerprint(parent_after),
        )
        if retain_descriptor:
            child_descriptor = -1
            return result
        return None
    finally:
        if child_descriptor >= 0:
            os.close(child_descriptor)
        if owned_parent >= 0:
            os.close(owned_parent)


def _runtime_report_path(repo_root: Path | str, transaction_id: str) -> Path:
    root = Path(repo_root).resolve()
    if not _TXN.fullmatch(transaction_id):
        raise Task8Error("invalid runtime report transaction identity")
    relative = (
        f"{EVIDENCE_PARENT}/{transaction_id}/{RUNTIME_AUTOMATION_DIRECTORY}")
    path = _contained(root, relative)
    expected_parent = evidence_root(root, transaction_id)
    if path != expected_parent / RUNTIME_AUTOMATION_DIRECTORY:
        raise Task8Error("runtime report path is not canonical")
    parent_info = _physical_directory(expected_parent)
    if (parent_info.st_uid != os.geteuid() or
            stat.S_IMODE(parent_info.st_mode) != 0o700):
        raise Task8Error("transaction evidence root ownership/mode differs")
    return path


def _validate_runtime_report_directory(
    owned: OwnedDirectory,
    repo_root: Path | str,
    *,
    require_empty: bool,
) -> Path:
    if (type(owned) is not OwnedDirectory or not isinstance(owned.path, Path) or
            type(owned.device) is not int or owned.device < 0 or
            type(owned.inode) is not int or owned.inode <= 0 or
            type(owned.owner_uid) is not int or owned.owner_uid < 0 or
            (owned.tree_snapshot is not None and
             type(owned.tree_snapshot) is not SandboxTreeSnapshot) or
            type(require_empty) is not bool):
        raise Task8Error("runtime report directory identity is malformed")
    path = _runtime_report_path(repo_root, owned.path.parent.name)
    if owned.path != path:
        raise Task8Error("runtime report directory path differs")
    flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) |
             getattr(os, "O_NOFOLLOW", 0))
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise Task8Error(f"runtime report directory is not physical: {path}") from exc
    try:
        info = os.fstat(descriptor)
        try:
            path_info = path.lstat()
        except OSError as exc:
            raise Task8Error("runtime report directory path disappeared") from exc
        identity = (info.st_dev, info.st_ino, info.st_uid)
        expected_identity = (owned.device, owned.inode, owned.owner_uid)
        if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(path_info.st_mode) or
                identity != expected_identity or
                (path_info.st_dev, path_info.st_ino, path_info.st_uid) != identity or
                owned.owner_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) != 0o700):
            raise Task8Error("runtime report directory ownership/mode differs")
        if require_empty and os.listdir(descriptor):
            raise Task8Error("runtime report directory is not empty before launch")
        try:
            final_info = path.lstat()
        except OSError as exc:
            raise Task8Error("runtime report directory path disappeared") from exc
        if (stat.S_ISLNK(final_info.st_mode) or
                (final_info.st_dev, final_info.st_ino, final_info.st_uid) != identity):
            raise Task8Error("runtime report directory identity changed")
    finally:
        os.close(descriptor)
    return path


def prepare_runtime_report_directory(
    transaction: "OuterTransaction",
) -> OwnedDirectory:
    transaction._validate_journal()
    transaction_id = transaction.journal["transactionId"]
    path = _runtime_report_path(transaction.repo_root, transaction_id)
    if path.exists() or path.is_symlink():
        raise Task8Error(f"runtime report path already exists: {path}")
    _mkdir_exclusive(path, 0o700)
    info = _physical_directory(path)
    owned = OwnedDirectory(
        path=path,
        device=info.st_dev,
        inode=info.st_ino,
        owner_uid=info.st_uid,
    )
    _validate_runtime_report_directory(
        owned, transaction.repo_root, require_empty=True)
    return owned


def _directory_identity(path: Path, *, exact_mode: int | None = None) -> list[int]:
    info = _physical_directory(path)
    mode = stat.S_IMODE(info.st_mode)
    if info.st_uid != os.geteuid():
        raise Task8Error(f"sandbox directory has foreign owner: {path}")
    if exact_mode is not None and mode != exact_mode:
        raise Task8Error(f"sandbox directory mode differs: {path}")
    if exact_mode is None and mode & 0o022:
        raise Task8Error(f"sandbox directory is group/world writable: {path}")
    return [int(info.st_dev), int(info.st_ino), int(info.st_uid), mode]


def _regular_identity(path: Path, *, exact_mode: int = 0o600) -> list[int]:
    info = _physical_regular(path, allow_empty=True)
    mode = stat.S_IMODE(info.st_mode)
    if info.st_uid != os.geteuid() or mode != exact_mode:
        raise Task8Error(f"sandbox reservation ownership/mode differs: {path}")
    return [int(info.st_dev), int(info.st_ino), int(info.st_uid), mode,
            int(info.st_size)]


def _identity_matches(path: Path, expected: list[int], *, directory: bool) -> None:
    observed = (_directory_identity(path, exact_mode=expected[3]) if directory
                else _regular_identity(path, exact_mode=expected[3]))
    if observed != expected:
        raise Task8Error(
            f"sandbox {'directory' if directory else 'file'} identity changed: {path}",
            status="RECOVERY_REQUIRED")


def _sandbox_reservation_payload(
    transaction_id: str, source_head: str, bundle_identifier: str,
    transaction_root: Path, report_root: Path,
) -> bytes:
    return rules.canonical_json_bytes({
        "bundleIdentifier": bundle_identifier,
        "owner": "garner-terrain-task8-reservation",
        "reportRoot": str(report_root),
        "schemaVersion": 1,
        "sourceHead": source_head,
        "transactionId": transaction_id,
        "transactionRoot": str(transaction_root),
    }) + b"\n"


def _sandbox_owner_payload(
    transaction_id: str, bundle_identifier: str,
) -> bytes:
    return rules.canonical_json_bytes({
        "bundleIdentifier": bundle_identifier,
        "owner": "garner-terrain-task8",
        "schemaVersion": 1,
        "transactionId": transaction_id,
    }) + b"\n"


def _sandbox_scratch_paths(
    record: dict[str, Any], repo_root: Path, transaction_id: str,
) -> SandboxReportScratch:
    return SandboxReportScratch(
        reservation_path=Path(record["reservationPath"]),
        transaction_root=Path(record["transactionRoot"]),
        report_root=Path(record["reportRoot"]),
        destination_root=_runtime_report_path(repo_root, transaction_id),
    )


def _valid_identity(value: Any, length: int) -> bool:
    return (type(value) is list and len(value) == length and
            all(type(item) is int and item >= 0 for item in value) and
            value[1] > 0 and value[2] >= 0 and 0 <= value[3] <= 0o7777 and
            (length == 4 or value[4] >= 0))


def _validate_sandbox_scratch_record(
    record: Any, repo_root: Path, transaction_id: str, source_head: str,
) -> None:
    if record is None:
        return
    expected = {
        "state", "bundleIdentifier", "containerRoot", "containerRootIdentity",
        "containerDataRoot", "containerDataIdentity", "scratchParent",
        "scratchParentIdentity", "reservationPath", "reservationIdentity",
        "reservationSha256", "transactionRoot", "reportRoot",
        "transactionIdentity", "reportIdentity", "markerSha256",
    }
    if type(record) is not dict or set(record) != expected:
        raise Task8Error("invalid SandboxScratch DTO", status="RECOVERY_REQUIRED")
    if record["state"] not in {
            "PLANNED", "RESERVED", "PREPARED", "COPIED", "CLEANING", "CLEANED"}:
        raise Task8Error("invalid sandbox scratch state", status="RECOVERY_REQUIRED")
    bundle = record["bundleIdentifier"]
    if type(bundle) is not str or not _BUNDLE_IDENTIFIER.fullmatch(bundle):
        raise Task8Error("invalid sandbox bundle identity", status="RECOVERY_REQUIRED")
    for key in ("containerRoot", "containerDataRoot", "scratchParent",
                "reservationPath", "transactionRoot", "reportRoot"):
        if not _normalized_absolute(record[key]):
            raise Task8Error(f"invalid sandbox absolute path: {key}",
                             status="RECOVERY_REQUIRED")
    container_root = Path(record["containerRoot"])
    data_root = Path(record["containerDataRoot"])
    parent = Path(record["scratchParent"])
    reservation = Path(record["reservationPath"])
    transaction_root = Path(record["transactionRoot"])
    report_root = Path(record["reportRoot"])
    if (data_root.parent != container_root or data_root.name != "Data" or
            reservation != parent / f".CorsairsTerrainTask8-{transaction_id}.reservation" or
            transaction_root != parent / f"CorsairsTerrainTask8-{transaction_id}" or
            report_root != transaction_root / RUNTIME_AUTOMATION_DIRECTORY):
        raise Task8Error("sandbox scratch path relation differs",
                         status="RECOVERY_REQUIRED")
    try:
        relative_parent = parent.relative_to(data_root)
    except ValueError as exc:
        raise Task8Error("sandbox scratch parent escapes container Data",
                         status="RECOVERY_REQUIRED") from exc
    if not relative_parent.parts:
        raise Task8Error("sandbox scratch parent must be below container Data",
                         status="RECOVERY_REQUIRED")
    for key in ("containerRootIdentity", "containerDataIdentity",
                "scratchParentIdentity"):
        if not _valid_identity(record[key], 4):
            raise Task8Error(f"invalid sandbox identity: {key}",
                             status="RECOVERY_REQUIRED")
    for key, length in (("reservationIdentity", 5),
                        ("transactionIdentity", 4), ("reportIdentity", 4)):
        if record[key] != [] and not _valid_identity(record[key], length):
            raise Task8Error(f"invalid sandbox identity: {key}",
                             status="RECOVERY_REQUIRED")
    if (record["reservationIdentity"] and
            record["reservationIdentity"][0] != record["scratchParentIdentity"][0]):
        raise Task8Error("sandbox reservation device differs from scratch parent",
                         status="RECOVERY_REQUIRED")
    if (record["transactionIdentity"] and
            record["transactionIdentity"][0] != record["scratchParentIdentity"][0]):
        raise Task8Error("sandbox transaction device differs from scratch parent",
                         status="RECOVERY_REQUIRED")
    if (record["reportIdentity"] and
            (not record["transactionIdentity"] or
             record["reportIdentity"][0] != record["transactionIdentity"][0])):
        raise Task8Error("sandbox report device differs from transaction root",
                         status="RECOVERY_REQUIRED")
    for key in ("reservationSha256", "markerSha256"):
        if (type(record[key]) is not str or
                not re.fullmatch(r"[0-9a-f]{64}", record[key])):
            raise Task8Error(f"invalid sandbox hash: {key}",
                             status="RECOVERY_REQUIRED")
    reservation_payload = _sandbox_reservation_payload(
        transaction_id, source_head, bundle, transaction_root, report_root)
    marker_payload = _sandbox_owner_payload(transaction_id, bundle)
    if (record["reservationSha256"] != hashlib.sha256(
            reservation_payload).hexdigest() or
            record["markerSha256"] != hashlib.sha256(marker_payload).hexdigest()):
        raise Task8Error("sandbox ownership hash differs", status="RECOVERY_REQUIRED")
    state = record["state"]
    if state == "PLANNED" and any(record[key] != [] for key in (
            "reservationIdentity", "transactionIdentity", "reportIdentity")):
        raise Task8Error("PLANNED sandbox scratch has child identity",
                         status="RECOVERY_REQUIRED")
    if state == "RESERVED" and (record["reservationIdentity"] == [] or
                                record["reportIdentity"] != []):
        raise Task8Error("RESERVED sandbox scratch identity differs",
                         status="RECOVERY_REQUIRED")
    if state in ("PREPARED", "COPIED") and any(record[key] == [] for key in (
            "reservationIdentity", "transactionIdentity", "reportIdentity")):
        raise Task8Error(f"{state} sandbox scratch lacks identity",
                         status="RECOVERY_REQUIRED")


def _validated_sandbox_parents(record: dict[str, Any]) -> None:
    for path_key, identity_key in (
        ("containerRoot", "containerRootIdentity"),
        ("containerDataRoot", "containerDataIdentity"),
        ("scratchParent", "scratchParentIdentity"),
    ):
        _identity_matches(Path(record[path_key]), record[identity_key], directory=True)


def _open_expected_pinned_directory(
    path: Path, expected_identity: list[int],
) -> tuple[int, os.stat_result]:
    descriptor, info = _open_pinned_directory(path)
    observed = [
        int(info.st_dev), int(info.st_ino), int(info.st_uid),
        stat.S_IMODE(info.st_mode),
    ]
    if observed != expected_identity or info.st_uid != os.geteuid():
        os.close(descriptor)
        raise Task8Error(f"pinned directory identity differs: {path}",
                         status="RECOVERY_REQUIRED")
    return descriptor, info


def _revalidate_pinned_directory_path(
    path: Path, held_descriptor: int, expected_identity: list[int],
) -> None:
    reopened = -1
    try:
        reopened, reopened_info = _open_expected_pinned_directory(
            path, expected_identity)
        held_info = os.fstat(held_descriptor)
        if not _same_entry(reopened_info, held_info):
            raise Task8Error(f"pinned directory path changed: {path}",
                             status="RECOVERY_REQUIRED")
    finally:
        if reopened >= 0:
            os.close(reopened)


def _require_reopened_directory_absence(
    path: Path,
    held_descriptor: int,
    expected_identity: list[int],
    absent_name: str,
) -> None:
    reopened = -1
    try:
        reopened, reopened_info = _open_expected_pinned_directory(
            path, expected_identity)
        held_info = os.fstat(held_descriptor)
        if not _same_entry(reopened_info, held_info):
            raise Task8Error(f"pinned directory path changed: {path}",
                             status="RECOVERY_REQUIRED")
        if _entry_stat_at(reopened, absent_name) is not None:
            raise Task8Error(
                f"canonical cleanup leaf remains: {path / absent_name}",
                status="RECOVERY_REQUIRED")
    finally:
        if reopened >= 0:
            os.close(reopened)


def prepare_sandbox_report_scratch(
    transaction: "OuterTransaction", environment: AttestedSandboxEnvironment,
    *, fault: Callable[[str], Any] | None = None,
) -> SandboxReportScratch:
    transaction._validate_journal()
    if transaction.journal["sandboxScratch"] is not None:
        raise Task8Error("sandbox scratch is already active")
    transaction_id = transaction.journal["transactionId"]
    source_head = transaction.journal["sourceHead"]
    _validate_attested_sandbox_environment(transaction, environment)
    transaction._fault(fault, "AFTER_SANDBOX_RUNTIME_REPORT_PROBE")
    data_root = environment.container_data_root
    container_root = environment.container_root
    parent = environment.automation_reports_root
    reservation = parent / f".CorsairsTerrainTask8-{transaction_id}.reservation"
    transaction_root_path = parent / f"CorsairsTerrainTask8-{transaction_id}"
    report_root = transaction_root_path / RUNTIME_AUTOMATION_DIRECTORY
    reservation_payload = _sandbox_reservation_payload(
        transaction_id, source_head, environment.bundle_identifier,
        transaction_root_path, report_root)
    marker_payload = _sandbox_owner_payload(
        transaction_id, environment.bundle_identifier)
    record = {
        "state": "PLANNED",
        "bundleIdentifier": environment.bundle_identifier,
        "containerRoot": str(container_root),
        "containerRootIdentity": list(environment.container_root_identity),
        "containerDataRoot": str(data_root),
        "containerDataIdentity": list(environment.container_data_identity),
        "scratchParent": str(parent),
        "scratchParentIdentity": list(environment.automation_reports_identity),
        "reservationPath": str(reservation),
        "reservationIdentity": [],
        "reservationSha256": hashlib.sha256(reservation_payload).hexdigest(),
        "transactionRoot": str(transaction_root_path),
        "reportRoot": str(report_root),
        "transactionIdentity": [],
        "reportIdentity": [],
        "markerSha256": hashlib.sha256(marker_payload).hexdigest(),
    }
    _validate_attested_sandbox_environment(transaction, environment)
    transaction.journal["sandboxScratch"] = record
    transaction.persist()
    transaction._fault(fault, "AFTER_SANDBOX_PLANNED_DURABLE")
    _validated_sandbox_parents(record)
    if reservation.exists() or reservation.is_symlink():
        raise Task8Error(f"sandbox reservation already exists: {reservation}")
    if transaction_root_path.exists() or transaction_root_path.is_symlink():
        raise Task8Error(f"sandbox transaction root already exists: {transaction_root_path}")
    _write_exclusive(
        reservation,
        reservation_payload,
        0o600,
        after_write=lambda: transaction._fault(
            fault, "AFTER_SANDBOX_RESERVATION_CREATE"),
        after_fsync=lambda: transaction._fault(
            fault, "AFTER_SANDBOX_RESERVATION_FILE_SYNC"),
    )
    transaction._fault(fault, "AFTER_SANDBOX_RESERVATION_READBACK")
    _sync_directory(parent)
    transaction._fault(fault, "AFTER_SANDBOX_RESERVATION_PARENT_SYNC")
    reservation_identity = _regular_identity(reservation)
    if reservation_identity[0] != record["scratchParentIdentity"][0]:
        raise Task8Error("sandbox reservation device differs from scratch parent")
    record["reservationIdentity"] = reservation_identity
    record["state"] = "RESERVED"
    transaction.persist()
    transaction._fault(fault, "AFTER_SANDBOX_RESERVED_DURABLE")
    _validated_sandbox_parents(record)
    _verify_sandbox_payload(
        reservation, reservation_payload, record["reservationSha256"],
        expected_identity=record["reservationIdentity"])
    _mkdir_exclusive(transaction_root_path, 0o700)
    transaction._fault(fault, "AFTER_SANDBOX_OWNER_ROOT_CREATE")
    transaction_identity = _directory_identity(
        transaction_root_path, exact_mode=0o700)
    if transaction_identity[0] != record["scratchParentIdentity"][0]:
        raise Task8Error("sandbox transaction device differs from scratch parent")
    record["transactionIdentity"] = transaction_identity
    transaction.persist()
    transaction._fault(fault, "AFTER_SANDBOX_OWNER_ROOT_IDENTITY_DURABLE")
    _validated_sandbox_parents(record)
    _identity_matches(
        transaction_root_path, record["transactionIdentity"], directory=True)
    _write_exclusive(
        transaction_root_path / "owner.json",
        marker_payload,
        0o600,
        after_fsync=lambda: transaction._fault(
            fault, "AFTER_SANDBOX_MARKER_FILE_SYNC"),
    )
    transaction._fault(fault, "AFTER_SANDBOX_MARKER_READBACK")
    _validated_sandbox_parents(record)
    _identity_matches(
        transaction_root_path, record["transactionIdentity"], directory=True)
    _verify_sandbox_payload(
        transaction_root_path / "owner.json", marker_payload,
        record["markerSha256"])
    _mkdir_exclusive(report_root, 0o700)
    transaction._fault(fault, "AFTER_SANDBOX_REPORT_ROOT_CREATE")
    report_identity = _directory_identity(report_root, exact_mode=0o700)
    if report_identity[0] != record["transactionIdentity"][0]:
        raise Task8Error("sandbox report device differs from transaction root")
    record["reportIdentity"] = report_identity
    _sync_directory(transaction_root_path)
    record["state"] = "PREPARED"
    transaction.persist()
    transaction._fault(fault, "AFTER_SANDBOX_PREPARED_DURABLE")
    return _sandbox_scratch_paths(record, transaction.repo_root, transaction_id)


def _tree_records_at(
    root_descriptor: int, root: Path, *, require_index: bool,
) -> SandboxTreeSnapshot:
    records: list[SandboxTreeRecord] = []
    root_device = int(os.fstat(root_descriptor).st_dev)

    def scan(directory_descriptor: int, relative: str) -> None:
        before = os.fstat(directory_descriptor)
        if (not stat.S_ISDIR(before.st_mode) or
                before.st_uid != os.geteuid() or before.st_dev != root_device):
            raise Task8Error(
                f"sandbox report directory identity differs: {root / relative}")
        try:
            names = sorted(os.listdir(directory_descriptor))
        except OSError as exc:
            raise Task8Error(
                f"sandbox report tree is unreadable: {root / relative}") from exc
        if (len(names) != len(set(names)) or
                any(not name or name in (".", "..") or "/" in name or
                    "\\" in name for name in names)):
            raise Task8Error("sandbox report directory names are invalid")
        for name in names:
            item_relative = name if not relative else f"{relative}/{name}"
            entry = _entry_stat_at(directory_descriptor, name, status="FAILED")
            if entry is None or entry.st_dev != root_device:
                raise Task8Error(
                    f"sandbox report entry identity differs: "
                    f"{root / item_relative}")
            if stat.S_ISDIR(entry.st_mode):
                child_descriptor = -1
                try:
                    child_descriptor, opened = _open_directory_at(
                        directory_descriptor, name, status="FAILED")
                    child_identity = _directory_fingerprint(opened)
                    records.append(SandboxTreeRecord(
                        kind="directory",
                        path=item_relative,
                        mode=stat.S_IMODE(opened.st_mode),
                        sha256="",
                        size_bytes=0,
                        identity=child_identity,
                    ))
                    scan(child_descriptor, item_relative)
                    held_after = os.fstat(child_descriptor)
                    entry_after = _entry_stat_at(
                        directory_descriptor, name, status="FAILED")
                    if (entry_after is None or
                            _directory_fingerprint(held_after) != child_identity or
                            _directory_fingerprint(entry_after) != child_identity):
                        raise Task8Error(
                            f"sandbox report directory changed during scan: "
                            f"{root / item_relative}")
                finally:
                    if child_descriptor >= 0:
                        os.close(child_descriptor)
            elif stat.S_ISREG(entry.st_mode):
                child_descriptor = -1
                try:
                    child_descriptor, opened = _open_regular_at(
                        directory_descriptor, name, status="FAILED")
                    identity = _regular_fingerprint(opened)
                    payload = _read_descriptor(child_descriptor)
                    held_after = os.fstat(child_descriptor)
                    entry_after = _entry_stat_at(
                        directory_descriptor, name, status="FAILED")
                    if (entry_after is None or
                            _regular_fingerprint(held_after) != identity or
                            _regular_fingerprint(entry_after) != identity or
                            len(payload) != opened.st_size):
                        raise Task8Error(
                            f"sandbox report file changed during scan: "
                            f"{root / item_relative}")
                    records.append(SandboxTreeRecord(
                        kind="file",
                        path=item_relative,
                        mode=stat.S_IMODE(opened.st_mode),
                        sha256=hashlib.sha256(payload).hexdigest(),
                        size_bytes=int(opened.st_size),
                        identity=identity,
                    ))
                finally:
                    if child_descriptor >= 0:
                        os.close(child_descriptor)
            else:
                raise Task8Error(
                    f"sandbox report has special entry: {root / item_relative}")
        after = os.fstat(directory_descriptor)
        if _directory_fingerprint(after) != _directory_fingerprint(before):
            raise Task8Error(
                f"sandbox report directory changed during scan: {root / relative}")

    try:
        root_before = os.fstat(root_descriptor)
        root_identity = _directory_fingerprint(root_before)
        scan(root_descriptor, "")
        if _directory_fingerprint(os.fstat(root_descriptor)) != root_identity:
            raise Task8Error("sandbox report root changed during scan")
    except Task8Error:
        raise
    except OSError as exc:
        raise Task8Error(f"sandbox report tree changed during scan: {root}") from exc
    records.sort(key=lambda item: (item.path, item.kind))
    paths = [item.path for item in records]
    if paths != sorted(set(paths)):
        raise Task8Error("sandbox report paths are not sorted unique")
    if require_index and not any(
            item.kind == "file" and item.path == "index.json" and
            item.size_bytes > 0 for item in records):
        raise Task8Error("sandbox runtime report lacks nonempty index.json")
    return SandboxTreeSnapshot(root_identity, tuple(records))


def _tree_records(root: Path, *, require_index: bool) -> SandboxTreeSnapshot:
    descriptor = -1
    try:
        descriptor, opened = _open_pinned_directory(root, status="FAILED")
        snapshot = _tree_records_at(
            descriptor, root, require_index=require_index)
        expected_identity = [
            int(opened.st_dev), int(opened.st_ino), int(opened.st_uid),
            stat.S_IMODE(opened.st_mode),
        ]
        _revalidate_pinned_directory_path(root, descriptor, expected_identity)
        if _directory_fingerprint(os.fstat(descriptor)) != snapshot.root_identity:
            raise Task8Error("sandbox report root changed after path revalidation")
        return snapshot
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _tree_content_projection(
    snapshot: SandboxTreeSnapshot,
) -> tuple[int, tuple[tuple[Any, ...], ...]]:
    return (
        snapshot.root_identity[3],
        tuple(
            (item.kind, item.path, item.mode, item.sha256, item.size_bytes)
            for item in snapshot.records),
    )


def _verify_open_tree_chain(
    frames: Sequence[_OpenTreeDirectory], *, label: str,
) -> None:
    for frame in frames:
        held = os.fstat(frame.descriptor)
        if (_directory_fingerprint(held) != frame.fingerprint or
                set(os.listdir(frame.descriptor)) != frame.names):
            raise Task8Error(
                f"{label} directory changed: {frame.relative or '.'}")
        if frame.parent_descriptor is not None:
            entry = _entry_stat_at(
                frame.parent_descriptor, frame.name, status="FAILED")
            if (entry is None or
                    _directory_fingerprint(entry) != frame.fingerprint):
                raise Task8Error(
                    f"{label} directory attachment changed: "
                    f"{frame.relative or '.'}")


def _verify_tree_file_at(
    parent_descriptor: int, name: str, record: SandboxTreeRecord,
    *, label: str,
) -> None:
    descriptor = -1
    try:
        descriptor, held = _verify_regular_at(
            parent_descriptor,
            name,
            expected_sha256=record.sha256,
            expected_size=record.size_bytes,
            expected_mode=record.mode,
            status="FAILED")
        if _regular_fingerprint(held) != record.identity:
            raise Task8Error(f"{label} file changed: {record.path}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _destination_ledger_snapshot(
    directory_fingerprints: dict[str, tuple[int, ...]],
    file_records: dict[str, SandboxTreeRecord],
) -> SandboxTreeSnapshot:
    records = [
        SandboxTreeRecord(
            kind="directory",
            path=relative,
            mode=fingerprint[3],
            sha256="",
            size_bytes=0,
            identity=fingerprint,
        )
        for relative, fingerprint in directory_fingerprints.items()
        if relative
    ]
    records.extend(file_records.values())
    records.sort(key=lambda item: (item.path, item.kind))
    return SandboxTreeSnapshot(
        directory_fingerprints[""], tuple(records))


def _copy_sandbox_file(
    source: Path,
    destination: Path,
    mode: int,
    *,
    source_parent_descriptor: int,
    destination_parent_descriptor: int,
    source_record: SandboxTreeRecord,
    after_fsync: Callable[[], None] | None = None,
) -> tuple[int, SandboxTreeRecord, tuple[int, ...]]:
    source_descriptor = -1
    destination_descriptor = -1
    reopened_descriptor = -1
    source_descriptor, before = _open_regular_at(
        source_parent_descriptor, source.name, status="FAILED")
    if (_regular_fingerprint(before) != source_record.identity or
            before.st_dev != os.fstat(source_parent_descriptor).st_dev or
            before.st_uid != os.geteuid() or
            stat.S_IMODE(before.st_mode) != mode):
        os.close(source_descriptor)
        raise Task8Error(f"sandbox report source identity changed: {source}")
    source_digest = hashlib.sha256()
    source_size = 0
    try:
        parent_before = os.fstat(destination_parent_descriptor)
        names_before = sorted(os.listdir(destination_parent_descriptor))
        if (destination.name in names_before or
                _entry_stat_at(
                    destination_parent_descriptor, destination.name,
                    status="FAILED") is not None):
            raise Task8Error(f"sandbox destination file exists: {destination}")
        destination_descriptor = os.open(
            destination.name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL |
            getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0),
            0o600,
            dir_fd=destination_parent_descriptor)
        parent_created = os.fstat(destination_parent_descriptor)
        names_created = sorted(os.listdir(destination_parent_descriptor))
        if (names_created != sorted((*names_before, destination.name)) or
                (parent_created.st_dev, parent_created.st_ino,
                 parent_created.st_uid, stat.S_IMODE(parent_created.st_mode)) !=
                (parent_before.st_dev, parent_before.st_ino,
                 parent_before.st_uid, stat.S_IMODE(parent_before.st_mode))):
            raise Task8Error(
                f"sandbox destination parent changed during create: {destination}")
        try:
            while True:
                block = os.read(source_descriptor, 1024 * 1024)
                if not block:
                    break
                source_digest.update(block)
                source_size += len(block)
                position = 0
                while position < len(block):
                    position += os.write(destination_descriptor, block[position:])
            source_after = os.fstat(source_descriptor)
            source_entry = _entry_stat_at(
                source_parent_descriptor, source.name, status="FAILED")
            if (source_entry is None or
                    _regular_fingerprint(source_after) != source_record.identity or
                    _regular_fingerprint(source_entry) != source_record.identity):
                raise Task8Error(
                    f"sandbox report source changed during copy: {source}")
            os.fchmod(destination_descriptor, mode)
            os.fsync(destination_descriptor)
            destination_synced = os.fstat(destination_descriptor)
            if after_fsync is not None:
                after_fsync()
            destination_final = os.fstat(destination_descriptor)
            if (_regular_fingerprint(destination_final) !=
                    _regular_fingerprint(destination_synced)):
                raise Task8Error(
                    f"sandbox report destination changed after fsync: "
                    f"{destination}")
        finally:
            os.close(destination_descriptor)
            destination_descriptor = -1
        parent_after = os.fstat(destination_parent_descriptor)
        names_after = sorted(os.listdir(destination_parent_descriptor))
        if (names_after != names_created or
                _directory_fingerprint(parent_after) !=
                _directory_fingerprint(parent_created)):
            raise Task8Error(
                f"sandbox destination parent changed during copy: {destination}")
        copied_digest = source_digest.hexdigest()
        destination_identity = _regular_fingerprint(destination_final)
        reopened_descriptor, reopened = _verify_regular_at(
            destination_parent_descriptor,
            destination.name,
            expected_sha256=copied_digest,
            expected_size=source_size,
            expected_mode=mode,
            status="FAILED")
        if (_regular_fingerprint(reopened) != destination_identity or
                destination_final.st_dev != parent_before.st_dev or
                destination_final.st_uid != os.geteuid() or
                destination_final.st_nlink != 1):
            raise Task8Error(f"sandbox report copy identity differs: {destination}")
    finally:
        os.close(source_descriptor)
        if destination_descriptor >= 0:
            os.close(destination_descriptor)
        if sys.exc_info()[0] is not None and reopened_descriptor >= 0:
            os.close(reopened_descriptor)
            reopened_descriptor = -1
    try:
        if source_size != before.st_size:
            raise Task8Error(
                f"sandbox report source changed during copy: {source}")
        if (source_size != source_record.size_bytes or
                source_digest.hexdigest() != source_record.sha256):
            raise Task8Error(f"sandbox report copy differs: {destination}")
        record = SandboxTreeRecord(
            kind="file",
            path=source_record.path,
            mode=mode,
            sha256=source_digest.hexdigest(),
            size_bytes=source_size,
            identity=destination_identity,
        )
        result = (
            reopened_descriptor, record, _directory_fingerprint(parent_after))
        reopened_descriptor = -1
        return result
    finally:
        if reopened_descriptor >= 0:
            os.close(reopened_descriptor)


def copy_sandbox_runtime_reports(
    transaction: "OuterTransaction", *, fault: Callable[[str], Any] | None = None,
) -> OwnedDirectory:
    transaction._validate_journal()
    record = transaction.journal["sandboxScratch"]
    if type(record) is not dict or record["state"] != "PREPARED":
        raise Task8Error("sandbox report copy requires PREPARED scratch")
    paths = _sandbox_scratch_paths(
        record, transaction.repo_root, transaction.journal["transactionId"])
    _validate_prepared_sandbox_scratch(transaction, paths)
    source_root_descriptor = -1
    destination_parent_descriptor = -1
    destination_root_descriptor = -1
    try:
        source_root_descriptor, source_opened = _open_expected_pinned_directory(
            paths.report_root, record["reportIdentity"])
        source_records = _tree_records_at(
            source_root_descriptor, paths.report_root, require_index=True)
        source_children: dict[str, list[SandboxTreeRecord]] = {}
        for item in source_records.records:
            item_path = PurePosixPath(item.path)
            parent_relative = (
                "" if item_path.parent.as_posix() == "." else
                item_path.parent.as_posix())
            source_children.setdefault(parent_relative, []).append(item)
        for items in source_children.values():
            items.sort(key=lambda item: (item.path, item.kind))

        destination_parent_descriptor, destination_parent_opened = (
            _open_pinned_directory(paths.destination_root.parent, status="FAILED"))
        if (destination_parent_opened.st_uid != os.geteuid() or
                stat.S_IMODE(destination_parent_opened.st_mode) != 0o700):
            raise Task8Error("runtime report parent ownership/mode differs")
        destination_parent_identity = [
            int(destination_parent_opened.st_dev),
            int(destination_parent_opened.st_ino),
            int(destination_parent_opened.st_uid),
            stat.S_IMODE(destination_parent_opened.st_mode),
        ]
        parent_names_before = set(os.listdir(destination_parent_descriptor))
        if (paths.destination_root.name in parent_names_before or
                _entry_stat_at(
                    destination_parent_descriptor, paths.destination_root.name,
                    status="FAILED") is not None):
            raise Task8Error(
                f"runtime report destination already exists: "
                f"{paths.destination_root}")
        root_created = _mkdir_exclusive(
            paths.destination_root,
            0o700,
            parent_descriptor=destination_parent_descriptor,
            retain_descriptor=True)
        if root_created is None:
            raise Task8Error("sandbox destination root creation was not retained")
        destination_root_descriptor, root_fingerprint, parent_fingerprint = (
            root_created)
        destination_parent_names = {
            *parent_names_before, paths.destination_root.name}
        source_root_frame = _OpenTreeDirectory(
            descriptor=source_root_descriptor,
            parent_descriptor=None,
            name=paths.report_root.name,
            relative="",
            fingerprint=source_records.root_identity,
            names={
                PurePosixPath(item.path).name
                for item in source_children.get("", [])
            },
        )
        destination_root_frame = _OpenTreeDirectory(
            descriptor=destination_root_descriptor,
            parent_descriptor=destination_parent_descriptor,
            name=paths.destination_root.name,
            relative="",
            fingerprint=root_fingerprint,
            names=set(),
        )
        directory_fingerprints: dict[str, tuple[int, ...]] = {}
        file_records: dict[str, SandboxTreeRecord] = {}

        def verify_destination_stack(
            stack: Sequence[_OpenTreeDirectory],
        ) -> None:
            if (_directory_fingerprint(
                    os.fstat(destination_parent_descriptor)) !=
                    parent_fingerprint or
                    set(os.listdir(destination_parent_descriptor)) !=
                    destination_parent_names):
                raise Task8Error(
                    "sandbox destination parent changed during copy")
            _verify_open_tree_chain(stack, label="sandbox destination")

        def copy_directory(
            source_frame: _OpenTreeDirectory,
            destination_frame: _OpenTreeDirectory,
            relative: str,
            desired_mode: int,
            source_stack: list[_OpenTreeDirectory],
            destination_stack: list[_OpenTreeDirectory],
        ) -> None:
            _verify_open_tree_chain(
                source_stack, label="sandbox source")
            verify_destination_stack(destination_stack)
            for item in source_children.get(relative, []):
                _verify_open_tree_chain(
                    source_stack, label="sandbox source")
                verify_destination_stack(destination_stack)
                item_path = PurePosixPath(item.path)
                item_name = item_path.name
                source_path = paths.report_root / item_path
                destination_path = paths.destination_root / item_path
                if item.kind == "directory":
                    source_child_descriptor = -1
                    destination_child_descriptor = -1
                    try:
                        source_child_descriptor, source_child_info = (
                            _open_directory_at(
                                source_frame.descriptor,
                                item_name,
                                status="FAILED"))
                        if (_directory_fingerprint(source_child_info) !=
                                item.identity):
                            raise Task8Error(
                                "sandbox source directory changed before copy")
                        source_child_frame = _OpenTreeDirectory(
                            descriptor=source_child_descriptor,
                            parent_descriptor=source_frame.descriptor,
                            name=item_name,
                            relative=item.path,
                            fingerprint=item.identity,
                            names={
                                PurePosixPath(child.path).name
                                for child in source_children.get(item.path, [])
                            },
                        )
                        created = _mkdir_exclusive(
                            destination_path,
                            0o700,
                            parent_descriptor=destination_frame.descriptor,
                            retain_descriptor=True)
                        if created is None:
                            raise Task8Error(
                                "sandbox destination directory creation "
                                "was not retained")
                        (destination_child_descriptor,
                         destination_child_fingerprint,
                         destination_parent_after) = created
                        destination_frame.names.add(item_name)
                        destination_frame.fingerprint = destination_parent_after
                        destination_child_frame = _OpenTreeDirectory(
                            descriptor=destination_child_descriptor,
                            parent_descriptor=destination_frame.descriptor,
                            name=item_name,
                            relative=item.path,
                            fingerprint=destination_child_fingerprint,
                            names=set(),
                        )
                        source_stack.append(source_child_frame)
                        destination_stack.append(destination_child_frame)
                        try:
                            _verify_open_tree_chain(
                                source_stack, label="sandbox source")
                            verify_destination_stack(destination_stack)
                            copy_directory(
                                source_child_frame,
                                destination_child_frame,
                                item.path,
                                item.mode,
                                source_stack,
                                destination_stack)
                        finally:
                            destination_stack.pop()
                            source_stack.pop()
                        directory_fingerprints[item.path] = (
                            destination_child_frame.fingerprint)
                    finally:
                        if destination_child_descriptor >= 0:
                            os.close(destination_child_descriptor)
                        if source_child_descriptor >= 0:
                            os.close(source_child_descriptor)
                elif item.kind == "file":
                    copied_descriptor = -1
                    copied_record: SandboxTreeRecord | None = None
                    try:
                        (copied_descriptor,
                         copied_record,
                         destination_parent_after) = _copy_sandbox_file(
                            source_path,
                            destination_path,
                            item.mode,
                            source_parent_descriptor=source_frame.descriptor,
                            destination_parent_descriptor=(
                                destination_frame.descriptor),
                            source_record=item,
                            after_fsync=lambda: transaction._fault(
                                fault,
                                "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC"))
                        copied_entry = _entry_stat_at(
                            destination_frame.descriptor,
                            item_name,
                            status="FAILED")
                        if (copied_entry is None or
                                _regular_fingerprint(
                                    os.fstat(copied_descriptor)) !=
                                copied_record.identity or
                                _regular_fingerprint(copied_entry) !=
                                copied_record.identity):
                            raise Task8Error(
                                "sandbox destination file changed before adoption")
                        destination_frame.names.add(item_name)
                        destination_frame.fingerprint = destination_parent_after
                        file_records[item.path] = copied_record
                    finally:
                        if copied_descriptor >= 0:
                            os.close(copied_descriptor)
                    _verify_tree_file_at(
                        source_frame.descriptor,
                        item_name,
                        item,
                        label="sandbox source")
                    if copied_record is None:
                        raise Task8Error("sandbox destination file was not copied")
                    _verify_tree_file_at(
                        destination_frame.descriptor,
                        item_name,
                        copied_record,
                        label="sandbox destination")
                    _verify_open_tree_chain(
                        source_stack, label="sandbox source")
                    verify_destination_stack(destination_stack)
                else:
                    raise Task8Error(
                        "sandbox source snapshot has invalid entry kind")
            _verify_open_tree_chain(
                source_stack, label="sandbox source")
            verify_destination_stack(destination_stack)
            current = os.fstat(destination_frame.descriptor)
            if stat.S_IMODE(current.st_mode) != desired_mode:
                os.fchmod(destination_frame.descriptor, desired_mode)
            os.fsync(destination_frame.descriptor)
            destination_frame.fingerprint = _directory_fingerprint(
                os.fstat(destination_frame.descriptor))
            directory_fingerprints[relative] = destination_frame.fingerprint
            _verify_open_tree_chain(
                source_stack, label="sandbox source")
            verify_destination_stack(destination_stack)

        copy_directory(
            source_root_frame,
            destination_root_frame,
            "",
            source_records.root_identity[3],
            [source_root_frame],
            [destination_root_frame])
        destination_creation = _destination_ledger_snapshot(
            directory_fingerprints, file_records)
        os.fsync(destination_parent_descriptor)
        verify_destination_stack([destination_root_frame])
        source_after = _tree_records_at(
            source_root_descriptor, paths.report_root, require_index=True)
        destination_records = _tree_records_at(
            destination_root_descriptor, paths.destination_root,
            require_index=True)
        destination_after = _tree_records_at(
            destination_root_descriptor, paths.destination_root,
            require_index=True)
        if (source_records != source_after or
                destination_creation != destination_records or
                destination_records != destination_after or
                _tree_content_projection(source_records) !=
                _tree_content_projection(destination_records)):
            raise Task8Error("sandbox runtime source/destination tree differs")
        source_identity = [
            int(source_opened.st_dev), int(source_opened.st_ino),
            int(source_opened.st_uid), stat.S_IMODE(source_opened.st_mode),
        ]
        _revalidate_pinned_directory_path(
            paths.report_root, source_root_descriptor, source_identity)
        if (_directory_fingerprint(os.fstat(source_root_descriptor)) !=
                source_after.root_identity):
            raise Task8Error(
                "sandbox source root changed after path revalidation")
        _revalidate_pinned_directory_path(
            paths.destination_root.parent,
            destination_parent_descriptor,
            destination_parent_identity)
        verify_destination_stack([destination_root_frame])
        transaction._fault(fault, "AFTER_SANDBOX_REPORT_COPY_REVALIDATED")
        _revalidate_pinned_directory_path(
            paths.report_root, source_root_descriptor, source_identity)
        _revalidate_pinned_directory_path(
            paths.destination_root.parent,
            destination_parent_descriptor,
            destination_parent_identity)
        source_after_fault = _tree_records_at(
            source_root_descriptor, paths.report_root, require_index=True)
        destination_after_fault = _tree_records_at(
            destination_root_descriptor, paths.destination_root,
            require_index=True)
        verify_destination_stack([destination_root_frame])
        if (source_after_fault != source_records or
                destination_after_fault != destination_creation or
                _directory_fingerprint(os.fstat(source_root_descriptor)) !=
                source_records.root_identity):
            raise Task8Error(
                "sandbox trees changed after durable-copy callback")
        record["state"] = "COPIED"
        transaction.persist()
        transaction._fault(fault, "AFTER_SANDBOX_COPIED_DURABLE")
        _revalidate_pinned_directory_path(
            paths.report_root, source_root_descriptor, source_identity)
        _revalidate_pinned_directory_path(
            paths.destination_root.parent,
            destination_parent_descriptor,
            destination_parent_identity)
        source_after_fault = _tree_records_at(
            source_root_descriptor, paths.report_root, require_index=True)
        destination_after_fault = _tree_records_at(
            destination_root_descriptor, paths.destination_root,
            require_index=True)
        verify_destination_stack([destination_root_frame])
        if (source_after_fault != source_records or
                destination_after_fault != destination_creation or
                _directory_fingerprint(os.fstat(source_root_descriptor)) !=
                source_records.root_identity):
            raise Task8Error(
                "sandbox trees changed after durable-copy callback")
        info = os.fstat(destination_root_descriptor)
        return OwnedDirectory(
            paths.destination_root,
            int(info.st_dev), int(info.st_ino), int(info.st_uid),
            destination_creation)
    finally:
        if destination_root_descriptor >= 0:
            os.close(destination_root_descriptor)
        if destination_parent_descriptor >= 0:
            os.close(destination_parent_descriptor)
        if source_root_descriptor >= 0:
            os.close(source_root_descriptor)


def _verify_sandbox_payload(
    path: Path, expected_payload: bytes, expected_sha256: str,
    *, expected_identity: list[int] | None = None,
) -> list[int]:
    info, payload = _read_physical_regular(
        path, allow_empty=True, status="RECOVERY_REQUIRED")
    observed = [
        int(info.st_dev), int(info.st_ino), int(info.st_uid),
        stat.S_IMODE(info.st_mode), int(info.st_size),
    ]
    if (info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600 or
            payload != expected_payload or
            hashlib.sha256(payload).hexdigest() != expected_sha256 or
            (expected_identity and observed != expected_identity)):
        raise Task8Error(f"sandbox ownership payload differs: {path}",
                         status="RECOVERY_REQUIRED")
    return observed


def _validate_sandbox_root_shape(
    record: dict[str, Any], paths: SandboxReportScratch,
    marker_payload: bytes,
) -> bool:
    state = record["state"]
    root_present = (
        paths.transaction_root.exists() or paths.transaction_root.is_symlink())
    if not root_present:
        if (state in ("PREPARED", "COPIED") or
                (state == "RESERVED" and record["transactionIdentity"])):
            raise Task8Error("sandbox transaction root disappeared",
                             status="RECOVERY_REQUIRED")
        return False
    if not record["transactionIdentity"]:
        identity = _directory_identity(paths.transaction_root, exact_mode=0o700)
        if (state not in ("RESERVED", "CLEANING") or
                identity[0] != record["scratchParentIdentity"][0] or
                os.listdir(paths.transaction_root)):
            raise Task8Error("unrecorded sandbox root is not exact empty root",
                             status="RECOVERY_REQUIRED")
        return True
    _identity_matches(
        paths.transaction_root, record["transactionIdentity"], directory=True)
    try:
        names = set(os.listdir(paths.transaction_root))
    except OSError as exc:
        raise Task8Error("sandbox transaction root is unreadable",
                         status="RECOVERY_REQUIRED") from exc
    allowed = {"owner.json", RUNTIME_AUTOMATION_DIRECTORY}
    if not names.issubset(allowed):
        raise Task8Error("sandbox transaction root has unexpected child",
                         status="RECOVERY_REQUIRED")
    marker = paths.transaction_root / "owner.json"
    marker_present = marker.exists() or marker.is_symlink()
    report_present = paths.report_root.exists() or paths.report_root.is_symlink()
    if marker_present:
        _verify_sandbox_payload(
            marker, marker_payload, record["markerSha256"])
    if report_present:
        if record["reportIdentity"]:
            _identity_matches(
                paths.report_root, record["reportIdentity"], directory=True)
        else:
            report_identity = _directory_identity(
                paths.report_root, exact_mode=0o700)
            if (report_identity[0] != record["transactionIdentity"][0] or
                    os.listdir(paths.report_root)):
                raise Task8Error("unrecorded sandbox report root is not empty",
                                 status="RECOVERY_REQUIRED")
        if record["reportIdentity"]:
            _tree_records(paths.report_root, require_index=False)
    if state == "RESERVED":
        if report_present and not marker_present:
            raise Task8Error("sandbox report root exists without owner marker",
                             status="RECOVERY_REQUIRED")
        if report_present and os.listdir(paths.report_root):
            raise Task8Error("RESERVED sandbox report root is not empty",
                             status="RECOVERY_REQUIRED")
    elif state in ("PREPARED", "COPIED"):
        if not marker_present or not report_present:
            raise Task8Error(f"{state} sandbox root is incomplete",
                             status="RECOVERY_REQUIRED")
    elif state == "CLEANED":
        raise Task8Error("CLEANED sandbox transaction root still exists",
                         status="RECOVERY_REQUIRED")
    return True


def _validate_prepared_sandbox_scratch(
    transaction: "OuterTransaction", expected: SandboxReportScratch,
) -> None:
    transaction._validate_journal()
    record = transaction.journal["sandboxScratch"]
    if type(record) is not dict or record["state"] != "PREPARED":
        raise Task8Error("runtime launch requires PREPARED sandbox scratch")
    paths = _sandbox_scratch_paths(
        record, transaction.repo_root, transaction.journal["transactionId"])
    if paths != expected:
        raise Task8Error("runtime sandbox paths differ from durable journal")
    _validated_sandbox_parents(record)
    reservation_payload = _sandbox_reservation_payload(
        transaction.journal["transactionId"], transaction.journal["sourceHead"],
        record["bundleIdentifier"], paths.transaction_root, paths.report_root)
    _verify_sandbox_payload(
        paths.reservation_path, reservation_payload,
        record["reservationSha256"],
        expected_identity=record["reservationIdentity"])
    marker_payload = _sandbox_owner_payload(
        transaction.journal["transactionId"], record["bundleIdentifier"])
    _validate_sandbox_root_shape(record, paths, marker_payload)


def _remove_owned_sandbox_subtree(
    path: Path, expected_identity: list[int], transaction: "OuterTransaction",
    fault: Callable[[str], Any] | None,
) -> None:
    parent_descriptor = -1
    root_descriptor = -1
    try:
        parent_descriptor, _ = _open_pinned_directory(path.parent)
        root_descriptor, root_info = _open_directory_at(
            parent_descriptor, path.name, expected_identity=expected_identity)
        _remove_owned_directory_contents_at(
            root_descriptor, int(root_info.st_dev), transaction, fault)
        removed_descriptor = root_descriptor
        root_descriptor = -1
        _remove_open_directory_at(
            parent_descriptor, path.name, removed_descriptor, root_info,
            transaction, fault)
        os.fsync(parent_descriptor)
    except Task8Error:
        raise
    except OSError as exc:
        raise Task8Error(
            f"descriptor-relative sandbox subtree cleanup failed: {path}",
            status="RECOVERY_REQUIRED") from exc
    finally:
        if root_descriptor >= 0:
            os.close(root_descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)


def _cleanup_sandbox_report_scratch_impl(
    transaction: "OuterTransaction", *, fault: Callable[[str], Any] | None = None,
) -> None:
    transaction._validate_journal()
    record = transaction.journal["sandboxScratch"]
    if record is None:
        return
    paths = _sandbox_scratch_paths(
        record, transaction.repo_root, transaction.journal["transactionId"])
    reservation_payload = _sandbox_reservation_payload(
        transaction.journal["transactionId"], transaction.journal["sourceHead"],
        record["bundleIdentifier"], paths.transaction_root, paths.report_root)
    marker_payload = _sandbox_owner_payload(
        transaction.journal["transactionId"], record["bundleIdentifier"])
    parent_descriptor = -1
    root_descriptor = -1
    report_descriptor = -1
    try:
        for path_key, identity_key in (
            ("containerRoot", "containerRootIdentity"),
            ("containerDataRoot", "containerDataIdentity"),
        ):
            checked, _ = _open_expected_pinned_directory(
                Path(record[path_key]), record[identity_key])
            os.close(checked)
        parent_descriptor, parent_info = _open_expected_pinned_directory(
            Path(record["scratchParent"]), record["scratchParentIdentity"])
        if (paths.reservation_path.parent != Path(record["scratchParent"]) or
                paths.transaction_root.parent != Path(record["scratchParent"]) or
                paths.report_root.parent != paths.transaction_root):
            raise Task8Error("sandbox cleanup leaf relation differs",
                             status="RECOVERY_REQUIRED")

        reservation_present = (
            _entry_stat_at(parent_descriptor, paths.reservation_path.name) is not None)
        if reservation_present:
            reservation_descriptor, _ = _verify_regular_at(
                parent_descriptor,
                paths.reservation_path.name,
                expected_identity=(record["reservationIdentity"] or None),
                expected_payload=reservation_payload,
                expected_sha256=record["reservationSha256"],
                expected_mode=0o600,
            )
            os.close(reservation_descriptor)
        elif record["state"] not in ("PLANNED", "CLEANING", "CLEANED"):
            raise Task8Error("sandbox reservation disappeared",
                             status="RECOVERY_REQUIRED")
        if record["state"] == "CLEANED" and reservation_present:
            raise Task8Error("CLEANED sandbox reservation still exists",
                             status="RECOVERY_REQUIRED")

        root_present = (
            _entry_stat_at(parent_descriptor, paths.transaction_root.name) is not None)
        root_info: os.stat_result | None = None
        report_info: os.stat_result | None = None
        marker_present = False
        report_present = False
        if not root_present:
            if (record["state"] in ("PREPARED", "COPIED") or
                    (record["state"] == "RESERVED" and
                     record["transactionIdentity"])):
                raise Task8Error("sandbox transaction root disappeared",
                                 status="RECOVERY_REQUIRED")
        else:
            root_descriptor, root_info = _open_directory_at(
                parent_descriptor,
                paths.transaction_root.name,
                expected_identity=(record["transactionIdentity"] or None),
            )
            root_identity = [
                int(root_info.st_dev), int(root_info.st_ino),
                int(root_info.st_uid), stat.S_IMODE(root_info.st_mode),
            ]
            if (root_info.st_dev != parent_info.st_dev or
                    stat.S_IMODE(root_info.st_mode) != 0o700):
                raise Task8Error("sandbox transaction root identity differs",
                                 status="RECOVERY_REQUIRED")
            root_names = sorted(os.listdir(root_descriptor))
            if not set(root_names).issubset(
                    {"owner.json", RUNTIME_AUTOMATION_DIRECTORY}):
                raise Task8Error("sandbox transaction root has unexpected child",
                                 status="RECOVERY_REQUIRED")
            if not record["transactionIdentity"]:
                if record["state"] not in ("RESERVED", "CLEANING") or root_names:
                    raise Task8Error(
                        "unrecorded sandbox root is not exact empty root",
                        status="RECOVERY_REQUIRED")
                record_identity = root_identity
            else:
                record_identity = record["transactionIdentity"]
            marker_present = "owner.json" in root_names
            report_present = RUNTIME_AUTOMATION_DIRECTORY in root_names
            if marker_present:
                marker_descriptor, _ = _verify_regular_at(
                    root_descriptor,
                    "owner.json",
                    expected_payload=marker_payload,
                    expected_sha256=record["markerSha256"],
                    expected_mode=0o600,
                )
                os.close(marker_descriptor)
            if report_present:
                report_descriptor, report_info = _open_directory_at(
                    root_descriptor,
                    RUNTIME_AUTOMATION_DIRECTORY,
                    expected_identity=(record["reportIdentity"] or None),
                )
                if (report_info.st_dev != root_info.st_dev or
                        stat.S_IMODE(report_info.st_mode) != 0o700):
                    raise Task8Error("sandbox report root identity differs",
                                     status="RECOVERY_REQUIRED")
                if not record["reportIdentity"] and os.listdir(report_descriptor):
                    raise Task8Error(
                        "unrecorded sandbox report root is not empty",
                        status="RECOVERY_REQUIRED")
            if record["state"] == "PLANNED":
                raise Task8Error("PLANNED sandbox unexpectedly has transaction root",
                                 status="RECOVERY_REQUIRED")
            if record["state"] == "RESERVED":
                if report_present and not marker_present:
                    raise Task8Error(
                        "sandbox report root exists without owner marker",
                        status="RECOVERY_REQUIRED")
                if report_present and os.listdir(report_descriptor):
                    raise Task8Error("RESERVED sandbox report root is not empty",
                                     status="RECOVERY_REQUIRED")
            elif record["state"] in ("PREPARED", "COPIED"):
                if not marker_present or not report_present:
                    raise Task8Error(
                        f"{record['state']} sandbox root is incomplete",
                        status="RECOVERY_REQUIRED")
            elif record["state"] == "CLEANED":
                raise Task8Error("CLEANED sandbox transaction root still exists",
                                 status="RECOVERY_REQUIRED")

        if record["state"] != "CLEANED":
            if record["state"] != "CLEANING":
                transaction._fault(fault, "BEFORE_SANDBOX_CLEANING")
                record["state"] = "CLEANING"
                transaction.persist()
                transaction._fault(fault, "AFTER_SANDBOX_CLEANING_DURABLE")
            if root_present:
                if report_present:
                    assert report_info is not None
                    _remove_owned_directory_contents_at(
                        report_descriptor, int(report_info.st_dev),
                        transaction, fault)
                    removed_report_descriptor = report_descriptor
                    report_descriptor = -1
                    _remove_open_directory_at(
                        root_descriptor, RUNTIME_AUTOMATION_DIRECTORY,
                        removed_report_descriptor, report_info,
                        transaction, fault)
                if _entry_stat_at(root_descriptor, "owner.json") is not None:
                    marker_descriptor, marker_info = _verify_regular_at(
                        root_descriptor,
                        "owner.json",
                        expected_payload=marker_payload,
                        expected_sha256=record["markerSha256"],
                        expected_mode=0o600,
                    )
                    _unlink_open_regular_at(
                        root_descriptor, "owner.json", marker_descriptor,
                        marker_info, transaction, fault)
                if os.listdir(root_descriptor):
                    raise Task8Error("sandbox root changed during cleanup",
                                     status="RECOVERY_REQUIRED")
                os.fsync(root_descriptor)
                assert root_info is not None
                removed_root_descriptor = root_descriptor
                root_descriptor = -1
                _remove_open_directory_at(
                    parent_descriptor, paths.transaction_root.name,
                    removed_root_descriptor, root_info, transaction, fault)
                transaction._fault(
                    fault, "AFTER_SANDBOX_TRANSACTION_ROOT_REMOVE")
            os.fsync(parent_descriptor)
            transaction._fault(
                fault, "AFTER_SANDBOX_TRANSACTION_PARENT_SYNC")
            if _entry_stat_at(
                    parent_descriptor, paths.reservation_path.name) is not None:
                reservation_descriptor, reservation_info = _verify_regular_at(
                    parent_descriptor,
                    paths.reservation_path.name,
                    expected_identity=(record["reservationIdentity"] or None),
                    expected_payload=reservation_payload,
                    expected_sha256=record["reservationSha256"],
                    expected_mode=0o600,
                )
                transaction._fault(fault, "BEFORE_SANDBOX_RESERVATION_REMOVE")
                _unlink_open_regular_at(
                    parent_descriptor, paths.reservation_path.name,
                    reservation_descriptor, reservation_info, None, None)
                transaction._fault(fault, "AFTER_SANDBOX_RESERVATION_REMOVE")
            os.fsync(parent_descriptor)
            transaction._fault(
                fault, "AFTER_SANDBOX_RESERVATION_PARENT_SYNC")
            if (_entry_stat_at(
                    parent_descriptor, paths.transaction_root.name) is not None or
                    _entry_stat_at(
                        parent_descriptor, paths.reservation_path.name) is not None):
                raise Task8Error("sandbox scratch cleanup did not remove exact paths",
                                 status="RECOVERY_REQUIRED")
            _revalidate_pinned_directory_path(
                Path(record["scratchParent"]), parent_descriptor,
                record["scratchParentIdentity"])
            record["state"] = "CLEANED"
            transaction.persist()
            transaction._fault(fault, "AFTER_SANDBOX_CLEANED_DURABLE")
        transaction.journal["sandboxScratch"] = None
        transaction.persist()
        transaction._fault(fault, "AFTER_SANDBOX_SCRATCH_NULL_DURABLE")
    finally:
        if report_descriptor >= 0:
            os.close(report_descriptor)
        if root_descriptor >= 0:
            os.close(root_descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)


def cleanup_sandbox_report_scratch(
    transaction: "OuterTransaction", *, fault: Callable[[str], Any] | None = None,
) -> None:
    try:
        _cleanup_sandbox_report_scratch_impl(transaction, fault=fault)
    except SimulatedCrash:
        raise
    except Task8Error as exc:
        if exc.status == "RECOVERY_REQUIRED":
            raise
        raise Task8Error(exc.detail, status="RECOVERY_REQUIRED") from exc
    except OSError as exc:
        raise Task8Error(
            f"sandbox scratch cleanup I/O failed: {exc}",
            status="RECOVERY_REQUIRED") from exc


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
                "sandboxScratch": None,
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
            "managedFamilyListings", "activeProcess", "sandboxScratch",
            "intendedBundleSha256", "issues",
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
        _validate_sandbox_scratch_record(
            self.journal["sandboxScratch"], self.repo_root,
            self.journal["transactionId"], self.journal["sourceHead"])
        intended = self.journal["intendedBundleSha256"]
        if (type(intended) is not str or
                (intended != "" and not re.fullmatch(r"[0-9a-f]{64}", intended)) or
                (self.journal["phase"] == "COMMITTED" and intended == "")):
            raise Task8Error("recovery journal bundle hash is invalid",
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
        cleanup_sandbox_report_scratch(self)
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

    def _verify_terminal_state(
        self,
        status_gate: Callable[[], tuple[str, str]] | None = None,
    ) -> None:
        phase = self.journal["phase"]
        try:
            if phase == "COMMITTED":
                path = self.repo_root / BASE_BUNDLE
                info = _physical_regular(path)
                if (info.st_size <= 0 or
                        _sha256(path) != self.journal["intendedBundleSha256"]):
                    raise Task8Error("committed base bundle hash differs")
                bundle = _load_report(path)
                issues = rules.validate_base_bundle(bundle, path, self.repo_root)
                if issues:
                    raise Task8Error(
                        f"committed base bundle validation failed: {issues[0]}")
                gate = status_gate or (lambda: git_status_gate(self.repo_root))
                head, status_value = gate()
                if head != self.journal["sourceHead"] or status_value:
                    raise Task8Error("committed recovery checkout is not exact clean HEAD")
            elif phase == "ROLLED_BACK":
                for record in self.journal["snapshots"]:
                    self._verify_snapshot(record)
                _, listings = self._snapshot_candidates()
                if listings != self.journal["managedFamilyListings"]:
                    raise Task8Error("rolled-back managed family listing differs")
            else:
                raise Task8Error("terminal verification requires terminal phase")
        except Task8Error as exc:
            if exc.status == "RECOVERY_REQUIRED":
                raise
            raise Task8Error(exc.detail, status="RECOVERY_REQUIRED") from exc
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            raise Task8Error(
                f"terminal state verification failed: {exc}",
                status="RECOVERY_REQUIRED") from exc

    def recover(
        self,
        source_head: str,
        *,
        process_probe: Callable[[int], ProcessRecord | None] | None = None,
        kill_group: Callable[[int], None] | None = None,
        nested_retry: Callable[[str, tuple[Path, ...]], None] | None = None,
        nested_validate: Callable[[str], None] | None = None,
        status_gate: Callable[[], tuple[str, str]] | None = None,
    ) -> None:
        if not self.journal:
            return
        self._validate_journal()
        if source_head != self.journal["sourceHead"]:
            raise Task8Error(
                f"recovery requires commit {self.journal['sourceHead']}",
                status="RECOVERY_REQUIRED")
        if self.journal["phase"] in ("ROLLED_BACK", "COMMITTED"):
            self._verify_terminal_state(status_gate)
            return
        if self.journal["activeProcess"] is not None:
            active = self.journal["activeProcess"]
            probe = process_probe or _probe_process
            observed = probe(active["pid"])
            if observed is None:
                if _process_group_exists(active["pgid"]):
                    terminator = kill_group or _terminate_process_group
                    terminator(active["pgid"])
                    if _process_group_exists(active["pgid"]):
                        raise Task8Error(
                            "surviving owned process group did not terminate",
                            status="RECOVERY_REQUIRED")
                self.journal["activeProcess"] = None
                self.persist()
            elif observed.start_token != active["startToken"]:
                raise Task8Error("recorded supervisor PID was reused",
                                 status="RECOVERY_REQUIRED")
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
                if (probe(active["pid"]) is not None or
                        _process_group_exists(active["pgid"])):
                    raise Task8Error("owned process group did not terminate",
                                     status="RECOVERY_REQUIRED")
                self.journal["activeProcess"] = None
                self.persist()
        cleanup_sandbox_report_scratch(self)
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
                target_pid = os.fork()
                if target_pid == 0:
                    environment = os.environ.copy()
                    environment.update(dict(command.env))
                    os.execve(executable_path, list(command.argv), environment)
                _, target_status = os.waitpid(target_pid, 0)
                target_returncode = os.waitstatus_to_exitcode(target_status)
                os._exit(
                    target_returncode
                    if target_returncode >= 0 else 128 - target_returncode)
            except BaseException as exc:
                try:
                    os.write(2, f"owned exec failed: {exc}\n".encode("utf-8"))
                except OSError:
                    pass
                os._exit(126)
        os.close(release_read)
        os.close(stdout_descriptor)
        os.close(stderr_descriptor)
        released = False
        leader_waited = False
        pgid = -1
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
            observed = _probe_process(pid)
            if observed is None or observed.pid != pid or observed.pgid != pid:
                raise Task8Error("owned supervisor identity is unavailable")
            try:
                observed_executable = str(
                    Path(observed.executable).resolve(strict=True))
            except OSError as exc:
                raise Task8Error(
                    f"owned supervisor executable is unreadable: {exc}") from exc
            argv_hash = hashlib.sha256(
                b"\0".join(os.fsencode(item) for item in observed.argv)).hexdigest()
            self.journal["activeProcess"] = {
                "step": command.name,
                "pid": pid,
                "pgid": pgid,
                "startToken": observed.start_token,
                "executable": observed_executable,
                "argvSha256": argv_hash,
            }
            self.journal["phase"] = "UNREAL_RUNNING"
            self.persist()
            self._fault(fault, "AFTER_ACTIVE_PROCESS_DURABLE")
            os.write(release_write, b"1")
            os.close(release_write)
            release_write = -1
            released = True
            self._fault(fault, "AFTER_OWNED_PROCESS_RELEASE")
            deadline = time.monotonic() + command.timeout_seconds
            status_value: int | None = None
            while status_value is None:
                waited, status_candidate = os.waitpid(pid, os.WNOHANG)
                if waited == pid:
                    status_value = status_candidate
                    leader_waited = True
                    break
                if time.monotonic() >= deadline:
                    raise Task8Error(f"owned step timed out: {command.name}")
                time.sleep(0.02)
            self._fault(fault, "AFTER_OWNED_PROCESS_WAIT")
            returncode = os.waitstatus_to_exitcode(status_value)
            active_record = dict(self.journal["activeProcess"])
            leaked_descendants = _reap_completed_owned_process_group(
                active_record)
            self.journal["activeProcess"] = None
            if returncode == 0 and leaked_descendants:
                self.persist()
                raise Task8Error(
                    f"owned successful step leaked a process-group descendant: "
                    f"{command.name}")
            if command.name in _COMPLETED_STEPS:
                self.journal["completedStep"] = command.name
            self.persist()
            stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            return CommandResult(returncode, stdout, stderr)
        except BaseException as exc:
            if release_write >= 0:
                os.close(release_write)
                release_write = -1
            if isinstance(exc, SimulatedCrash):
                if not released and not leader_waited:
                    try:
                        os.waitpid(pid, 0)
                    except ChildProcessError:
                        pass
                raise
            if isinstance(exc, _ProcessIdentityMismatch):
                raise
            try:
                if released and pgid > 0 and _process_group_exists(pgid):
                    _terminate_process_group(pgid)
                if not leader_waited:
                    try:
                        os.waitpid(pid, 0)
                    except ChildProcessError:
                        pass
                    leader_waited = True
                if pgid > 0 and _process_group_exists(pgid):
                    _terminate_process_group(pgid)
                if pgid > 0 and _process_group_exists(pgid):
                    raise Task8Error(
                        "owned process group survived exception cleanup",
                        status="RECOVERY_REQUIRED")
                active = self.journal.get("activeProcess")
                if type(active) is dict and active.get("pid") == pid:
                    self.journal["activeProcess"] = None
                    self.persist()
            except BaseException as cleanup_error:
                if isinstance(cleanup_error, Task8Error) and \
                        cleanup_error.status == "RECOVERY_REQUIRED":
                    raise
                raise Task8Error(
                    f"owned process exception cleanup failed: {cleanup_error}",
                    status="RECOVERY_REQUIRED") from cleanup_error
            raise

    def _fault(self, fault: Callable[[str], Any] | None, point: str) -> None:
        if fault is None:
            return
        action = fault(point)
        if action == "crash":
            raise SimulatedCrash(f"injected crash at {point}")
        if action == "fail":
            raise Task8Error(f"injected fail at {point}")

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
        parent_descriptor = -1
        recovery_descriptor = -1
        snapshot_descriptor = -1
        try:
            parent_descriptor, parent_info = _open_pinned_directory(recovery.parent)
            if _entry_stat_at(parent_descriptor, recovery.name) is None:
                _require_reopened_directory_absence(
                    recovery.parent,
                    parent_descriptor,
                    list(_directory_identity_tuple(parent_info)),
                    recovery.name,
                )
                return
            recovery_descriptor, recovery_info = _open_directory_at(
                parent_descriptor, recovery.name)
            if stat.S_IMODE(recovery_info.st_mode) != 0o700:
                raise Task8Error("recovery root mode differs",
                                 status="RECOVERY_REQUIRED")
            root_names = sorted(os.listdir(recovery_descriptor))
            if root_names != ["snapshots"]:
                raise Task8Error("recovery root shape differs",
                                 status="RECOVERY_REQUIRED")
            snapshot_descriptor, snapshot_info = _open_directory_at(
                recovery_descriptor, "snapshots")
            if (snapshot_info.st_dev != recovery_info.st_dev or
                    stat.S_IMODE(snapshot_info.st_mode) != 0o700):
                raise Task8Error("recovery snapshot directory differs",
                                 status="RECOVERY_REQUIRED")
            expected: dict[str, dict[str, Any]] = {}
            expected_parent = recovery / "snapshots"
            for record in self.journal["snapshots"]:
                if record["priorType"] != "regular":
                    continue
                backup = _contained(self.repo_root, record["backupPath"])
                if backup.parent != expected_parent or backup.name in expected:
                    raise Task8Error("recovery backup path shape differs",
                                     status="RECOVERY_REQUIRED")
                expected[backup.name] = record
            if sorted(os.listdir(snapshot_descriptor)) != sorted(expected):
                raise Task8Error("recovery snapshot listing differs",
                                 status="RECOVERY_REQUIRED")
            for name in sorted(expected):
                record = expected[name]
                file_descriptor, file_info = _verify_regular_at(
                    snapshot_descriptor,
                    name,
                    expected_sha256=record["priorSha256"],
                    expected_size=record["priorSizeBytes"],
                    expected_mode=0o600,
                )
                if file_info.st_dev != snapshot_info.st_dev:
                    os.close(file_descriptor)
                    raise Task8Error("recovery snapshot crossed device",
                                     status="RECOVERY_REQUIRED")
                _unlink_open_regular_at(
                    snapshot_descriptor, name, file_descriptor, file_info,
                    None, None)
                if name in os.listdir(snapshot_descriptor):
                    raise Task8Error("recovery snapshot leaf reappeared",
                                     status="RECOVERY_REQUIRED")
            os.fsync(snapshot_descriptor)
            removed_snapshot_descriptor = snapshot_descriptor
            snapshot_descriptor = -1
            _remove_open_directory_at(
                recovery_descriptor, "snapshots", removed_snapshot_descriptor,
                snapshot_info, None, None)
            os.fsync(recovery_descriptor)
            removed_recovery_descriptor = recovery_descriptor
            recovery_descriptor = -1
            _remove_open_directory_at(
                parent_descriptor, recovery.name, removed_recovery_descriptor,
                recovery_info, None, None)
            os.fsync(parent_descriptor)
            _require_reopened_directory_absence(
                recovery.parent,
                parent_descriptor,
                list(_directory_identity_tuple(parent_info)),
                recovery.name,
            )
        except Task8Error:
            raise
        except OSError as exc:
            raise Task8Error(
                "descriptor-relative recovery cleanup failed",
                status="RECOVERY_REQUIRED") from exc
        finally:
            if snapshot_descriptor >= 0:
                os.close(snapshot_descriptor)
            if recovery_descriptor >= 0:
                os.close(recovery_descriptor)
            if parent_descriptor >= 0:
                os.close(parent_descriptor)

    def finish(
        self,
        *,
        status_gate: Callable[[], tuple[str, str]] | None = None,
    ) -> None:
        self._validate_journal()
        if self.journal.get("phase") not in ("COMMITTED", "ROLLED_BACK"):
            raise Task8Error("only terminal transaction may be cleaned",
                             status="RECOVERY_REQUIRED")
        if self.journal.get("sandboxScratch") is not None:
            raise Task8Error("terminal transaction retains sandbox scratch",
                             status="RECOVERY_REQUIRED")
        self._verify_terminal_state(status_gate)
        path = journal_path(self.repo_root)
        reports_descriptor = -1
        try:
            reports_descriptor, reports_info = _open_pinned_directory(path.parent)
            expected_payload = rules.canonical_json_bytes(self.journal) + b"\n"
            self._remove_recovery_material()
            reopened_descriptor, reopened_info = _open_pinned_directory(path.parent)
            try:
                if not _same_entry(reports_info, reopened_info):
                    raise Task8Error("reports parent changed during terminal cleanup",
                                     status="RECOVERY_REQUIRED")
            finally:
                os.close(reopened_descriptor)
            journal_descriptor, journal_info = _verify_regular_at(
                reports_descriptor,
                path.name,
                expected_payload=expected_payload,
                expected_sha256=hashlib.sha256(expected_payload).hexdigest(),
                expected_size=len(expected_payload),
                expected_mode=0o600,
            )
            _unlink_open_regular_at(
                reports_descriptor, path.name, journal_descriptor, journal_info,
                None, None)
            os.fsync(reports_descriptor)
            reopened_descriptor, reopened_info = _open_pinned_directory(path.parent)
            try:
                if (not _same_entry(reports_info, reopened_info) or
                        _entry_stat_at(reopened_descriptor, path.name) is not None):
                    raise Task8Error("terminal journal cleanup path changed",
                                     status="RECOVERY_REQUIRED")
            finally:
                os.close(reopened_descriptor)
            self.close()
        except Task8Error:
            raise
        except OSError as exc:
            raise Task8Error("terminal journal cleanup failed",
                             status="RECOVERY_REQUIRED") from exc
        finally:
            if reports_descriptor >= 0:
                os.close(reports_descriptor)

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
    executable_result = subprocess.run(
        ("ps", "-p", str(pid), "-o", "comm="),
        capture_output=True, text=True, check=False, timeout=5)
    command_result = subprocess.run(
        ("ps", "-p", str(pid), "-o", "command="),
        capture_output=True, text=True, check=False, timeout=5)
    executable = executable_result.stdout.strip()
    command_text = command_result.stdout.strip()
    if (executable_result.returncode != 0 or
            command_result.returncode != 0 or
            not executable or not command_text):
        raise Task8Error(f"cannot inspect live pid {pid}",
                         status="RECOVERY_REQUIRED")
    try:
        argv = tuple(shlex.split(command_text))
    except ValueError as exc:
        raise Task8Error(f"cannot parse process argv for pid {pid}",
                         status="RECOVERY_REQUIRED") from exc
    return ProcessRecord(pid, pgid, token, executable, argv)


def _process_group_exists(pgid: int) -> bool:
    if pgid <= 0:
        raise Task8Error("refusing invalid process group",
                         status="RECOVERY_REQUIRED")
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _terminate_process_group(pgid: int) -> None:
    if not _process_group_exists(pgid):
        return
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except PermissionError as exc:
        raise Task8Error("cannot signal owned process group",
                         status="RECOVERY_REQUIRED") from exc
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if not _process_group_exists(pgid):
            return
        time.sleep(0.05)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError:
        # A same-process test can retain the already-dead supervisor as its
        # zombie child. The caller reaps it, then repeats/verifies the group.
        return


def _reap_completed_owned_process_group(
    active: dict[str, Any],
    *,
    process_probe: Callable[[int], ProcessRecord | None] | None = None,
    group_exists: Callable[[int], bool] | None = None,
    terminator: Callable[[int], None] | None = None,
) -> bool:
    """Reap descendants left in the recorded group after its leader was waited."""
    probe = process_probe or _probe_process
    exists = group_exists or _process_group_exists
    terminate = terminator or _terminate_process_group
    pgid = active["pgid"]
    if not exists(pgid):
        return False
    observed = probe(pgid)
    if observed is not None:
        try:
            observed_executable = str(
                Path(observed.executable).resolve(strict=True))
        except OSError as exc:
            raise _ProcessIdentityMismatch(
                f"completed supervisor identity is unreadable: {exc}",
                status="RECOVERY_REQUIRED") from exc
        argv_hash = hashlib.sha256(
            b"\0".join(os.fsencode(item) for item in observed.argv)).hexdigest()
        if (observed.pid != active["pid"] or
                observed.pgid != active["pgid"] or
                observed.start_token != active["startToken"] or
                observed_executable != active["executable"] or
                argv_hash != active["argvSha256"]):
            raise _ProcessIdentityMismatch(
                "completed supervisor PID/PGID was reused",
                status="RECOVERY_REQUIRED")
    terminate(pgid)
    deadline = time.monotonic() + 5.0
    while exists(pgid) and time.monotonic() < deadline:
        time.sleep(0.02)
    if exists(pgid):
        raise Task8Error(
            "completed owned process group did not terminate",
            status="RECOVERY_REQUIRED")
    return True


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
        refuse_competing_processes(process_probe())
        require_healthy_thermal(*thermal_probe())
        command = (resolve_command(planned_command)
                   if resolve_command is not None else planned_command)
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


def _evidence_tree_files(root: Path) -> list[Path]:
    def fail(error: OSError) -> None:
        raise Task8Error(
            f"automation evidence tree is unreadable: {error.filename}") from error

    paths = []
    try:
        for directory, subdirs, names in os.walk(
                root, topdown=True, onerror=fail, followlinks=False):
            current = Path(directory)
            _physical_directory(current)
            subdirs.sort()
            names.sort()
            for name in subdirs:
                _physical_directory(current / name)
            for name in names:
                path = current / name
                _physical_regular(path, allow_empty=True)
                paths.append(path)
    except Task8Error:
        raise
    except OSError as exc:
        raise Task8Error(
            f"automation evidence tree changed while scanning: {root}") from exc
    if len(paths) != len(set(paths)):
        raise Task8Error("automation evidence file paths are not sorted unique")
    return sorted(paths)


def _evidence_set(
    root: Path, repo_root: Path, transaction_id: str, source_head: str,
    *, expected_directory: OwnedDirectory | None = None,
) -> dict[str, Any]:
    expected_snapshot = None
    if expected_directory is None:
        _physical_directory(root)
    else:
        expected = _validate_runtime_report_directory(
            expected_directory, repo_root, require_empty=False)
        if root != expected:
            raise Task8Error("automation evidence root differs from owned directory")
        try:
            _physical_regular(root / "index.json")
        except OSError as exc:
            raise Task8Error("runtime automation index.json is missing") from exc
        expected_snapshot = expected_directory.tree_snapshot
        if (expected_snapshot is not None and
                _tree_records(root, require_index=True) != expected_snapshot):
            raise Task8Error(
                "runtime automation tree differs from copied commitment")
    paths = _evidence_tree_files(root)
    try:
        files = [_evidence(path, repo_root, allow_empty=True) for path in paths]
    except OSError as exc:
        raise Task8Error(
            "automation evidence tree changed during hashing") from exc
    if not files:
        raise Task8Error(f"automation evidence is empty: {root}")
    if _evidence_tree_files(root) != paths:
        raise Task8Error("automation evidence tree changed during hashing")
    if expected_directory is not None:
        _validate_runtime_report_directory(
            expected_directory, repo_root, require_empty=False)
        if (expected_snapshot is not None and
                _tree_records(root, require_index=True) != expected_snapshot):
            raise Task8Error(
                "runtime automation tree changed after evidence hashing")
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


_UNREALPAK_MOUNT_PREFIX = "../../../"
_UNREALPAK_MOUNT = re.compile(
    r'^\s*(?:LogPakFile:\s*Display:\s*)?Listing .+ with mount point '
    r'"(?P<mount>[^\"]+)"\s*$',
    re.IGNORECASE)
_UNREALPAK_MEMBER = re.compile(
    r'^\s*(?:LogPakFile:\s*Display:\s*)?"(?P<path>[^\"]+)".*?'
    r'\bsize:\s*(?P<size>[0-9]+)\s+bytes\b',
    re.IGNORECASE)
_UNREALPAK_SUMMARY = re.compile(
    r'^\s*(?:LogPakFile:\s*Display:\s*)?(?P<count>[0-9]+)\s+files\s+'
    r'\([0-9]+\s+bytes\),\s+\([0-9]+\s+filtered\s+bytes\)\.\s*$',
    re.IGNORECASE)


def parse_unrealpak_list(output: str) -> list[dict[str, Any]]:
    lines = output.splitlines()
    mounts = [
        (index, match.group("mount"))
        for index, line in enumerate(lines)
        if (match := _UNREALPAK_MOUNT.search(line)) is not None
    ]
    if len(mounts) != 1 or mounts[0][1] != _UNREALPAK_MOUNT_PREFIX:
        raise Task8Error("UnrealPak emitted missing or ambiguous mount point")
    summaries = [
        (index, int(match.group("count")))
        for index, line in enumerate(lines)
        if (match := _UNREALPAK_SUMMARY.search(line)) is not None
    ]
    if len(summaries) != 1:
        raise Task8Error("UnrealPak emitted missing or ambiguous file summary")
    members = []
    member_lines = []
    for index, line in enumerate(lines):
        match = _UNREALPAK_MEMBER.search(line)
        if match is None:
            continue
        relative = match.group("path")
        if not _normalized_relative(relative):
            raise Task8Error(f"UnrealPak emitted invalid member path: {relative}")
        member_lines.append(index)
        members.append({
            "path": _UNREALPAK_MOUNT_PREFIX + relative,
            "sizeBytes": int(match.group("size")),
        })
    members.sort(key=lambda item: item["path"])
    unique_count = len({item["path"] for item in members})
    if not members or unique_count != len(members):
        raise Task8Error("UnrealPak member list is empty or duplicated")
    if not (mounts[0][0] < min(member_lines) and
            summaries[0][0] > max(member_lines)):
        raise Task8Error("UnrealPak member list is not terminally framed")
    if summaries[0][1] != unique_count:
        raise Task8Error("UnrealPak member count differs from terminal summary")
    return members


def _unrealpak_extract_plan(
    unrealpak: Path,
    container: Path,
    output_root: Path,
    canonical_member: str,
) -> tuple[CommandSpec, Path]:
    if (type(canonical_member) is not str or
            not canonical_member.startswith(_UNREALPAK_MOUNT_PREFIX)):
        raise Task8Error("UnrealPak extraction member lacks canonical mount")
    relative = canonical_member[len(_UNREALPAK_MOUNT_PREFIX):]
    if not _normalized_relative(relative):
        raise Task8Error("UnrealPak extraction member is not normalized")
    return (
        CommandSpec("cook-package", _nice(
            str(unrealpak), str(container), "-Extract", str(output_root),
            f"-Filter={relative}")),
        output_root / PurePosixPath(relative),
    )


def _normalized_absolute(value: Any) -> bool:
    if (type(value) is not str or not value or "\0" in value or
            "\\" in value or value.endswith("/")):
        return False
    candidate = Path(value)
    return (candidate.anchor == os.sep and candidate.is_absolute() and
            os.path.normpath(value) == value and
            all(part not in ("", ".", "..") for part in candidate.parts[1:]))


def parse_sandbox_probe_event(
    output: str, transaction_id: str, source_head: str,
) -> dict[str, Any]:
    if RUNTIME_EVENT_PREFIX in output:
        raise Task8Error("packaged sandbox probe emitted a runtime event")
    payloads = []
    for line in output.splitlines():
        position = line.find(SANDBOX_EVENT_PREFIX)
        if position >= 0:
            payloads.append(line[position + len(SANDBOX_EVENT_PREFIX):])
    if len(payloads) != 1:
        raise Task8Error("packaged sandbox probe emitted zero or duplicate events")
    payload = payloads[0]
    def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for key, item in pairs:
            if key in value:
                raise ValueError(f"duplicate JSON member: {key}")
            value[key] = item
        return value

    def reject_nonfinite(token: str) -> Any:
        raise ValueError(f"non-finite JSON number: {token}")

    try:
        value = json.loads(
            payload, object_pairs_hook=strict_object,
            parse_constant=reject_nonfinite)
        canonical_payload = rules.canonical_json_bytes(value).decode("utf-8")
    except (json.JSONDecodeError, TypeError, ValueError) as exc:
        raise Task8Error(f"packaged sandbox JSON is truncated/invalid: {exc}") from exc
    expected = {
        "automationReportsRoot", "bundleIdentifier", "containerDataRoot",
        "issues", "reportType", "schemaVersion", "sourceHead", "status",
        "transactionId",
    }
    if (type(value) is not dict or set(value) != expected or
            canonical_payload != payload):
        raise Task8Error("packaged sandbox event is not canonical strict JSON")
    bundle = value["bundleIdentifier"]
    exact = {
        "schemaVersion": 1,
        "reportType": "garner-terrain-packaged-sandbox",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "issues": [],
    }
    if (type(value["schemaVersion"]) is not int or
            type(value["reportType"]) is not str or
            type(value["status"]) is not str or
            type(value["transactionId"]) is not str or
            type(value["sourceHead"]) is not str or
            type(value["issues"]) is not list or
            any(value[key] != expected_value
                for key, expected_value in exact.items())):
        raise Task8Error("packaged sandbox probe identity/status differs")
    if type(bundle) is not str or not _BUNDLE_IDENTIFIER.fullmatch(bundle):
        raise Task8Error("packaged sandbox bundle identifier is invalid")
    if (not _normalized_absolute(value["containerDataRoot"]) or
            not _normalized_absolute(value["automationReportsRoot"])):
        raise Task8Error("packaged sandbox paths are not normalized absolute paths")
    data_root = Path(value["containerDataRoot"])
    reports_root = Path(value["automationReportsRoot"])
    if data_root.name != "Data":
        raise Task8Error("packaged sandbox container root must have Data leaf")
    try:
        relative = reports_root.relative_to(data_root)
    except ValueError as exc:
        raise Task8Error("automation reports root is outside container Data") from exc
    if not relative.parts:
        raise Task8Error("automation reports root must be below container Data")
    return value


def parse_runtime_observation_event(
    output: str, transaction_id: str, source_head: str,
) -> dict[str, Any]:
    prefix = RUNTIME_EVENT_PREFIX
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


def _codesign_plist(output: str) -> tuple[dict[str, Any], bytes]:
    starts = [match.start() for match in re.finditer(r"<\?xml\b", output)]
    ends = [match.end() for match in re.finditer(r"</plist>", output)]
    if len(starts) != 1 or len(ends) != 1 or starts[0] >= ends[0]:
        raise Task8Error("codesign emitted zero or multiple entitlement plists")
    raw = output[starts[0]:ends[0]].encode("utf-8")
    try:
        value = plistlib.loads(raw)
    except (ValueError, plistlib.InvalidFileException) as exc:
        raise Task8Error(f"codesign entitlement plist is invalid: {exc}") from exc
    if type(value) is not dict:
        raise Task8Error("codesign entitlement plist root is not a dictionary")
    canonical = plistlib.dumps(value, fmt=plistlib.FMT_XML, sort_keys=True)
    if plistlib.loads(canonical) != value:
        raise Task8Error("canonical entitlement plist readback differs")
    return value, canonical


def _codesign_identifier(output: str) -> str:
    identifiers = []
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("Identifier="):
            identifiers.append(line.split("=", 1)[1])
    if (len(identifiers) != 1 or
            not _BUNDLE_IDENTIFIER.fullmatch(identifiers[0])):
        raise Task8Error("codesign emitted zero, duplicate, or invalid Identifier")
    return identifiers[0]


def _validate_verified_sandbox_app(
    transaction: OuterTransaction, verified: VerifiedSandboxApp,
) -> tuple[Path, Path, Path, dict[str, Any], dict[str, Any]]:
    transaction._validate_journal()
    transaction_id = transaction.journal["transactionId"]
    if type(verified) is not VerifiedSandboxApp:
        raise Task8Error("packaged command requires preverified signed app")
    bundle = verified.bundle_identifier
    if (not _BUNDLE_IDENTIFIER.fullmatch(bundle) or
            verified.signing_identifier != bundle or
            not _normalized_relative(verified.app_bundle_path)):
        raise Task8Error("preverified signed-app identity is malformed")
    executable = verified.packaged_executable
    info_evidence = verified.info_plist
    for label, evidence in (
            ("packaged executable", executable),
            ("archived Info.plist", info_evidence)):
        if (type(evidence) is not dict or set(evidence) != {
                "path", "sha256", "sizeBytes"} or
                not _normalized_relative(evidence.get("path"))):
            raise Task8Error(f"{label} evidence is malformed")
    root = transaction.repo_root
    executable_path = root / PurePosixPath(executable["path"])
    app = root / PurePosixPath(verified.app_bundle_path)
    archive_root = package_run_root(root, transaction_id) / "archive"
    try:
        app.relative_to(archive_root)
    except ValueError as exc:
        raise Task8Error("preverified app is outside transaction archive") from exc
    if app.suffix != ".app":
        raise Task8Error("preverified archive path is not an app bundle")
    _physical_directory(app)
    try:
        enclosing_app = executable_path.parents[2]
    except IndexError as exc:
        raise Task8Error("packaged executable has no enclosing app") from exc
    if (enclosing_app != app or executable_path.parent.name != "MacOS" or
            executable_path.parents[1].name != "Contents"):
        raise Task8Error("preverified app does not enclose packaged executable")
    if (_evidence(executable_path, root) != executable or
            not os.access(executable_path, os.X_OK)):
        raise Task8Error("packaged executable changed after signature verification")
    info_path = root / PurePosixPath(info_evidence["path"])
    if info_path != app / "Contents/Info.plist":
        raise Task8Error("preverified Info.plist is not from the exact app")
    info_stat, info_payload = _read_physical_regular(info_path)
    observed_info = {
        "path": info_path.relative_to(root).as_posix(),
        "sha256": hashlib.sha256(info_payload).hexdigest(),
        "sizeBytes": int(info_stat.st_size),
    }
    if observed_info != info_evidence:
        raise Task8Error("archived Info.plist changed after signature verification")
    try:
        info_value = plistlib.loads(info_payload)
        entitlements = plistlib.loads(verified.canonical_entitlements)
    except (OSError, ValueError, plistlib.InvalidFileException) as exc:
        raise Task8Error(f"preverified signed-app plist is invalid: {exc}") from exc
    if (type(info_value) is not dict or
            info_value.get("CFBundleIdentifier") != bundle or
            type(entitlements) is not dict or
            entitlements.get("com.apple.security.app-sandbox") is not True or
            plistlib.dumps(
                entitlements, fmt=plistlib.FMT_XML, sort_keys=True) !=
            verified.canonical_entitlements):
        raise Task8Error("preverified signed-app plist identity differs")
    return app, executable_path, info_path, info_value, entitlements


def verify_signed_sandbox_app(
    transaction: OuterTransaction,
    source_head: str,
    executable: dict[str, Any],
    *,
    command_runner: Callable[[CommandSpec], CommandResult] | None = None,
) -> VerifiedSandboxApp:
    """Verify the exact archived signed app before executing its probe."""
    transaction._validate_journal()
    if source_head != transaction.journal["sourceHead"]:
        raise Task8Error("signed-app verification source HEAD differs")
    if (type(executable) is not dict or set(executable) != {
            "path", "sha256", "sizeBytes"} or
            not _normalized_relative(executable.get("path"))):
        raise Task8Error("packaged executable evidence is malformed")
    root = transaction.repo_root
    executable_path = root / PurePosixPath(executable["path"])
    try:
        app = executable_path.parents[2]
        app.relative_to(
            package_run_root(root, transaction.journal["transactionId"]) /
            "archive")
    except (IndexError, ValueError) as exc:
        raise Task8Error("packaged executable has no transaction archive app") from exc
    if (app.suffix != ".app" or executable_path.parent.name != "MacOS" or
            executable_path.parents[1].name != "Contents"):
        raise Task8Error("packaged executable does not belong to one app")
    _physical_directory(app)
    executable_info, executable_sha256 = _hash_physical_regular(executable_path)
    executable_fingerprint = _regular_fingerprint(executable_info)
    if ({
            "path": executable_path.relative_to(root).as_posix(),
            "sha256": executable_sha256,
            "sizeBytes": int(executable_info.st_size),
            } != executable or not os.access(executable_path, os.X_OK)):
        raise Task8Error("packaged executable changed before signature verification")
    info_path = app / "Contents/Info.plist"
    info_stat, info_payload = _read_physical_regular(info_path)
    info_fingerprint = _regular_fingerprint(info_stat)
    info_evidence = {
        "path": info_path.relative_to(root).as_posix(),
        "sha256": hashlib.sha256(info_payload).hexdigest(),
        "sizeBytes": int(info_stat.st_size),
    }
    try:
        info_value = plistlib.loads(info_payload)
    except (ValueError, plistlib.InvalidFileException) as exc:
        raise Task8Error(f"archived Info.plist is invalid: {exc}") from exc
    bundle = info_value.get("CFBundleIdentifier") if type(info_value) is dict else None
    if type(bundle) is not str or not _BUNDLE_IDENTIFIER.fullmatch(bundle):
        raise Task8Error("archived Info.plist bundle identifier is invalid")

    def require_signed_inputs_unchanged() -> None:
        observed_executable, observed_executable_sha = _hash_physical_regular(
            executable_path)
        observed_info, observed_info_payload = _read_physical_regular(info_path)
        if (_regular_fingerprint(observed_executable) != executable_fingerprint or
                observed_executable_sha != executable["sha256"] or
                _regular_fingerprint(observed_info) != info_fingerprint or
                observed_info_payload != info_payload):
            raise Task8Error("signed app inputs changed during codesign verification")

    invoke = command_runner or transaction.run_owned_command
    verify_command = CommandSpec("cook-package", _nice(
        "/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)))
    verify = invoke(verify_command)
    if verify.returncode != 0:
        raise Task8Error(f"codesign strict verification failed: {verify.stderr.strip()}")
    entitlement_result = invoke(CommandSpec("cook-package", _nice(
        "/usr/bin/codesign", "-d", "--entitlements", ":-", str(app))))
    if entitlement_result.returncode != 0:
        raise Task8Error(
            f"codesign entitlement read failed: {entitlement_result.stderr.strip()}")
    entitlements, canonical_entitlements = _codesign_plist(
        entitlement_result.stdout + "\n" + entitlement_result.stderr)
    identity_result = invoke(CommandSpec("cook-package", _nice(
        "/usr/bin/codesign", "-d", "--verbose=4", str(app))))
    if identity_result.returncode != 0:
        raise Task8Error(
            f"codesign identity read failed: {identity_result.stderr.strip()}")
    signing_identifier = _codesign_identifier(
        identity_result.stdout + "\n" + identity_result.stderr)
    if (signing_identifier != bundle or
            entitlements.get("com.apple.security.app-sandbox") is not True):
        raise Task8Error("signed app identity or sandbox entitlement differs")
    require_signed_inputs_unchanged()
    final_verify = invoke(verify_command)
    if final_verify.returncode != 0:
        raise Task8Error(
            f"final codesign strict verification failed: "
            f"{final_verify.stderr.strip()}")
    require_signed_inputs_unchanged()
    verified = VerifiedSandboxApp(
        app_bundle_path=app.relative_to(root).as_posix(),
        bundle_identifier=bundle,
        signing_identifier=signing_identifier,
        packaged_executable=json.loads(json.dumps(executable)),
        info_plist=info_evidence,
        canonical_entitlements=canonical_entitlements,
    )
    _validate_verified_sandbox_app(transaction, verified)
    return verified


def _directory_identity_tuple(info: os.stat_result) -> tuple[int, int, int, int]:
    return (
        int(info.st_dev), int(info.st_ino), int(info.st_uid),
        stat.S_IMODE(info.st_mode),
    )


def _sandbox_directory_paths(
    container_root: Path, data_root: Path, reports_root: Path,
) -> tuple[Path, ...]:
    if data_root.parent != container_root or data_root.name != "Data":
        raise Task8Error("sandbox container/Data path relation differs")
    try:
        relative = reports_root.relative_to(data_root)
    except ValueError as exc:
        raise Task8Error("sandbox reports root escapes container Data") from exc
    if not relative.parts:
        raise Task8Error("sandbox reports root is not a strict descendant")
    paths = [container_root, data_root]
    current = data_root
    for component in relative.parts:
        if component in ("", ".", ".."):
            raise Task8Error("sandbox reports path has an invalid component")
        current = current / component
        paths.append(current)
    if current != reports_root:
        raise Task8Error("sandbox reports component chain differs")
    return tuple(paths)


def _safe_attested_directory_identity(
    path: Path, info: os.stat_result, *, status: str,
) -> tuple[int, int, int, int]:
    identity = _directory_identity_tuple(info)
    if info.st_uid != os.geteuid() or identity[3] & 0o022:
        raise Task8Error(
            f"sandbox directory ownership/mode differs: {path}", status=status)
    return identity


def _open_attested_directory_chain(
    paths: tuple[Path, ...],
    expected: tuple[AttestedDirectoryComponent, ...] | None = None,
    *,
    status: str = "RECOVERY_REQUIRED",
) -> list[tuple[AttestedDirectoryComponent, int]]:
    if not paths or (expected is not None and len(expected) != len(paths)):
        raise Task8Error("attested sandbox directory chain is malformed",
                         status=status)
    held: list[tuple[AttestedDirectoryComponent, int]] = []
    try:
        for index, path in enumerate(paths):
            if index == 0:
                descriptor, info = _open_pinned_directory(path, status=status)
            else:
                parent_component, parent_descriptor = held[-1]
                if path.parent != parent_component.path:
                    raise Task8Error(
                        "attested sandbox directory chain is not contiguous",
                        status=status)
                descriptor, info = _open_directory_at(
                    parent_descriptor, path.name, status=status)
            try:
                component = AttestedDirectoryComponent(
                    path, _safe_attested_directory_identity(
                        path, info, status=status))
                if expected is not None and component != expected[index]:
                    raise Task8Error(
                        f"attested sandbox component identity differs: {path}",
                        status=status)
            except BaseException:
                os.close(descriptor)
                raise
            held.append((component, descriptor))
        return held
    except OSError as exc:
        for _, descriptor in reversed(held):
            os.close(descriptor)
        raise Task8Error(
            "cannot open attested sandbox directory chain",
            status=status) from exc
    except BaseException:
        for _, descriptor in reversed(held):
            os.close(descriptor)
        raise


def _revalidate_attested_directory_chain(
    held: list[tuple[AttestedDirectoryComponent, int]],
    *,
    status: str = "RECOVERY_REQUIRED",
) -> None:
    if not held:
        raise Task8Error("attested sandbox directory chain is empty", status=status)
    for index, (component, descriptor) in enumerate(held):
        try:
            opened = os.fstat(descriptor)
        except OSError as exc:
            raise Task8Error(
                f"cannot revalidate sandbox component: {component.path}",
                status=status) from exc
        if (_safe_attested_directory_identity(
                component.path, opened, status=status) != component.identity):
            raise Task8Error(
                f"attested sandbox component changed: {component.path}",
                status=status)
        if index == 0:
            _revalidate_pinned_directory_path(
                component.path, descriptor, list(component.identity))
            continue
        parent_component, parent_descriptor = held[index - 1]
        if component.path.parent != parent_component.path:
            raise Task8Error(
                "attested sandbox directory chain is not contiguous",
                status=status)
        current = _entry_stat_at(
            parent_descriptor, component.path.name, status=status)
        if (current is None or not stat.S_ISDIR(current.st_mode) or
                _directory_identity_tuple(current) != component.identity or
                not _same_entry(opened, current)):
            raise Task8Error(
                f"attested sandbox component path changed: {component.path}",
                status=status)


def _cleanup_partial_sandbox_attestation(
    package_reports: Path,
    parent_descriptor: int,
    parent_identity: list[int],
    published: list[tuple[Path, bytes, tuple[int, ...]]],
) -> None:
    if not published:
        return
    try:
        _revalidate_pinned_directory_path(
            package_reports, parent_descriptor, parent_identity)
        parent_info = os.fstat(parent_descriptor)
        if (parent_info.st_uid != os.geteuid() or
                stat.S_IMODE(parent_info.st_mode) != 0o700):
            raise Task8Error(
                "sandbox attestation parent identity differs during cleanup",
                status="RECOVERY_REQUIRED")
        for path, payload, fingerprint in reversed(published):
            if path.parent != package_reports:
                raise Task8Error(
                    "sandbox attestation cleanup path escapes package evidence",
                    status="RECOVERY_REQUIRED")
            descriptor, opened = _verify_regular_at(
                parent_descriptor,
                path.name,
                expected_payload=payload,
                expected_sha256=hashlib.sha256(payload).hexdigest(),
                expected_size=len(payload),
                expected_mode=0o600,
                expected_identity=[
                    fingerprint[0], fingerprint[1], fingerprint[2],
                    fingerprint[3], fingerprint[5],
                ],
            )
            if _regular_fingerprint(opened) != fingerprint:
                os.close(descriptor)
                raise Task8Error(
                    f"sandbox attestation leaf changed before cleanup: {path.name}",
                    status="RECOVERY_REQUIRED")
            _unlink_open_regular_at(
                parent_descriptor, path.name, descriptor, opened, None, None)
        os.fsync(parent_descriptor)
        _revalidate_pinned_directory_path(
            package_reports, parent_descriptor, parent_identity)
        if any(_entry_stat_at(parent_descriptor, path.name) is not None
               for path, _, _ in published):
            raise Task8Error(
                "partial sandbox attestation remains after cleanup",
                status="RECOVERY_REQUIRED")
    except Task8Error:
        raise
    except OSError as exc:
        raise Task8Error(
            "partial sandbox attestation cleanup failed",
            status="RECOVERY_REQUIRED") from exc


def _validate_attested_sandbox_environment(
    transaction: OuterTransaction,
    environment: AttestedSandboxEnvironment,
) -> None:
    transaction._validate_journal()
    if type(environment) is not AttestedSandboxEnvironment:
        raise Task8Error("sandbox scratch requires attested environment")
    if (environment.transaction_id != transaction.journal["transactionId"] or
            environment.source_head != transaction.journal["sourceHead"] or
            not _BUNDLE_IDENTIFIER.fullmatch(environment.bundle_identifier)):
        raise Task8Error("attested sandbox environment identity differs")
    path_values = (
        environment.container_root,
        environment.container_data_root,
        environment.automation_reports_root,
        environment.metadata_path,
    )
    if (any(not isinstance(path, Path) or not _normalized_absolute(str(path))
            for path in path_values) or
            environment.container_data_root.parent != environment.container_root or
            environment.container_data_root.name != "Data" or
            environment.metadata_path != environment.container_root /
            ".com.apple.containermanagerd.metadata.plist"):
        raise Task8Error("attested sandbox path relation differs")
    chain_paths = _sandbox_directory_paths(
        environment.container_root,
        environment.container_data_root,
        environment.automation_reports_root)
    identities = (
        environment.container_root_identity,
        environment.container_data_identity,
        environment.automation_reports_identity,
    )
    if any(type(value) is not tuple or len(value) != 4 or
           any(type(item) is not int or item < 0 for item in value)
           for value in identities):
        raise Task8Error("attested sandbox directory identity is malformed")
    if (type(environment.directory_chain) is not tuple or
            len(environment.directory_chain) != len(chain_paths) or
            any(type(component) is not AttestedDirectoryComponent
                for component in environment.directory_chain) or
            tuple(component.path for component in environment.directory_chain) !=
            chain_paths or
            environment.directory_chain[0].identity !=
            environment.container_root_identity or
            environment.directory_chain[1].identity !=
            environment.container_data_identity or
            environment.directory_chain[-1].identity !=
            environment.automation_reports_identity):
        raise Task8Error("attested sandbox component ledger is malformed")
    if (type(environment.metadata_fingerprint) is not tuple or
            len(environment.metadata_fingerprint) != 8 or
            any(type(item) is not int or item < 0
                for item in environment.metadata_fingerprint) or
            type(environment.metadata_sha256) is not str or
            not re.fullmatch(r"[0-9a-f]{64}", environment.metadata_sha256)):
        raise Task8Error("attested sandbox metadata identity is malformed")

    held: list[tuple[AttestedDirectoryComponent, int]] = []
    metadata_descriptor = -1
    try:
        held = _open_attested_directory_chain(
            chain_paths, environment.directory_chain)
        container_descriptor = held[0][1]
        metadata_descriptor, metadata_info = _open_regular_at(
            container_descriptor, environment.metadata_path.name)
        metadata_payload = _read_descriptor(metadata_descriptor)
        metadata_after = os.fstat(metadata_descriptor)
        metadata_entry = _entry_stat_at(
            container_descriptor, environment.metadata_path.name)
        if (metadata_entry is None or
                _regular_fingerprint(metadata_info) !=
                environment.metadata_fingerprint or
                _regular_fingerprint(metadata_after) !=
                environment.metadata_fingerprint or
                not _same_entry(metadata_after, metadata_entry) or
                hashlib.sha256(metadata_payload).hexdigest() !=
                environment.metadata_sha256):
            raise Task8Error("attested container metadata changed")
        try:
            metadata = plistlib.loads(metadata_payload)
        except (ValueError, plistlib.InvalidFileException) as exc:
            raise Task8Error("attested container metadata is invalid") from exc
        if (type(metadata) is not dict or
                metadata.get("MCMMetadataIdentifier") !=
                environment.bundle_identifier or
                metadata.get("MCMMetadataCreator") !=
                environment.bundle_identifier):
            raise Task8Error("attested container metadata identity differs")
        _revalidate_attested_directory_chain(held)
    except OSError as exc:
        raise Task8Error(
            "cannot read attested container metadata",
            status="RECOVERY_REQUIRED") from exc
    finally:
        if metadata_descriptor >= 0:
            os.close(metadata_descriptor)
        for _, descriptor in reversed(held):
            os.close(descriptor)

    evidence = environment.attestation_evidence
    if (type(evidence) is not dict or set(evidence) != {
            "path", "sha256", "sizeBytes"} or
            not _normalized_relative(evidence.get("path"))):
        raise Task8Error("attested sandbox evidence is malformed")
    evidence_path = transaction.repo_root / PurePosixPath(evidence["path"])
    if _evidence(evidence_path, transaction.repo_root) != evidence:
        raise Task8Error("attested sandbox evidence changed")
    report = _load_report(evidence_path)
    issues = rules.validate_sandbox_attestation(report, transaction.repo_root)
    if (issues or report.get("transactionId") != environment.transaction_id or
            report.get("sourceHead") != environment.source_head or
            report.get("bundleIdentifier") != environment.bundle_identifier):
        raise Task8Error(
            f"attested sandbox evidence validation failed: {issues[:1]}")


def prepare_sandbox_attestation(
    transaction: OuterTransaction,
    source_head: str,
    verified: VerifiedSandboxApp,
    probe: dict[str, Any],
) -> AttestedSandboxEnvironment:
    """Bind the probed OS paths to the already verified signed archive app."""
    transaction._validate_journal()
    if source_head != transaction.journal["sourceHead"]:
        raise Task8Error("sandbox attestation source HEAD differs")
    transaction_id = transaction.journal["transactionId"]
    root = transaction.repo_root
    probe = parse_sandbox_probe_event(
        SANDBOX_EVENT_PREFIX + rules.canonical_json_bytes(probe).decode("utf-8"),
        transaction_id, source_head)
    app, _, info_path, _, _ = _validate_verified_sandbox_app(
        transaction, verified)
    bundle = verified.bundle_identifier
    if probe["bundleIdentifier"] != bundle:
        raise Task8Error("signed app/probe bundle identity differs")
    data_root = Path(probe["containerDataRoot"])
    reports_root = Path(probe["automationReportsRoot"])
    container_root = data_root.parent
    relative_reports = reports_root.relative_to(data_root)
    chain_paths = _sandbox_directory_paths(
        container_root, data_root, reports_root)
    metadata_path = container_root / ".com.apple.containermanagerd.metadata.plist"
    held_directories: list[tuple[AttestedDirectoryComponent, int]] = []
    metadata_descriptor = -1
    try:
        held_directories = _open_attested_directory_chain(
            chain_paths, status="FAILED")
        metadata_descriptor, metadata_info = _open_regular_at(
            held_directories[0][1], metadata_path.name, status="FAILED")
        metadata_payload = _read_descriptor(metadata_descriptor)
        metadata_after = os.fstat(metadata_descriptor)
        metadata_entry = _entry_stat_at(
            held_directories[0][1], metadata_path.name, status="FAILED")
        metadata_fingerprint = _regular_fingerprint(metadata_info)
        if (metadata_entry is None or
                _regular_fingerprint(metadata_after) != metadata_fingerprint or
                not _same_entry(metadata_after, metadata_entry)):
            raise Task8Error("application-container metadata changed while binding")
        try:
            metadata = plistlib.loads(metadata_payload)
        except (ValueError, plistlib.InvalidFileException) as exc:
            raise Task8Error(
                f"application-container metadata is invalid: {exc}") from exc
        if (type(metadata) is not dict or
                metadata.get("MCMMetadataIdentifier") != bundle or
                metadata.get("MCMMetadataCreator") != bundle):
            raise Task8Error("application-container metadata identity differs")
        _revalidate_attested_directory_chain(
            held_directories, status="FAILED")
    except OSError as exc:
        raise Task8Error(
            "cannot read application-container metadata") from exc
    finally:
        if metadata_descriptor >= 0:
            os.close(metadata_descriptor)
        for _, descriptor in reversed(held_directories):
            os.close(descriptor)
    package_reports = evidence_root(root, transaction_id) / "package"
    if not package_reports.exists():
        _mkdir_exclusive(package_reports, 0o700)
    else:
        info = _physical_directory(package_reports)
        if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise Task8Error("package evidence root ownership/mode differs")
    entitlements_path = package_reports / "app-entitlements.plist"
    attestation_path = package_reports / "app-sandbox.json"
    package_descriptor, package_info = _open_pinned_directory(
        package_reports, status="FAILED")
    package_identity = [
        int(package_info.st_dev), int(package_info.st_ino),
        int(package_info.st_uid), stat.S_IMODE(package_info.st_mode),
    ]
    published: list[tuple[Path, bytes, tuple[int, ...]]] = []

    def publish_attestation_file(path: Path, payload: bytes) -> dict[str, Any]:
        _revalidate_pinned_directory_path(
            package_reports, package_descriptor, package_identity)
        _write_exclusive(path, payload, 0o600)
        descriptor, opened = _verify_regular_at(
            package_descriptor,
            path.name,
            expected_payload=payload,
            expected_sha256=hashlib.sha256(payload).hexdigest(),
            expected_size=len(payload),
            expected_mode=0o600,
        )
        try:
            fingerprint = _regular_fingerprint(opened)
        finally:
            os.close(descriptor)
        published.append((path, payload, fingerprint))
        _revalidate_pinned_directory_path(
            package_reports, package_descriptor, package_identity)
        os.fsync(package_descriptor)
        return {
            "path": path.relative_to(root).as_posix(),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "sizeBytes": len(payload),
        }

    try:
        if (_entry_stat_at(
                package_descriptor, entitlements_path.name,
                status="FAILED") is not None or
                _entry_stat_at(
                    package_descriptor, attestation_path.name,
                    status="FAILED") is not None):
            raise Task8Error("sandbox attestation evidence already exists")
        entitlements_payload = verified.canonical_entitlements
        entitlements_evidence = publish_attestation_file(
            entitlements_path, entitlements_payload)
        attestation = {
            "schemaVersion": 1,
            "reportType": "garner-terrain-app-sandbox",
            "status": "PASS",
            "transactionId": transaction_id,
            "sourceHead": source_head,
            "appBundlePath": app.relative_to(root).as_posix(),
            "bundleIdentifier": bundle,
            "signingIdentifier": verified.signing_identifier,
            "infoPlist": _evidence(info_path, root),
            "entitlementsPlist": entitlements_evidence,
            "codesignVerified": True,
            "appSandbox": True,
            "containerDataLeaf": data_root.name,
            "automationReportsRelativePath": relative_reports.as_posix(),
            "containerMetadataIdentifier": metadata["MCMMetadataIdentifier"],
            "containerMetadataCreator": metadata["MCMMetadataCreator"],
            "issues": [],
        }
        issues = rules.validate_sandbox_attestation(attestation, root)
        if issues:
            raise Task8Error(
                f"sandbox attestation self-validation failed: {issues[0]}")
        attestation_payload = rules.canonical_json_bytes(attestation) + b"\n"
        attestation_evidence = publish_attestation_file(
            attestation_path, attestation_payload)
        if _load_report(attestation_path) != attestation:
            raise Task8Error("sandbox attestation durable readback differs")
        environment = AttestedSandboxEnvironment(
            transaction_id=transaction_id,
            source_head=source_head,
            bundle_identifier=bundle,
            container_root=container_root,
            container_root_identity=held_directories[0][0].identity,
            container_data_root=data_root,
            container_data_identity=held_directories[1][0].identity,
            automation_reports_root=reports_root,
            automation_reports_identity=held_directories[-1][0].identity,
            directory_chain=tuple(
                component for component, _ in held_directories),
            metadata_path=metadata_path,
            metadata_fingerprint=metadata_fingerprint,
            metadata_sha256=hashlib.sha256(metadata_payload).hexdigest(),
            attestation_evidence=attestation_evidence,
        )
        _validate_attested_sandbox_environment(transaction, environment)
        return environment
    except SimulatedCrash:
        raise
    except BaseException as exc:
        try:
            _cleanup_partial_sandbox_attestation(
                package_reports, package_descriptor, package_identity, published)
        except Task8Error as cleanup_error:
            raise cleanup_error from exc
        raise
    finally:
        os.close(package_descriptor)


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
        extract_command, extracted = _unrealpak_extract_plan(
            unrealpak, physical, member_root, member_path)
        result = transaction.run_owned_command(extract_command)
        if result.returncode != 0:
            raise Task8Error(f"UnrealPak extraction failed: {result.stderr.strip()}")
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
    return {
        "cookReport": cook_report,
        "cookReportPath": cook_report_path,
        "packagedExecutable": executable_evidence,
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


def finalize_sandbox_package_evidence(
    transaction: OuterTransaction,
    prepared: dict[str, Any],
    sandbox_attestation: dict[str, Any],
) -> dict[str, Any]:
    """Publish strict cook/package wrappers only after sandbox attestation."""
    if type(prepared) is not dict or set(prepared) != {
            "cookReport", "cookReportPath", "packagedExecutable", "package"}:
        raise Task8Error("prepared package evidence shape differs")
    cook_report = json.loads(json.dumps(prepared["cookReport"]))
    package = json.loads(json.dumps(prepared["package"]))
    if "sandboxAttestation" in cook_report or "sandboxAttestation" in package:
        raise Task8Error("sandbox attestation was already finalized")
    cook_report["sandboxAttestation"] = sandbox_attestation
    package["sandboxAttestation"] = sandbox_attestation
    cook_path = prepared["cookReportPath"]
    if not isinstance(cook_path, Path):
        raise Task8Error("cook report path is malformed")
    if cook_path.exists() or cook_path.is_symlink():
        raise Task8Error("cook report already exists before sandbox finalization")
    issues = rules.validate_cook_package_report(cook_report, transaction.repo_root)
    if issues:
        raise Task8Error(f"cook-package report self-validation failed: {issues[0]}")
    _write_exclusive(
        cook_path, rules.canonical_json_bytes(cook_report) + b"\n", 0o600)
    _sync_directory(cook_path.parent)
    if _load_report(cook_path) != cook_report:
        raise Task8Error("cook-package report durable readback differs")
    return {
        "cookReport": cook_report,
        "cookReportPath": cook_path,
        "cookReportEvidence": _evidence(cook_path, transaction.repo_root),
        "packagedExecutable": prepared["packagedExecutable"],
        "package": package,
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
        context["verifiedSandboxApp"] = verify_signed_sandbox_app(
            transaction, source_head,
            context["preparedPackage"]["packagedExecutable"])
    elif command.name == "runtime-report-probe":
        probe = parse_sandbox_probe_event(
            result.stdout + "\n" + result.stderr, transaction_id, source_head)
        sandbox_environment = prepare_sandbox_attestation(
            transaction, source_head, context["verifiedSandboxApp"], probe)
        context["preparedPackage"] = finalize_sandbox_package_evidence(
            transaction, context["preparedPackage"],
            sandbox_environment.attestation_evidence)
        context["sandboxProbe"] = probe
        context["sandboxEnvironment"] = sandbox_environment
        context["sandboxScratch"] = prepare_sandbox_report_scratch(
            transaction, sandbox_environment)
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
        context["runtimeReportDirectory"] = copy_sandbox_runtime_reports(transaction)
        context["runtimeAutomation"] = _evidence_set(
            reports / "reference-terrain-runtime",
            root, transaction_id, source_head,
            expected_directory=context["runtimeReportDirectory"])
        context["package"] = finalize_package_evidence(
            context["preparedPackage"], observation)
        cleanup_sandbox_report_scratch(transaction)
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
                command, root, context["verifiedSandboxApp"],
                context.get("sandboxScratch"), transaction=transaction)
            if command.name in ("runtime-report-probe", "runtime-smoke") else command)
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
    except SimulatedCrash:
        transaction.close()
        raise
    except BaseException as exc:
        try:
            if transaction.lock_descriptor >= 0 and transaction.journal:
                cleanup_sandbox_report_scratch(transaction)
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
