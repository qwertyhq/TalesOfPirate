import hashlib
import json
import os
from dataclasses import dataclass, replace
from pathlib import Path
import plistlib
import signal
import socket
import stat
import sys
import tempfile
import time
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


@dataclass(frozen=True)
class SandboxFaultCase:
    point: str
    expected_state: str
    occurrence: int = 1


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

    def _sandbox_probe(self, bundle_identifier="com.example.CorsairsUE"):
        container = self.repo.root / "SandboxContainer"
        data_root = container / "Data"
        reports_root = (
            data_root /
            "Library/Application Support/Epic/CorsairsUE/Saved/Automation/Reports")
        reports_root.mkdir(parents=True, mode=0o700)
        for path in (
                container, data_root, data_root / "Library",
                data_root / "Library/Application Support",
                data_root / "Library/Application Support/Epic",
                data_root / "Library/Application Support/Epic/CorsairsUE",
                data_root / "Library/Application Support/Epic/CorsairsUE/Saved",
                data_root /
                "Library/Application Support/Epic/CorsairsUE/Saved/Automation",
                reports_root):
            path.chmod(0o700)
        return {
            "automationReportsRoot": str(reports_root),
            "bundleIdentifier": bundle_identifier,
            "containerDataRoot": str(data_root),
            "issues": [],
            "reportType": "garner-terrain-packaged-sandbox",
            "schemaVersion": 1,
            "sourceHead": HEAD,
            "status": "PASS",
            "transactionId": TXN,
        }

    def _verified_sandbox_app(self, bundle_identifier="com.example.CorsairsUE"):
        app = (
            build.package_run_root(self.repo.root, TXN) /
            "archive/Mac/Actual.app")
        executable_path = app / "Contents/MacOS/ActualLaunch"
        executable_path.parent.mkdir(parents=True, exist_ok=True)
        executable_path.write_bytes(b"binary")
        executable_path.chmod(0o755)
        info_path = app / "Contents/Info.plist"
        info_path.write_bytes(plistlib.dumps({
            "CFBundleIdentifier": bundle_identifier,
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        entitlements = plistlib.dumps({
            "com.apple.security.app-sandbox": True,
        }, fmt=plistlib.FMT_XML, sort_keys=True)
        return build.VerifiedSandboxApp(
            app_bundle_path=app.relative_to(self.repo.root).as_posix(),
            bundle_identifier=bundle_identifier,
            signing_identifier=bundle_identifier,
            packaged_executable=build._evidence(executable_path, self.repo.root),
            info_plist=build._evidence(info_path, self.repo.root),
            canonical_entitlements=entitlements,
        )

    def _sandbox_component_paths(self, probe):
        data_root = Path(probe["containerDataRoot"])
        reports_root = Path(probe["automationReportsRoot"])
        paths = [data_root.parent, data_root]
        current = data_root
        for component in reports_root.relative_to(data_root).parts:
            current = current / component
            paths.append(current)
        return tuple(paths)

    def _attested_sandbox_environment(
            self, transaction, bundle_identifier="com.example.CorsairsUE"):
        probe = self._sandbox_probe(bundle_identifier)
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        binding = build.prepare_sandbox_attestation(
            transaction, HEAD,
            self._verified_sandbox_app(bundle_identifier), probe)
        self.assertIsInstance(binding, build.AttestedSandboxEnvironment)
        return binding

    def _fault_for_case(self, case, action):
        occurrences = 0

        def inject(point):
            nonlocal occurrences
            if point == case.point:
                occurrences += 1
                if occurrences == case.occurrence:
                    return action
            return None

        return inject, lambda: occurrences

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

    def test_command_resolution_occurs_only_after_process_and_thermal_gates(self):
        command = build.CommandSpec("terrain-reference", ("nice", "-n", "10"))
        competing = build.ProcessRecord(
            20, 20, "token", "/Engine/UnrealEditor-Cmd", ("UnrealEditor",))
        for name, process_rows, thermal in (
            ("process", [competing], (0, HEALTHY_THERMAL)),
            ("thermal", [], (1, HEALTHY_THERMAL)),
        ):
            with self.subTest(failure=name):
                events = []
                with self.assertRaises(build.Task8Error):
                    build.execute_command_plan(
                        mock.Mock(),
                        [command],
                        process_probe=lambda: events.append("process") or process_rows,
                        thermal_probe=lambda: events.append("thermal") or thermal,
                        resolve_command=lambda value: events.append("resolve") or value,
                        runner=lambda _value: events.append("invoke") or
                        build.CommandResult(0),
                    )
                self.assertNotIn("resolve", events)
                self.assertNotIn("invoke", events)

        events = []
        build.execute_command_plan(
            mock.Mock(),
            [command],
            process_probe=lambda: events.append("process") or [],
            thermal_probe=lambda: events.append("thermal") or (0, HEALTHY_THERMAL),
            resolve_command=lambda value: events.append("resolve") or value,
            runner=lambda _value: events.append("invoke") or build.CommandResult(0),
        )
        self.assertEqual(events, ["process", "thermal", "resolve", "invoke"])

    def test_run_task8_simulated_crash_abandons_without_in_process_rollback(self):
        planned = (
            build.CommandSpec("terrain-reference", ("nice", "-n", "10")),)
        with mock.patch.object(
                build, "git_status_gate", return_value=(HEAD, "")), \
                mock.patch.object(
                    build, "preflight_plan", return_value=planned), \
                mock.patch.object(
                    build, "execute_command_plan",
                    side_effect=build.SimulatedCrash("injected crash")):
            with self.assertRaises(build.SimulatedCrash):
                build.run_task8(
                    self.repo.root,
                    build.TOP_MANIFEST,
                    build.rules.REFERENCE_MAP_PACKAGE,
                    self.repo.root / "artifacts/maps/reports")
        durable = build._strict_json(build.journal_path(self.repo.root))
        self.assertEqual(durable["phase"], "SNAPSHOT")
        self.assertEqual(durable["completedStep"], "snapshot")
        self.assertIsNone(durable["sandboxScratch"])

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD)
        self.assertEqual(recovered.journal["phase"], "ROLLED_BACK")

    def test_all_heavy_commands_are_sequential_nice_and_capped_at_two(self):
        commands = build.production_commands(self.repo.root, TXN, HEAD)
        names = [item.name for item in commands]
        self.assertEqual(names, [
            "terrain-reference", "installer-1", "installer-2", "editor-build",
            "level-build", "import-1", "import-2", "checker",
            "editor-automation", "game-build", "cook-package",
            "runtime-report-probe", "runtime-smoke",
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

    def test_sandbox_probe_precedes_runtime_and_runtime_has_no_repo_report_path(self):
        commands = build.production_commands(self.repo.root, TXN, HEAD)
        names = [item.name for item in commands]
        self.assertEqual(names[-2:], ["runtime-report-probe", "runtime-smoke"])
        probe, runtime = commands[-2:]
        self.assertIn(build.ARCHIVE_EXECUTABLE_TOKEN, probe.argv)
        self.assertIn("-CorsairsTerrainSandboxProbe", probe.argv)
        self.assertFalse(any(
            item.startswith("-ReportExportPath=") for item in probe.argv))
        self.assertIn(build.SANDBOX_REPORT_TOKEN, runtime.argv)
        self.assertNotIn(
            str(build.evidence_root(self.repo.root, TXN) /
                build.RUNTIME_AUTOMATION_DIRECTORY),
            "\n".join(runtime.argv),
        )

        data_root = self.repo.root / "container/Data"
        reports_root = data_root / "Library/Application Support/Epic/CorsairsUE/" \
            "Saved/Automation/Reports"
        event = {
            "automationReportsRoot": str(reports_root),
            "bundleIdentifier": "com.example.CorsairsUE",
            "containerDataRoot": str(data_root),
            "issues": [],
            "reportType": "garner-terrain-packaged-sandbox",
            "schemaVersion": 1,
            "sourceHead": HEAD,
            "status": "PASS",
            "transactionId": TXN,
        }
        payload = json.dumps(event, sort_keys=True, separators=(",", ":"))
        parsed = build.parse_sandbox_probe_event(
            f"prefix\n{build.SANDBOX_EVENT_PREFIX}{payload}\nsuffix\n", TXN, HEAD)
        self.assertEqual(parsed, event)
        with self.assertRaises(build.Task8Error):
            build.parse_sandbox_probe_event(
                f"{build.SANDBOX_EVENT_PREFIX}{payload}\n"
                "CORSAIRS_TERRAIN_RUNTIME_JSON={}\n",
                TXN, HEAD)
        single_component = {**event, "bundleIdentifier": "CorsairsUE"}
        self.assertEqual(
            build.parse_sandbox_probe_event(
                build.SANDBOX_EVENT_PREFIX + json.dumps(
                    single_component, sort_keys=True, separators=(",", ":")),
                TXN, HEAD),
            single_component,
        )
        for key in tuple(event):
            with self.subTest(probe_field=key):
                changed = dict(event)
                changed.pop(key)
                changed_payload = json.dumps(
                    changed, sort_keys=True, separators=(",", ":"))
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(
                        build.SANDBOX_EVENT_PREFIX + changed_payload, TXN, HEAD)
        for name, changed in (
            ("unknown", {**event, "unexpected": True}),
            ("duplicate", event),
            ("wrong-bundle", {**event, "bundleIdentifier": "not_a_bundle"}),
            ("wrong-data-leaf", {**event, "containerDataRoot": str(data_root.parent)}),
            ("outside-reports", {
                **event, "automationReportsRoot": str(data_root.parent / "Reports")}),
        ):
            with self.subTest(probe_mutation=name):
                changed_payload = json.dumps(
                    changed, sort_keys=True, separators=(",", ":"))
                output = build.SANDBOX_EVENT_PREFIX + changed_payload
                if name == "duplicate":
                    output += "\n" + output
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(output, TXN, HEAD)

    def test_sandbox_probe_rejects_boolean_and_float_schema_versions(self):
        event = self._sandbox_probe()
        for schema_version in (True, 1.0):
            with self.subTest(schema_version=repr(schema_version)):
                changed = {**event, "schemaVersion": schema_version}
                payload = json.dumps(
                    changed, sort_keys=True, separators=(",", ":"))
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(
                        build.SANDBOX_EVENT_PREFIX + payload, TXN, HEAD)

    def test_sandbox_probe_rejects_payload_whitespace_and_double_slash_root(self):
        event = self._sandbox_probe()
        payload = json.dumps(event, sort_keys=True, separators=(",", ":"))
        for name, output in (
            ("leading whitespace", build.SANDBOX_EVENT_PREFIX + " " + payload),
            ("trailing whitespace", build.SANDBOX_EVENT_PREFIX + payload + " "),
        ):
            with self.subTest(name=name):
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(output, TXN, HEAD)

        doubled = {
            **event,
            "containerDataRoot": "//tmp/CorsairsUE/Data",
            "automationReportsRoot": "//tmp/CorsairsUE/Data/Reports",
        }
        with self.assertRaises(build.Task8Error):
            build.parse_sandbox_probe_event(
                build.SANDBOX_EVENT_PREFIX + json.dumps(
                    doubled, sort_keys=True, separators=(",", ":")),
                TXN, HEAD)

    def test_sandbox_probe_nonfinite_json_is_canonical_task8_error(self):
        event = self._sandbox_probe()
        payload = json.dumps(
            {**event, "schemaVersion": float("nan")},
            sort_keys=True, separators=(",", ":"), allow_nan=True)
        with self.assertRaises(build.Task8Error):
            build.parse_sandbox_probe_event(
                build.SANDBOX_EVENT_PREFIX + payload, TXN, HEAD)

    def test_sandbox_probe_full_field_type_and_value_matrix(self):
        event = self._sandbox_probe()
        wrong_types = {
            "schemaVersion": "1",
            "reportType": 1,
            "status": [],
            "transactionId": {},
            "sourceHead": 40,
            "bundleIdentifier": [],
            "containerDataRoot": 3,
            "automationReportsRoot": False,
            "issues": {},
        }
        wrong_values = {
            "schemaVersion": 2,
            "reportType": "wrong-sandbox-report",
            "status": "FAIL",
            "transactionId": "c" * 32,
            "sourceHead": "c" * 40,
            "bundleIdentifier": "not_a_bundle",
            "containerDataRoot": str(
                Path(event["containerDataRoot"]).parent / "WrongLeaf"),
            "automationReportsRoot": event["containerDataRoot"],
            "issues": ["unexpected"],
        }
        for mutation, values in (("type", wrong_types), ("value", wrong_values)):
            for field, changed_value in values.items():
                with self.subTest(mutation=mutation, field=field):
                    changed = {**event, field: changed_value}
                    payload = json.dumps(
                        changed, sort_keys=True, separators=(",", ":"))
                    with self.assertRaises(build.Task8Error):
                        build.parse_sandbox_probe_event(
                            build.SANDBOX_EVENT_PREFIX + payload, TXN, HEAD)

    def test_sandbox_probe_strict_framing_matrix(self):
        event = self._sandbox_probe()
        canonical = json.dumps(event, sort_keys=True, separators=(",", ":"))
        reversed_payload = json.dumps(
            dict(reversed(tuple(event.items()))),
            sort_keys=False, separators=(",", ":"))
        duplicate_member = canonical[:-1] + ',"status":"PASS"}'
        cases = {
            "zero event": "ordinary log output",
            "duplicate event": "\n".join((
                build.SANDBOX_EVENT_PREFIX + canonical,
                build.SANDBOX_EVENT_PREFIX + canonical,
            )),
            "duplicate member": build.SANDBOX_EVENT_PREFIX + duplicate_member,
            "reordered members": build.SANDBOX_EVENT_PREFIX + reversed_payload,
            "internal whitespace": (
                build.SANDBOX_EVENT_PREFIX + canonical.replace(",", ", ", 1)),
            "nonobject": build.SANDBOX_EVENT_PREFIX + "[]",
            "truncated": build.SANDBOX_EVENT_PREFIX + canonical[:-1],
            "malformed": build.SANDBOX_EVENT_PREFIX + "{not-json}",
        }
        for name, output in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(output, TXN, HEAD)

    def test_sandbox_probe_absolute_path_grammar_matrix(self):
        event = self._sandbox_probe()
        data = event["containerDataRoot"]
        reports = event["automationReportsRoot"]
        cases = {
            "relative data": {**event, "containerDataRoot": "container/Data"},
            "relative reports": {
                **event, "automationReportsRoot": "Data/Reports"},
            "trailing slash": {
                **event, "automationReportsRoot": reports + "/"},
            "dot component": {
                **event, "automationReportsRoot": data + "/./Reports"},
            "dotdot component": {
                **event, "automationReportsRoot": data + "/Tmp/../Reports"},
            "empty component": {
                **event, "automationReportsRoot": data + "//Reports"},
            "backslash": {
                **event, "automationReportsRoot": data + "/Bad\\Reports"},
            "double slash anchor": {
                **event,
                "containerDataRoot": "//tmp/CorsairsUE/Data",
                "automationReportsRoot": "//tmp/CorsairsUE/Data/Reports",
            },
            "equal roots": {**event, "automationReportsRoot": data},
            "sibling prefix": {
                **event,
                "automationReportsRoot": str(Path(data).with_name("DataSibling")),
            },
        }
        for name, changed in cases.items():
            with self.subTest(name=name):
                payload = json.dumps(
                    changed, sort_keys=True, separators=(",", ":"))
                with self.assertRaises(build.Task8Error):
                    build.parse_sandbox_probe_event(
                        build.SANDBOX_EVENT_PREFIX + payload, TXN, HEAD)

        direct_child = {**event, "automationReportsRoot": data + "/Reports"}
        payload = json.dumps(
            direct_child, sort_keys=True, separators=(",", ":"))
        self.assertEqual(
            build.parse_sandbox_probe_event(
                build.SANDBOX_EVENT_PREFIX + payload, TXN, HEAD),
            direct_child)

    def test_sandbox_runtime_tree_is_durably_copied_then_exact_scratch_is_cleaned(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        scratch = build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(transaction.journal["sandboxScratch"]["state"], "PREPARED")
        self.assertEqual(scratch.report_root.parent.name, f"CorsairsTerrainTask8-{TXN}")
        self.assertNotEqual(
            scratch.report_root,
            build.evidence_root(self.repo.root, TXN) /
            build.RUNTIME_AUTOMATION_DIRECTORY,
        )

        index = scratch.report_root / "index.json"
        nested = scratch.report_root / "nested/result.json"
        nested.parent.mkdir(mode=0o750)
        index.write_bytes(b'{"succeeded":1}\n')
        nested.write_bytes(b'{"nested":true}\n')
        index.chmod(0o640)
        nested.chmod(0o600)
        owned = build.copy_sandbox_runtime_reports(transaction)
        destination = (
            build.evidence_root(self.repo.root, TXN) /
            build.RUNTIME_AUTOMATION_DIRECTORY)
        self.assertEqual(owned.path, destination)
        self.assertEqual((destination / "index.json").read_bytes(), index.read_bytes())
        self.assertEqual((destination / "nested/result.json").read_bytes(),
                         nested.read_bytes())
        self.assertEqual(
            stat.S_IMODE((destination / "index.json").stat().st_mode), 0o640)
        self.assertEqual(
            stat.S_IMODE((destination / "nested/result.json").stat().st_mode), 0o600)
        self.assertEqual(transaction.journal["sandboxScratch"]["state"], "COPIED")

        build.cleanup_sandbox_report_scratch(transaction)
        self.assertIsNone(transaction.journal["sandboxScratch"])
        self.assertFalse(scratch.transaction_root.exists())
        self.assertFalse(scratch.reservation_path.exists())
        self.assertTrue((destination / "index.json").is_file())

    def test_sandbox_prepare_rejects_post_attestation_reports_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        reports_root = binding.automation_reports_root
        detached = reports_root.with_name(reports_root.name + "-attested")
        reports_root.rename(detached)
        reports_root.mkdir(mode=0o700)
        replacement_identity = reports_root.stat().st_ino

        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertTrue(reports_root.is_dir())
        self.assertEqual(reports_root.stat().st_ino, replacement_identity)
        self.assertEqual(os.listdir(reports_root), [])
        self.assertIsNone(transaction.journal["sandboxScratch"])

    def test_sandbox_prepare_rejects_same_bytes_metadata_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        metadata = binding.metadata_path
        payload = metadata.read_bytes()
        detached = metadata.with_name(metadata.name + ".attested")
        metadata.rename(detached)
        metadata.write_bytes(payload)
        replacement_inode = metadata.stat().st_ino

        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(metadata.read_bytes(), payload)
        self.assertEqual(metadata.stat().st_ino, replacement_inode)
        self.assertIsNone(transaction.journal["sandboxScratch"])

    def test_sandbox_attestation_rejects_writable_intermediate_component(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        writable = (
            Path(probe["containerDataRoot"]) /
            "Library/Application Support/Epic")
        writable.chmod(0o777)
        try:
            container = Path(probe["containerDataRoot"]).parent
            metadata = container / ".com.apple.containermanagerd.metadata.plist"
            metadata.write_bytes(plistlib.dumps({
                "MCMMetadataCreator": probe["bundleIdentifier"],
                "MCMMetadataIdentifier": probe["bundleIdentifier"],
            }, fmt=plistlib.FMT_XML, sort_keys=True))
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)
        finally:
            writable.chmod(0o700)

    def test_sandbox_attestation_rejects_intermediate_inode_swap_with_same_endpoints(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        data_root = Path(probe["containerDataRoot"])
        container = data_root.parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        intermediate = data_root / "Library/Application Support/Epic"
        original_inode = intermediate.stat().st_ino
        detached = intermediate.with_name("Epic-attested")
        original_revalidate = build._revalidate_pinned_directory_path
        swapped = False

        def swap_then_revalidate(path, descriptor, identity):
            nonlocal swapped
            if not swapped:
                intermediate.rename(detached)
                intermediate.mkdir(mode=0o700)
                (detached / "CorsairsUE").rename(intermediate / "CorsairsUE")
                swapped = True
            return original_revalidate(path, descriptor, identity)

        with mock.patch.object(
                build, "_revalidate_pinned_directory_path",
                side_effect=swap_then_revalidate):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)
        self.assertTrue(swapped)
        self.assertNotEqual(intermediate.stat().st_ino, original_inode)

    def test_sandbox_attestation_physical_component_kind_matrix(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        verified = self._verified_sandbox_app()
        for target in self._sandbox_component_paths(probe):
            for kind in ("missing", "regular", "symlink"):
                with self.subTest(path=target.name, kind=kind):
                    detached = target.with_name(target.name + ".physical-case")
                    target.rename(detached)
                    try:
                        if kind == "regular":
                            target.write_bytes(b"foreign")
                        elif kind == "symlink":
                            target.symlink_to(detached, target_is_directory=True)
                        with self.assertRaises(build.Task8Error):
                            build.prepare_sandbox_attestation(
                                transaction, HEAD, verified, probe)
                    finally:
                        if target.is_symlink() or target.is_file():
                            target.unlink()
                        detached.rename(target)
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_component_owner_and_mode_matrix(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        verified = self._verified_sandbox_app()
        component_paths = self._sandbox_component_paths(probe)
        for target in component_paths:
            original_mode = stat.S_IMODE(target.stat().st_mode)
            for unsafe_mode in (0o720, 0o702):
                with self.subTest(
                        path=target.name, unsafe_mode=oct(unsafe_mode)):
                    target.chmod(unsafe_mode)
                    try:
                        with self.assertRaises(build.Task8Error):
                            build.prepare_sandbox_attestation(
                                transaction, HEAD, verified, probe)
                    finally:
                        target.chmod(original_mode)

        original_identity = build._safe_attested_directory_identity
        for target in component_paths:
            observed = False

            def foreign_owner(path, info, *, status):
                nonlocal observed
                if path == target:
                    values = list(info)
                    values[4] = os.geteuid() + 1
                    info = os.stat_result(values)
                    observed = True
                return original_identity(path, info, status=status)

            with self.subTest(path=target.name, mutation="foreign-owner"), \
                    mock.patch.object(
                        build, "_safe_attested_directory_identity",
                        side_effect=foreign_owner):
                with self.assertRaises(build.Task8Error):
                    build.prepare_sandbox_attestation(
                        transaction, HEAD, verified, probe)
            self.assertTrue(observed)

        target_info = component_paths[-1].stat()
        values = list(target_info)
        values[4] = os.geteuid() + 1
        with self.assertRaises(build.Task8Error):
            build._safe_attested_directory_identity(
                component_paths[-1], os.stat_result(values), status="FAILED")
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_rejects_unreadable_component_matrix(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        verified = self._verified_sandbox_app()
        for target in self._sandbox_component_paths(probe):
            original_mode = stat.S_IMODE(target.stat().st_mode)
            with self.subTest(path=target.name):
                target.chmod(0)
                try:
                    with self.assertRaises(build.Task8Error):
                        build.prepare_sandbox_attestation(
                            transaction, HEAD, verified, probe)
                finally:
                    target.chmod(original_mode)

    def test_sandbox_scratch_revalidates_intermediate_component_ledger(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        data_root = binding.container_data_root
        intermediate = data_root / "Library/Application Support/Epic"
        detached = intermediate.with_name("Epic-attested")
        intermediate.rename(detached)
        intermediate.mkdir(mode=0o700)
        (detached / "CorsairsUE").rename(intermediate / "CorsairsUE")
        replacement_inode = intermediate.stat().st_ino

        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(intermediate.stat().st_ino, replacement_inode)
        self.assertIsNone(transaction.journal["sandboxScratch"])

    def test_sandbox_attestation_metadata_physical_kind_matrix(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        payload = plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True)
        verified = self._verified_sandbox_app()
        for kind in (
                "missing", "directory", "symlink", "hardlink", "fifo",
                "unreadable"):
            with self.subTest(kind=kind):
                source = metadata.with_name(metadata.name + ".source")
                try:
                    if kind == "directory":
                        metadata.mkdir()
                    elif kind == "symlink":
                        source.write_bytes(payload)
                        metadata.symlink_to(source)
                    elif kind == "hardlink":
                        source.write_bytes(payload)
                        os.link(source, metadata)
                    elif kind == "fifo":
                        os.mkfifo(metadata)
                    elif kind == "unreadable":
                        metadata.write_bytes(payload)
                        metadata.chmod(0)
                    with self.assertRaises(build.Task8Error):
                        build.prepare_sandbox_attestation(
                            transaction, HEAD, verified, probe)
                finally:
                    if metadata.is_symlink() or (
                            metadata.exists() and not metadata.is_dir()):
                        if kind == "unreadable":
                            metadata.chmod(0o600)
                        metadata.unlink()
                    elif metadata.is_dir():
                        metadata.rmdir()
                    if source.exists():
                        source.unlink()

    def test_sandbox_attestation_metadata_plist_member_matrix(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        bundle = probe["bundleIdentifier"]
        cases = {
            "malformed": b"not-a-plist",
            "non-dict": plistlib.dumps([], fmt=plistlib.FMT_XML),
            "missing identifier": plistlib.dumps({
                "MCMMetadataCreator": bundle}, fmt=plistlib.FMT_XML),
            "missing creator": plistlib.dumps({
                "MCMMetadataIdentifier": bundle}, fmt=plistlib.FMT_XML),
            "identifier type": plistlib.dumps({
                "MCMMetadataCreator": bundle,
                "MCMMetadataIdentifier": 1}, fmt=plistlib.FMT_XML),
            "creator type": plistlib.dumps({
                "MCMMetadataCreator": [],
                "MCMMetadataIdentifier": bundle}, fmt=plistlib.FMT_XML),
            "identifier value": plistlib.dumps({
                "MCMMetadataCreator": bundle,
                "MCMMetadataIdentifier": "com.example.Other"},
                fmt=plistlib.FMT_XML),
            "creator value": plistlib.dumps({
                "MCMMetadataCreator": "com.example.Other",
                "MCMMetadataIdentifier": bundle}, fmt=plistlib.FMT_XML),
        }
        verified = self._verified_sandbox_app()
        for name, payload in cases.items():
            with self.subTest(name=name):
                metadata.write_bytes(payload)
                try:
                    with self.assertRaises(build.Task8Error):
                        build.prepare_sandbox_attestation(
                            transaction, HEAD, verified, probe)
                finally:
                    metadata.unlink()
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_rejects_metadata_swap_during_read(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        payload = plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True)
        metadata.write_bytes(payload)
        detached = metadata.with_name(metadata.name + ".opened")
        original_read = build._read_descriptor
        swapped = False

        def read_then_swap(descriptor):
            nonlocal swapped
            observed = original_read(descriptor)
            if not swapped:
                metadata.rename(detached)
                metadata.write_bytes(observed)
                swapped = True
            return observed

        with mock.patch.object(
                build, "_read_descriptor", side_effect=read_then_swap):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)
        self.assertTrue(swapped)
        self.assertEqual(metadata.read_bytes(), payload)
        self.assertNotEqual(metadata.stat().st_ino, detached.stat().st_ino)
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_rejects_signed_probe_bundle_mismatch(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe("com.example.Other")
        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_attestation(
                transaction, HEAD, self._verified_sandbox_app(), probe)

    def test_sandbox_attestation_final_validation_failure_removes_exact_partial_pair(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        package_reports.mkdir(parents=True, mode=0o700)
        prior = package_reports / "prior-package-evidence.json"
        prior.write_bytes(b"prior\n")

        with mock.patch.object(
                build, "_validate_attested_sandbox_environment",
                side_effect=build.Task8Error("injected final validation")):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)

        self.assertEqual(prior.read_bytes(), b"prior\n")
        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_self_validation_failure_removes_entitlements(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"

        with mock.patch.object(
                build.rules, "validate_sandbox_attestation",
                return_value=["injected self-validation failure"]):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)

        self.assertFalse((package_reports / "app-entitlements.plist").exists())
        self.assertFalse((package_reports / "app-sandbox.json").exists())

    def test_sandbox_attestation_cleanup_never_deletes_replaced_leaf(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        package_reports = build.evidence_root(self.repo.root, TXN) / "package"
        attestation_path = package_reports / "app-sandbox.json"
        detached = package_reports / "app-sandbox.original"
        replacement_inode = None

        def replace_then_fail(_transaction, _environment):
            nonlocal replacement_inode
            payload = attestation_path.read_bytes()
            attestation_path.rename(detached)
            attestation_path.write_bytes(payload)
            attestation_path.chmod(0o600)
            replacement_inode = attestation_path.stat().st_ino
            raise build.Task8Error("injected final validation")

        with mock.patch.object(
                build, "_validate_attested_sandbox_environment",
                side_effect=replace_then_fail):
            with self.assertRaises(build.Task8Error) as raised:
                build.prepare_sandbox_attestation(
                    transaction, HEAD, self._verified_sandbox_app(), probe)

        self.assertEqual(raised.exception.status, "RECOVERY_REQUIRED")
        self.assertIsNotNone(replacement_inode)
        self.assertEqual(attestation_path.stat().st_ino, replacement_inode)
        self.assertTrue(detached.is_file())
        self.assertTrue((package_reports / "app-entitlements.plist").is_file())

    def test_sandbox_scratch_rejects_unattested_raw_probe(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(
                transaction, self._sandbox_probe())
        self.assertIsNone(transaction.journal["sandboxScratch"])

    def test_bundle_identifier_matrix_accepts_single_component_and_rejects_underscore(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        self.assertEqual(
            build._codesign_identifier("Identifier=CorsairsUE\n"),
            "CorsairsUE")
        with self.assertRaises(build.Task8Error):
            build._codesign_identifier("Identifier=not_a_bundle\n")

        verified = self._verified_sandbox_app("CorsairsUE")
        build._validate_verified_sandbox_app(transaction, verified)
        with self.assertRaises(build.Task8Error):
            build._validate_verified_sandbox_app(
                transaction,
                replace(
                    verified,
                    bundle_identifier="not_a_bundle",
                    signing_identifier="not_a_bundle"))

        environment = self._attested_sandbox_environment(
            transaction, "CorsairsUE")
        build._validate_attested_sandbox_environment(transaction, environment)
        with self.assertRaises(build.Task8Error):
            build._validate_attested_sandbox_environment(
                transaction,
                replace(environment, bundle_identifier="not_a_bundle"))

        build.prepare_sandbox_report_scratch(transaction, environment)
        record = transaction.journal["sandboxScratch"]
        build._validate_sandbox_scratch_record(
            record, self.repo.root, TXN, HEAD)
        changed = json.loads(json.dumps(record))
        changed["bundleIdentifier"] = "not_a_bundle"
        with self.assertRaises(build.Task8Error):
            build._validate_sandbox_scratch_record(
                changed, self.repo.root, TXN, HEAD)

    def test_sandbox_scratch_journal_requires_parent_strictly_below_data(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        build.prepare_sandbox_report_scratch(transaction, binding)
        record = json.loads(json.dumps(transaction.journal["sandboxScratch"]))
        parent = Path(record["containerDataRoot"])
        transaction_root = parent / f"CorsairsTerrainTask8-{TXN}"
        report_root = transaction_root / build.RUNTIME_AUTOMATION_DIRECTORY
        record["scratchParent"] = str(parent)
        record["scratchParentIdentity"] = list(record["containerDataIdentity"])
        record["reservationPath"] = str(
            parent / f".CorsairsTerrainTask8-{TXN}.reservation")
        record["transactionRoot"] = str(transaction_root)
        record["reportRoot"] = str(report_root)
        reservation_payload = build._sandbox_reservation_payload(
            TXN, HEAD, record["bundleIdentifier"], transaction_root, report_root)
        record["reservationSha256"] = hashlib.sha256(
            reservation_payload).hexdigest()
        with self.assertRaises(build.Task8Error):
            build._validate_sandbox_scratch_record(
                record, self.repo.root, TXN, HEAD)

    def test_sandbox_created_report_must_share_transaction_device(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        original_identity = build._directory_identity

        def cross_device_report(path, **kwargs):
            identity = original_identity(path, **kwargs)
            if Path(path).name == build.RUNTIME_AUTOMATION_DIRECTORY:
                identity[0] += 1
            return identity

        with mock.patch.object(
                build, "_directory_identity", side_effect=cross_device_report):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertNotEqual(
            transaction.journal["sandboxScratch"]["state"], "PREPARED")

    def test_sandbox_created_reservation_must_share_scratch_parent_device(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        original_identity = build._regular_identity

        def cross_device_reservation(path, **kwargs):
            identity = original_identity(path, **kwargs)
            if Path(path).name.endswith(".reservation"):
                identity[0] += 1
            return identity

        with mock.patch.object(
                build, "_regular_identity", side_effect=cross_device_reservation):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "PLANNED")
        self.assertEqual(
            transaction.journal["sandboxScratch"]["reservationIdentity"], [])

    def test_sandbox_created_transaction_must_share_scratch_parent_device(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        original_identity = build._directory_identity

        def cross_device_transaction(path, **kwargs):
            identity = original_identity(path, **kwargs)
            if Path(path).name == f"CorsairsTerrainTask8-{TXN}":
                identity[0] += 1
            return identity

        with mock.patch.object(
                build, "_directory_identity", side_effect=cross_device_transaction):
            with self.assertRaises(build.Task8Error):
                build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "RESERVED")
        self.assertEqual(
            transaction.journal["sandboxScratch"]["transactionIdentity"], [])

    def test_sandbox_runtime_tree_rejects_symlink_and_hard_link(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        index = scratch.report_root / "index.json"
        index.write_bytes(b'{"succeeded":1}\n')
        unsafe = scratch.report_root / "unsafe.json"
        unsafe.symlink_to(index)
        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction)
        unsafe.unlink()
        os.link(index, unsafe)
        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction)
        unsafe.unlink()
        self.assertFalse(scratch.destination_root.exists())

    def test_sandbox_runtime_tree_special_and_unreadable_entry_matrix(self):
        cases = (
            "unreadable-file", "unreadable-directory", "fifo", "socket",
            "character-device", "block-device",
        )
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                    prefix="t8-tree-") as temporary:
                root = Path(temporary)
                (root / "index.json").write_bytes(b'{"succeeded":1}\n')
                unsafe = root / "unsafe"
                socket_handle = None
                patched_stat = None
                try:
                    if case == "unreadable-file":
                        unsafe.write_bytes(b"private")
                        unsafe.chmod(0)
                    elif case == "unreadable-directory":
                        unsafe.mkdir(mode=0o700)
                        (unsafe / "private.json").write_bytes(b"private")
                        unsafe.chmod(0)
                    elif case == "fifo":
                        os.mkfifo(unsafe, 0o600)
                    elif case == "socket":
                        socket_handle = socket.socket(
                            socket.AF_UNIX, socket.SOCK_STREAM)
                        socket_handle.bind(str(unsafe))
                    else:
                        unsafe.write_bytes(b"device-placeholder")
                        original_stat = build._entry_stat_at
                        synthetic_mode = (
                            stat.S_IFCHR if case == "character-device"
                            else stat.S_IFBLK) | 0o600

                        def device_stat(parent_descriptor, name, **kwargs):
                            info = original_stat(
                                parent_descriptor, name, **kwargs)
                            if name == unsafe.name and info is not None:
                                values = list(info)
                                values[0] = synthetic_mode
                                return os.stat_result(values)
                            return info

                        patched_stat = mock.patch.object(
                            build, "_entry_stat_at", side_effect=device_stat)
                        patched_stat.start()
                    with self.assertRaises(build.Task8Error):
                        build._tree_records(root, require_index=True)
                finally:
                    if patched_stat is not None:
                        patched_stat.stop()
                    if socket_handle is not None:
                        socket_handle.close()
                    if case == "unreadable-file" and unsafe.exists():
                        unsafe.chmod(0o600)
                    if case == "unreadable-directory" and unsafe.exists():
                        unsafe.chmod(0o700)

    def test_sandbox_copy_rejects_same_bytes_source_inode_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        index = scratch.report_root / "index.json"
        payload = b'{"succeeded":1}\n'
        index.write_bytes(payload)
        index.chmod(0o640)
        detached = self.repo.root / "index.attested"
        replaced = False

        def replace_after_copy(point):
            nonlocal replaced
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    not replaced):
                index.rename(detached)
                index.write_bytes(payload)
                index.chmod(0o640)
                replaced = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(
                transaction, fault=replace_after_copy)
        self.assertTrue(replaced)
        self.assertEqual(index.read_bytes(), payload)
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "PREPARED")

    def test_sandbox_copy_rejects_same_mode_source_directory_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        nested = scratch.report_root / "nested"
        nested.mkdir(mode=0o700)
        detached = self.repo.root / "nested.attested"
        replaced = False

        def replace_after_file_copy(point):
            nonlocal replaced
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    not replaced):
                nested.rename(detached)
                nested.mkdir(mode=0o700)
                replaced = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(
                transaction, fault=replace_after_file_copy)
        self.assertTrue(replaced)
        self.assertTrue(nested.is_dir())
        self.assertEqual(os.listdir(nested), [])

    def test_sandbox_copy_source_mutation_matrix(self):
        cases = ("bytes", "size", "mode", "add", "remove", "rename")
        original_repo = self.repo
        for case in cases:
            with self.subTest(case=case):
                case_repo = FakeRepo()
                transaction = None
                self.repo = case_repo
                try:
                    transaction = build.OuterTransaction.begin(
                        case_repo.root, TXN, HEAD)
                    scratch = build.prepare_sandbox_report_scratch(
                        transaction,
                        self._attested_sandbox_environment(transaction))
                    index = scratch.report_root / "index.json"
                    index.write_bytes(b"AAAA\n")
                    index.chmod(0o640)
                    other = scratch.report_root / "other.json"
                    other.write_bytes(b"other\n")
                    other.chmod(0o640)
                    index_inode = index.stat().st_ino
                    mutated = False

                    def mutate_source(point):
                        nonlocal mutated
                        if (point !=
                                "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" or
                                mutated):
                            return None
                        if case == "bytes":
                            index.write_bytes(b"BBBB\n")
                        elif case == "size":
                            with index.open("ab") as stream:
                                stream.write(b"extra")
                        elif case == "mode":
                            index.chmod(0o600)
                        elif case == "add":
                            (scratch.report_root / "added.json").write_bytes(
                                b"added\n")
                        elif case == "remove":
                            other.unlink()
                        else:
                            other.rename(scratch.report_root / "renamed.json")
                        mutated = True
                        return None

                    with self.assertRaises(build.Task8Error):
                        build.copy_sandbox_runtime_reports(
                            transaction, fault=mutate_source)
                    self.assertTrue(mutated)
                    if case in ("bytes", "size", "mode"):
                        self.assertEqual(index.stat().st_ino, index_inode)
                finally:
                    if transaction is not None:
                        transaction.close()
                    self.repo = original_repo
                    case_repo.close()

    def test_sandbox_tree_scan_rejects_same_inode_stream_mutation(self):
        root = self.repo.root / "stream-mutation"
        root.mkdir(mode=0o700)
        index = root / "index.json"
        index.write_bytes(b"AAAA\n")
        inode = index.stat().st_ino
        original_read = os.read
        mutated = False

        def mutate_during_read(descriptor, size):
            nonlocal mutated
            block = original_read(descriptor, size)
            if (block and os.fstat(descriptor).st_ino == inode and not mutated):
                index.write_bytes(b"BBBB\n")
                mutated = True
            return block

        with mock.patch.object(build.os, "read", side_effect=mutate_during_read):
            with self.assertRaises(build.Task8Error):
                build._tree_records(root, require_index=True)
        self.assertTrue(mutated)
        self.assertEqual(index.stat().st_ino, inode)

    def test_sandbox_copy_rejects_same_bytes_destination_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        payload = b'{"succeeded":1}\n'
        (scratch.report_root / "index.json").write_bytes(payload)
        destination_index = scratch.destination_root / "index.json"
        detached = self.repo.root / "destination-index.attested"
        original_scan = build._tree_records_at
        replaced = False

        def replace_after_destination_scan(
                descriptor, root, *, require_index):
            nonlocal replaced
            snapshot = original_scan(
                descriptor, root, require_index=require_index)
            if (Path(root) == scratch.destination_root and
                    destination_index.is_file() and not replaced):
                mode = stat.S_IMODE(destination_index.stat().st_mode)
                destination_index.rename(detached)
                destination_index.write_bytes(payload)
                destination_index.chmod(mode)
                replaced = True
            return snapshot

        with mock.patch.object(
                build, "_tree_records_at",
                side_effect=replace_after_destination_scan):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(replaced)
        self.assertEqual(destination_index.read_bytes(), payload)

    def test_sandbox_copy_hook_rejects_copied_file_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        payload = b'{"succeeded":1}\n'
        (scratch.report_root / "index.json").write_bytes(payload)
        destination = scratch.destination_root / "index.json"
        detached = self.repo.root / "hook-file.attested"
        mutated = False

        def mutate(point):
            nonlocal mutated
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    not mutated):
                mode = stat.S_IMODE(destination.stat().st_mode)
                destination.rename(detached)
                destination.write_bytes(payload)
                destination.chmod(mode)
                mutated = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction, fault=mutate)
        self.assertTrue(mutated)

    def test_sandbox_copy_rejects_file_replacement_before_helper_returns(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        payload = b'{"succeeded":1}\n'
        (scratch.report_root / "index.json").write_bytes(payload)
        destination = scratch.destination_root / "index.json"
        detached = self.repo.root / "helper-file.attested"
        original_copy = build._copy_sandbox_file
        replaced = False

        def replace_before_return(*args, **kwargs):
            nonlocal replaced
            result = original_copy(*args, **kwargs)
            if not replaced:
                mode = stat.S_IMODE(destination.stat().st_mode)
                destination.rename(detached)
                destination.write_bytes(payload)
                destination.chmod(mode)
                replaced = True
            return result

        with mock.patch.object(
                build, "_copy_sandbox_file", side_effect=replace_before_return):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(replaced)

    def test_sandbox_copy_rejects_root_replacement_before_mkdir_helper_returns(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        detached = self.repo.root / "helper-root.attested"
        original_mkdir = build._mkdir_exclusive
        replaced = False

        def replace_before_return(path, *args, **kwargs):
            nonlocal replaced
            result = original_mkdir(path, *args, **kwargs)
            if Path(path) == scratch.destination_root and not replaced:
                scratch.destination_root.rename(detached)
                scratch.destination_root.mkdir(mode=0o700)
                replaced = True
            return result

        with mock.patch.object(
                build, "_mkdir_exclusive", side_effect=replace_before_return):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(replaced)

    def test_sandbox_copy_rejects_nested_replacement_before_mkdir_helper_returns(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        source_nested = scratch.report_root / "nested"
        source_nested.mkdir(mode=0o700)
        (source_nested / "result.json").write_bytes(b'{"nested":true}\n')
        destination_nested = scratch.destination_root / "nested"
        detached = self.repo.root / "helper-nested.attested"
        original_mkdir = build._mkdir_exclusive
        replaced = False

        def replace_before_return(path, *args, **kwargs):
            nonlocal replaced
            result = original_mkdir(path, *args, **kwargs)
            if Path(path) == destination_nested and not replaced:
                destination_nested.rename(detached)
                destination_nested.mkdir(mode=0o700)
                replaced = True
            return result

        with mock.patch.object(
                build, "_mkdir_exclusive", side_effect=replace_before_return):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(replaced)

    def test_sandbox_copy_never_writes_through_swapped_destination_root(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        outside = self.repo.root / "outside-copy-victim"
        outside.mkdir()
        victim = outside / "foreign.json"
        victim.write_bytes(b"foreign")
        outside_before = [
            (item.relative_to(outside).as_posix(), item.read_bytes())
            for item in sorted(outside.rglob("*")) if item.is_file()
        ]
        detached = self.repo.root / "destination-root-before-copy"
        original_copy = build._copy_sandbox_file
        swapped = False

        def swap_before_copy(*args, **kwargs):
            nonlocal swapped
            if not swapped:
                scratch.destination_root.rename(detached)
                scratch.destination_root.symlink_to(
                    outside, target_is_directory=True)
                swapped = True
            return original_copy(*args, **kwargs)

        with mock.patch.object(
                build, "_copy_sandbox_file", side_effect=swap_before_copy):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(swapped)
        outside_after = [
            (item.relative_to(outside).as_posix(), item.read_bytes())
            for item in sorted(outside.rglob("*")) if item.is_file()
        ]
        self.assertEqual(outside_after, outside_before)

    def test_sandbox_copy_populates_readonly_nested_directory_before_final_mode(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        source_nested = scratch.report_root / "readonly"
        source_nested.mkdir(mode=0o700)
        nested_payload = b'{"readonly":true}\n'
        (source_nested / "result.json").write_bytes(nested_payload)
        source_nested.chmod(0o500)
        self.addCleanup(
            lambda: source_nested.chmod(0o700) if source_nested.exists() else None)

        owned = build.copy_sandbox_runtime_reports(transaction)
        destination_nested = owned.path / "readonly"
        self.addCleanup(
            lambda: destination_nested.chmod(0o700)
            if destination_nested.exists() else None)
        self.assertEqual(
            stat.S_IMODE(destination_nested.stat().st_mode), 0o500)
        self.assertEqual(
            (destination_nested / "result.json").read_bytes(), nested_payload)

    def test_sandbox_copy_revalidates_after_durable_callback(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        payload = b'{"succeeded":1}\n'
        (scratch.report_root / "index.json").write_bytes(payload)
        destination = scratch.destination_root / "index.json"
        detached = self.repo.root / "after-durable.attested"
        mutated = False

        def mutate(point):
            nonlocal mutated
            if point == "AFTER_SANDBOX_COPIED_DURABLE" and not mutated:
                mode = stat.S_IMODE(destination.stat().st_mode)
                destination.rename(detached)
                destination.write_bytes(payload)
                destination.chmod(mode)
                mutated = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction, fault=mutate)
        self.assertTrue(mutated)
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "COPIED")

    def test_sandbox_copy_closes_each_retained_file_descriptor_before_next_file(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        for index in range(24):
            (scratch.report_root / f"file-{index:02d}.json").write_bytes(
                f'{{"index":{index}}}\n'.encode("ascii"))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        original_copy = build._copy_sandbox_file
        previous_descriptor = None
        observed = 0

        def track_retained_descriptor(*args, **kwargs):
            nonlocal previous_descriptor, observed
            if previous_descriptor is not None:
                with self.assertRaises(OSError):
                    os.fstat(previous_descriptor)
            result = original_copy(*args, **kwargs)
            previous_descriptor = result[0]
            observed += 1
            return result

        with mock.patch.object(
                build, "_copy_sandbox_file",
                side_effect=track_retained_descriptor):
            build.copy_sandbox_runtime_reports(transaction)
        self.assertEqual(observed, 25)
        with self.assertRaises(OSError):
            os.fstat(previous_descriptor)

    def test_sandbox_copy_rejects_parent_ctime_change_during_file_write(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        original_write = os.write
        mutated = False

        def mutate_parent(descriptor, payload):
            nonlocal mutated
            written = original_write(descriptor, payload)
            if scratch.destination_root.is_dir() and not mutated:
                scratch.destination_root.chmod(0o750)
                scratch.destination_root.chmod(0o700)
                mutated = True
            return written

        with mock.patch.object(build.os, "write", side_effect=mutate_parent):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(mutated)

    def test_sandbox_copy_rejects_parent_ctime_change_during_directory_setup(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        destination_parent = scratch.destination_root.parent
        original_fsync = os.fsync
        mutated = False

        def mutate_parent(descriptor):
            nonlocal mutated
            original_fsync(descriptor)
            if scratch.destination_root.is_dir() and not mutated:
                destination_parent.chmod(0o750)
                destination_parent.chmod(0o700)
                mutated = True

        with mock.patch.object(build.os, "fsync", side_effect=mutate_parent):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(mutated)

    def test_sandbox_copy_file_verification_is_linear(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        file_count = 33
        for index in range(file_count - 1):
            (scratch.report_root / f"file-{index:02d}.json").write_bytes(
                f'{{"index":{index}}}\n'.encode("ascii"))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        original_verify = build._verify_regular_at
        verify_calls = 0

        def count_verify(*args, **kwargs):
            nonlocal verify_calls
            verify_calls += 1
            return original_verify(*args, **kwargs)

        with mock.patch.object(
                build, "_verify_regular_at", side_effect=count_verify):
            build.copy_sandbox_runtime_reports(transaction)
        self.assertLessEqual(verify_calls, file_count * 3 + 8)

    def test_sandbox_copy_directory_descriptors_are_bounded_by_depth(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        for index in range(64):
            (scratch.report_root / f"sibling-{index:02d}").mkdir(mode=0o700)
        baseline = len(os.listdir("/dev/fd"))
        peak = baseline
        original_mkdir = build._mkdir_exclusive

        def track_descriptors(*args, **kwargs):
            nonlocal peak
            result = original_mkdir(*args, **kwargs)
            peak = max(peak, len(os.listdir("/dev/fd")))
            return result

        with mock.patch.object(
                build, "_mkdir_exclusive", side_effect=track_descriptors):
            build.copy_sandbox_runtime_reports(transaction)
        self.assertLessEqual(peak - baseline, 12)

    def test_sandbox_copy_rejects_source_root_ctime_change_after_path_revalidation(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        original_revalidate = build._revalidate_pinned_directory_path
        mutated = False

        def mutate_after_revalidate(path, *args, **kwargs):
            nonlocal mutated
            result = original_revalidate(path, *args, **kwargs)
            if Path(path) == scratch.report_root and not mutated:
                mode = stat.S_IMODE(scratch.report_root.stat().st_mode)
                scratch.report_root.chmod(0o750 if mode != 0o750 else 0o700)
                scratch.report_root.chmod(mode)
                mutated = True
            return result

        with mock.patch.object(
                build, "_revalidate_pinned_directory_path",
                side_effect=mutate_after_revalidate):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(mutated)

    def test_sandbox_copy_rejects_nested_source_rewrite_after_final_revalidation(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        nested = scratch.report_root / "nested"
        nested.mkdir(mode=0o700)
        source = nested / "result.json"
        source.write_bytes(b"AAAA\n")
        original_revalidate = build._revalidate_pinned_directory_path
        source_revalidations = 0
        mutated = False

        def mutate_after_final_source_revalidation(path, *args, **kwargs):
            nonlocal source_revalidations, mutated
            result = original_revalidate(path, *args, **kwargs)
            if Path(path) == scratch.report_root:
                source_revalidations += 1
                if source_revalidations == 4 and not mutated:
                    source.write_bytes(b"BBBB\n")
                    mutated = True
            return result

        with mock.patch.object(
                build, "_revalidate_pinned_directory_path",
                side_effect=mutate_after_final_source_revalidation):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(mutated)
        self.assertEqual(source.read_bytes(), b"BBBB\n")

    def test_sandbox_copy_rejects_destination_rewrite_after_final_parent_revalidation(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b"AAAA\n")
        destination = scratch.destination_root / "index.json"
        destination_parent = scratch.destination_root.parent
        original_revalidate = build._revalidate_pinned_directory_path
        parent_revalidations = 0
        mutated = False

        def mutate_after_final_parent_revalidation(path, *args, **kwargs):
            nonlocal parent_revalidations, mutated
            result = original_revalidate(path, *args, **kwargs)
            if Path(path) == destination_parent:
                parent_revalidations += 1
                if parent_revalidations == 3 and not mutated:
                    destination.write_bytes(b"BBBB\n")
                    mutated = True
            return result

        with mock.patch.object(
                build, "_revalidate_pinned_directory_path",
                side_effect=mutate_after_final_parent_revalidation):
            with self.assertRaises(build.Task8Error):
                build.copy_sandbox_runtime_reports(transaction)
        self.assertTrue(mutated)
        self.assertEqual(destination.read_bytes(), b"BBBB\n")

    def test_runtime_evidence_rejects_destination_rewrite_after_copy_returns(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        source = scratch.report_root / "index.json"
        source.write_bytes(b"AAAA\n")
        owned = build.copy_sandbox_runtime_reports(transaction)
        destination = scratch.destination_root / "index.json"
        destination.write_bytes(b"BBBB\n")

        with self.assertRaises(build.Task8Error):
            build._evidence_set(
                owned.path, self.repo.root, TXN, HEAD,
                expected_directory=owned)
        self.assertEqual(destination.read_bytes(), b"BBBB\n")

    def test_sandbox_copy_post_barrier_destination_mutation_matrix(self):
        cases = ("bytes", "size", "mode", "add", "remove", "root-mode")
        original_repo = self.repo
        for case in cases:
            with self.subTest(case=case):
                case_repo = FakeRepo()
                transaction = None
                self.repo = case_repo
                try:
                    transaction = build.OuterTransaction.begin(
                        case_repo.root, TXN, HEAD)
                    scratch = build.prepare_sandbox_report_scratch(
                        transaction,
                        self._attested_sandbox_environment(transaction))
                    (scratch.report_root / "index.json").write_bytes(b"AAAA\n")
                    destination = scratch.destination_root / "index.json"
                    mutated = False

                    def mutate_destination(point):
                        nonlocal mutated
                        if (point != "AFTER_SANDBOX_REPORT_COPY_REVALIDATED" or
                                mutated):
                            return None
                        if case == "bytes":
                            destination.write_bytes(b"BBBB\n")
                        elif case == "size":
                            with destination.open("ab") as stream:
                                stream.write(b"extra")
                        elif case == "mode":
                            destination.chmod(0o600)
                        elif case == "add":
                            (scratch.destination_root / "added.json").write_bytes(
                                b"added\n")
                        elif case == "remove":
                            destination.unlink()
                        else:
                            scratch.destination_root.chmod(0o750)
                        mutated = True
                        return None

                    with self.assertRaises(build.Task8Error):
                        build.copy_sandbox_runtime_reports(
                            transaction, fault=mutate_destination)
                    self.assertTrue(mutated)
                finally:
                    if transaction is not None:
                        transaction.close()
                    self.repo = original_repo
                    case_repo.close()

    def test_runtime_evidence_destination_commitment_mutation_matrix(self):
        cases = (
            "bytes", "size", "mode", "add", "remove", "root-mode",
            "root-replace", "expected-device", "expected-owner",
        )
        original_repo = self.repo
        for case in cases:
            with self.subTest(case=case):
                case_repo = FakeRepo()
                transaction = None
                self.repo = case_repo
                try:
                    transaction = build.OuterTransaction.begin(
                        case_repo.root, TXN, HEAD)
                    scratch = build.prepare_sandbox_report_scratch(
                        transaction,
                        self._attested_sandbox_environment(transaction))
                    (scratch.report_root / "index.json").write_bytes(b"AAAA\n")
                    owned = build.copy_sandbox_runtime_reports(transaction)
                    destination = scratch.destination_root / "index.json"
                    observed = owned
                    if case == "bytes":
                        destination.write_bytes(b"BBBB\n")
                    elif case == "size":
                        with destination.open("ab") as stream:
                            stream.write(b"extra")
                    elif case == "mode":
                        destination.chmod(0o600)
                    elif case == "add":
                        (scratch.destination_root / "added.json").write_bytes(
                            b"added\n")
                    elif case == "remove":
                        destination.unlink()
                    elif case == "root-mode":
                        scratch.destination_root.chmod(0o750)
                    elif case == "root-replace":
                        detached = case_repo.root / "detached-runtime-evidence"
                        scratch.destination_root.rename(detached)
                        scratch.destination_root.mkdir(mode=0o700)
                        destination.write_bytes(b"AAAA\n")
                    elif case == "expected-device":
                        observed = replace(owned, device=owned.device + 1)
                    else:
                        observed = replace(owned, owner_uid=owned.owner_uid + 1)
                    with self.assertRaises(build.Task8Error):
                        build._evidence_set(
                            observed.path, case_repo.root, TXN, HEAD,
                            expected_directory=observed)
                finally:
                    if transaction is not None:
                        transaction.close()
                    self.repo = original_repo
                    case_repo.close()

    def test_runtime_evidence_rejects_mutation_after_each_path_scan(self):
        original_repo = self.repo
        for occurrence in (1, 2):
            for mutation in ("rewrite", "add", "remove"):
                with self.subTest(occurrence=occurrence, mutation=mutation):
                    case_repo = FakeRepo()
                    transaction = None
                    self.repo = case_repo
                    try:
                        transaction = build.OuterTransaction.begin(
                            case_repo.root, TXN, HEAD)
                        scratch = build.prepare_sandbox_report_scratch(
                            transaction,
                            self._attested_sandbox_environment(transaction))
                        (scratch.report_root / "index.json").write_bytes(b"AAAA\n")
                        owned = build.copy_sandbox_runtime_reports(transaction)
                        destination = scratch.destination_root / "index.json"
                        original_scan = build._evidence_tree_files
                        scans = 0
                        mutated = False

                        def mutate_after_scan(root):
                            nonlocal scans, mutated
                            paths = original_scan(root)
                            scans += 1
                            if scans == occurrence:
                                if mutation == "rewrite":
                                    destination.write_bytes(b"BBBB\n")
                                elif mutation == "add":
                                    (scratch.destination_root / "added.json").write_bytes(
                                        b"added\n")
                                else:
                                    destination.unlink()
                                mutated = True
                            return paths

                        with mock.patch.object(
                                build, "_evidence_tree_files",
                                side_effect=mutate_after_scan):
                            with self.assertRaises(build.Task8Error):
                                build._evidence_set(
                                    owned.path, case_repo.root, TXN, HEAD,
                                    expected_directory=owned)
                        self.assertTrue(mutated)
                        self.assertGreaterEqual(scans, occurrence)
                    finally:
                        if transaction is not None:
                            transaction.close()
                        self.repo = original_repo
                        case_repo.close()

    def test_runtime_evidence_projection_is_reverse_lexically_sorted(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b"index\n")
        (scratch.report_root / "z.json").write_bytes(b"z\n")
        nested = scratch.report_root / "a"
        nested.mkdir(mode=0o700)
        (nested / "result.json").write_bytes(b"nested\n")
        owned = build.copy_sandbox_runtime_reports(transaction)
        evidence = build._evidence_set(
            owned.path, self.repo.root, TXN, HEAD,
            expected_directory=owned)
        paths = [item["path"] for item in evidence["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(
            [Path(path).relative_to(
                Path(evidence["root"])).as_posix() for path in paths],
            ["a/result.json", "index.json", "z.json"])

    def test_sandbox_tree_scan_rejects_cross_device_child(self):
        root = self.repo.root / "cross-device-tree"
        root.mkdir()
        (root / "index.json").write_bytes(b'{}\n')
        original_stat = build._entry_stat_at
        changed = False

        def cross_device(parent_descriptor, name, **kwargs):
            nonlocal changed
            info = original_stat(parent_descriptor, name, **kwargs)
            if name == "index.json" and info is not None and not changed:
                values = list(info)
                values[2] = info.st_dev + 1
                info = os.stat_result(values)
                changed = True
            return info

        with mock.patch.object(
                build, "_entry_stat_at", side_effect=cross_device):
            with self.assertRaises(build.Task8Error):
                build._tree_records(root, require_index=True)
        self.assertTrue(changed)

    def test_sandbox_copy_hook_rejects_destination_root_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        payload = b'{"succeeded":1}\n'
        (scratch.report_root / "index.json").write_bytes(payload)
        detached = self.repo.root / "hook-root.attested"
        mutated = False

        def mutate(point):
            nonlocal mutated
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    not mutated):
                copied = scratch.destination_root / "index.json"
                copied_mode = stat.S_IMODE(copied.stat().st_mode)
                scratch.destination_root.rename(detached)
                scratch.destination_root.mkdir(mode=0o700)
                copied.write_bytes(payload)
                copied.chmod(copied_mode)
                mutated = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction, fault=mutate)
        self.assertTrue(mutated)

    def test_sandbox_copy_hook_rejects_nested_directory_replacement(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        source_nested = scratch.report_root / "nested"
        source_nested.mkdir(mode=0o700)
        nested_payload = b'{"nested":true}\n'
        (source_nested / "result.json").write_bytes(nested_payload)
        destination_nested = scratch.destination_root / "nested"
        detached = self.repo.root / "hook-nested.attested"
        mutated = False

        def mutate(point):
            nonlocal mutated
            copied = destination_nested / "result.json"
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    copied.is_file() and not mutated):
                file_mode = stat.S_IMODE(copied.stat().st_mode)
                directory_mode = stat.S_IMODE(destination_nested.stat().st_mode)
                destination_nested.rename(detached)
                destination_nested.mkdir(mode=directory_mode)
                copied.write_bytes(nested_payload)
                copied.chmod(file_mode)
                mutated = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction, fault=mutate)
        self.assertTrue(mutated)

    def test_sandbox_copy_hook_rejects_chmod_away_and_back(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        destination = scratch.destination_root / "index.json"
        mutated = False

        def mutate(point):
            nonlocal mutated
            if (point == "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC" and
                    not mutated):
                mode = stat.S_IMODE(destination.stat().st_mode)
                destination.chmod(0o600 if mode != 0o600 else 0o640)
                destination.chmod(mode)
                mutated = True
            return None

        with self.assertRaises(build.Task8Error):
            build.copy_sandbox_runtime_reports(transaction, fault=mutate)
        self.assertTrue(mutated)

    def test_sandbox_reservation_crash_recovers_without_touching_foreign_sibling(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        binding = self._attested_sandbox_environment(transaction)
        sibling = binding.automation_reports_root / "foreign.txt"
        sibling.write_bytes(b"foreign")
        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(
                transaction, binding,
                fault=lambda point: (
                    "crash" if point == "AFTER_SANDBOX_RESERVED_DURABLE"
                    else None))
        self.assertEqual(transaction.journal["sandboxScratch"]["state"], "RESERVED")
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD)
        self.assertEqual(sibling.read_bytes(), b"foreign")
        self.assertIsNone(recovered.journal["sandboxScratch"])
        self.assertEqual(recovered.journal["phase"], "ROLLED_BACK")

    def test_sandbox_prepare_durable_seams_fail_or_crash_recover_exact_state(self):
        cases = (
            SandboxFaultCase("AFTER_SANDBOX_RUNTIME_REPORT_PROBE", None),
            SandboxFaultCase("AFTER_SANDBOX_PLANNED_DURABLE", "PLANNED"),
            SandboxFaultCase("AFTER_SANDBOX_RESERVATION_CREATE", "PLANNED"),
            SandboxFaultCase("AFTER_SANDBOX_RESERVATION_FILE_SYNC", "PLANNED"),
            SandboxFaultCase("AFTER_SANDBOX_RESERVATION_READBACK", "PLANNED"),
            SandboxFaultCase("AFTER_SANDBOX_RESERVATION_PARENT_SYNC", "PLANNED"),
            SandboxFaultCase("AFTER_SANDBOX_RESERVED_DURABLE", "RESERVED"),
            SandboxFaultCase("AFTER_SANDBOX_OWNER_ROOT_CREATE", "RESERVED"),
            SandboxFaultCase(
                "AFTER_SANDBOX_OWNER_ROOT_IDENTITY_DURABLE", "RESERVED"),
            SandboxFaultCase("AFTER_SANDBOX_MARKER_FILE_SYNC", "RESERVED"),
            SandboxFaultCase("AFTER_SANDBOX_MARKER_READBACK", "RESERVED"),
            SandboxFaultCase("AFTER_SANDBOX_REPORT_ROOT_CREATE", "RESERVED"),
            SandboxFaultCase("AFTER_SANDBOX_PREPARED_DURABLE", "PREPARED"),
        )
        original_repo = self.repo
        for case in cases:
            for action in ("fail", "crash"):
                with self.subTest(point=case.point, action=action):
                    case_repo = FakeRepo()
                    transaction = None
                    recovered = None
                    self.repo = case_repo
                    try:
                        transaction = build.OuterTransaction.begin(
                            case_repo.root, TXN, HEAD)
                        binding = self._attested_sandbox_environment(transaction)
                        sibling = binding.automation_reports_root / "foreign.txt"
                        sibling.write_bytes(b"foreign")
                        sibling.chmod(0o640)
                        sibling_before = build._regular_fingerprint(sibling.stat())
                        reservation = (
                            binding.automation_reports_root /
                            f".CorsairsTerrainTask8-{TXN}.reservation")
                        transaction_root = (
                            binding.automation_reports_root /
                            f"CorsairsTerrainTask8-{TXN}")
                        fault, occurrences = self._fault_for_case(case, action)
                        expected_error = (
                            build.SimulatedCrash if action == "crash"
                            else build.Task8Error)
                        with self.assertRaises(expected_error) as caught:
                            build.prepare_sandbox_report_scratch(
                                transaction, binding, fault=fault)
                        if action == "fail":
                            self.assertIs(type(caught.exception), build.Task8Error)
                        self.assertEqual(occurrences(), case.occurrence)
                        durable = build._strict_json(
                            build.journal_path(case_repo.root))
                        scratch = durable["sandboxScratch"]
                        if case.expected_state is None:
                            self.assertIsNone(scratch)
                        else:
                            self.assertIsInstance(scratch, dict)
                            self.assertEqual(scratch["state"], case.expected_state)
                        if case.point in {
                                "AFTER_SANDBOX_RESERVATION_CREATE",
                                "AFTER_SANDBOX_RESERVATION_FILE_SYNC",
                                "AFTER_SANDBOX_RESERVATION_READBACK",
                                "AFTER_SANDBOX_RESERVATION_PARENT_SYNC"}:
                            self.assertEqual(scratch["reservationIdentity"], [])
                        if case.point in {
                                "AFTER_SANDBOX_RESERVED_DURABLE",
                                "AFTER_SANDBOX_OWNER_ROOT_CREATE"}:
                            self.assertNotEqual(scratch["reservationIdentity"], [])
                            self.assertEqual(scratch["transactionIdentity"], [])
                        if case.point in {
                                "AFTER_SANDBOX_OWNER_ROOT_IDENTITY_DURABLE",
                                "AFTER_SANDBOX_MARKER_FILE_SYNC",
                                "AFTER_SANDBOX_MARKER_READBACK",
                                "AFTER_SANDBOX_REPORT_ROOT_CREATE"}:
                            self.assertNotEqual(scratch["transactionIdentity"], [])
                            self.assertEqual(scratch["reportIdentity"], [])
                        if case.point == "AFTER_SANDBOX_PREPARED_DURABLE":
                            self.assertNotEqual(scratch["reportIdentity"], [])

                        if action == "fail":
                            transaction.rollback()
                            terminal = transaction
                        else:
                            transaction.abandon_for_test()
                            transaction = None
                            recovered = build.OuterTransaction.acquire(case_repo.root)
                            recovered.recover(HEAD)
                            terminal = recovered
                        self.assertEqual(terminal.journal["phase"], "ROLLED_BACK")
                        self.assertIsNone(terminal.journal["sandboxScratch"])
                        self.assertFalse(reservation.exists())
                        self.assertFalse(reservation.is_symlink())
                        self.assertFalse(transaction_root.exists())
                        self.assertFalse(transaction_root.is_symlink())
                        self.assertEqual(sibling.read_bytes(), b"foreign")
                        self.assertEqual(
                            build._regular_fingerprint(sibling.stat()),
                            sibling_before)
                    finally:
                        if transaction is not None:
                            transaction.close()
                        if recovered is not None:
                            recovered.close()
                        self.repo = original_repo
                        case_repo.close()

    def test_sandbox_copy_durable_seams_fail_or_crash_recover_exact_state(self):
        cases = (
            SandboxFaultCase(
                "AFTER_SANDBOX_REPORT_DESTINATION_FILE_SYNC", "PREPARED"),
            SandboxFaultCase(
                "AFTER_SANDBOX_REPORT_COPY_REVALIDATED", "PREPARED"),
            SandboxFaultCase("AFTER_SANDBOX_COPIED_DURABLE", "COPIED"),
        )
        original_repo = self.repo
        for case in cases:
            for action in ("fail", "crash"):
                with self.subTest(point=case.point, action=action):
                    case_repo = FakeRepo()
                    transaction = None
                    recovered = None
                    self.repo = case_repo
                    try:
                        transaction = build.OuterTransaction.begin(
                            case_repo.root, TXN, HEAD)
                        binding = self._attested_sandbox_environment(transaction)
                        sibling = binding.automation_reports_root / "foreign.txt"
                        sibling.write_bytes(b"foreign")
                        sibling.chmod(0o640)
                        sibling_before = build._regular_fingerprint(sibling.stat())
                        fault, occurrences = self._fault_for_case(case, action)
                        scratch = build.prepare_sandbox_report_scratch(
                            transaction, binding, fault=fault)
                        payload = b'{"succeeded":1}\n'
                        (scratch.report_root / "index.json").write_bytes(payload)
                        expected_error = (
                            build.SimulatedCrash if action == "crash"
                            else build.Task8Error)
                        with self.assertRaises(expected_error) as caught:
                            build.copy_sandbox_runtime_reports(
                                transaction, fault=fault)
                        if action == "fail":
                            self.assertIs(type(caught.exception), build.Task8Error)
                        self.assertEqual(occurrences(), case.occurrence)
                        durable = build._strict_json(
                            build.journal_path(case_repo.root))
                        self.assertEqual(
                            durable["sandboxScratch"]["state"],
                            case.expected_state)
                        destination_index = scratch.destination_root / "index.json"
                        self.assertEqual(destination_index.read_bytes(), payload)

                        if action == "fail":
                            transaction.rollback()
                            terminal = transaction
                        else:
                            transaction.abandon_for_test()
                            transaction = None
                            recovered = build.OuterTransaction.acquire(case_repo.root)
                            recovered.recover(HEAD)
                            terminal = recovered
                        self.assertEqual(terminal.journal["phase"], "ROLLED_BACK")
                        self.assertIsNone(terminal.journal["sandboxScratch"])
                        self.assertFalse(scratch.reservation_path.exists())
                        self.assertFalse(scratch.reservation_path.is_symlink())
                        self.assertFalse(scratch.transaction_root.exists())
                        self.assertFalse(scratch.transaction_root.is_symlink())
                        self.assertEqual(destination_index.read_bytes(), payload)
                        self.assertEqual(sibling.read_bytes(), b"foreign")
                        self.assertEqual(
                            build._regular_fingerprint(sibling.stat()),
                            sibling_before)
                    finally:
                        if transaction is not None:
                            transaction.close()
                        if recovered is not None:
                            recovered.close()
                        self.repo = original_repo
                        case_repo.close()

    def test_sandbox_cleanup_durable_seams_fail_or_crash_recover_exact_state(self):
        cases = (
            SandboxFaultCase("BEFORE_SANDBOX_CLEANING", "COPIED"),
            SandboxFaultCase("AFTER_SANDBOX_CLEANING_DURABLE", "CLEANING"),
            *(SandboxFaultCase(
                "BEFORE_SANDBOX_DESCENDANT_REMOVE", "CLEANING", occurrence)
              for occurrence in range(1, 5)),
            *(SandboxFaultCase(
                "AFTER_SANDBOX_DESCENDANT_REMOVE", "CLEANING", occurrence)
              for occurrence in range(1, 5)),
            SandboxFaultCase(
                "AFTER_SANDBOX_TRANSACTION_ROOT_REMOVE", "CLEANING"),
            SandboxFaultCase(
                "AFTER_SANDBOX_TRANSACTION_PARENT_SYNC", "CLEANING"),
            SandboxFaultCase(
                "BEFORE_SANDBOX_RESERVATION_REMOVE", "CLEANING"),
            SandboxFaultCase(
                "AFTER_SANDBOX_RESERVATION_REMOVE", "CLEANING"),
            SandboxFaultCase(
                "AFTER_SANDBOX_RESERVATION_PARENT_SYNC", "CLEANING", 2),
            SandboxFaultCase("AFTER_SANDBOX_CLEANED_DURABLE", "CLEANED"),
            SandboxFaultCase("AFTER_SANDBOX_SCRATCH_NULL_DURABLE", None),
        )
        original_repo = self.repo
        for case in cases:
            for action in ("fail", "crash"):
                with self.subTest(
                        point=case.point,
                        occurrence=case.occurrence,
                        action=action):
                    case_repo = FakeRepo()
                    transaction = None
                    recovered = None
                    self.repo = case_repo
                    try:
                        transaction = build.OuterTransaction.begin(
                            case_repo.root, TXN, HEAD)
                        binding = self._attested_sandbox_environment(transaction)
                        sibling = binding.automation_reports_root / "foreign.txt"
                        sibling.write_bytes(b"foreign")
                        sibling.chmod(0o640)
                        sibling_before = build._regular_fingerprint(sibling.stat())
                        fault, occurrences = self._fault_for_case(case, action)
                        scratch = build.prepare_sandbox_report_scratch(
                            transaction, binding, fault=fault)
                        payload = b'{"succeeded":1}\n'
                        (scratch.report_root / "index.json").write_bytes(payload)
                        build.copy_sandbox_runtime_reports(
                            transaction, fault=fault)
                        destination_index = scratch.destination_root / "index.json"
                        self.assertEqual(destination_index.read_bytes(), payload)
                        expected_error = (
                            build.SimulatedCrash if action == "crash"
                            else build.Task8Error)
                        with self.assertRaises(expected_error) as caught:
                            build.cleanup_sandbox_report_scratch(
                                transaction, fault=fault)
                        if action == "fail":
                            self.assertIs(type(caught.exception), build.Task8Error)
                        self.assertEqual(occurrences(), case.occurrence)
                        durable = build._strict_json(
                            build.journal_path(case_repo.root))
                        durable_scratch = durable["sandboxScratch"]
                        if case.expected_state is None:
                            self.assertIsNone(durable_scratch)
                        else:
                            self.assertIsInstance(durable_scratch, dict)
                            self.assertEqual(
                                durable_scratch["state"], case.expected_state)

                        if action == "fail":
                            transaction.rollback()
                            terminal = transaction
                        else:
                            transaction.abandon_for_test()
                            transaction = None
                            recovered = build.OuterTransaction.acquire(case_repo.root)
                            recovered.recover(HEAD)
                            terminal = recovered
                        self.assertEqual(terminal.journal["phase"], "ROLLED_BACK")
                        self.assertIsNone(terminal.journal["sandboxScratch"])
                        self.assertFalse(scratch.reservation_path.exists())
                        self.assertFalse(scratch.reservation_path.is_symlink())
                        self.assertFalse(scratch.transaction_root.exists())
                        self.assertFalse(scratch.transaction_root.is_symlink())
                        self.assertEqual(destination_index.read_bytes(), payload)
                        self.assertEqual(sibling.read_bytes(), b"foreign")
                        self.assertEqual(
                            build._regular_fingerprint(sibling.stat()),
                            sibling_before)
                    finally:
                        if transaction is not None:
                            transaction.close()
                        if recovered is not None:
                            recovered.close()
                        self.repo = original_repo
                        case_repo.close()

    def test_preexisting_sandbox_reservation_is_rejected_and_preserved(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        binding = self._attested_sandbox_environment(transaction)
        reservation = (
            binding.automation_reports_root /
            f".CorsairsTerrainTask8-{TXN}.reservation")
        reservation.write_bytes(b"foreign")
        reservation.chmod(0o600)
        with self.assertRaises(build.Task8Error):
            build.prepare_sandbox_report_scratch(transaction, binding)
        self.assertEqual(reservation.read_bytes(), b"foreign")
        self.assertFalse(
            (binding.automation_reports_root /
             f"CorsairsTerrainTask8-{TXN}").exists())

    def test_sandbox_cleanup_rejects_changed_owner_marker(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        marker = scratch.transaction_root / "owner.json"
        marker.write_bytes(b"foreign\n")
        with self.assertRaises(build.Task8Error) as caught:
            build.cleanup_sandbox_report_scratch(transaction)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(scratch.transaction_root.is_dir())
        self.assertEqual(marker.read_bytes(), b"foreign\n")
        transaction.abandon_for_test()

    def test_sandbox_cleanup_rejects_unexpected_transaction_root_child(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        foreign = scratch.transaction_root / "foreign.txt"
        foreign.write_bytes(b"foreign")
        with self.assertRaises(build.Task8Error) as caught:
            build.cleanup_sandbox_report_scratch(transaction)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(scratch.transaction_root.is_dir())
        self.assertEqual(foreign.read_bytes(), b"foreign")
        transaction.abandon_for_test()

    def test_sandbox_cleanup_never_follows_a_swapped_report_root(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        (scratch.report_root / "owned.json").write_bytes(b"owned")
        detached = scratch.transaction_root / "detached-report"
        outside = self.repo.root / "outside-sandbox-cleanup"
        outside.mkdir()
        victim = outside / "victim.json"
        victim.write_bytes(b"foreign")
        outside_before = [
            (child.relative_to(outside).as_posix(),
             hashlib.sha256(child.read_bytes()).hexdigest())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ]
        original_scandir = os.scandir
        original_listdir = os.listdir
        swapped = False

        def swap_report_root():
            nonlocal swapped
            if swapped:
                return
            scratch.report_root.rename(detached)
            scratch.report_root.symlink_to(outside, target_is_directory=True)
            swapped = True

        def racing_scandir(path):
            if Path(path) == scratch.report_root:
                swap_report_root()
            return original_scandir(path)

        def racing_listdir(path):
            if isinstance(path, int):
                swap_report_root()
            return original_listdir(path)

        with mock.patch.object(build.os, "scandir", side_effect=racing_scandir), \
                mock.patch.object(build.os, "listdir", side_effect=racing_listdir):
            with self.assertRaises(build.Task8Error) as caught:
                build._remove_owned_sandbox_subtree(
                    scratch.report_root,
                    transaction.journal["sandboxScratch"]["reportIdentity"],
                    transaction,
                    None,
                )
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual([
            (child.relative_to(outside).as_posix(),
             hashlib.sha256(child.read_bytes()).hexdigest())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ], outside_before)
        transaction.abandon_for_test()

    def test_sandbox_cleanup_nested_swap_preserves_outside_tree(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        nested = scratch.report_root / "nested"
        nested.mkdir(mode=0o700)
        (nested / "same.json").write_bytes(b"owned")
        detached = scratch.report_root / "nested-detached"
        outside = self.repo.root / "outside-nested-cleanup"
        outside.mkdir()
        (outside / "same.json").write_bytes(b"foreign")
        outside_before = [
            (child.relative_to(outside).as_posix(), child.read_bytes())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ]
        swapped = False

        def swap_nested_at_remove(point):
            nonlocal swapped
            if point == "BEFORE_SANDBOX_DESCENDANT_REMOVE" and not swapped:
                nested.rename(detached)
                nested.symlink_to(outside, target_is_directory=True)
                swapped = True
            return None

        with self.assertRaises(build.Task8Error) as caught:
            build.cleanup_sandbox_report_scratch(
                transaction, fault=swap_nested_at_remove)
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual([
            (child.relative_to(outside).as_posix(), child.read_bytes())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ], outside_before)
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "CLEANING")
        transaction.abandon_for_test()

    def test_sandbox_cleanup_parent_swap_preserves_outside_reservation(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        parent = scratch.reservation_path.parent
        detached = parent.with_name(parent.name + "-detached")
        outside = self.repo.root / "outside-scratch-parent"
        outside.mkdir()
        outside_reservation = outside / scratch.reservation_path.name
        outside_reservation.write_bytes(b"foreign-reservation")
        outside_before = outside_reservation.read_bytes()
        swapped = False

        def swap_parent_at_reservation(point):
            nonlocal swapped
            if point == "BEFORE_SANDBOX_RESERVATION_REMOVE" and not swapped:
                parent.rename(detached)
                parent.symlink_to(outside, target_is_directory=True)
                swapped = True
            return None

        with self.assertRaises(build.Task8Error) as caught:
            build.cleanup_sandbox_report_scratch(
                transaction, fault=swap_parent_at_reservation)
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(outside_reservation.read_bytes(), outside_before)
        self.assertIsNotNone(transaction.journal["sandboxScratch"])
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "CLEANING")
        transaction.abandon_for_test()

    def test_sandbox_cleanup_closes_nested_descriptor_after_failure(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        nested = scratch.report_root / "nested"
        nested.mkdir(mode=0o700)
        (nested / "unsafe").symlink_to(self.repo.root / "foreign")
        before = len(os.listdir("/dev/fd"))
        for _ in range(8):
            with self.assertRaises(build.Task8Error):
                build.cleanup_sandbox_report_scratch(transaction)
        after = len(os.listdir("/dev/fd"))
        self.assertLessEqual(after, before)
        self.assertIsNotNone(transaction.journal["sandboxScratch"])
        transaction.abandon_for_test()

    def test_regular_unlink_rechecks_hardlink_count_after_fault_hook(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        parent = self.repo.root / "unlink-hook"
        parent.mkdir()
        owned = parent / "owned.json"
        owned.write_bytes(b"owned")
        sibling = parent / "same-inode.json"
        parent_descriptor, _ = build._open_pinned_directory(parent)
        descriptor, info = build._open_regular_at(parent_descriptor, owned.name)

        def add_hardlink(point):
            if point == "BEFORE_SANDBOX_DESCENDANT_REMOVE":
                os.link(owned, sibling)
            return None

        try:
            with self.assertRaises(build.Task8Error) as caught:
                build._unlink_open_regular_at(
                    parent_descriptor, owned.name, descriptor, info,
                    transaction, add_hardlink)
            descriptor = -1
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            os.close(parent_descriptor)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(owned.read_bytes(), b"owned")
        self.assertEqual(sibling.read_bytes(), b"owned")

    def test_transaction_root_rmdir_syscall_swap_is_detected(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        away = scratch.transaction_root.with_name(
            scratch.transaction_root.name + "-away")
        sibling = scratch.transaction_root.parent / "foreign-sibling"
        sibling.mkdir()
        victim = sibling / "victim.json"
        victim.write_bytes(b"foreign")
        original_rmdir = os.rmdir
        substituted = False

        def swap_at_rmdir(name, *, dir_fd=None):
            nonlocal substituted
            if (name == scratch.transaction_root.name and
                    dir_fd is not None and not substituted):
                os.rename(
                    name, away.name, src_dir_fd=dir_fd, dst_dir_fd=dir_fd)
                os.mkdir(name, 0o700, dir_fd=dir_fd)
                substituted = True
            return original_rmdir(name, dir_fd=dir_fd)

        with mock.patch.object(build.os, "rmdir", side_effect=swap_at_rmdir):
            with self.assertRaises(build.Task8Error) as caught:
                build.cleanup_sandbox_report_scratch(transaction)
        self.assertTrue(substituted)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(away.is_dir())
        self.assertEqual(victim.read_bytes(), b"foreign")
        self.assertEqual(
            transaction.journal["sandboxScratch"]["state"], "CLEANING")
        transaction.abandon_for_test()

    def test_descriptor_relative_directory_remove_succeeds_normally_on_apfs(self):
        parent = self.repo.root / "normal-rmdir"
        parent.mkdir()
        child = parent / "owned"
        child.mkdir(mode=0o700)
        parent_descriptor, _ = build._open_pinned_directory(parent)
        child_descriptor, child_info = build._open_directory_at(
            parent_descriptor, child.name)
        try:
            build._remove_open_directory_at(
                parent_descriptor, child.name, child_descriptor, child_info,
                None, None)
            child_descriptor = -1
        finally:
            if child_descriptor >= 0:
                os.close(child_descriptor)
            os.close(parent_descriptor)
        self.assertFalse(child.exists())

    def test_recovery_cleanup_never_follows_a_swapped_recovery_root(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        recovery = build.recovery_root(self.repo.root, TXN)
        (recovery / "owned.json").write_bytes(b"owned")
        detached = recovery.with_name(recovery.name + "-detached")
        outside = self.repo.root / "outside-recovery-cleanup"
        outside.mkdir()
        victim = outside / "victim.json"
        victim.write_bytes(b"foreign")
        outside_before = [
            (child.relative_to(outside).as_posix(),
             hashlib.sha256(child.read_bytes()).hexdigest())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ]
        original_walk = os.walk
        original_listdir = os.listdir
        swapped = False

        def swap_recovery_root():
            nonlocal swapped
            if swapped:
                return
            recovery.rename(detached)
            recovery.symlink_to(outside, target_is_directory=True)
            swapped = True

        def racing_walk(top, *args, **kwargs):
            if Path(top) == recovery:
                swap_recovery_root()
            return original_walk(top, *args, **kwargs)

        def racing_listdir(path):
            if isinstance(path, int):
                swap_recovery_root()
            return original_listdir(path)

        with mock.patch.object(build.os, "walk", side_effect=racing_walk), \
                mock.patch.object(build.os, "listdir", side_effect=racing_listdir):
            with self.assertRaises(build.Task8Error) as caught:
                transaction._remove_recovery_material()
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual([
            (child.relative_to(outside).as_posix(),
             hashlib.sha256(child.read_bytes()).hexdigest())
            for child in sorted(outside.rglob("*")) if child.is_file()
        ], outside_before)
        transaction.abandon_for_test()

    def test_recovery_cleanup_revalidates_swapped_recovery_parent(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        recovery = build.recovery_root(self.repo.root, TXN)
        parent = recovery.parent
        detached = parent.with_name(parent.name + "-detached")
        outside = self.repo.root / "outside-recovery-parent"
        outside.mkdir()
        outside_txn = outside / recovery.name
        outside_txn.mkdir()
        victim = outside_txn / "victim.json"
        victim.write_bytes(b"foreign")
        original_remove = build._remove_open_directory_at
        swapped = False

        def swap_after_recovery_root(*args, **kwargs):
            nonlocal swapped
            result = original_remove(*args, **kwargs)
            if args[1] == recovery.name and not swapped:
                parent.rename(detached)
                parent.symlink_to(outside, target_is_directory=True)
                swapped = True
            return result

        with mock.patch.object(
                build, "_remove_open_directory_at",
                side_effect=swap_after_recovery_root):
            with self.assertRaises(build.Task8Error) as caught:
                transaction._remove_recovery_material()
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(victim.read_bytes(), b"foreign")
        transaction.abandon_for_test()

    def test_absent_recovery_root_still_reopens_canonical_parent(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        recovery = build.recovery_root(self.repo.root, TXN)
        transaction.rollback()
        transaction._remove_recovery_material()
        parent = recovery.parent
        detached = parent.with_name(parent.name + "-absent-detached")
        outside = self.repo.root / "outside-absent-recovery-parent"
        outside.mkdir()
        victim = outside / "victim.json"
        victim.write_bytes(b"foreign")
        original_entry = build._entry_stat_at
        swapped = False

        def swap_after_absence(parent_descriptor, name, **kwargs):
            nonlocal swapped
            result = original_entry(parent_descriptor, name, **kwargs)
            if name == recovery.name and result is None and not swapped:
                parent.rename(detached)
                parent.symlink_to(outside, target_is_directory=True)
                swapped = True
            return result

        with mock.patch.object(
                build, "_entry_stat_at", side_effect=swap_after_absence):
            with self.assertRaises(build.Task8Error) as caught:
                transaction._remove_recovery_material()
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(victim.read_bytes(), b"foreign")
        transaction.abandon_for_test()

    def test_sandbox_cleanup_replays_after_durable_root_removal(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        binding = self._attested_sandbox_environment(transaction)
        scratch = build.prepare_sandbox_report_scratch(transaction, binding)
        (scratch.report_root / "index.json").write_bytes(b'{"succeeded":1}\n')
        build.copy_sandbox_runtime_reports(transaction)
        with self.assertRaises(build.Task8Error):
            build.cleanup_sandbox_report_scratch(
                transaction,
                fault=lambda point: (
                    "crash" if point == "AFTER_SANDBOX_TRANSACTION_ROOT_REMOVE"
                    else None))
        self.assertEqual(transaction.journal["sandboxScratch"]["state"], "CLEANING")
        self.assertFalse(scratch.transaction_root.exists())
        self.assertTrue(scratch.reservation_path.is_file())
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD)
        self.assertEqual(recovered.journal["phase"], "ROLLED_BACK")
        self.assertIsNone(recovered.journal["sandboxScratch"])
        self.assertFalse(scratch.reservation_path.exists())
        self.assertTrue(scratch.destination_root.joinpath("index.json").is_file())

    def test_signed_app_attestation_binds_probe_without_publishing_absolute_paths(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        probe = self._sandbox_probe()
        container = Path(probe["containerDataRoot"]).parent
        metadata = container / ".com.apple.containermanagerd.metadata.plist"
        metadata.write_bytes(plistlib.dumps({
            "MCMMetadataCreator": probe["bundleIdentifier"],
            "MCMMetadataIdentifier": probe["bundleIdentifier"],
            "private-path": str(container),
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        app = (
            build.package_run_root(self.repo.root, TXN) /
            "archive/Mac/Actual.app")
        executable_path = app / "Contents/MacOS/Actual"
        executable_path.parent.mkdir(parents=True)
        executable_path.write_bytes(b"binary")
        executable_path.chmod(0o755)
        info = app / "Contents/Info.plist"
        info.write_bytes(plistlib.dumps({
            "CFBundleIdentifier": probe["bundleIdentifier"],
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        executable = build._evidence(executable_path, self.repo.root)
        entitlements = plistlib.dumps({
            "com.apple.security.app-sandbox": True,
            "com.apple.security.get-task-allow": True,
        }, fmt=plistlib.FMT_XML, sort_keys=True).decode("utf-8")
        commands = []

        def runner(command):
            commands.append(command.argv)
            if "--verify" in command.argv:
                return build.CommandResult(0)
            if "--entitlements" in command.argv:
                return build.CommandResult(0, "Executable=Actual\n" + entitlements)
            return build.CommandResult(
                0, stderr=f"Identifier={probe['bundleIdentifier']}\n")

        verified = build.verify_signed_sandbox_app(
            transaction, HEAD, executable, command_runner=runner)
        probe_command = build.production_commands(
            self.repo.root, TXN, HEAD)[-2]
        resolved = build.resolve_runtime_command(
            probe_command, self.repo.root, verified,
            transaction=transaction)
        self.assertEqual(resolved.argv[3], str(executable_path))
        executable_path.write_bytes(b"changed")
        with self.assertRaises(build.Task8Error):
            build.resolve_runtime_command(
                probe_command, self.repo.root, verified,
                transaction=transaction)
        executable_path.write_bytes(b"binary")
        environment = build.prepare_sandbox_attestation(
            transaction, HEAD, verified, probe)
        self.assertIsInstance(environment, build.AttestedSandboxEnvironment)
        evidence = environment.attestation_evidence
        report = json.loads(
            (self.repo.root / evidence["path"]).read_text(encoding="utf-8"))
        self.assertEqual(
            build.rules.validate_sandbox_attestation(report, self.repo.root), [])
        serialized = json.dumps(report, sort_keys=True)
        self.assertNotIn(probe["containerDataRoot"], serialized)
        self.assertNotIn(probe["automationReportsRoot"], serialized)
        self.assertNotIn("private-path", serialized)
        self.assertEqual(len(commands), 4)

    def test_signed_app_verification_rejects_info_mutation_during_codesign(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        seed = self._verified_sandbox_app()
        info_path = self.repo.root / seed.info_plist["path"]
        replacement = plistlib.dumps({
            "CFBundleIdentifier": seed.bundle_identifier,
            "MutatedDuringCodesign": True,
        }, fmt=plistlib.FMT_XML, sort_keys=True)
        mutated = False

        def runner(command):
            nonlocal mutated
            if "--verify" in command.argv:
                info_path.write_bytes(replacement)
                mutated = True
                return build.CommandResult(0)
            if "--entitlements" in command.argv:
                return build.CommandResult(
                    0, plistlib.dumps({
                        "com.apple.security.app-sandbox": True,
                    }, fmt=plistlib.FMT_XML, sort_keys=True).decode("utf-8"))
            return build.CommandResult(
                0, stderr=f"Identifier={seed.bundle_identifier}\n")

        with self.assertRaises(build.Task8Error):
            build.verify_signed_sandbox_app(
                transaction, HEAD, seed.packaged_executable,
                command_runner=runner)
        self.assertTrue(mutated)
        self.assertEqual(info_path.read_bytes(), replacement)

    def test_runtime_command_uses_receipt_inventory_executable(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        command = build.production_commands(self.repo.root, TXN, HEAD)[-2]
        self.assertIn(build.ARCHIVE_EXECUTABLE_TOKEN, command.argv)
        verified = self._verified_sandbox_app()
        resolved = build.resolve_runtime_command(
            command, self.repo.root, verified, transaction=transaction)
        self.assertNotIn(build.ARCHIVE_EXECUTABLE_TOKEN, resolved.argv)
        self.assertEqual(
            resolved.argv[3],
            str(self.repo.root / verified.packaged_executable["path"]))

    def test_runtime_report_directory_is_exact_owned_empty_and_hashed(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        scratch = build.prepare_sandbox_report_scratch(
            transaction, self._attested_sandbox_environment(transaction))
        expected = (
            build.evidence_root(self.repo.root, TXN) /
            "reference-terrain-runtime")
        self.assertFalse(expected.exists())

        command = build.production_commands(self.repo.root, TXN, HEAD)[-1]
        verified = self._verified_sandbox_app()
        resolved = build.resolve_runtime_command(
            command, self.repo.root, verified, scratch,
            transaction=transaction)
        self.assertEqual(
            tuple(item for item in resolved.argv
                  if item.startswith("-ReportExportPath=")),
            (f"-ReportExportPath={scratch.report_root}",),
        )
        marker = scratch.transaction_root / "owner.json"
        marker_payload = marker.read_bytes()
        marker.write_bytes(b"changed\n")
        with self.assertRaises(build.Task8Error):
            build.resolve_runtime_command(
                command, self.repo.root, verified, scratch,
                transaction=transaction)
        marker.write_bytes(marker_payload)

        index = scratch.report_root / "index.json"
        index.write_bytes(b'{"succeeded":1}\n')
        owned = build.copy_sandbox_runtime_reports(transaction)
        copied_index = expected / "index.json"
        evidence = build._evidence_set(
            expected, self.repo.root, TXN, HEAD,
            expected_directory=owned)
        self.assertEqual(evidence["files"], [{
            "path": copied_index.relative_to(self.repo.root).as_posix(),
            "sha256": hashlib.sha256(copied_index.read_bytes()).hexdigest(),
            "sizeBytes": copied_index.stat().st_size,
        }])

        build.cleanup_sandbox_report_scratch(transaction)
        transaction.rollback()
        transaction.finish()
        self.assertEqual(copied_index.read_bytes(), b'{"succeeded":1}\n')
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

    def test_cook_evidence_defers_runtime_scratch_until_probe(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        self.addCleanup(transaction.close)
        context = {"gameBuild": {"target": "game"}}
        order = []
        verified = object()

        def prepare_package(*_args):
            order.append("package-evidence")
            return {
                "package": "prepared",
                "packagedExecutable": "executable-evidence",
            }

        def verify_app(*_args):
            order.append("signed-app-verification")
            return verified

        with mock.patch.object(
                build, "prepare_package_evidence", side_effect=prepare_package), \
                mock.patch.object(
                    build, "verify_signed_sandbox_app",
                    side_effect=verify_app):
            build._production_after_step(
                transaction, context, HEAD,
                build.CommandSpec("cook-package", ("unused",)),
                build.CommandResult(0))

        self.assertEqual(order, ["package-evidence", "signed-app-verification"])
        self.assertIs(context["verifiedSandboxApp"], verified)
        self.assertNotIn("runtimeReportDirectory", context)

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
        self.assertTrue(build._normalized_relative("Reports"))
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
        payload = build.rules.canonical_json_bytes({}) + b"\n"
        bundle = self.repo.write(build.BASE_BUNDLE, payload, 0o600)
        transaction.journal["phase"] = "COMMITTED"
        transaction.journal["intendedBundleSha256"] = hashlib.sha256(
            payload).hexdigest()
        transaction.persist()
        with mock.patch.object(
                build.rules, "validate_base_bundle", return_value=[]):
            transaction.finish(status_gate=lambda: (HEAD, ""))
        self.assertFalse(build.journal_path(self.repo.root).exists())
        self.assertFalse(build.recovery_root(self.repo.root, TXN).exists())
        self.assertEqual(bundle.read_bytes(), payload)
        self.assertTrue(build.lock_path(self.repo.root).is_file())
        self.assertEqual(
            build.lock_path(self.repo.root).read_bytes(), build.LOCK_MARKER)

    def test_committed_recovery_revalidates_bundle_hash_validator_and_clean_head(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        payload = build.rules.canonical_json_bytes({}) + b"\n"
        bundle = self.repo.write(build.BASE_BUNDLE, payload, 0o600)
        transaction.journal["phase"] = "COMMITTED"
        transaction.journal["intendedBundleSha256"] = hashlib.sha256(
            payload).hexdigest()
        transaction.persist()
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        bundle.write_bytes(b"tampered\n")
        with mock.patch.object(
                build.rules, "validate_base_bundle", return_value=[]), \
                self.assertRaises(build.Task8Error) as hash_error:
            recovered.recover(HEAD, status_gate=lambda: (HEAD, ""))
        self.assertEqual(hash_error.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(build.journal_path(self.repo.root).is_file())

        bundle.write_bytes(payload)
        with self.assertRaises(build.Task8Error) as validator_error:
            recovered.recover(HEAD, status_gate=lambda: (HEAD, ""))
        self.assertEqual(validator_error.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(build.journal_path(self.repo.root).is_file())

        with mock.patch.object(
                build.rules, "validate_base_bundle", return_value=[]), \
                self.assertRaises(build.Task8Error) as dirty_error:
            recovered.recover(HEAD, status_gate=lambda: (HEAD, " M tracked"))
        self.assertEqual(dirty_error.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(build.journal_path(self.repo.root).is_file())

        with mock.patch.object(
                build.rules, "validate_base_bundle", return_value=[]):
            recovered.recover(HEAD, status_gate=lambda: (HEAD, ""))

    def test_rolled_back_recovery_revalidates_snapshot_and_family_absence(self):
        primary = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["mesh"] + ".uasset", b"old", 0o600)
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        transaction.rollback()
        introduced = self.repo.write(
            build.MANAGED_PACKAGE_STEMS["mesh"] + ".uexp", b"foreign", 0o600)
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        with self.assertRaises(build.Task8Error) as caught:
            recovered.recover(HEAD)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(primary.read_bytes(), b"old")
        self.assertEqual(introduced.read_bytes(), b"foreign")
        self.assertTrue(build.journal_path(self.repo.root).is_file())

    def test_finish_never_follows_a_swapped_reports_parent_for_journal_unlink(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        transaction.rollback()
        reports = self.repo.root / "artifacts/maps/reports"
        journal = build.journal_path(self.repo.root)
        journal_payload = journal.read_bytes()
        detached = reports.with_name("reports-detached")
        outside = self.repo.root / "outside-reports-parent"
        outside.mkdir()
        outside_journal = outside / journal.name
        outside_journal.write_bytes(journal_payload)
        outside_journal.chmod(0o600)
        original_remove = transaction._remove_recovery_material
        swapped = False

        def remove_then_swap():
            nonlocal swapped
            original_remove()
            reports.rename(detached)
            reports.symlink_to(outside, target_is_directory=True)
            swapped = True

        with mock.patch.object(
                transaction, "_remove_recovery_material",
                side_effect=remove_then_swap):
            with self.assertRaises(build.Task8Error) as caught:
                transaction.finish()
        self.assertTrue(swapped)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(outside_journal.read_bytes(), journal_payload)
        self.assertEqual((detached / journal.name).read_bytes(), journal_payload)
        transaction.abandon_for_test()

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

    def test_crash_after_release_recovers_live_supervised_process_group(self):
        ready = self.repo.root / "owned-ready"
        finished = self.repo.root / "owned-finished"
        script = (
            "from pathlib import Path; import time; "
            f"Path({str(ready)!r}).write_text('ready'); "
            "time.sleep(3); "
            f"Path({str(finished)!r}).write_text('finished')")
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        command = build.CommandSpec(
            "terrain-reference",
            ("nice", "-n", "10", sys.executable, "-B", "-c", script),
            timeout_seconds=10)

        def crash_after_child_is_live(point):
            if point != "AFTER_OWNED_PROCESS_RELEASE":
                return None
            deadline = time.monotonic() + 2
            while not ready.is_file() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.is_file())
            return "crash"

        with self.assertRaises(build.Task8Error):
            transaction.run_owned_command(
                command, fault=crash_after_child_is_live)
        active = dict(transaction.journal["activeProcess"])
        self.assertIsNotNone(build._probe_process(active["pid"]))
        transaction.abandon_for_test()

        def reap_owned_group(pgid):
            try:
                build._terminate_process_group(pgid)
            except PermissionError:
                # A same-process crash simulation can leave the supervisor as
                # our zombie child until waitpid below; a real restarted
                # recovery process does not own that zombie.
                pass
            try:
                os.waitpid(active["pid"], 0)
            except ChildProcessError:
                pass

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        recovered.recover(HEAD, kill_group=reap_owned_group)
        self.assertEqual(recovered.journal["phase"], "ROLLED_BACK")
        self.assertIsNone(recovered.journal["activeProcess"])
        self.assertIsNone(build._probe_process(active["pid"]))
        self.assertFalse(finished.exists())

    def test_owned_process_nonzero_timeout_and_interrupt_clear_live_group(self):
        cases = (
            ("nonzero", "import sys; sys.exit(7)", 10, None, 7),
            ("timeout", "import time; time.sleep(10)", 1, build.Task8Error, None),
            ("interrupt", "import time; time.sleep(10)", 10,
             KeyboardInterrupt, None),
        )
        for name, script, timeout, expected_error, expected_code in cases:
            with self.subTest(case=name):
                repo = FakeRepo()
                self.addCleanup(repo.close)
                transaction = build.OuterTransaction.begin(
                    repo.root, os.urandom(16).hex(), HEAD)
                active = None
                try:
                    command = build.CommandSpec(
                        "terrain-reference",
                        ("nice", "-n", "10", sys.executable, "-B", "-c", script),
                        timeout_seconds=timeout)
                    if expected_error is None:
                        result = transaction.run_owned_command(command)
                        self.assertEqual(result.returncode, expected_code)
                    else:
                        def interrupt_after_release(point):
                            if (name == "interrupt" and
                                    point == "AFTER_OWNED_PROCESS_RELEASE"):
                                raise KeyboardInterrupt()
                            return None

                        with self.assertRaises(expected_error):
                            transaction.run_owned_command(
                                command, fault=interrupt_after_release)
                    active = transaction.journal.get("activeProcess")
                    self.assertIsNone(active)
                finally:
                    if active is None:
                        active = transaction.journal.get("activeProcess")
                    if active is not None:
                        try:
                            os.killpg(active["pgid"], signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        try:
                            os.waitpid(active["pid"], 0)
                        except ChildProcessError:
                            pass
                    transaction.abandon_for_test()

    def test_owned_success_rejects_and_reaps_forked_same_group_descendant(self):
        ready = self.repo.root / "success-descendant"
        script = "\n".join((
            "from pathlib import Path",
            "import os",
            "import time",
            "child = os.fork()",
            "if child == 0:",
            f"    Path({str(ready)!r}).write_text(str(os.getpid()))",
            "    time.sleep(30)",
            "    os._exit(0)",
            "os._exit(0)",
        ))
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        descendant_pid = None
        try:
            with self.assertRaises(build.Task8Error):
                transaction.run_owned_command(build.CommandSpec(
                    "terrain-reference",
                    ("nice", "-n", "10", sys.executable, "-B", "-c", script),
                    timeout_seconds=10))
            deadline = time.monotonic() + 2
            while not ready.is_file() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.is_file())
            descendant_pid = int(ready.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(descendant_pid, 0)
            self.assertIsNone(transaction.journal["activeProcess"])
            self.assertNotEqual(
                transaction.journal["completedStep"], "terrain-reference")
        finally:
            if descendant_pid is None and ready.is_file():
                descendant_pid = int(ready.read_text())
            if descendant_pid is not None:
                try:
                    os.killpg(os.getpgid(descendant_pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            transaction.abandon_for_test()

    def test_owned_nonzero_reaps_forked_same_group_descendant_before_return(self):
        ready = self.repo.root / "nonzero-descendant"
        script = "\n".join((
            "from pathlib import Path",
            "import os",
            "import time",
            "child = os.fork()",
            "if child == 0:",
            f"    Path({str(ready)!r}).write_text(str(os.getpid()))",
            "    time.sleep(30)",
            "    os._exit(0)",
            "os._exit(7)",
        ))
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        descendant_pid = None
        try:
            result = transaction.run_owned_command(build.CommandSpec(
                "terrain-reference",
                ("nice", "-n", "10", sys.executable, "-B", "-c", script),
                timeout_seconds=10))
            self.assertEqual(result.returncode, 7)
            deadline = time.monotonic() + 2
            while not ready.is_file() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.is_file())
            descendant_pid = int(ready.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(descendant_pid, 0)
            self.assertIsNone(transaction.journal["activeProcess"])
        finally:
            if descendant_pid is None and ready.is_file():
                descendant_pid = int(ready.read_text())
            if descendant_pid is not None:
                try:
                    os.killpg(os.getpgid(descendant_pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            transaction.abandon_for_test()

    def test_recovery_pid_reuse_fails_closed_without_killing_group(self):
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)
        active = {
            "step": "terrain-reference",
            "pid": 42420,
            "pgid": 42420,
            "startToken": "owned-start",
            "executable": "/usr/bin/python3",
            "argvSha256": hashlib.sha256(b"python3").hexdigest(),
        }
        transaction.journal["activeProcess"] = active
        transaction.journal["phase"] = "UNREAL_RUNNING"
        transaction.persist()
        killed = []
        observed = build.ProcessRecord(
            active["pid"], active["pgid"], "reused-start",
            active["executable"], ("python3",))
        with self.assertRaises(build.Task8Error) as caught:
            transaction.recover(
                HEAD,
                process_probe=lambda _pid: observed,
                kill_group=killed.append)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(killed, [])
        self.assertEqual(transaction.journal["activeProcess"], active)
        self.assertTrue(build.journal_path(self.repo.root).is_file())
        transaction.abandon_for_test()

    def test_postwait_pid_reuse_fails_closed_without_signalling_group(self):
        executable = str(Path(sys.executable).resolve())
        active = {
            "step": "terrain-reference",
            "pid": 42421,
            "pgid": 42421,
            "startToken": "owned-start",
            "executable": executable,
            "argvSha256": hashlib.sha256(b"python3").hexdigest(),
        }
        observed = build.ProcessRecord(
            active["pid"], active["pgid"], "reused-start",
            executable, ("python3",))
        killed = []
        with self.assertRaises(build.Task8Error) as caught:
            build._reap_completed_owned_process_group(
                active,
                process_probe=lambda _pid: observed,
                group_exists=lambda _pgid: True,
                terminator=killed.append)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(killed, [])
        self.assertEqual(active["startToken"], "owned-start")

    def test_recovery_kills_surviving_group_after_supervisor_is_gone(self):
        ready = self.repo.root / "orphan-ready"
        finished = self.repo.root / "orphan-finished"
        script = (
            "from pathlib import Path; import os,time; "
            f"Path({str(ready)!r}).write_text(str(os.getpid())); "
            "time.sleep(10); "
            f"Path({str(finished)!r}).write_text('finished')")
        transaction = build.OuterTransaction.begin(self.repo.root, TXN, HEAD)

        def crash_after_ready(point):
            if point != "AFTER_OWNED_PROCESS_RELEASE":
                return None
            deadline = time.monotonic() + 2
            while not ready.is_file() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.is_file())
            return "crash"

        with self.assertRaises(build.Task8Error):
            transaction.run_owned_command(
                build.CommandSpec(
                    "terrain-reference",
                    ("nice", "-n", "10", sys.executable, "-B", "-c", script),
                    timeout_seconds=15),
                fault=crash_after_ready)
        active = dict(transaction.journal["activeProcess"])
        target_pid = int(ready.read_text())
        os.kill(active["pid"], signal.SIGKILL)
        os.waitpid(active["pid"], 0)
        self.assertIsNone(build._probe_process(active["pid"]))
        self.assertTrue(build._process_group_exists(active["pgid"]))
        transaction.abandon_for_test()

        recovered = build.OuterTransaction.acquire(self.repo.root)
        self.addCleanup(recovered.close)
        try:
            recovered.recover(HEAD)
            self.assertFalse(build._process_group_exists(active["pgid"]))
            with self.assertRaises(ProcessLookupError):
                os.kill(target_pid, 0)
            self.assertFalse(finished.exists())
        finally:
            if build._process_group_exists(active["pgid"]):
                try:
                    os.killpg(active["pgid"], signal.SIGKILL)
                except ProcessLookupError:
                    pass

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
