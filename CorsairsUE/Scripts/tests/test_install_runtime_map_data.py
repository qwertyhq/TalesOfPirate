import copy
import errno
import hashlib
import inspect
import json
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest

if os.name != "nt":
    import fcntl

from CorsairsUE.Scripts import install_runtime_map_data


HASH_X = hashlib.sha256(b"x").hexdigest()
LEAVES = {
    "height": "garner.height.r16",
    "block": "garner.block.raw",
    "region": "garner.region.raw",
    "terrainMetadata": "garner.terrain.json",
    "albedo": "garner.albedo_17_21.png",
    "meshGltf": "garner.terrain_17_21.gltf",
    "meshBin": "garner.terrain_17_21.bin",
}


class ManifestFixture:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".corsairs-task7-python-", dir=Path.cwd())
        self.root = Path(self.temporary.name)
        self.relative_root = self.root.relative_to(Path.cwd())
        self.output = self.root / "maps"
        self.run = self.output / "runs" / "run-001"
        self.run.mkdir(parents=True)
        for leaf in LEAVES.values():
            (self.run / leaf).write_bytes(b"x")
        source_paths = [
            self.root / "Client/map/garner.map",
            self.root / "databases/gamedata.sqlite",
            self.root / "Client/texture/terrain/alpha/total.png",
            self.root / "Client/texture/terrain/brick05.png",
        ]
        for path in source_paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
        rel = lambda suffix: (self.relative_root / suffix).as_posix()
        self.manifest = {
            "schemaVersion": 1,
            "algorithmVersion": "legacy-fixed-pipeline-v1",
            "source": {
                "map": {"path": rel("Client/map/garner.map"), "sha256": HASH_X},
                "database": {
                    "path": rel("databases/gamedata.sqlite"),
                    "sha256": HASH_X,
                },
                "clientRoot": rel("Client"),
                "alphaAtlas": {
                    "path": rel("Client/texture/terrain/alpha/total.png"),
                    "sha256": HASH_X,
                },
                "usedTextures": [
                    {
                        "textureId": 4,
                        "path": rel("Client/texture/terrain/brick05.png"),
                        "sha256": HASH_X,
                    }
                ],
            },
            "page": {
                "x": 17,
                "y": 21,
                "sourceCellBounds": {"x": 2176, "y": 2688, "width": 128, "height": 128},
                "pixelsPerCell": 32,
                "pixelWidth": 4096,
                "pixelHeight": 4096,
                "ambient": [1, 1, 1],
                "dwTColor": 0,
            },
            "requiredPresentRect": {"x": 2193, "y": 2756, "width": 80, "height": 47},
            "usedTextureIds": [4],
            "sectionPresence": {
                "originX": 272,
                "originY": 336,
                "width": 16,
                "height": 16,
                "rowMajorMask": [1] * 256,
            },
            "files": {
                key: {
                    "path": f"runs/run-001/{leaf}",
                    "sha256": HASH_X,
                    "sizeBytes": 1,
                }
                for key, leaf in LEAVES.items()
            },
            "metrics": {
                "peakRssBytes": 1,
                "peakTextureCacheBytes": 1,
                "peakRgbaRowBytes": 1,
                "pngBytes": 1,
                "totalOutputBytes": 7,
                "maxHeightErrorCm": 0,
                "rmsHeightErrorCm": 0,
                "sharedBoundaryMaxCm": 0,
                "absentSectionCount": 0,
                "unresolvedLayerCount": 0,
            },
        }
        self.manifest_path = self.output / "garner.reference-albedo.json"
        self.write_manifest()
        self.target = self.root / "runtime"
        self.target.mkdir()
        (self.target / "garner.height.r16").write_bytes(b"x")
        self.runtime_contract_path = self.target / "garner.runtime.json"
        self.runtime_contract_path.write_text(
            json.dumps({
                "schemaVersion": 1,
                "map": "garner",
                "gridWidth": 4096,
                "gridHeight": 4096,
                "files": {
                    "height": {
                        "name": "garner.height.r16",
                        "sha256": HASH_X,
                        "sizeBytes": 1,
                    },
                    "block": {
                        "name": "garner.block.raw",
                        "sha256": HASH_X,
                        "sizeBytes": 1,
                    },
                    "region": {
                        "name": "garner.region.raw",
                        "sha256": HASH_X,
                        "sizeBytes": 1,
                    },
                    "terrainMetadata": {
                        "name": "garner.terrain.json",
                        "sha256": HASH_X,
                        "sizeBytes": 1,
                    },
                },
            }, separators=(",", ":")),
            encoding="utf-8",
        )

    def write_manifest(self):
        self.manifest_path.write_text(
            json.dumps(self.manifest, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )

    def create_second_run(self):
        second = self.output / "runs" / "run-002"
        second.mkdir()
        for leaf in LEAVES.values():
            (second / leaf).write_bytes(b"x")
        manifest = copy.deepcopy(self.manifest)
        for value in manifest["files"].values():
            value["path"] = value["path"].replace("run-001", "run-002")
        manifest["metrics"]["peakRssBytes"] = 2
        path = self.output / "garner.reference-albedo.second.json"
        path.write_text(
            json.dumps(manifest, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )
        return path, manifest

    def close(self):
        self.temporary.cleanup()


class RuntimeMapInstallerTests(unittest.TestCase):
    def test_module_exposes_deterministic_manifest_comparison(self):
        self.assertTrue(
            callable(install_runtime_map_data.compare_deterministic_manifests))

    def test_installs_verified_navigation_triple_and_preserves_height(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        result = install_runtime_map_data.install_runtime_map_data(
            fixture.manifest_path, fixture.target)
        self.assertEqual(result, "OK")
        self.assertEqual((fixture.target / "garner.block.raw").read_bytes(), b"x")
        self.assertEqual((fixture.target / "garner.region.raw").read_bytes(), b"x")
        self.assertEqual((fixture.target / "garner.terrain.json").read_bytes(), b"x")
        self.assertEqual(
            list(fixture.target.glob(".garner-runtime-install*")), [])
        self.assertEqual((fixture.target / "garner.height.r16").read_bytes(), b"x")

    def test_rejects_wrong_tracked_height_before_runtime_mutation(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.target / "garner.block.raw"
        region = fixture.target / "garner.region.raw"
        metadata = fixture.target / "garner.terrain.json"
        block.write_bytes(b"old-block")
        region.write_bytes(b"old-region")
        metadata.write_bytes(b"old-metadata")
        (fixture.target / "garner.height.r16").write_bytes(b"wrong-height")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)

        self.assertIn("runtime height", str(caught.exception))
        self.assertEqual(block.read_bytes(), b"old-block")
        self.assertEqual(region.read_bytes(), b"old-region")
        self.assertEqual(metadata.read_bytes(), b"old-metadata")

    def test_fault_after_region_replace_rolls_back_complete_triple(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.target / "garner.block.raw"
        region = fixture.target / "garner.region.raw"
        metadata = fixture.target / "garner.terrain.json"
        block.write_bytes(b"old-block")
        region.write_bytes(b"old-region")
        metadata.write_bytes(b"old-metadata")
        reached = []

        def fail(point):
            if point == "INSTALL_AFTER_REGION_REPLACE" and not reached:
                reached.append(point)
                return "fail"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fail)

        self.assertEqual(reached, ["INSTALL_AFTER_REGION_REPLACE"])
        self.assertEqual(block.read_bytes(), b"old-block")
        self.assertEqual(region.read_bytes(), b"old-region")
        self.assertEqual(metadata.read_bytes(), b"old-metadata")

    @unittest.skipIf(os.name == "nt", "POSIX retained-descriptor race fixture")
    def test_exclusive_create_writes_through_retained_descriptor(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        path = fixture.target / ".owned"
        foreign = fixture.target / "foreign-sentinel"
        displaced = fixture.target / ".owned.opened"
        foreign.write_bytes(b"must-survive")

        class SwappingDurableFs(install_runtime_map_data.PosixDurableFs):
            def open_exclusive_temp(self, candidate):
                opened = super().open_exclusive_temp(candidate)
                os.replace(candidate, displaced)
                os.symlink(foreign, candidate)
                return opened

        with self.assertRaises(
                (install_runtime_map_data.DurableFsError,
                 install_runtime_map_data.InstallError)):
            install_runtime_map_data._write_owned(
                SwappingDurableFs(), path, b"owned-payload", 0o600)
        self.assertEqual(foreign.read_bytes(), b"must-survive")
        self.assertEqual(displaced.read_bytes(), b"owned-payload")

    def test_manifest_source_mutation_after_validation_preserves_pair(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block_target = fixture.target / "garner.block.raw"
        metadata_target = fixture.target / "garner.terrain.json"
        block_target.write_bytes(b"old-block")
        metadata_target.write_bytes(b"old-metadata")
        source = fixture.run / "garner.block.raw"
        reached = []

        def mutate(point):
            if point == "INSTALL_AFTER_MANIFEST_VALIDATE" and not reached:
                reached.append(point)
                source.write_bytes(b"changed-after-validate")
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=mutate)
        self.assertEqual(reached, ["INSTALL_AFTER_MANIFEST_VALIDATE"])
        self.assertIn("source changed after manifest validation", str(caught.exception))
        self.assertEqual(block_target.read_bytes(), b"old-block")
        self.assertEqual(metadata_target.read_bytes(), b"old-metadata")

    @unittest.skipIf(os.name == "nt", "POSIX inode-swap fixture")
    def test_target_identity_change_before_backup_or_replace_is_rejected(self):
        for prior_exists in (True, False):
            with self.subTest(prior_exists=prior_exists):
                fixture = ManifestFixture()
                try:
                    block_target = fixture.target / "garner.block.raw"
                    metadata_target = fixture.target / "garner.terrain.json"
                    if prior_exists:
                        block_target.write_bytes(b"old-block")
                        metadata_target.write_bytes(b"old-metadata")
                    reached = []
                    replacement_identity = []

                    def mutate(point):
                        if (point == "INSTALL_AFTER_METADATA_STAGE_FLUSH" and
                                not reached):
                            reached.append(point)
                            replacement = fixture.target / ".foreign-block"
                            replacement.write_bytes(
                                b"old-block" if prior_exists else b"foreign")
                            os.chmod(replacement, 0o600)
                            os.replace(replacement, block_target)
                            replacement_identity.append(
                                install_runtime_map_data._physical_identity(
                                    block_target))
                        return None

                    with self.assertRaises(
                            install_runtime_map_data.InstallError):
                        install_runtime_map_data.install_runtime_map_data(
                            fixture.manifest_path, fixture.target, fault=mutate)
                    self.assertEqual(reached,
                                     ["INSTALL_AFTER_METADATA_STAGE_FLUSH"])
                    self.assertEqual(
                        block_target.read_bytes(),
                        b"old-block" if prior_exists else b"foreign")
                    self.assertEqual(
                        install_runtime_map_data._physical_identity(block_target),
                        replacement_identity[0])
                finally:
                    fixture.close()

    @unittest.skipIf(os.name == "nt", "POSIX inode-swap fixture")
    def test_same_bytes_inode_swap_after_validation_is_rejected(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block_target = fixture.target / "garner.block.raw"
        metadata_target = fixture.target / "garner.terrain.json"
        block_target.write_bytes(b"old-block")
        metadata_target.write_bytes(b"old-metadata")
        source = fixture.run / "garner.block.raw"
        displaced = fixture.run / "garner.block.raw.original"
        reached = []

        def mutate(point):
            if point == "INSTALL_AFTER_MANIFEST_VALIDATE" and not reached:
                reached.append(point)
                os.replace(source, displaced)
                source.write_bytes(b"x")
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=mutate)
        self.assertEqual(reached, ["INSTALL_AFTER_MANIFEST_VALIDATE"])
        self.assertIn("physical identity changed", str(caught.exception))
        self.assertEqual(block_target.read_bytes(), b"old-block")
        self.assertEqual(metadata_target.read_bytes(), b"old-metadata")

    @unittest.skipIf(os.name == "nt", "POSIX hard-link fixture")
    def test_manifest_rejects_hard_linked_run_products(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.run / "garner.block.raw"
        block.unlink()
        os.link(fixture.run / "garner.height.r16", block)
        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)
        self.assertEqual(
            str(caught.exception),
            "WRITE_FAILED INVALID_FILE /files/height/path: "
            "outputs must have seven distinct physical identities")

    @unittest.skipIf(os.name == "nt", "POSIX directory-symlink fixture")
    def test_texture_path_requires_lexical_client_root_prefix(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        alias = fixture.root / "AliasClient"
        alias.symlink_to(fixture.root / "Client", target_is_directory=True)
        fixture.manifest["source"]["usedTextures"][0]["path"] = (
            fixture.relative_root /
            "AliasClient/texture/terrain/brick05.png").as_posix()
        fixture.write_manifest()
        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)
        self.assertEqual(
            str(caught.exception),
            "WRITE_FAILED INVALID_PROVENANCE /source/usedTextures/0/path: "
            "texture path is not lexically beneath client root")

    def test_parser_is_total_for_huge_numbers_and_lone_surrogates(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        huge = copy.deepcopy(fixture.manifest)
        huge["page"]["ambient"][0] = 10 ** 400
        fixture.manifest = huge
        fixture.write_manifest()
        with self.assertRaises(install_runtime_map_data.InstallError) as huge_error:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)
        self.assertEqual(
            str(huge_error.exception),
            "WRITE_FAILED INVALID_SCHEMA /page/ambient/0: expected finite number")

        surrogate = copy.deepcopy(huge)
        surrogate["page"]["ambient"][0] = 1
        surrogate["source"]["map"]["path"] = "Client/map/\ud800.map"
        fixture.manifest_path.write_bytes(json.dumps(
            surrogate, ensure_ascii=True, separators=(",", ":")).encode("utf-8"))
        with self.assertRaises(
                install_runtime_map_data.InstallError) as surrogate_error:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)
        self.assertEqual(
            str(surrogate_error.exception),
            "WRITE_FAILED invalid manifest JSON: non-Unicode scalar at "
            "/source/map/path")

    def test_parser_is_total_for_deep_manifest_and_journal(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        deep = (b"[" * 2048) + b"0" + (b"]" * 2048)
        fixture.manifest_path.write_bytes(deep)
        with self.assertRaises(install_runtime_map_data.InstallError) as manifest:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)
        self.assertEqual(manifest.exception.status, "WRITE_FAILED")
        with self.assertRaises(install_runtime_map_data.InstallError) as journal:
            install_runtime_map_data._parse_journal(deep, fixture.target)
        self.assertEqual(journal.exception.status, "RECOVERY_REQUIRED")

    def test_strict_parser_required_key_and_type_matrix(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        mutations = []
        original = copy.deepcopy(fixture.manifest)

        def mutate_at(path, operation, replacement=None):
            value = copy.deepcopy(original)
            cursor = value
            if operation == "unknown":
                for component in path:
                    cursor = cursor[component]
                cursor["__unknown__"] = 1
                return value
            for component in path[:-1]:
                cursor = cursor[component]
            if operation == "delete":
                del cursor[path[-1]]
            elif operation == "replace":
                cursor[path[-1]] = replacement
            return value

        def wrong_type(value):
            if type(value) is dict:
                return []
            if type(value) is list:
                return {}
            if type(value) is str:
                return 0
            if type(value) is bool:
                return 0
            return "invalid"

        def collect(value, path=()):
            if type(value) is dict:
                mutations.append((
                    "unknown-" + "/".join(map(str, path or ("root",))),
                    mutate_at(path, "unknown")))
                for key, child in value.items():
                    child_path = path + (key,)
                    mutations.append((
                        "missing-" + "/".join(map(str, child_path)),
                        mutate_at(child_path, "delete")))
                    mutations.append((
                        "wrong-type-" + "/".join(map(str, child_path)),
                        mutate_at(child_path, "replace", wrong_type(child))))
                    collect(child, child_path)
            elif type(value) is list and value:
                child_path = path + (0,)
                mutations.append((
                    "wrong-element-" + "/".join(map(str, child_path)),
                    mutate_at(child_path, "replace", wrong_type(value[0]))))
                collect(value[0], child_path)

        collect(original)
        for label, value in mutations:
            with self.subTest(label=label):
                fixture.manifest = value
                fixture.write_manifest()
                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.validate_manifest(
                        fixture.manifest_path)

    def test_rejects_invalid_paths_and_tamper_before_target_mutation(self):
        mutations = []
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        leaf_only = copy.deepcopy(fixture.manifest)
        leaf_only["files"]["block"]["path"] = "garner.block.raw"
        mutations.append(leaf_only)
        mixed = copy.deepcopy(fixture.manifest)
        mixed["files"]["terrainMetadata"]["path"] = (
            "runs/run-002/garner.terrain.json")
        mutations.append(mixed)
        traversal = copy.deepcopy(fixture.manifest)
        traversal["files"]["block"]["path"] = (
            "runs/../run-001/garner.block.raw")
        mutations.append(traversal)
        wrong_leaf = copy.deepcopy(fixture.manifest)
        wrong_leaf["files"]["block"]["path"] = (
            "runs/run-001/garner.region.raw")
        mutations.append(wrong_leaf)
        wrong_hash = copy.deepcopy(fixture.manifest)
        wrong_hash["files"]["block"]["sha256"] = "0" * 64
        mutations.append(wrong_hash)
        for manifest in mutations:
            fixture.manifest = manifest
            fixture.write_manifest()
            with self.assertRaises(install_runtime_map_data.InstallError):
                install_runtime_map_data.install_runtime_map_data(
                    fixture.manifest_path, fixture.target)
            self.assertFalse((fixture.target / "garner.block.raw").exists())
            self.assertFalse((fixture.target / "garner.terrain.json").exists())

    def test_identical_second_install_is_inode_preserving_noop(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target), "OK")
        block = fixture.target / "garner.block.raw"
        metadata = fixture.target / "garner.terrain.json"
        before = [(path.stat().st_ino, path.stat().st_mtime_ns,
                   stat.S_IMODE(path.stat().st_mode))
                  for path in (block, metadata)]
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target), "NOOP")
        after = [(path.stat().st_ino, path.stat().st_mtime_ns,
                  stat.S_IMODE(path.stat().st_mode))
                 for path in (block, metadata)]
        self.assertEqual(after, before)
        self.assertEqual(
            list(fixture.target.glob(".garner-runtime-install*")), [])

    def test_precommit_fault_restores_complete_prior_pair(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.target / "garner.block.raw"
        metadata = fixture.target / "garner.terrain.json"
        block.write_bytes(b"old-block")
        metadata.write_bytes(b"old-metadata")
        injected = []

        def fault(point):
            if point == "INSTALL_AFTER_BLOCK_REPLACE" and not injected:
                injected.append(point)
                return "fail"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)
        self.assertEqual(caught.exception.status, "WRITE_FAILED")
        self.assertEqual(block.read_bytes(), b"old-block")
        self.assertEqual(metadata.read_bytes(), b"old-metadata")
        self.assertEqual(injected, ["INSTALL_AFTER_BLOCK_REPLACE"])

    def test_fresh_startup_recovers_crash_and_committed_pair(self):
        for point, expected_status, recovered_result in (
            ("INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE", "WRITE_FAILED", "OK"),
            ("INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE", "RECOVERY_REQUIRED", "NOOP"),
        ):
            with self.subTest(point=point):
                fixture = ManifestFixture()
                self.addCleanup(fixture.close)
                (fixture.target / "garner.block.raw").write_bytes(b"old-block")
                (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
                injected = []

                def fault(candidate):
                    if candidate == point and not injected:
                        injected.append(candidate)
                        return "crash"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError) as caught:
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=fault)
                self.assertEqual(caught.exception.status, expected_status)
                self.assertEqual(
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target), recovered_result)
                self.assertEqual(
                    (fixture.target / "garner.block.raw").read_bytes(), b"x")
                self.assertEqual(
                    (fixture.target / "garner.terrain.json").read_bytes(), b"x")

    def test_snapshot_records_exact_reservations_tombstones_and_identities(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.region.raw").write_bytes(b"old-region")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
        reached = []

        def fault(point):
            if point == "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)
        journal = json.loads((fixture.target /
            ".garner-runtime-install.transaction.json").read_text("utf-8"))
        self.assertEqual(
            set(journal),
            {"version", "phase", "transactionId", "manifestPath",
             "manifestSha256", "recoveryCommand", "journalRetired",
             "entries"})
        for key, entry in journal["entries"].items():
            self.assertEqual(
                set(entry),
                {"target", "stage", "backup", "stageReservation",
                 "backupReservation", "rollbackStage", "rollbackReservation",
                 "targetTombstone", "stageTombstone", "backupTombstone",
                 "rollbackTombstone", "priorExists", "priorSha256",
                 "priorMode", "priorIdentity", "intendedSha256",
                 "intendedSize", "intendedMode", "stageIdentity",
                 "backupIdentity", "rollbackIdentity"},
                key)
            for field in ("stageReservation", "backupReservation",
                          "rollbackStage", "rollbackReservation",
                          "targetTombstone", "stageTombstone",
                          "backupTombstone", "rollbackTombstone"):
                self.assertEqual(Path(entry[field]).parent, fixture.target)
            self.assertEqual(len(entry["stageIdentity"]), 3)
            self.assertEqual(len(entry["backupIdentity"]), 3)

    def test_windows_entry_finalization_is_typed_and_write_through_ordered(self):
        class RecordingWindowsFs:
            def __init__(self):
                self.calls = []
                self.state = {Path("stage.block"): b"payload"}

            def reservation_bytes(self, source, reserved, kind, tx, digest):
                del source, reserved, kind, tx, digest
                return b"typed-reservation"

            def entry_exists(self, path):
                return Path(path) in self.state

            def entry_bytes(self, path):
                return self.state[Path(path)]

            def entry_hash(self, path):
                return "1" * 64 if self.state[Path(path)] == b"payload" else "0" * 64

            def entry_identity(self, path):
                del path
                return (7, 8, 9)

            def entry_mode(self, path):
                del path
                return 0o600

            def reserve_move_target(self, source, reserved, kind, tx, digest):
                self.calls.append(("reserve", Path(source).name,
                                   Path(reserved).name, kind, tx, digest))
                self.state[Path(reserved)] = self.reservation_bytes(
                    source, reserved, kind, tx, digest)

            def replace_same_volume(self, source, destination):
                self.calls.append(("replace", Path(source).name,
                                   Path(destination).name))
                self.state[Path(destination)] = self.state.pop(Path(source))

            def flush_file(self, path):
                self.calls.append(("flush", Path(path).name))

            def verify_finalized_entry(self, path, digest, identity, mode):
                return (self.entry_hash(path) == digest and
                        self.entry_identity(path) == tuple(identity) and
                        self.entry_mode(path) == mode)

            def remove_owned(self, path, **_recorded):
                self.calls.append(("remove", Path(path).name))
                self.state.pop(Path(path))

        entry = install_runtime_map_data.DurableEntryToFinalize(
            source=Path("stage.block"),
            reservation=Path("reserve.block"),
            kind="finalize-install-entry",
            transaction_id="0011223344556677-00000001",
            source_hash="1" * 64,
            source_identity=(7, 8, 9),
            source_mode=0o600,
        )
        durable_fs = RecordingWindowsFs()
        install_runtime_map_data._finalize_windows_entries(durable_fs, [entry])
        self.assertEqual(
            durable_fs.calls,
            [("flush", "stage.block"),
             ("reserve", "stage.block", "reserve.block",
              "finalize-install-entry", "0011223344556677-00000001", "1" * 64),
             ("replace", "stage.block", "reserve.block"),
             ("replace", "reserve.block", "stage.block")])
        self.assertEqual(durable_fs.state, {Path("stage.block"): b"payload"})

        class CrashAfterFirstReplace(RecordingWindowsFs):
            def replace_same_volume(self, source, destination):
                super().replace_same_volume(source, destination)
                if sum(call[0] == "replace" for call in self.calls) == 1:
                    raise install_runtime_map_data.DurableFsError("crash-after-first-replace")

        crashed_fs = CrashAfterFirstReplace()
        with self.assertRaises(install_runtime_map_data.DurableFsError):
            install_runtime_map_data._finalize_windows_entries(
                crashed_fs, [entry])
        self.assertEqual(
            crashed_fs.state, {Path("reserve.block"): b"payload"})

        # Fresh adapter instance over the persisted state classifies the
        # reservation as post-move payload, restores it, then completes the
        # full flush-before-reserve there-and-back barrier.
        restarted_fs = RecordingWindowsFs()
        restarted_fs.state = dict(crashed_fs.state)
        install_runtime_map_data._finalize_windows_entries(restarted_fs, [entry])
        self.assertEqual(
            restarted_fs.calls[:2],
            [("replace", "reserve.block", "stage.block"),
             ("flush", "stage.block")])
        self.assertEqual(
            restarted_fs.state, {Path("stage.block"): b"payload"})

        pre_move_fs = RecordingWindowsFs()
        pre_move_fs.state[Path("reserve.block")] = b"typed-reservation"
        install_runtime_map_data._finalize_windows_entries(pre_move_fs, [entry])
        self.assertEqual(pre_move_fs.calls[0], ("remove", "reserve.block"))

        corrupt_fs = RecordingWindowsFs()
        corrupt_fs.state = {Path("reserve.block"): b"typed-reservation"}
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "neither record nor payload"):
            install_runtime_map_data._finalize_windows_entries(
                corrupt_fs, [entry])

        class SameBytesForeignIdentity(RecordingWindowsFs):
            def entry_identity(self, path):
                return ((99, 98, 97) if Path(path) == Path("reserve.block")
                        else super().entry_identity(path))

        foreign_fs = SameBytesForeignIdentity()
        foreign_fs.state = {Path("reserve.block"): b"payload"}
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "neither record nor payload"):
            install_runtime_map_data._finalize_windows_entries(
                foreign_fs, [entry])
        self.assertEqual(
            foreign_fs.state, {Path("reserve.block"): b"payload"})

        class ReservationSwapFs(RecordingWindowsFs):
            def __init__(self):
                super().__init__()
                self.identities = {
                    Path("stage.block"): (7, 8, 9),
                    Path("reserve.block"): (4, 5, 6),
                }

            def entry_identity(self, path):
                return self.identities[Path(path)]

            def remove_owned(self, path, **recorded):
                candidate = Path(path)
                if "expected_identity" not in recorded:
                    self.state.pop(candidate)
                    self.identities.pop(candidate)
                    return
                self.identities[candidate] = (40, 50, 60)
                if (tuple(recorded.get("expected_identity", ())) !=
                        self.identities[candidate]):
                    raise install_runtime_map_data.DurableFsError(
                        "foreign reservation identity")
                self.state.pop(candidate)

            def reserve_move_target(self, source, reserved, kind, tx, digest):
                super().reserve_move_target(source, reserved, kind, tx, digest)
                self.identities[Path(reserved)] = (4, 5, 6)

            def replace_same_volume(self, source, destination):
                super().replace_same_volume(source, destination)
                self.identities[Path(destination)] = self.identities.pop(
                    Path(source))

        swapped_record = ReservationSwapFs()
        swapped_record.state[Path("reserve.block")] = b"typed-reservation"
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "foreign reservation identity"):
            install_runtime_map_data._finalize_windows_entries(
                swapped_record, [entry])
        self.assertEqual(
            swapped_record.state[Path("reserve.block")],
            b"typed-reservation")

    def test_windows_remove_owned_state_machine_readonly_and_swap_behavior(self):
        self._exercise_windows_runtime_mock()

    def test_deterministic_comparison_normalizes_only_run_and_rss(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        second_path, second = fixture.create_second_run()
        self.assertEqual(
            install_runtime_map_data.compare_deterministic_manifests(
                fixture.manifest_path, second_path), [])
        second["metrics"]["peakTextureCacheBytes"] = 2
        second_path.write_text(
            json.dumps(second, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8")
        self.assertEqual(
            install_runtime_map_data.compare_deterministic_manifests(
                fixture.manifest_path, second_path),
            ["manifests differ outside run ID and peak RSS"])

    def test_startup_recovery_retires_canonical_journal_before_removal(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        injected = []

        def fault(point):
            if point == "INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE" and not injected:
                injected.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)

        class RecordingDurableFs(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.replacements = []

            def replace_same_volume(self, source, destination):
                self.replacements.append((Path(source).name, Path(destination).name))
                return super().replace_same_volume(source, destination)

        durable_fs = RecordingDurableFs()
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=durable_fs),
            "OK")
        journal_retire = (
            ".garner-runtime-install.transaction.json",
            ".garner-runtime-install.transaction.json.retired",
        )
        self.assertEqual(durable_fs.replacements.count(journal_retire), 2)

    def test_journal_retirement_pre_move_reservation_is_replayed(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
        reached = []

        def crash(point):
            if point == "INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=crash)
        canonical = fixture.target / ".garner-runtime-install.transaction.json"
        retired = fixture.target / ".garner-runtime-install.transaction.json.retired"
        journal_bytes = canonical.read_bytes()
        journal = json.loads(journal_bytes)
        retired.write_bytes(
            install_runtime_map_data.WindowsDurableFs._reservation_bytes(
                canonical, retired, "retire-install-journal",
                journal["transactionId"], hashlib.sha256(journal_bytes).hexdigest()))

        class HybridFs(install_runtime_map_data.PosixDurableFs):
            _reservation_bytes = staticmethod(
                install_runtime_map_data.WindowsDurableFs._reservation_bytes)

        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target,
                durable_fs=HybridFs()), "OK")
        self.assertFalse(retired.exists())

    def test_persistent_recovery_failure_retains_journal_and_backups(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
        publication_fault = []
        recovery_faults = []

        def fault(point):
            if point == "INSTALL_AFTER_BLOCK_REPLACE" and not publication_fault:
                publication_fault.append(point)
                return "fail"
            if point == "INSTALL_RECOVERY_BLOCK_REPLACE":
                recovery_faults.append(point)
                return "fail"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(publication_fault, ["INSTALL_AFTER_BLOCK_REPLACE"])
        self.assertEqual(recovery_faults, ["INSTALL_RECOVERY_BLOCK_REPLACE"])
        self.assertTrue(caught.exception.recovery_command)
        self.assertTrue((fixture.target /
                         ".garner-runtime-install.transaction.json").exists())
        self.assertEqual(len(list(fixture.target.glob(
            ".garner-runtime-install.backup.*"))), 2)

    def test_committed_cleanup_rejects_foreign_backup_and_retry_succeeds(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
        reached = []

        def fault(point):
            if point == "INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)
        journal_path = fixture.target / ".garner-runtime-install.transaction.json"
        journal = json.loads(journal_path.read_text("utf-8"))
        backup = Path(journal["entries"]["block"]["backup"])
        backup.unlink()
        backup.write_bytes(b"foreign-sentinel")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn("owned artifact identity/hash/mode mismatch", str(caught.exception))
        self.assertEqual(backup.read_bytes(), b"foreign-sentinel")
        backup.unlink()
        # The retained journal is actionable: restoring the recorded immutable
        # backup bytes lets the next invocation finish cleanup and NOOP.
        backup.write_bytes(b"old-block")
        os.chmod(backup, journal["entries"]["block"]["priorMode"])
        journal["entries"]["block"]["backupIdentity"] = list(
            install_runtime_map_data._physical_identity(backup))
        journal_path.write_bytes(install_runtime_map_data._journal_bytes(journal))
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target), "NOOP")

    def test_startup_durable_error_is_recovery_required_and_releases_lock(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
        reached = []

        def crash(point):
            if point == "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=crash)

        class FailingRecoveryFs(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.lock = None

            def lock_exclusive(self, canonical, retired, fault=None):
                self.lock = super().lock_exclusive(
                    canonical, retired, fault=fault)
                return self.lock

            def remove_owned(self, path, *args, **kwargs):
                if Path(path).name.startswith(".garner-runtime-install.backup"):
                    raise install_runtime_map_data.DurableFsError(
                        "DURABLE_FS_ERROR op=RemoveOwned path=\"injected\" "
                        "native=errno:5:")
                return super().remove_owned(path, *args, **kwargs)

        durable_fs = FailingRecoveryFs()
        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=durable_fs)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(caught.exception.recovery_command)
        self.assertIsNotNone(durable_fs.lock)
        self.assertEqual(durable_fs.lock.descriptor, -1)

    def test_unexpected_post_lock_exception_is_converted_and_replayable(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)

        class TrackingFs(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.lock = None

            def lock_exclusive(self, canonical, retired, fault=None):
                self.lock = super().lock_exclusive(
                    canonical, retired, fault=fault)
                return self.lock

        durable_fs = TrackingFs()

        def invalid_fault(point):
            if point == "INSTALL_AFTER_MANIFEST_VALIDATE":
                return "not-a-valid-fault-action"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target,
                durable_fs=durable_fs, fault=invalid_fault)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn("unexpected post-lock exception: ValueError", str(caught.exception))
        self.assertTrue(caught.exception.recovery_command)
        self.assertIsNotNone(durable_fs.lock)
        self.assertEqual(durable_fs.lock.descriptor, -1)
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target), "OK")

    def test_full_literal_install_crash_matrix_reaches_every_boundary(self):
        forward_points = (
            "INSTALL_AFTER_BLOCK_STAGE_FLUSH",
            "INSTALL_AFTER_METADATA_STAGE_FLUSH",
            "INSTALL_AFTER_BACKUPS_DURABILITY_BARRIER",
            "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE",
            "INSTALL_BEFORE_BLOCK_REPLACE",
            "INSTALL_AFTER_BLOCK_REPLACE",
            "INSTALL_AFTER_BLOCK_REPLACE_DURABILITY_BARRIER",
            "INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE",
            "INSTALL_BEFORE_METADATA_REPLACE",
            "INSTALL_AFTER_METADATA_REPLACE",
            "INSTALL_AFTER_METADATA_REPLACE_DURABILITY_BARRIER",
            "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE",
            "INSTALL_AFTER_PAIR_VERIFY",
            "INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE",
            "INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE",
            "INSTALL_AFTER_JOURNAL_RETIRE",
            "INSTALL_AFTER_LOCK_RESERVATION_DURABLE",
            "INSTALL_AFTER_LOCK_MOVE_TO_RETIRED",
            "INSTALL_AFTER_LOCK_DELETE_PENDING",
        )
        for point in forward_points:
            with self.subTest(point=point):
                fixture = ManifestFixture()
                self.addCleanup(fixture.close)
                (fixture.target / "garner.block.raw").write_bytes(b"old-block")
                (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
                reached = []

                def crash(candidate):
                    if candidate == point and not reached:
                        reached.append(candidate)
                        return "crash"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=crash)
                self.assertEqual(reached, [point])
                result = install_runtime_map_data.install_runtime_map_data(
                    fixture.manifest_path, fixture.target)
                self.assertIn(result, ("OK", "NOOP"))
                self.assertEqual(
                    (fixture.target / "garner.block.raw").read_bytes(), b"x")
                self.assertEqual(
                    (fixture.target / "garner.terrain.json").read_bytes(), b"x")

        recovery_points = (
            "INSTALL_RECOVERY_BLOCK_REPLACE",
            "INSTALL_RECOVERY_METADATA_REPLACE",
            "INSTALL_RECOVERY_AFTER_MODE_STAGE_FLUSH",
            "INSTALL_RECOVERY_DURABILITY_BARRIER",
            "INSTALL_RECOVERY_VERIFY",
        )
        for point in recovery_points:
            with self.subTest(point=point):
                fixture = ManifestFixture()
                self.addCleanup(fixture.close)
                (fixture.target / "garner.block.raw").write_bytes(b"old-block")
                (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
                first = []

                def first_crash(candidate):
                    if (candidate == "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE"
                            and not first):
                        first.append(candidate)
                        return "crash"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=first_crash)
                reached = []

                def recovery_crash(candidate):
                    if candidate == point and not reached:
                        reached.append(candidate)
                        return "crash"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError) as caught:
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=recovery_crash)
                self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
                self.assertEqual(reached, [point])
                self.assertEqual(
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target), "OK")

    @unittest.skipIf(os.name == "nt", "POSIX prior-mode matrix fixture")
    def test_pair_boundaries_cover_absent_nondefault_and_partial_prior(self):
        for prior_state in ("absent", "nondefault", "partial"):
            for action in ("fail", "crash"):
                for point in (
                    "INSTALL_AFTER_BLOCK_REPLACE",
                    "INSTALL_AFTER_METADATA_REPLACE",
                ):
                    with self.subTest(
                            prior_state=prior_state, action=action, point=point):
                        fixture = ManifestFixture()
                        try:
                            block = fixture.target / "garner.block.raw"
                            metadata = fixture.target / "garner.terrain.json"
                            if prior_state != "absent":
                                block.write_bytes(b"prior-block")
                                os.chmod(block, 0o640)
                            if prior_state == "nondefault":
                                metadata.write_bytes(b"prior-metadata")
                                os.chmod(metadata, 0o604)
                            expected = {
                                block: ((b"prior-block", 0o640)
                                        if prior_state != "absent" else None),
                                metadata: ((b"prior-metadata", 0o604)
                                           if prior_state == "nondefault"
                                           else None),
                            }
                            reached = []

                            def interrupt(candidate):
                                if candidate == point and not reached:
                                    reached.append(candidate)
                                    return action
                                return None

                            with self.assertRaises(
                                    install_runtime_map_data.InstallError):
                                install_runtime_map_data.install_runtime_map_data(
                                    fixture.manifest_path, fixture.target,
                                    fault=interrupt)
                            self.assertEqual(reached, [point])
                            if action == "crash":
                                invalid = fixture.output / "invalid-restart.json"
                                invalid.write_text("{}", encoding="utf-8")
                                with self.assertRaises(
                                        install_runtime_map_data.InstallError):
                                    install_runtime_map_data.install_runtime_map_data(
                                        invalid, fixture.target)
                            for path, prior in expected.items():
                                if prior is None:
                                    self.assertFalse(path.exists())
                                else:
                                    self.assertEqual(path.read_bytes(), prior[0])
                                    self.assertEqual(
                                        stat.S_IMODE(path.stat().st_mode),
                                        prior[1])
                            self.assertEqual(
                                install_runtime_map_data.install_runtime_map_data(
                                    fixture.manifest_path, fixture.target),
                                "OK")
                        finally:
                            fixture.close()

    def test_full_literal_install_fail_matrix_recovers_fresh_process(self):
        forward_points = (
            "INSTALL_AFTER_BLOCK_STAGE_FLUSH",
            "INSTALL_AFTER_METADATA_STAGE_FLUSH",
            "INSTALL_AFTER_BACKUPS_DURABILITY_BARRIER",
            "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE",
            "INSTALL_BEFORE_BLOCK_REPLACE",
            "INSTALL_AFTER_BLOCK_REPLACE",
            "INSTALL_AFTER_BLOCK_REPLACE_DURABILITY_BARRIER",
            "INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE",
            "INSTALL_BEFORE_METADATA_REPLACE",
            "INSTALL_AFTER_METADATA_REPLACE",
            "INSTALL_AFTER_METADATA_REPLACE_DURABILITY_BARRIER",
            "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE",
            "INSTALL_AFTER_PAIR_VERIFY",
            "INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE",
            "INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE",
            "INSTALL_AFTER_JOURNAL_RETIRE",
            "INSTALL_AFTER_LOCK_RESERVATION_DURABLE",
            "INSTALL_AFTER_LOCK_MOVE_TO_RETIRED",
            "INSTALL_AFTER_LOCK_DELETE_PENDING",
        )
        for point in forward_points:
            with self.subTest(point=point):
                fixture = ManifestFixture()
                self.addCleanup(fixture.close)
                (fixture.target / "garner.block.raw").write_bytes(b"old-block")
                (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
                reached = []

                def fail(candidate):
                    if candidate == point and not reached:
                        reached.append(candidate)
                        return "fail"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=fail)
                self.assertEqual(reached, [point])
                self.assertIn(
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target), ("OK", "NOOP"))

        for point in (
            "INSTALL_RECOVERY_BLOCK_REPLACE",
            "INSTALL_RECOVERY_METADATA_REPLACE",
            "INSTALL_RECOVERY_AFTER_MODE_STAGE_FLUSH",
            "INSTALL_RECOVERY_DURABILITY_BARRIER",
            "INSTALL_RECOVERY_VERIFY",
        ):
            with self.subTest(recovery_point=point):
                fixture = ManifestFixture()
                self.addCleanup(fixture.close)
                (fixture.target / "garner.block.raw").write_bytes(b"old-block")
                (fixture.target / "garner.terrain.json").write_bytes(b"old-meta")
                seeded = []

                def seed(candidate):
                    if (candidate == "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE"
                            and not seeded):
                        seeded.append(candidate)
                        return "crash"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target, fault=seed)
                reached = []

                def fail_recovery(candidate):
                    if candidate == point and not reached:
                        reached.append(candidate)
                        return "fail"
                    return None

                with self.assertRaises(install_runtime_map_data.InstallError):
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target,
                        fault=fail_recovery)
                self.assertEqual(reached, [point])
                self.assertEqual(
                    install_runtime_map_data.install_runtime_map_data(
                        fixture.manifest_path, fixture.target), "OK")

    def test_each_stage_and_backup_flush_failure_preserves_prior_pair(self):
        failure_leaves = (
            ".garner-runtime-install.stage.",
            ".garner-runtime-install.backup.",
        )
        for category in failure_leaves:
            for key in ("block", "metadata"):
                with self.subTest(category=category, key=key):
                    fixture = ManifestFixture()
                    self.addCleanup(fixture.close)
                    block = fixture.target / "garner.block.raw"
                    metadata = fixture.target / "garner.terrain.json"
                    block.write_bytes(b"old-block")
                    metadata.write_bytes(b"old-meta")

                    class FailingFlushFs(install_runtime_map_data.PosixDurableFs):
                        def flush_file(self, path):
                            leaf = Path(path).name
                            if category in leaf and leaf.endswith("." + key):
                                raise install_runtime_map_data.DurableFsError(
                                    "DURABLE_FS_ERROR op=FlushFile "
                                    f"path={json.dumps(Path(path).as_posix())} "
                                    "native=errno:5:")
                            return super().flush_file(path)

                    with self.assertRaises(install_runtime_map_data.InstallError):
                        install_runtime_map_data.install_runtime_map_data(
                            fixture.manifest_path, fixture.target,
                            durable_fs=FailingFlushFs())
                    self.assertEqual(block.read_bytes(), b"old-block")
                    self.assertEqual(metadata.read_bytes(), b"old-meta")

    def test_rejects_unexpected_retired_artifact_before_manifest_parse(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.target / "garner.block.raw"
        metadata = fixture.target / "garner.terrain.json"
        block.write_bytes(b"old-block")
        metadata.write_bytes(b"old-metadata")
        foreign = fixture.target / ".garner-runtime-install.foreign.retired"
        foreign.write_bytes(b"foreign")
        fixture.manifest_path.write_text("{}", encoding="utf-8")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)

        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        lock_path = fixture.target / ".garner-runtime-install.lock"
        expected_paths = tuple(sorted(
            (foreign, lock_path), key=lambda path: path.as_posix()))
        self.assertEqual(caught.exception.recovery_paths, expected_paths)
        self.assertEqual(
            str(caught.exception),
            "RECOVERY_REQUIRED unexpected retired runtime artifact: "
            f"{foreign.as_posix()} paths=" + ",".join(
                json.dumps(path.as_posix()) for path in expected_paths) + " "
            "command=" + install_runtime_map_data._recovery_command(
                fixture.manifest_path, fixture.target))
        self.assertEqual(block.read_bytes(), b"old-block")
        self.assertEqual(metadata.read_bytes(), b"old-metadata")
        self.assertEqual(foreign.read_bytes(), b"foreign")

    def test_pretransaction_error_retires_windows_like_lock(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        fixture.manifest_path.write_text("{}", encoding="utf-8")

        class RecordingDurableFs:
            def __init__(self):
                self.lock = SimpleNamespace(handle=object())
                self.retired = 0
                self.abandoned = 0

            def lock_exclusive(self, canonical, retired, fault=None):
                del canonical, retired, fault
                return self.lock

            def retire_lock(self, lock, fault=None):
                del fault
                self.assert_same(lock)
                self.retired += 1
                lock.handle = None

            def abandon_lock(self, lock):
                self.assert_same(lock)
                self.abandoned += 1
                lock.handle = None

            def assert_same(self, lock):
                if lock is not self.lock:
                    raise AssertionError("wrong lock")

        durable_fs = RecordingDurableFs()
        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=durable_fs)
        self.assertEqual(durable_fs.retired, 1)
        self.assertEqual(durable_fs.abandoned, 0)
        self.assertIsNone(durable_fs.lock.handle)

    def test_windows_adapter_binds_typed_reservations_and_exact_flags(self):
        source = inspect.getsource(install_runtime_map_data.WindowsDurableFs)
        self.assertIn("reserve_move_target", source)
        module_source = inspect.getsource(install_runtime_map_data)
        self.assertIn("def _private_mode", module_source)
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT", source)
        self.assertIn("GetFileInformationByHandleEx", source)
        self.assertIn("LockFileEx", source)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | self.MOVEFILE_WRITE_THROUGH", source)
        self.assertNotIn("_write_owned(durable_fs, update, payload, 0o600)",
                         module_source)

    def test_windows_exclusive_identity_failure_closes_retained_handle(self):
        calls = []

        class Kernel:
            def CloseHandle(self, handle):
                calls.append(("close", handle))
                return 1

        durable_fs = object.__new__(install_runtime_map_data.WindowsDurableFs)
        durable_fs.kernel32 = Kernel()
        durable_fs._open = lambda *args, **kwargs: 73

        def reject_identity(*args, **kwargs):
            raise install_runtime_map_data.DurableFsError("identity rejected")

        durable_fs._identity = reject_identity
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "identity rejected"):
            durable_fs.open_exclusive_temp(Path("owned.tmp"))
        self.assertEqual(calls, [("close", 73)])

    def test_windows_runtime_mock_flush_lock_retire_and_cross_volume(self):
        self._exercise_windows_runtime_mock()

    def _exercise_windows_runtime_mock(self):
        class Cell:
            def __init__(self, value=0):
                self.value = value

        class Buffer:
            def __init__(self, value):
                self.raw = (b"\0" * value if isinstance(value, int)
                            else bytes(value))

        class Basic:
            def __init__(self):
                self.FileAttributes = 0

        class Disposition:
            def __init__(self):
                self.DeleteFile = 0

        class Information:
            def __init__(self):
                self.FileAttributes = 0
                self.VolumeSerialNumber = 0
                self.NumberOfLinks = 0
                self.FileIndexHigh = 0
                self.FileIndexLow = 0

        class FakeCtypes:
            def __init__(self, owner):
                self.owner = owner

            @staticmethod
            def byref(value):
                return value

            @staticmethod
            def sizeof(_value):
                return 1

            @staticmethod
            def create_string_buffer(value):
                return Buffer(value)

            @staticmethod
            def c_longlong():
                return Cell()

            @staticmethod
            def c_void_p(value):
                return SimpleNamespace(value=value)

            @staticmethod
            def memmove(destination, source, _size):
                destination.FileAttributes = source.FileAttributes

            def get_last_error(self):
                return self.owner.last_error

        class Kernel:
            def __init__(self, owner):
                self.owner = owner

            def _bound_path(self, handle):
                identity = self.owner.handle_identities[handle]
                return next(
                    candidate for candidate, observed
                    in self.owner.identities.items()
                    if observed == identity)

            def CreateFileW(
                    self, path, access, share, _security, disposition,
                    flags, _template):
                candidate = Path(path)
                self.owner.calls.append((
                    "create-file", candidate.name, access, share,
                    disposition, flags))
                if (flags & self.owner.FILE_FLAG_OPEN_REPARSE_POINT) == 0:
                    raise AssertionError("no-follow flag is mandatory")
                if (disposition == self.owner.CREATE_NEW and
                        candidate in self.owner.state):
                    self.owner.last_error = 183
                    return -1
                if (disposition == self.owner.OPEN_EXISTING and
                        candidate not in self.owner.state):
                    self.owner.last_error = 2
                    return -1
                if candidate not in self.owner.state:
                    self.owner.state[candidate] = b""
                    self.owner.attributes[candidate] = (
                        self.owner.FILE_ATTRIBUTE_NORMAL)
                    self.owner.identities[candidate] = (
                        1, 2, self.owner.next_identity)
                    self.owner.links[candidate] = self.owner.next_created_links
                    self.owner.next_created_links = 1
                    self.owner.next_identity += 1
                handle = self.owner.next_handle
                self.owner.next_handle += 1
                self.owner.handles[handle] = candidate
                self.owner.handle_identities[handle] = (
                    self.owner.identities[candidate])
                return handle

            def CloseHandle(self, handle):
                self.owner.calls.append(("close", handle))
                identity = self.owner.handle_identities.get(handle)
                if identity == self.owner.delete_pending_identity:
                    for path, candidate in list(self.owner.identities.items()):
                        if candidate == identity:
                            self.owner.state.pop(path, None)
                            self.owner.attributes.pop(path, None)
                            self.owner.identities.pop(path, None)
                            self.owner.links.pop(path, None)
                self.owner.handles.pop(handle, None)
                self.owner.handle_identities.pop(handle, None)
                return 1

            def LockFileEx(self, handle, *_args):
                self.owner.calls.append(("lock", handle))
                return 1

            def GetFileSizeEx(self, handle, output):
                output.value = len(self.owner.state[self._bound_path(handle)])
                self.owner.calls.append(("size", handle, output.value))
                return 1

            def WriteFile(self, handle, buffer, count, written, _overlap):
                path = self.owner.handles[handle]
                self.owner.state[path] = buffer.raw[:count]
                written.value = count
                self.owner.calls.append(("write", handle, count))
                return 1

            def FlushFileBuffers(self, handle):
                self.owner.calls.append(("flush", handle))
                return 1

            def SetFilePointerEx(self, handle, *_args):
                self.owner.calls.append(("seek", handle))
                return 1

            def SetEndOfFile(self, handle):
                self.owner.state[self.owner.handles[handle]] = b""
                self.owner.calls.append(("truncate", handle))
                return 1

            def ReadFile(self, handle, buffer, count, read, _overlap):
                payload = self.owner.state[self._bound_path(handle)][:count]
                buffer.raw = payload
                read.value = len(payload)
                self.owner.calls.append(("read", handle, len(payload)))
                return 1

            def GetFileInformationByHandleEx(
                    self, handle, _kind, output, _size):
                output.FileAttributes = self.owner.attributes[
                    self._bound_path(handle)]
                self.owner.calls.append(("get-basic", handle))
                return 1

            def GetFileInformationByHandle(self, handle, output):
                path = self.owner.handles[handle]
                identity = self.owner.handle_identities[handle]
                if path not in self.owner.attributes:
                    path = next(
                        candidate for candidate, observed
                        in self.owner.identities.items()
                        if observed == identity)
                output.FileAttributes = self.owner.attributes[path]
                output.VolumeSerialNumber = identity[0]
                output.FileIndexHigh = identity[1]
                output.FileIndexLow = identity[2]
                output.NumberOfLinks = self.owner.links[path]
                self.owner.calls.append(("get-identity", handle))
                return 1

            def SetFileInformationByHandle(
                    self, handle, kind, info, _size):
                if kind == 4:
                    if not info.DeleteFile:
                        raise AssertionError("delete disposition must be true")
                    self.owner.delete_pending_identity = (
                        self.owner.handle_identities[handle])
                    self.owner.calls.append(("set-disposition", handle))
                    return 1
                self.owner.attributes[self._bound_path(handle)] = (
                    info.FileAttributes)
                self.owner.calls.append(
                    ("set-basic", handle, info.FileAttributes))
                return 1

            def GetFileAttributesW(self, path):
                candidate = Path(path)
                self.owner.calls.append(("get-attributes", candidate.name))
                if candidate not in self.owner.state:
                    self.owner.last_error = 2
                    return self.owner.INVALID_FILE_ATTRIBUTES
                return self.owner.attributes[candidate]

            def DeleteFileW(self, path):
                candidate = Path(path)
                self.owner.calls.append(("delete-pending", candidate.name))
                identity = self.owner.identities[candidate]
                if identity in self.owner.handle_identities.values():
                    self.owner.delete_pending_identity = identity
                else:
                    self.owner.state.pop(candidate, None)
                    self.owner.attributes.pop(candidate, None)
                    self.owner.identities.pop(candidate, None)
                    self.owner.links.pop(candidate, None)
                return 1

            def MoveFileExW(self, source, destination, flags):
                source = Path(source)
                destination = Path(destination)
                self.owner.calls.append((
                    "move-file", source.name, destination.name, flags))
                if flags != (self.owner.MOVEFILE_REPLACE_EXISTING |
                             self.owner.MOVEFILE_WRITE_THROUGH):
                    raise AssertionError("write-through replace flags required")
                if source not in self.owner.state:
                    self.owner.last_error = 2
                    return 0
                self.owner.state[destination] = self.owner.state.pop(source)
                self.owner.attributes[destination] = (
                    self.owner.attributes.pop(source))
                self.owner.identities[destination] = (
                    self.owner.identities.pop(source))
                self.owner.links[destination] = self.owner.links.pop(source)
                return 1

        class FakeWindowsFs(install_runtime_map_data.WindowsDurableFs):
            def __init__(self):
                self.calls = []
                self.state = {}
                self.attributes = {}
                self.identities = {}
                self.links = {}
                self.handles = {}
                self.handle_identities = {}
                self.next_handle = 10
                self.next_identity = 100
                self.next_created_links = 1
                self.last_error = 2
                self.delete_pending_identity = None
                self.ctypes = FakeCtypes(self)
                self.wintypes = SimpleNamespace(DWORD=Cell)
                self.FileAttributeTagInfo = Basic
                self.ByHandleFileInformation = Information
                self.FileBasicInfo = Basic
                self.FileDispositionInfo = Disposition
                self.Overlapped = object
                self.kernel32 = Kernel(self)

            def physical_identity(self, path):
                return self.identities[Path(path)]

            def physical_bytes(self, path):
                return self.state[Path(path)]

            def physical_hash(self, path):
                return hashlib.sha256(self.state[Path(path)]).hexdigest()

            def physical_mode(self, path):
                return self.attributes[Path(path)]

            @staticmethod
            def _volume_serial(_path):
                return 1

        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = FakeWindowsFs()
        payload = fixture.target / "readonly.stage"
        durable_fs.state[payload] = b"payload"
        durable_fs.attributes[payload] = durable_fs.FILE_ATTRIBUTE_READONLY
        durable_fs.identities[payload] = (1, 2, 3)
        durable_fs.links[payload] = 1
        durable_fs.flush_file(payload)
        self.assertEqual(
            durable_fs.attributes[payload], durable_fs.FILE_ATTRIBUTE_READONLY)
        self.assertIn(("set-basic", 10, durable_fs.FILE_ATTRIBUTE_NORMAL),
                      durable_fs.calls)
        self.assertIn(("set-basic", 11, durable_fs.FILE_ATTRIBUTE_READONLY),
                      durable_fs.calls)

        linked = fixture.target / "linked-exclusive.tmp"
        durable_fs.next_created_links = 2
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "native=win32:1142:"):
            durable_fs.open_exclusive_temp(linked)
        self.assertNotIn(
            linked, durable_fs.handles.values(),
            "identity rejection must close the retained handle")

        empty = fixture.target / ".foreign-empty.lock"
        empty_retired = fixture.target / ".foreign-empty.lock.retired"
        durable_fs.state[empty] = b""
        durable_fs.attributes[empty] = durable_fs.FILE_ATTRIBUTE_NORMAL
        durable_fs.identities[empty] = (1, 2, 99)
        durable_fs.links[empty] = 1
        with self.assertRaises(install_runtime_map_data.DurableFsError):
            durable_fs.lock_exclusive(empty, empty_retired)
        self.assertEqual(durable_fs.state[empty], b"")

        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        lock = durable_fs.lock_exclusive(canonical, retired)
        marker = (
            f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode())
        self.assertEqual(durable_fs.state[canonical], marker)
        identity_call = next(
            index for index, call in enumerate(durable_fs.calls)
            if call[0] == "get-identity")
        write_call = next(
            index for index, call in enumerate(durable_fs.calls)
            if call[0] == "write")
        self.assertLess(identity_call, write_call)
        canonical_creates = [
            call for call in durable_fs.calls
            if call[0] == "create-file" and call[1] == canonical.name]
        self.assertEqual(canonical_creates[0][4], durable_fs.CREATE_NEW)
        self.assertEqual(
            canonical_creates[0][3],
            durable_fs.FILE_SHARE_READ | durable_fs.FILE_SHARE_WRITE |
            durable_fs.FILE_SHARE_DELETE)

        lock_handle = lock.handle
        durable_fs.retire_lock(lock)
        self.assertIsNone(lock.handle)
        self.assertNotIn(canonical, durable_fs.state)
        self.assertNotIn(retired, durable_fs.state)
        self.assertEqual(durable_fs.calls[-1][0], "close")
        disposition_index = next(
            index for index, call in enumerate(durable_fs.calls)
            if call[0] == "set-disposition")
        self.assertLess(disposition_index, len(durable_fs.calls) - 1)
        self.assertIn(("close", lock_handle), durable_fs.calls)

        pre_move_fs = FakeWindowsFs()
        pre_move_owner = pre_move_fs.lock_exclusive(canonical, retired)
        pre_move_fs.abandon_lock(pre_move_owner)
        generation = "-".join(
            f"{part:08x}" for part in pre_move_owner.identity)
        pre_move_fs.reserve_move_target(
            canonical, retired, "retire-lock", generation,
            hashlib.sha256(pre_move_owner.marker).hexdigest())
        recovered_pre_move = pre_move_fs.lock_exclusive(canonical, retired)
        self.assertNotIn(retired, pre_move_fs.state)
        pre_move_fs.abandon_lock(recovered_pre_move)

        stale_fs = FakeWindowsFs()
        stale_owner = stale_fs.lock_exclusive(canonical, retired)
        stale_fs.abandon_lock(stale_owner)
        stale_fs.replace_same_volume(canonical, retired)
        replacement_owner = stale_fs.lock_exclusive(canonical, retired)
        self.assertNotIn(retired, stale_fs.state)
        stale_fs.abandon_lock(replacement_owner)

        retiring = FakeWindowsFs()
        retirement_owner = retiring.lock_exclusive(canonical, retired)
        displaced_retired = fixture.target / ".retired-old-owner"
        foreign_retired_identity = (1, 2, 799)
        retirement_swapped = []

        def swap_retired_after_verify(point):
            if (point == "INSTALL_AFTER_LOCK_MOVE_TO_RETIRED" and
                    not retirement_swapped):
                retiring.state[displaced_retired] = retiring.state.pop(retired)
                retiring.attributes[displaced_retired] = (
                    retiring.attributes.pop(retired))
                retiring.identities[displaced_retired] = (
                    retiring.identities.pop(retired))
                retiring.links[displaced_retired] = retiring.links.pop(retired)
                retiring.state[retired] = retirement_owner.marker
                retiring.attributes[retired] = retiring.FILE_ATTRIBUTE_NORMAL
                retiring.identities[retired] = foreign_retired_identity
                retiring.links[retired] = 1
                retirement_swapped.append(point)

        with self.assertRaises(install_runtime_map_data.DurableFsError):
            retiring.retire_lock(
                retirement_owner, swap_retired_after_verify)
        self.assertEqual(
            retirement_swapped, ["INSTALL_AFTER_LOCK_MOVE_TO_RETIRED"])
        self.assertEqual(retiring.state[retired], retirement_owner.marker)
        self.assertEqual(retiring.identities[retired], foreign_retired_identity)
        self.assertEqual(
            retiring.state[displaced_retired], retirement_owner.marker)

        deleting = FakeWindowsFs()
        owned = fixture.target / ".owned-delete"
        displaced = fixture.target / ".owned-delete.displaced"
        deleting.state[owned] = b"same-bytes"
        deleting.attributes[owned] = deleting.FILE_ATTRIBUTE_NORMAL
        deleting.identities[owned] = (1, 2, 700)
        deleting.links[owned] = 1
        original_identity = deleting.identities[owned]
        replacement_identity = (1, 2, 701)
        swapped = []

        def swap_after_verify(point):
            if point == "TEST_WINDOWS_REMOVE_AFTER_VERIFY" and not swapped:
                deleting.state[displaced] = deleting.state.pop(owned)
                deleting.attributes[displaced] = deleting.attributes.pop(owned)
                deleting.identities[displaced] = deleting.identities.pop(owned)
                deleting.links[displaced] = deleting.links.pop(owned)
                deleting.state[owned] = b"same-bytes"
                deleting.attributes[owned] = deleting.FILE_ATTRIBUTE_NORMAL
                deleting.identities[owned] = replacement_identity
                deleting.links[owned] = 1
                swapped.append(point)

        with self.assertRaises(install_runtime_map_data.DurableFsError):
            deleting.remove_owned(
                owned,
                source_hash=hashlib.sha256(b"same-bytes").hexdigest(),
                expected_mode=deleting.FILE_ATTRIBUTE_NORMAL,
                expected_identity=original_identity,
                fault=swap_after_verify,
                fault_point="TEST_WINDOWS_REMOVE_AFTER_VERIFY")
        self.assertEqual(swapped, ["TEST_WINDOWS_REMOVE_AFTER_VERIFY"])
        self.assertEqual(deleting.state[owned], b"same-bytes")
        self.assertEqual(deleting.identities[owned], replacement_identity)
        self.assertNotIn(displaced, deleting.state)
        disposition = [call for call in deleting.calls
                       if call[0] == "set-disposition"]
        self.assertEqual(len(disposition), 1)
        delete_open = next(
            call for call in deleting.calls
            if call[0] == "create-file" and call[1] == owned.name)
        self.assertEqual(delete_open[3],
                         deleting.FILE_SHARE_READ |
                         deleting.FILE_SHARE_WRITE)

        foreign = FakeWindowsFs()
        foreign._volume_serial = lambda path: (
            1 if Path(path).name == "source" else 2)
        with self.assertRaisesRegex(
                install_runtime_map_data.DurableFsError,
                "native=contract:CROSS_VOLUME"):
            install_runtime_map_data.WindowsDurableFs.replace_same_volume(
                foreign, Path("source"), Path("target") / "leaf")

        native_failure = FakeWindowsFs()
        native_failure._volume_serial = lambda _path: (_ for _ in ()).throw(
            native_failure._error("ReplaceSameVolume", Path("source"), 5))
        with self.assertRaises(install_runtime_map_data.DurableFsError) as caught:
            install_runtime_map_data.WindowsDurableFs.replace_same_volume(
                native_failure, Path("source"), Path("target") / "leaf")
        self.assertIn('source="source"', str(caught.exception))
        self.assertIn('destination="target/leaf"', str(caught.exception))
        self.assertIn("native=win32:5:", str(caught.exception))

    @unittest.skipIf(os.name == "nt", "POSIX hard-link identity fixture")
    def test_durable_fs_lock_rejects_same_inode_retired_marker(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        owner = durable_fs.lock_exclusive(canonical, retired)
        durable_fs.abandon_lock(owner)
        os.link(canonical, retired)

        with self.assertRaises(install_runtime_map_data.DurableFsError) as caught:
            durable_fs.lock_exclusive(canonical, retired)

        self.assertIn("DURABLE_FS_ERROR op=LockExclusive", str(caught.exception))
        self.assertTrue(canonical.exists())
        self.assertTrue(retired.exists())

    @unittest.skipIf(os.name == "nt", "POSIX update-swap fixture")
    def test_journal_update_swap_is_retained_and_blocks_fresh_startup(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)

        class SwapJournalUpdateFs(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.update = None
                self.displaced = None

            def replace_same_volume(self, source, destination):
                if (self.update is None and source.name.startswith(
                        ".garner-runtime-install.journal.")):
                    self.update = source
                    self.displaced = Path(str(source) + ".owned")
                    os.replace(source, self.displaced)
                    source.write_bytes(b"foreign-update-sentinel")
                    os.chmod(source, 0o600)
                    raise install_runtime_map_data.DurableFsError(
                        "injected journal update replacement")
                return super().replace_same_volume(source, destination)

        swapping = SwapJournalUpdateFs()
        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=swapping)
        self.assertIsNotNone(swapping.update)
        self.assertEqual(
            swapping.update.read_bytes(), b"foreign-update-sentinel")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn(swapping.update, caught.exception.recovery_paths)
        self.assertEqual(
            swapping.update.read_bytes(), b"foreign-update-sentinel")

    @unittest.skipIf(os.name == "nt", "POSIX barrier-failure fixture")
    def test_journal_rename_before_barrier_is_recovery_required(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        journal = fixture.target / install_runtime_map_data._JOURNAL_NAME

        class FailFirstJournalBarrier(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.failed = False

            def sync_directory_or_equivalent(
                    self, directory, entries_to_finalize=()):
                if not self.failed and journal.exists():
                    self.failed = True
                    raise install_runtime_map_data.DurableFsError(
                        "journal rename durability uncertain")
                return super().sync_directory_or_equivalent(
                    directory, entries_to_finalize)

        durable_fs = FailFirstJournalBarrier()
        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=durable_fs)
        self.assertTrue(durable_fs.failed)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertTrue(journal.exists())
        self.assertEqual(
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target), "OK")

    @unittest.skipIf(os.name == "nt", "POSIX barrier-failure fixture")
    def test_later_journal_rename_before_barrier_preserves_recovery_contract(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        journal = fixture.target / install_runtime_map_data._JOURNAL_NAME
        expected_command = install_runtime_map_data._recovery_command(
            fixture.manifest_path, fixture.target)

        class FailSecondJournalBarrier(install_runtime_map_data.PosixDurableFs):
            def __init__(self):
                self.journal_replacements = 0
                self.fail_next_barrier = False

            def replace_same_volume(self, source, destination):
                result = super().replace_same_volume(source, destination)
                if source.name.startswith(".garner-runtime-install.journal."):
                    self.journal_replacements += 1
                    self.fail_next_barrier = self.journal_replacements == 2
                return result

            def sync_directory_or_equivalent(
                    self, directory, entries_to_finalize=()):
                if self.fail_next_barrier:
                    self.fail_next_barrier = False
                    raise install_runtime_map_data.DurableFsError(
                        "second journal rename durability uncertain")
                return super().sync_directory_or_equivalent(
                    directory, entries_to_finalize)

        durable_fs = FailSecondJournalBarrier()
        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, durable_fs=durable_fs)

        self.assertEqual(durable_fs.journal_replacements, 2)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn(journal, caught.exception.recovery_paths)
        self.assertEqual(caught.exception.recovery_command, expected_command)
        self.assertTrue(journal.exists())

    def test_retired_journal_requires_independently_recorded_hash(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        retired = fixture.target / install_runtime_map_data._RETIRED_JOURNAL_NAME
        second_manifest, _ = fixture.create_second_run()
        reached = []

        def crash(point):
            if point == "INSTALL_AFTER_JOURNAL_RETIRE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=crash)
        self.assertEqual(reached, ["INSTALL_AFTER_JOURNAL_RETIRE"])
        journal = json.loads(retired.read_text("utf-8"))
        journal["manifestPath"] = second_manifest.as_posix()
        journal["manifestSha256"] = hashlib.sha256(
            second_manifest.read_bytes()).hexdigest()
        changed = json.dumps(
            journal, ensure_ascii=False, separators=(",", ":")).encode()
        retired.write_bytes(changed)

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                second_manifest, fixture.target)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(retired.read_bytes(), changed)

    def test_journal_rejects_lone_surrogate_before_runtime_mutation(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        block = fixture.target / "garner.block.raw"
        metadata = fixture.target / "garner.terrain.json"
        block.write_bytes(b"old-block")
        metadata.write_bytes(b"old-metadata")
        reached = []

        def crash(point):
            if point == "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE" and not reached:
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=crash)
        journal_path = fixture.target / install_runtime_map_data._JOURNAL_NAME
        journal = json.loads(journal_path.read_text("utf-8"))
        journal["manifestPath"] = "\ud800"
        journal_path.write_bytes(json.dumps(
            journal, ensure_ascii=True, separators=(",", ":")).encode())

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(block.read_bytes(), b"old-block")
        self.assertEqual(metadata.read_bytes(), b"old-metadata")

    def test_startup_reports_recorded_command_and_plural_evidence(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        (fixture.target / "garner.block.raw").write_bytes(b"old-block")
        (fixture.target / "garner.terrain.json").write_bytes(b"old-metadata")
        second_manifest, _ = fixture.create_second_run()
        reached = []

        def crash(point):
            if (point == "INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE" and
                    not reached):
                reached.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=crash)
        journal_path = fixture.target / install_runtime_map_data._JOURNAL_NAME
        recorded = json.loads(journal_path.read_text("utf-8"))
        expected_command = install_runtime_map_data._recovery_command(
            fixture.manifest_path, fixture.target)
        self.assertEqual(recorded["recoveryCommand"], expected_command)

        recovery_reached = []

        def fail_recovery(point):
            if point == "INSTALL_RECOVERY_BLOCK_REPLACE" and not recovery_reached:
                recovery_reached.append(point)
                return "fail"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                second_manifest, fixture.target, fault=fail_recovery)
        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertEqual(caught.exception.recovery_command, expected_command)
        self.assertGreaterEqual(len(caught.exception.recovery_paths), 3)
        self.assertIn(journal_path, caught.exception.recovery_paths)

    @unittest.skipIf(os.name == "nt", "POSIX retained-identity fixture")
    def test_posix_remove_owned_preserves_exact_bytes_path_replacement(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        owned = fixture.target / ".owned-cleanup"
        displaced = fixture.target / ".owned-cleanup.displaced"
        payload = b"same-bytes"
        owned.write_bytes(payload)
        os.chmod(owned, 0o600)
        recorded_identity = install_runtime_map_data._physical_identity(owned)
        recorded_hash = hashlib.sha256(payload).hexdigest()
        replacement_identity = []

        def swap(_point):
            os.replace(owned, displaced)
            owned.write_bytes(payload)
            os.chmod(owned, 0o600)
            replacement_identity.append(
                install_runtime_map_data._physical_identity(owned))

        with self.assertRaises(install_runtime_map_data.DurableFsError):
            durable_fs.remove_owned(
                owned,
                source_hash=recorded_hash,
                expected_mode=0o600,
                expected_identity=recorded_identity,
                fault=swap,
                fault_point="TEST_AFTER_OWNED_VERIFY",
            )

        self.assertEqual(owned.read_bytes(), payload)
        self.assertEqual(
            install_runtime_map_data._physical_identity(owned),
            replacement_identity[0])
        self.assertEqual(displaced.read_bytes(), payload)

    @unittest.skipIf(os.name == "nt", "POSIX retained-lock fixture")
    def test_posix_lock_retirement_preserves_exact_bytes_path_replacement(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        displaced = fixture.target / ".garner-runtime-install.lock.displaced"
        lock = durable_fs.lock_exclusive(canonical, retired)
        replacement_identity = []

        def swap(point):
            if (point == "INSTALL_AFTER_LOCK_RESERVATION_DURABLE" and
                    not replacement_identity):
                os.replace(canonical, displaced)
                canonical.write_bytes(lock.marker)
                os.chmod(canonical, 0o600)
                replacement_identity.append(
                    install_runtime_map_data._physical_identity(canonical))

        with self.assertRaises((install_runtime_map_data.DurableFsError,
                                OSError)):
            durable_fs.retire_lock(lock, swap)

        self.assertTrue(canonical.exists())
        self.assertEqual(canonical.read_bytes(), lock.marker)
        self.assertEqual(
            install_runtime_map_data._physical_identity(canonical),
            replacement_identity[0])
        self.assertEqual(displaced.read_bytes(), lock.marker)

    @unittest.skipIf(os.name == "nt", "POSIX exact marker fixture")
    def test_durable_fs_lock_rejects_marker_with_trailing_bytes(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        marker = (
            f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n".encode())
        canonical.write_bytes(marker + b"foreign-tail")

        with self.assertRaises(install_runtime_map_data.DurableFsError) as caught:
            durable_fs.lock_exclusive(canonical, retired)

        self.assertIn("DURABLE_FS_ERROR op=LockExclusive", str(caught.exception))
        self.assertEqual(canonical.read_bytes(), marker + b"foreign-tail")

    @unittest.skipIf(os.name == "nt", "POSIX exact-create fixture")
    def test_durable_fs_lock_rejects_preexisting_empty_file(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        canonical.write_bytes(b"")
        original_identity = install_runtime_map_data._physical_identity(canonical)

        with self.assertRaises(install_runtime_map_data.DurableFsError):
            durable_fs.lock_exclusive(canonical, retired)
        self.assertEqual(canonical.read_bytes(), b"")
        self.assertEqual(
            install_runtime_map_data._physical_identity(canonical),
            original_identity)

    @unittest.skipIf(os.name == "nt", "POSIX marker durability fixture")
    def test_lock_marker_internal_faults_are_replayable(self):
        points = (
            "INSTALL_LOCK_MARKER_AFTER_WRITE_BEFORE_FLUSH",
            "INSTALL_LOCK_MARKER_AFTER_FLUSH_BEFORE_READBACK",
            "INSTALL_LOCK_MARKER_AFTER_READBACK",
        )
        for point in points:
            with self.subTest(point=point):
                fixture = ManifestFixture()
                try:
                    durable_fs = install_runtime_map_data.PosixDurableFs()
                    canonical = fixture.target / ".garner-runtime-install.lock"
                    retired = fixture.target / ".garner-runtime-install.lock.retired"
                    marker = (
                        f"corsairs-durable-lock-v1\npath={canonical.as_posix()}\n"
                        .encode())
                    reached = []

                    def interrupt(candidate):
                        if candidate == point and not reached:
                            reached.append(candidate)
                            return "crash"
                        return None

                    with self.assertRaises(
                            install_runtime_map_data._InjectedFault):
                        durable_fs.lock_exclusive(
                            canonical, retired, fault=interrupt)
                    self.assertEqual(reached, [point])
                    self.assertEqual(canonical.read_bytes(), marker)
                    replayed = durable_fs.lock_exclusive(canonical, retired)
                    durable_fs.retire_lock(replayed)
                    self.assertFalse(canonical.exists())
                finally:
                    fixture.close()

    @unittest.skipIf(os.name == "nt", "POSIX stale-waiter fixture")
    def test_stale_waiter_never_becomes_new_canonical_owner(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        durable_fs = install_runtime_map_data.PosixDurableFs()
        canonical = fixture.target / ".garner-runtime-install.lock"
        retired = fixture.target / ".garner-runtime-install.lock.retired"
        owner = durable_fs.lock_exclusive(canonical, retired)
        stale = os.open(canonical, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
        old_identity = os.fstat(stale).st_ino
        try:
            with self.assertRaises(BlockingIOError):
                fcntl.flock(stale, fcntl.LOCK_EX | fcntl.LOCK_NB)
            durable_fs.retire_lock(owner)
            fcntl.flock(stale, fcntl.LOCK_EX | fcntl.LOCK_NB)
            replacement = durable_fs.lock_exclusive(canonical, retired)
            try:
                self.assertNotEqual(
                    old_identity, os.fstat(replacement.descriptor).st_ino)
                self.assertEqual(
                    install_runtime_map_data._physical_identity(canonical),
                    (os.fstat(replacement.descriptor).st_dev,
                     os.fstat(replacement.descriptor).st_ino,
                     os.fstat(replacement.descriptor).st_nlink))
            finally:
                durable_fs.retire_lock(replacement)
        finally:
            os.close(stale)

    def test_corrupt_journal_schema_is_recovery_required(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        for payload in (b"{}", b'{"version":"1"}'):
            with self.subTest(payload=payload):
                with self.assertRaises(
                        install_runtime_map_data.InstallError) as caught:
                    install_runtime_map_data._parse_journal(
                        payload, fixture.target)
                self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
                self.assertIn(
                    fixture.target /
                    ".garner-runtime-install.transaction.json",
                    caught.exception.recovery_paths)

    @unittest.skipIf(os.name == "nt", "POSIX hard-link journal fixture")
    def test_physical_journal_corruption_merges_all_recovery_evidence(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        journal = fixture.target / ".garner-runtime-install.transaction.json"
        alias = fixture.target / "journal-foreign-hardlink"
        stage = fixture.target / ".garner-runtime-install.stage.foreign.block"
        journal.write_bytes(b"{}")
        os.link(journal, alias)
        stage.write_bytes(b"owned-state-needs-recovery")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)

        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn(journal, caught.exception.recovery_paths)
        self.assertIn(stage, caught.exception.recovery_paths)

    def test_recovery_rejects_transaction_path_alias_without_deleting_it(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        injected = []

        def fault(point):
            if point == "INSTALL_AFTER_PREPARED_JOURNAL_DURABLE" and not injected:
                injected.append(point)
                return "crash"
            return None

        with self.assertRaises(install_runtime_map_data.InstallError):
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target, fault=fault)
        journal_path = (
            fixture.target / ".garner-runtime-install.transaction.json")
        journal = json.loads(journal_path.read_text(encoding="utf-8"))
        unrelated = fixture.target / "unrelated.txt"
        unrelated.write_bytes(b"must-survive")
        journal["entries"]["block"]["stage"] = unrelated.as_posix()
        journal_path.write_text(
            json.dumps(journal, sort_keys=True, separators=(",", ":")),
            encoding="utf-8")

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.install_runtime_map_data(
                fixture.manifest_path, fixture.target)

        self.assertEqual(caught.exception.status, "RECOVERY_REQUIRED")
        self.assertIn("install journal path", str(caught.exception))
        self.assertEqual(unrelated.read_bytes(), b"must-survive")

    def test_independent_manifest_budget_limits(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        mutations = (
            ("peakTextureCacheBytes", 32 * 1024 * 1024 + 1),
            ("peakRgbaRowBytes", 16 * 1024 + 1),
            ("pngBytes", 96 * 1024 * 1024 + 1),
            ("maxHeightErrorCm", 5.000001),
            ("rmsHeightErrorCm", 2.000001),
            ("sharedBoundaryMaxCm", 0.000001),
        )
        for key, value in mutations:
            with self.subTest(key=key):
                fixture.manifest["metrics"][key] = value
                fixture.write_manifest()
                with self.assertRaises(
                        install_runtime_map_data.InstallError) as caught:
                    install_runtime_map_data.validate_manifest(
                        fixture.manifest_path)
                self.assertEqual(
                    str(caught.exception),
                    f"WRITE_FAILED BUDGET_EXCEEDED /metrics/{key}")
                fixture.manifest["metrics"][key] = 1 if key not in (
                    "maxHeightErrorCm", "rmsHeightErrorCm",
                    "sharedBoundaryMaxCm") else 0

    def test_rejects_duplicate_texture_source_path_alias(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        duplicate = copy.deepcopy(
            fixture.manifest["source"]["usedTextures"][0])
        duplicate["textureId"] = 5
        fixture.manifest["usedTextureIds"].append(5)
        fixture.manifest["source"]["usedTextures"].append(duplicate)
        fixture.write_manifest()

        with self.assertRaises(install_runtime_map_data.InstallError) as caught:
            install_runtime_map_data.validate_manifest(fixture.manifest_path)

        self.assertEqual(
            str(caught.exception),
            "WRITE_FAILED INVALID_PROVENANCE "
            "/source/usedTextures/1/path: duplicate or case-alias source path")


if __name__ == "__main__":
    unittest.main()
