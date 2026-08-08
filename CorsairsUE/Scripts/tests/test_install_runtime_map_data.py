import copy
import hashlib
import inspect
import json
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest

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

    def test_installs_verified_pair_and_cleans_owned_transaction(self):
        fixture = ManifestFixture()
        self.addCleanup(fixture.close)
        result = install_runtime_map_data.install_runtime_map_data(
            fixture.manifest_path, fixture.target)
        self.assertEqual(result, "OK")
        self.assertEqual((fixture.target / "garner.block.raw").read_bytes(), b"x")
        self.assertEqual((fixture.target / "garner.terrain.json").read_bytes(), b"x")
        self.assertEqual(
            list(fixture.target.glob(".garner-runtime-install*")), [])
        self.assertFalse((fixture.target / "garner.height.r16").exists())

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
        self.assertEqual(
            str(caught.exception),
            "RECOVERY_REQUIRED unexpected retired runtime artifact: "
            f"{foreign.as_posix()} paths={json.dumps(foreign.as_posix())}")
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

            def lock_exclusive(self, canonical, retired):
                del canonical, retired
                return self.lock

            def retire_lock(self, lock):
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
