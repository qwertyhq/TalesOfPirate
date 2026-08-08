import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock

from scripts import build_garner_reference_terrain as build


TXN = "a" * 32
HEAD = "b" * 40
HEALTHY_THERMAL = "\n".join((
    "Note: No thermal warning level has been recorded",
    "Note: No performance warning level has been recorded",
    "Note: No CPU power status has been recorded",
))


class FakeRepo:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".corsairs-task8-orchestrator-", dir=Path.cwd())
        self.root = Path(self.temporary.name)
        (self.root / "artifacts/maps/reports").mkdir(parents=True)
        (self.root / "artifacts/maps").mkdir(parents=True, exist_ok=True)
        (self.root / "CorsairsUE/Data/Heights").mkdir(parents=True)
        for stem in build.MANAGED_PACKAGE_STEMS.values():
            path = self.root / stem
            path.parent.mkdir(parents=True, exist_ok=True)

    def close(self):
        self.temporary.cleanup()

    def write(self, relative, data, mode=0o640):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        return path


class OrchestratorContractTests(unittest.TestCase):
    def setUp(self):
        self.repo = FakeRepo()
        self.addCleanup(self.repo.close)

    def test_thermal_warning_or_pmset_failure_launches_no_heavy_command(self):
        for returncode, output in (
            (1, HEALTHY_THERMAL),
            (0, "CPU Power notify"),
            (0, HEALTHY_THERMAL.replace(
                "No CPU power status has been recorded", "")),
        ):
            with self.subTest(returncode=returncode, output=output):
                with self.assertRaises(build.Task8Error):
                    build.require_healthy_thermal(returncode, output)
        build.require_healthy_thermal(0, HEALTHY_THERMAL)

    def test_preexisting_process_is_refused_and_never_killed(self):
        rows = [
            build.ProcessRecord(20, 20, "token", "/Engine/UnrealEditor-Cmd", ("x",)),
            build.ProcessRecord(21, 21, "token", "/usr/bin/rg", ("rg", "UnrealEditor")),
        ]
        self.assertEqual(build.competing_processes(rows), [rows[0]])
        killed = []
        with self.assertRaises(build.Task8Error):
            build.refuse_competing_processes(rows, killed.append)
        self.assertEqual(killed, [])

    def test_all_heavy_commands_are_sequential_nice_and_capped_at_two(self):
        commands = build.production_commands(self.repo.root, TXN, HEAD)
        names = [item.name for item in commands]
        self.assertEqual(names, [
            "terrain-reference", "installer-1", "installer-2", "editor-build",
            "level-build", "import-1", "import-2", "checker",
            "editor-automation", "game-build", "cook-package", "runtime-smoke",
        ])
        self.assertLess(names.index("installer-2"), names.index("game-build"))
        self.assertLess(names.index("installer-2"), names.index("cook-package"))
        for command in commands:
            self.assertEqual(command.argv[:3], ("nice", "-n", "10"))
            self.assertNotIn("Game.exe", " ".join(command.argv))
            self.assertNotIn("CrossOver", " ".join(command.argv))
            self.assertNotIn("wine", " ".join(command.argv).lower())
        self.assertIn("-MaxParallelActions=2", commands[3].argv)
        self.assertIn("-MaxParallelActions=2", commands[9].argv)
        self.assertIn("-MaxParallelActions=2", commands[10].argv)
        self.assertEqual(commands[1].argv, commands[2].argv)

    def test_runtime_command_uses_receipt_inventory_executable(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        owned = build.prepare_runtime_report_directory(transaction)
        command = build.production_commands(self.repo.root, TXN, HEAD)[-1]
        self.assertIn(build.ARCHIVE_EXECUTABLE_TOKEN, command.argv)
        executable = {
            "path": (
                f"artifacts/maps/package-run/{TXN}/archive/Mac/Actual.app/"
                "Contents/MacOS/ActualLaunch"),
            "sha256": "c" * 64,
            "sizeBytes": 1,
        }
        resolved = build.resolve_runtime_command(
            command, self.repo.root, executable, owned)
        self.assertNotIn(build.ARCHIVE_EXECUTABLE_TOKEN, resolved.argv)
        self.assertEqual(
            resolved.argv[3],
            str(self.repo.root / executable["path"]))

    def test_runtime_report_directory_is_exact_owned_empty_and_hashed(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        owned = build.prepare_runtime_report_directory(transaction)
        expected = (
            build.evidence_root(self.repo.root, TXN) /
            "reference-terrain-runtime")
        self.assertEqual(owned.path, expected)
        self.assertEqual(owned.device, expected.lstat().st_dev)
        self.assertEqual(owned.inode, expected.lstat().st_ino)
        self.assertEqual(owned.owner_uid, os.geteuid())
        self.assertEqual(stat.S_IMODE(expected.lstat().st_mode), 0o700)
        self.assertEqual(tuple(expected.iterdir()), ())

        command = build.production_commands(self.repo.root, TXN, HEAD)[-1]
        executable = {
            "path": (
                f"artifacts/maps/package-run/{TXN}/archive/Mac/Actual.app/"
                "Contents/MacOS/ActualLaunch"),
            "sha256": "c" * 64,
            "sizeBytes": 1,
        }
        resolved = build.resolve_runtime_command(
            command, self.repo.root, executable, owned)
        self.assertEqual(
            tuple(item for item in resolved.argv
                  if item.startswith("-ReportExportPath=")),
            (f"-ReportExportPath={expected}",),
        )

        index = expected / "index.json"
        index.write_bytes(b'{"succeeded":1}\n')
        evidence = build._evidence_set(
            expected, self.repo.root, TXN, HEAD,
            expected_directory=owned)
        self.assertEqual(evidence["files"], [{
            "path": index.relative_to(self.repo.root).as_posix(),
            "sha256": hashlib.sha256(index.read_bytes()).hexdigest(),
            "sizeBytes": index.stat().st_size,
        }])

        transaction.rollback()
        transaction.finish()
        self.assertEqual(index.read_bytes(), b'{"succeeded":1}\n')
        self.assertFalse(build.recovery_root(self.repo.root, TXN).exists())

    def test_runtime_report_directory_rejects_foreign_or_changed_leaf(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        path = (
            build.evidence_root(self.repo.root, TXN) /
            "reference-terrain-runtime")
        symlink_target = build.evidence_root(self.repo.root, TXN) / "foreign"
        symlink_target.mkdir()

        cases = ("empty-directory", "nonempty-directory", "regular", "symlink")
        for case in cases:
            with self.subTest(case=case):
                if case == "empty-directory":
                    path.mkdir()
                elif case == "nonempty-directory":
                    path.mkdir()
                    (path / "foreign.txt").write_bytes(b"foreign")
                elif case == "regular":
                    path.write_bytes(b"foreign")
                else:
                    path.symlink_to(symlink_target, target_is_directory=True)
                try:
                    with self.assertRaises(build.Task8Error):
                        build.prepare_runtime_report_directory(transaction)
                finally:
                    if case == "nonempty-directory":
                        self.assertEqual(
                            (path / "foreign.txt").read_bytes(), b"foreign")
                        (path / "foreign.txt").unlink()
                        path.rmdir()
                    elif case == "empty-directory":
                        self.assertTrue(path.is_dir())
                        path.rmdir()
                    elif case == "regular":
                        self.assertEqual(path.read_bytes(), b"foreign")
                        path.unlink()
                    else:
                        self.assertTrue(path.is_symlink())
                        path.unlink()

        owned = build.prepare_runtime_report_directory(transaction)
        command = build.production_commands(self.repo.root, TXN, HEAD)[-1]
        executable = {
            "path": (
                f"artifacts/maps/package-run/{TXN}/archive/Mac/Actual.app/"
                "Contents/MacOS/ActualLaunch"),
            "sha256": "c" * 64,
            "sizeBytes": 1,
        }
        (path / "unexpected").write_bytes(b"not-empty")
        with self.assertRaises(build.Task8Error):
            build.resolve_runtime_command(
                command, self.repo.root, executable, owned)
        (path / "unexpected").unlink()

        path.chmod(0o755)
        with self.assertRaises(build.Task8Error):
            build.resolve_runtime_command(
                command, self.repo.root, executable, owned)
        path.chmod(0o700)

        (path / "not-index.json").write_bytes(b"wrong report leaf")
        with self.assertRaises(build.Task8Error):
            build._evidence_set(
                path, self.repo.root, TXN, HEAD,
                expected_directory=owned)
        (path / "not-index.json").unlink()

        retired = path.with_name(path.name + ".retired-for-test")
        path.rename(retired)
        path.mkdir(mode=0o700)
        with self.assertRaises(build.Task8Error):
            build.resolve_runtime_command(
                command, self.repo.root, executable, owned)

    def test_cook_evidence_prepares_runtime_report_directory_before_runtime(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        context = {"gameBuild": {"target": "game"}}
        order = []
        owned = object()

        def prepare_package(*_args):
            order.append("package-evidence")
            return {"package": "prepared"}

        def prepare_report(*_args):
            order.append("runtime-report-directory")
            return owned

        with mock.patch.object(
                build, "prepare_package_evidence", side_effect=prepare_package), \
                mock.patch.object(
                    build, "prepare_runtime_report_directory",
                    side_effect=prepare_report):
            build._production_after_step(
                transaction, context, HEAD,
                build.CommandSpec("cook-package", ("unused",)),
                build.CommandResult(0))

        self.assertEqual(order, ["package-evidence", "runtime-report-directory"])
        self.assertIs(context["runtimeReportDirectory"], owned)

    def test_runtime_evidence_rejects_unreadable_nested_directory(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        owned = build.prepare_runtime_report_directory(transaction)
        (owned.path / "index.json").write_bytes(b'{"succeeded":1}\n')
        hidden = owned.path / "hidden"
        hidden.mkdir(mode=0o700)
        (hidden / "hidden.json").write_bytes(b'{"hidden":true}\n')
        hidden.chmod(0)
        try:
            with self.assertRaises(build.Task8Error):
                build._evidence_set(
                    owned.path, self.repo.root, TXN, HEAD,
                    expected_directory=owned)
        finally:
            hidden.chmod(0o700)
        evidence = build._evidence_set(
            owned.path, self.repo.root, TXN, HEAD,
            expected_directory=owned)
        self.assertEqual(
            [item["path"] for item in evidence["files"]],
            sorted((
                (hidden / "hidden.json").relative_to(self.repo.root).as_posix(),
                (owned.path / "index.json").relative_to(
                    self.repo.root).as_posix(),
            )),
        )

    def test_cook_package_forwards_supported_skip_zen_switch(self):
        command = next(
            item for item in build.production_commands(self.repo.root, TXN, HEAD)
            if item.name == "cook-package")
        self.assertEqual(command.argv[4], "BuildCookRun")
        forwarding = tuple(
            item for item in command.argv
            if item.startswith("-AdditionalCookerOptions="))
        self.assertEqual(
            forwarding, ("-AdditionalCookerOptions=-SkipZenStore",))
        emitted_cook_switches = tuple(
            item.split("=", 1)[1] for item in forwarding)
        self.assertEqual(emitted_cook_switches, ("-SkipZenStore",))
        self.assertNotIn("-ZenStore", emitted_cook_switches)

    def test_cook_output_dir_ends_in_uat_platform_leaf(self):
        command = next(
            item for item in build.production_commands(self.repo.root, TXN, HEAD)
            if item.name == "cook-package")
        output_args = tuple(
            item for item in command.argv if item.startswith("-CookOutputDir="))
        expected = (
            "-CookOutputDir="
            f"{self.repo.root}/artifacts/maps/package-run/{TXN}/cooked/Mac")
        self.assertEqual(output_args, (expected,))
        self.assertEqual(Path(output_args[0].split("=", 1)[1]).name, "Mac")

    def test_cook_package_finalizes_app_before_archive(self):
        command = next(
            item for item in build.production_commands(self.repo.root, TXN, HEAD)
            if item.name == "cook-package")
        cook_index = command.argv.index("-cook")
        self.assertEqual(
            command.argv[cook_index:cook_index + 5],
            ("-cook", "-stage", "-pak", "-package", "-archive"))
        self.assertEqual(command.argv.count("-package"), 1)

    def test_unrealpak_list_normalizes_ue58_mount_relative_members(self):
        output = "\n".join((
            'LogPakFile: Display: Listing CorsairsUE-Mac.pak with mount point "../../../"',
            'LogPakFile: Display: "CorsairsUE/Data/Heights/garner.block.raw" '
            'offset: 137722, size: 3260865 bytes, sha1: ABC, compression: Oodle.',
            'LogPakFile: Display: "CorsairsUE/Data/Heights/garner.terrain.json" '
            'offset: 3415028, size: 286 bytes, sha1: DEF, compression: None.',
            'LogPakFile: Display: 2 files (3261151 bytes), (0 filtered bytes).',
        ))
        self.assertEqual(build.parse_unrealpak_list(output), [
            {
                "path": "../../../CorsairsUE/Data/Heights/garner.block.raw",
                "sizeBytes": 3260865,
            },
            {
                "path": "../../../CorsairsUE/Data/Heights/garner.terrain.json",
                "sizeBytes": 286,
            },
        ])

    def test_unrealpak_list_rejects_ambiguous_or_unsafe_paths(self):
        header = (
            'LogPakFile: Display: Listing CorsairsUE-Mac.pak with mount point '
            '"../../../"')
        member = (
            'LogPakFile: Display: "CorsairsUE/Data/Heights/garner.block.raw" '
            'offset: 0, size: 1 bytes, sha1: ABC, compression: None.')
        summary = (
            'LogPakFile: Display: 1 files (1 bytes), (0 filtered bytes).')
        cases = {
            "missing mount": "\n".join((member, summary)),
            "wrong mount": "\n".join((
                header.replace('../../../', '../../'), member, summary)),
            "multiple mounts": "\n".join((header, header, member, summary)),
            "duplicate member": "\n".join((
                header, member, member, summary.replace("1 files", "2 files"))),
            "absolute member": "\n".join((
                header, member.replace('"CorsairsUE/', '"/CorsairsUE/'),
                summary)),
            "traversal member": "\n".join((
                header, member.replace("Data/Heights", "Data/../Heights"),
                summary)),
        }
        for name, output in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(build.Task8Error):
                    build.parse_unrealpak_list(output)

    def test_unrealpak_list_requires_one_matching_terminal_summary(self):
        header = (
            'LogPakFile: Display: Listing CorsairsUE-Mac.pak with mount point '
            '"../../../"')
        member = (
            'LogPakFile: Display: "CorsairsUE/Data/Heights/garner.block.raw" '
            'offset: 0, size: 1 bytes, sha1: ABC, compression: None.')
        summary = (
            'LogPakFile: Display: 1 files (1 bytes), (0 filtered bytes).')
        cases = {
            "missing": "\n".join((header, member)),
            "mismatched count": "\n".join((
                header, member, summary.replace("1 files", "2 files"))),
            "multiple": "\n".join((header, member, summary, summary)),
            "nonterminal": "\n".join((header, summary, member)),
        }
        for name, output in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(build.Task8Error):
                    build.parse_unrealpak_list(output)

    def test_normalized_relative_rejects_bare_dot_and_empty_components(self):
        self.assertFalse(build._normalized_relative("."))
        self.assertFalse(build._normalized_relative(""))
        self.assertFalse(build._normalized_relative("CorsairsUE//Data/file"))
        self.assertFalse(build._normalized_relative("CorsairsUE/Data/file/"))
        self.assertTrue(build._normalized_relative("CorsairsUE/Data/file"))

    def test_unrealpak_extract_uses_ue58_positional_output_and_relative_filter(self):
        unrealpak = Path("/Engine/Binaries/Mac/UnrealPak")
        container = Path("/archive/CorsairsUE-Mac.pak")
        output = Path("/evidence/extracted/member")
        member = "../../../CorsairsUE/Data/Heights/garner.block.raw"
        command, extracted = build._unrealpak_extract_plan(
            unrealpak, container, output, member)
        self.assertEqual(command.name, "cook-package")
        self.assertEqual(command.argv, (
            "nice", "-n", "10", str(unrealpak), str(container),
            "-Extract", str(output),
            "-Filter=CorsairsUE/Data/Heights/garner.block.raw",
        ))
        self.assertEqual(command.argv.count("-Extract"), 1)
        self.assertEqual(sum(
            item.startswith("-Filter=") for item in command.argv), 1)
        self.assertEqual(
            extracted,
            output / "CorsairsUE/Data/Heights/garner.block.raw")
        for invalid in (
            "CorsairsUE/Data/Heights/garner.block.raw",
            "../../../",
            "../../../../CorsairsUE/Data/Heights/garner.block.raw",
            "../../../CorsairsUE/Data/../Heights/garner.block.raw",
            "../../../CorsairsUE//Data/Heights/garner.block.raw",
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(build.Task8Error):
                    build._unrealpak_extract_plan(
                        unrealpak, container, output, invalid)

    def test_clean_checkout_orders_installer_before_game_build_and_cook(self):
        seen = []

        def status_gate():
            seen.append("clean")
            return HEAD, ""

        commands = build.preflight_plan(
            self.repo.root, TXN, status_gate=status_gate,
            process_probe=lambda: [], thermal_probe=lambda: (0, HEALTHY_THERMAL))
        self.assertEqual(seen, ["clean"])
        names = [item.name for item in commands]
        self.assertLess(names.index("installer-2"), names.index("game-build"))

    def test_dirty_or_wrong_head_checkout_fails_before_mutation(self):
        sentinel = self.repo.write("sentinel", b"unchanged")
        for head, status in ((HEAD, " M tracked"), ("not-a-head", "")):
            with self.subTest(head=head, status=status):
                with self.assertRaises(build.Task8Error):
                    build.preflight_plan(
                        self.repo.root, TXN,
                        status_gate=lambda: (head, status),
                        process_probe=lambda: [],
                        thermal_probe=lambda: (0, HEALTHY_THERMAL))
                self.assertEqual(sentinel.read_bytes(), b"unchanged")
                self.assertFalse(build.journal_path(self.repo.root).exists())

    def test_snapshot_includes_full_package_families_and_modes(self):
        primary = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["texture"] + ".uasset", b"texture", 0o444)
        sidecar = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["texture"] + ".ubulk", b"bulk", 0o600)
        top = self.repo.write(build.TOP_MANIFEST, b"manifest", 0o640)
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        paths = {item["path"]: item for item in transaction.journal["snapshots"]}
        for path in (primary, sidecar, top):
            relative = path.relative_to(self.repo.root).as_posix()
            self.assertIn(relative, paths)
            self.assertEqual(paths[relative]["priorSha256"],
                             hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(paths[relative]["priorMode"],
                             stat.S_IMODE(path.stat().st_mode))
        self.assertEqual(
            transaction.journal["managedFamilyListings"]["texture"],
            sorted((primary.relative_to(self.repo.root).as_posix(),
                    sidecar.relative_to(self.repo.root).as_posix())))

    def test_failure_after_unreal_mutation_restores_package_bytes(self):
        primary = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["mesh"] + ".uasset", b"old", 0o444)
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        replacement = primary.with_name("replacement.uasset")
        replacement.write_bytes(b"new")
        replacement.chmod(0o600)
        os.replace(replacement, primary)
        introduced = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["mesh"] + ".uexp", b"new-sidecar")
        transaction.rollback()
        self.assertEqual(primary.read_bytes(), b"old")
        self.assertEqual(stat.S_IMODE(primary.stat().st_mode), 0o444)
        self.assertFalse(introduced.exists())
        self.assertEqual(transaction.journal["phase"], "ROLLED_BACK")
        transaction.close()

    def test_crash_recovers_on_fresh_invocation(self):
        bundle = self.repo.write(build.BASE_BUNDLE, b"old-bundle", 0o600)
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        bundle.write_bytes(b"partial-new")
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD)
        self.assertEqual(bundle.read_bytes(), b"old-bundle")
        self.assertEqual(recovered.journal["phase"], "ROLLED_BACK")

    def test_base_bundle_publish_failure_preserves_previous_bytes(self):
        bundle = self.repo.write(build.BASE_BUNDLE, b"old-bundle", 0o640)
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        with self.assertRaises(build.Task8Error):
            transaction.publish_bundle(
                {"schemaVersion": 1},
                validate=lambda _data, _path: [],
                fault=lambda point: "fail" if point == "AFTER_BUNDLE_REPLACE" else None)
        self.assertEqual(bundle.read_bytes(), b"old-bundle")
        transaction.close()

    def test_success_removes_recovery_journal_but_keeps_lock_marker(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        transaction.journal["phase"] = "COMMITTED"
        transaction.persist()
        transaction.finish()
        self.assertFalse(build.journal_path(self.repo.root).exists())
        self.assertFalse(build.recovery_root(self.repo.root, TXN).exists())
        self.assertTrue(build.lock_path(self.repo.root).is_file())
        self.assertEqual(
            build.lock_path(self.repo.root).read_bytes(), build.LOCK_MARKER)

    def test_owned_process_handshake_is_durable_and_cleared_after_wait(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        command = build.CommandSpec(
            "terrain-reference",
            ("nice", "-n", "10", sys.executable, "-c", "print('owned-ok')"),
            (("PYTHONDONTWRITEBYTECODE", "1"),), timeout_seconds=10)
        result = transaction.run_owned_command(command)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "owned-ok")
        self.assertIsNone(transaction.journal["activeProcess"])
        self.assertEqual(transaction.journal["completedStep"], "terrain-reference")
        persisted = json.loads(build.journal_path(self.repo.root).read_text())
        self.assertIsNone(persisted["activeProcess"])

    def test_crash_after_active_process_handshake_recovers_fresh(self):
        target = self.repo.write(build.BASE_BUNDLE, b"old")
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        command = build.CommandSpec(
            "terrain-reference",
            ("nice", "-n", "10", sys.executable, "-c", "print('never')"),
            timeout_seconds=10)
        with self.assertRaises(build.Task8Error):
            transaction.run_owned_command(
                command,
                fault=lambda point: (
                    "crash" if point == "AFTER_ACTIVE_PROCESS_DURABLE" else None))
        self.assertIsNotNone(transaction.journal["activeProcess"])
        target.write_bytes(b"changed")
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD)
        self.assertEqual(target.read_bytes(), b"old")
        self.assertIsNone(recovered.journal["activeProcess"])

    def test_outer_recovery_resolves_publisher_before_installer(self):
        target = self.repo.write(build.BASE_BUNDLE, b"old")
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        target.write_bytes(b"new")
        for owner in ("publisher", "installer"):
            first = build.inner_control_paths(self.repo.root, owner)[0]
            first.parent.mkdir(parents=True, exist_ok=True)
            first.write_bytes(owner.encode())
        transaction.abandon_for_test()

        order = []

        def retry(owner, paths):
            order.append(("retry", owner))
            for path in paths:
                if path.exists():
                    path.unlink()

        def validate(owner):
            order.append(("validate", owner))

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(
            HEAD, nested_retry=retry, nested_validate=validate)
        self.assertEqual(order, [
            ("retry", "publisher"), ("validate", "publisher"),
            ("retry", "installer"), ("validate", "installer"),
        ])
        self.assertEqual(target.read_bytes(), b"old")

    def test_failed_inner_recovery_preserves_outer_snapshot(self):
        target = self.repo.write(build.BASE_BUNDLE, b"old")
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        target.write_bytes(b"new")
        control = build.inner_control_paths(self.repo.root, "publisher")[0]
        control.parent.mkdir(parents=True, exist_ok=True)
        control.write_bytes(b"publisher")
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        with self.assertRaises(build.Task8Error):
            recovered.recover(
                HEAD,
                nested_retry=lambda _owner, _paths: (_ for _ in ()).throw(
                    build.Task8Error("inner failed")),
                nested_validate=lambda _owner: None)
        self.assertEqual(target.read_bytes(), b"new")
        self.assertTrue(control.exists())
        self.assertTrue(build.journal_path(self.repo.root).exists())


if __name__ == "__main__":
    unittest.main()
