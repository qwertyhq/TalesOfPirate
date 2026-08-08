import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest

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
            command, self.repo.root, executable)
        self.assertNotIn(build.ARCHIVE_EXECUTABLE_TOKEN, resolved.argv)
        self.assertEqual(
            resolved.argv[3],
            str(self.repo.root / executable["path"]))

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
