import copy
import hashlib
import json
import os
from pathlib import Path
import plistlib
import tempfile
import unittest

from CorsairsUE.Scripts import reference_terrain_rules as rules


HASH_X = hashlib.sha256(b"x").hexdigest()
HASH_Y = hashlib.sha256(b"y").hexdigest()
TXN = "1" * 32
HEAD = "2" * 40
LEAVES = {
    "height": "garner.height.r16",
    "block": "garner.block.raw",
    "region": "garner.region.raw",
    "terrainMetadata": "garner.terrain.json",
    "albedo": "garner.albedo_17_21.png",
    "meshGltf": "garner.terrain_17_21.gltf",
    "meshBin": "garner.terrain_17_21.bin",
}


class Fixture:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".corsairs-task8-rules-", dir=Path.cwd())
        self.root = Path(self.temporary.name)
        self.maps = self.root / "artifacts/maps"
        self.run = self.maps / "runs/run-001"
        self.run.mkdir(parents=True)
        for leaf in LEAVES.values():
            (self.run / leaf).write_bytes(b"x")
        for suffix in (
            "Client/map/garner.map",
            "databases/gamedata.sqlite",
            "Client/texture/terrain/alpha/total.png",
            "Client/texture/terrain/brick05.png",
        ):
            path = self.root / suffix
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
        rel = lambda suffix: Path(suffix).as_posix()
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
                "usedTextures": [{
                    "textureId": 4,
                    "path": rel("Client/texture/terrain/brick05.png"),
                    "sha256": HASH_X,
                }],
            },
            "page": {
                "x": 17,
                "y": 21,
                "sourceCellBounds": {
                    "x": 2176, "y": 2688, "width": 128, "height": 128,
                },
                "pixelsPerCell": 32,
                "pixelWidth": 4096,
                "pixelHeight": 4096,
                "ambient": [1, 1, 1],
                "dwTColor": 0,
            },
            "requiredPresentRect": {
                "x": 2193, "y": 2756, "width": 80, "height": 47,
            },
            "usedTextureIds": [4],
            "sectionPresence": {
                "originX": 272, "originY": 336, "width": 16, "height": 16,
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
        self.manifest_path = self.maps / "garner.reference-albedo.json"
        self.manifest_path.write_text(
            json.dumps(self.manifest, sort_keys=True, separators=(",", ":")),
            encoding="utf-8")

    def close(self):
        self.temporary.cleanup()

    def file_evidence(self, path):
        path = Path(path)
        raw = path.read_bytes()
        return {
            "path": path.relative_to(self.root).as_posix(),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "sizeBytes": len(raw),
        }


def package_file(path, repo_root):
    raw = Path(path).read_bytes()
    return {
        "path": Path(path).relative_to(repo_root).as_posix(),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "sizeBytes": len(raw),
    }


def package_family(name, package, files, repo_root):
    evidence = [package_file(path, repo_root) for path in sorted(files)]
    payload = json.dumps(
        evidence, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")
    return {
        "family": name,
        "package": package,
        "files": evidence,
        "familySha256": hashlib.sha256(payload).hexdigest(),
    }


def reference_state():
    paths = rules.asset_paths("garner", 17, 21)
    return {
        "mesh": {
            "objectPath": paths["mesh"],
            "sourceGltfSha256": HASH_X,
            "sourceBinSha256": HASH_X,
            "naniteEnabled": True,
            "materialSlot0": paths["instance"],
        },
        "texture": {
            "objectPath": paths["texture"],
            "sourceWidth": 4096,
            "sourceHeight": 4096,
            "sourceFormat": "RGBA8",
            "sourceSha256": HASH_X,
            "srgb": True,
            "compression": "TC_DEFAULT",
            "filter": "TF_BILINEAR",
            "addressX": "TA_CLAMP",
            "addressY": "TA_CLAMP",
            "mipGenSettings": "TMGS_FROM_TEXTURE_GROUP",
            "lodGroup": "TEXTUREGROUP_WORLD",
            "lodBias": 0,
            "neverStream": False,
        },
        "material": {
            "objectPath": paths["material"],
            "blendMode": "BLEND_MASKED",
            "shadingModel": "MSM_UNLIT",
            "parameterName": "BaseColorTexture",
            "samplerType": "SAMPLERTYPE_COLOR",
            "samplerSource": "SSM_FROM_TEXTURE_ASSET",
            "rgbOutput": "MP_EMISSIVE_COLOR",
            "alphaOutput": "MP_OPACITY_MASK",
            "usageFlags": ["MATUSAGE_NANITE", "MATUSAGE_STATIC_MESH"],
        },
        "instance": {
            "objectPath": paths["instance"],
            "parent": paths["material"],
            "baseColorTexture": paths["texture"],
        },
        "actor": {
            "objectPath": rules.reference_actor_object_path(
                rules.REFERENCE_MAP_PACKAGE, "garner", 17, 21),
            "className": "/Script/Engine.StaticMeshActor",
            "label": "ReferenceTerrain_Garner_17_21",
            "tag": "CorsairsReferenceTerrain",
            "locationCm": [217600.0, -268800.0, 0.0],
            "staticMesh": paths["mesh"],
            "boundsMinCm": [217600.0, -281600.0],
            "boundsMaxCm": [230400.0, -268800.0],
            "componentMaterialSlot0": paths["instance"],
        },
    }


class ReferenceTerrainRulesTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)

    def _sandbox_attestation_fixture(self):
        app = (
            self.fixture.root /
            f"artifacts/maps/package-run/{TXN}/archive/Mac/Actual.app")
        info_path = app / "Contents/Info.plist"
        info_path.parent.mkdir(parents=True, exist_ok=True)
        info_payload = plistlib.dumps({
            "CFBundleIdentifier": "com.example.CorsairsUE",
        }, fmt=plistlib.FMT_XML, sort_keys=True)
        info_path.write_bytes(info_payload)
        package_reports = (
            self.fixture.root / f"artifacts/maps/reports/runs/{TXN}/package")
        package_reports.mkdir(parents=True, exist_ok=True)
        entitlements_path = package_reports / "app-entitlements.plist"
        entitlement_value = {
            "com.apple.security.app-sandbox": True,
            "com.apple.security.get-task-allow": True,
        }
        entitlements_payload = plistlib.dumps(
            entitlement_value, fmt=plistlib.FMT_XML, sort_keys=True)
        entitlements_path.write_bytes(entitlements_payload)
        attestation = {
            "appBundlePath": app.relative_to(self.fixture.root).as_posix(),
            "appSandbox": True,
            "automationReportsRelativePath": (
                "Library/Application Support/Epic/CorsairsUE/"
                "Saved/Automation/Reports"),
            "bundleIdentifier": "com.example.CorsairsUE",
            "codesignVerified": True,
            "containerDataLeaf": "Data",
            "containerMetadataCreator": "com.example.CorsairsUE",
            "containerMetadataIdentifier": "com.example.CorsairsUE",
            "entitlementsPlist": self.fixture.file_evidence(entitlements_path),
            "infoPlist": self.fixture.file_evidence(info_path),
            "issues": [],
            "reportType": "garner-terrain-app-sandbox",
            "schemaVersion": 1,
            "signingIdentifier": "com.example.CorsairsUE",
            "sourceHead": HEAD,
            "status": "PASS",
            "transactionId": TXN,
        }
        return attestation, {
            "app": app,
            "info": info_path,
            "infoPayload": info_payload,
            "entitlements": entitlements_path,
            "entitlementsPayload": entitlements_payload,
            "packageReports": package_reports,
        }

    def _assert_sandbox_attestation_issue(self, attestation, code, field):
        issues = rules.validate_sandbox_attestation(
            attestation, self.fixture.root)
        self.assertTrue(issues)
        self.assertEqual((issues[0]["code"], issues[0]["field"]), (code, field))

    def _write_sandbox_attestation_evidence(
            self, attestation, paths, leaf="app-sandbox.json"):
        path = paths["packageReports"] / leaf
        path.write_bytes(rules.canonical_json_bytes(attestation) + b"\n")
        return self.fixture.file_evidence(path)

    def test_normalized_relative_rejects_dot_and_empty_components(self):
        self.assertFalse(rules._normalized_relative("."))
        self.assertFalse(rules._normalized_relative(""))
        self.assertFalse(rules._normalized_relative("Reports//index.json"))
        self.assertFalse(rules._normalized_relative("Reports/"))
        self.assertTrue(rules._normalized_relative("Reports"))

    def test_asset_paths_are_canonical(self):
        self.assertEqual(rules.asset_paths("garner", 17, 21), {
            "mesh": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
            "texture": "/Game/Terrain/Reference/Garner/T_Garner_17_21",
            "material": "/Game/Terrain/Reference/M_TerrainReference",
            "instance": "/Game/Terrain/Reference/Garner/MI_Garner_17_21",
        })

    def test_reference_actor_path_includes_world_object(self):
        self.assertEqual(
            rules.reference_actor_object_path("/Game/Maps/Garner", "garner", 17, 21),
            "/Game/Maps/Garner.Garner:PersistentLevel."
            "ReferenceTerrain_Garner_17_21")

    def test_overlap_is_strict_on_all_edges(self):
        page = (0.0, 0.0, 10.0, 10.0)
        actors = [
            ("inside", 1.0, 1.0, 2.0, 2.0),
            ("left-edge", -2.0, 1.0, 0.0, 2.0),
            ("right-edge", 10.0, 1.0, 12.0, 2.0),
            ("bottom-edge", 1.0, -2.0, 2.0, 0.0),
            ("top-edge", 1.0, 10.0, 2.0, 12.0),
            ("crossing", -1.0, 5.0, 11.0, 6.0),
        ]
        self.assertEqual(
            rules.overlapping_legacy_tiles(page, actors),
            ["crossing", "inside"])

    def test_legacy_entry_points_reject_reference_namespace(self):
        for entry in ("setup_terrain_material", "apply_terrain_material", "place_terrain"):
            with self.subTest(entry=entry):
                with self.assertRaises(ValueError):
                    rules.assert_legacy_target_allowed(
                        entry, "/Game/Terrain/Reference/Garner/SM_Garner_17_21")
                rules.assert_legacy_target_allowed(entry, "/Game/Terrain/Garner_tile")

    def test_validate_manifest_mutation_matrix(self):
        self.assertEqual(
            rules.validate_manifest(self.fixture.manifest, self.fixture.manifest_path), [])
        mutations = [
            ("/schemaVersion", lambda value: value.pop("schemaVersion")),
            ("/algorithmVersion", lambda value: value.__setitem__("algorithmVersion", "x")),
            ("/page/x", lambda value: value["page"].__setitem__("x", 18)),
            ("/files/albedo/sha256", lambda value: value["files"]["albedo"].__setitem__("sha256", HASH_Y)),
            ("/metrics/peakRssBytes", lambda value: value["metrics"].__setitem__("peakRssBytes", 0)),
            ("/metrics/totalOutputBytes", lambda value: value["metrics"].__setitem__("totalOutputBytes", 6)),
        ]
        for field, mutate in mutations:
            with self.subTest(field=field):
                changed = copy.deepcopy(self.fixture.manifest)
                mutate(changed)
                issues = rules.validate_manifest(changed, self.fixture.manifest_path)
                self.assertTrue(issues)
                self.assertEqual(issues[0]["field"], field)

    def _map_family(self):
        primary = self.fixture.root / "CorsairsUE/Content/Maps/Garner.umap"
        primary.parent.mkdir(parents=True, exist_ok=True)
        primary.write_bytes(b"x")
        return package_family(
            "map", "/Game/Maps/Garner", [primary], self.fixture.root)

    def _level_report(self):
        return {
            "schemaVersion": 1,
            "reportType": "garner-reference-terrain-level-build",
            "status": "PASS",
            "transactionId": TXN,
            "sourceHead": HEAD,
            "manifest": self.fixture.file_evidence(self.fixture.manifest_path),
            "mapPackage": rules.REFERENCE_MAP_PACKAGE,
            "worldObject": rules.REFERENCE_WORLD_OBJECT,
            "mapPackageHash": self._map_family(),
            "markerObject": rules.REFERENCE_BUILD_MARKER,
            "gameModeClass": rules.REFERENCE_GAME_MODE,
            "replacedExistingMap": False,
            "issues": [],
        }

    def _families(self):
        families = []
        for family in rules.PACKAGE_FAMILIES:
            stem = self.fixture.root / rules.PACKAGE_STEMS[family]
            stem.parent.mkdir(parents=True, exist_ok=True)
            primary = stem.with_suffix(".umap" if family == "map" else ".uasset")
            primary.write_bytes(b"x")
            families.append(rules.package_family_evidence(
                family, rules.PACKAGE_PATHS[family], stem, self.fixture.root))
        return families

    def _import_report(self):
        families = self._families()
        return {
            "schemaVersion": 1,
            "reportType": "garner-reference-terrain-import",
            "status": "PASS",
            "transactionId": TXN,
            "sourceHead": HEAD,
            "manifest": self.fixture.file_evidence(self.fixture.manifest_path),
            "mapPackage": rules.REFERENCE_MAP_PACKAGE,
            "worldObject": rules.REFERENCE_WORLD_OBJECT,
            "markerObject": rules.REFERENCE_BUILD_MARKER,
            "gameModeClass": rules.REFERENCE_GAME_MODE,
            "beforeMapPackageHash": copy.deepcopy(families[4]),
            "referenceState": reference_state(),
            "created": [],
            "updated": [],
            "deleted": [],
            "savedPackages": [],
            "finalPackageHashes": families,
            "issues": [],
        }

    def test_level_build_report_requires_corsairs_game_mode(self):
        report = self._level_report()
        self.assertEqual(
            rules.validate_level_build_report(report, self.fixture.root), [])
        report["gameModeClass"] = "/Script/Engine.GameModeBase"
        issues = rules.validate_level_build_report(report, self.fixture.root)
        self.assertTrue(issues)
        self.assertEqual(issues[0]["field"], "/gameModeClass")

    def test_validate_level_build_report_mutation_matrix(self):
        report = self._level_report()
        for key in tuple(report):
            with self.subTest(key=key):
                changed = copy.deepcopy(report)
                changed.pop(key)
                issues = rules.validate_level_build_report(
                    changed, self.fixture.root)
                self.assertTrue(issues)
                self.assertEqual(issues[0]["field"], f"/{key}")

    def test_validate_import_report_mutation_matrix(self):
        report = self._import_report()
        self.assertEqual(
            rules.validate_import_report(report, self.fixture.root), [])
        for key in tuple(report):
            with self.subTest(key=key):
                changed = copy.deepcopy(report)
                changed.pop(key)
                issues = rules.validate_import_report(changed, self.fixture.root)
                self.assertTrue(issues)
                self.assertEqual(issues[0]["field"], f"/{key}")

    def test_import_report_binds_reference_sources_to_manifest(self):
        report = self._import_report()
        report["referenceState"]["texture"]["sourceSha256"] = HASH_Y
        issues = rules.validate_import_report(report, self.fixture.root)
        self.assertTrue(issues)
        self.assertEqual(
            issues[0]["field"], "/referenceState/texture/sourceSha256")

        report = self._import_report()
        report["referenceState"]["mesh"]["sourceBinSha256"] = HASH_Y
        issues = rules.validate_import_report(report, self.fixture.root)
        self.assertTrue(issues)
        self.assertEqual(
            issues[0]["field"], "/referenceState/mesh/sourceBinSha256")

    def test_editor_report_target_is_transaction_private(self):
        run_root = self.fixture.root / f"artifacts/maps/reports/runs/{TXN}"
        run_root.mkdir(parents=True)
        accepted = run_root / "report.json"
        self.assertEqual(
            rules.validate_editor_identity_target(
                accepted, TXN, HEAD, self.fixture.root), [])
        for path, transaction, head in (
            (self.fixture.root / "artifacts/maps/reports/report.json", TXN, HEAD),
            (accepted, "bad", HEAD),
            (accepted, TXN, "bad"),
        ):
            with self.subTest(path=path, transaction=transaction, head=head):
                self.assertTrue(rules.validate_editor_identity_target(
                    path, transaction, head, self.fixture.root))

    def test_inventory_cook_and_runtime_report_mutation_matrices(self):
        runtime = {
            "schemaVersion": 1,
            "reportType": "garner-terrain-packaged-runtime",
            "status": "PASS",
            "transactionId": TXN,
            "sourceHead": HEAD,
            "mapPackage": rules.REFERENCE_MAP_PACKAGE,
            "worldObject": rules.REFERENCE_WORLD_OBJECT,
            "gameModeClass": rules.REFERENCE_GAME_MODE,
            "actorObject": rules.reference_actor_object_path(
                rules.REFERENCE_MAP_PACKAGE, "garner", 17, 21),
            "runtimeInputs": [
                {"projectRelativePath": path, "sha256": HASH_X, "sizeBytes": 1}
                for path in rules.RUNTIME_INPUT_PATHS
            ],
            "issues": [],
        }
        self.assertEqual(rules.validate_runtime_observation(runtime), [])
        for key in tuple(runtime):
            with self.subTest(dto="runtime", key=key):
                changed = copy.deepcopy(runtime)
                changed.pop(key)
                issues = rules.validate_runtime_observation(changed)
                self.assertTrue(issues)
                self.assertEqual(issues[0]["field"], f"/{key}")

        inventory_root = self.fixture.root / f"artifacts/maps/package-run/{TXN}/stage"
        inventory_root.mkdir(parents=True)
        member = inventory_root / "one.bin"
        member.write_bytes(b"x")
        inventory = {
            "schemaVersion": 1,
            "reportType": "garner-terrain-stage-inventory",
            "transactionId": TXN,
            "sourceHead": HEAD,
            "root": inventory_root.relative_to(self.fixture.root).as_posix(),
            "files": [self.fixture.file_evidence(member)],
            "issues": [],
        }
        self.assertEqual(
            rules.validate_inventory_report(inventory, self.fixture.root), [])
        changed = copy.deepcopy(inventory)
        changed["files"][0]["sha256"] = HASH_Y
        issues = rules.validate_inventory_report(changed, self.fixture.root)
        self.assertTrue(issues)
        self.assertEqual(issues[0]["field"], "/files/0/sha256")

    def test_inventory_accepts_framework_generated_empty_files(self):
        inventory_root = (
            self.fixture.root /
            f"artifacts/maps/package-run/{TXN}/automation-empty")
        inventory_root.mkdir(parents=True)
        member = inventory_root / "empty.log"
        member.write_bytes(b"")
        inventory = {
            "schemaVersion": 1,
            "reportType": "garner-terrain-stage-inventory",
            "transactionId": TXN,
            "sourceHead": HEAD,
            "root": inventory_root.relative_to(self.fixture.root).as_posix(),
            "files": [rules.file_evidence(
                member, self.fixture.root, allow_empty=True)],
            "issues": [],
        }
        self.assertEqual(
            rules.validate_inventory_report(inventory, self.fixture.root), [])

    def test_sandbox_attestation_accepts_sanitized_literal_fixture(self):
        attestation, paths = self._sandbox_attestation_fixture()
        self.assertEqual(
            rules.validate_sandbox_attestation(attestation, self.fixture.root), [])
        single_component = copy.deepcopy(attestation)
        for key in (
                "bundleIdentifier", "signingIdentifier",
                "containerMetadataIdentifier", "containerMetadataCreator"):
            single_component[key] = "CorsairsUE"
        paths["info"].write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "CorsairsUE",
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        single_component["infoPlist"] = self.fixture.file_evidence(paths["info"])
        self.assertEqual(
            rules.validate_sandbox_attestation(
                single_component, self.fixture.root), [])
        executable = {
            "path": (
                f"artifacts/maps/package-run/{TXN}/archive/Mac/Actual.app/"
                "Contents/MacOS/Actual"),
        }
        self.assertTrue(rules._attested_app_contains_executable(
            attestation, executable))
        changed_app = copy.deepcopy(attestation)
        changed_app["appBundlePath"] = (
            f"artifacts/maps/package-run/{TXN}/archive/Mac/Other.app")
        self.assertFalse(rules._attested_app_contains_executable(
            changed_app, executable))

    def test_sandbox_attestation_top_level_mutation_matrix(self):
        attestation, _ = self._sandbox_attestation_fixture()
        top_level_fields = (
            "schemaVersion", "reportType", "status", "transactionId",
            "sourceHead", "appBundlePath", "bundleIdentifier",
            "signingIdentifier", "infoPlist", "entitlementsPlist",
            "codesignVerified", "appSandbox", "containerDataLeaf",
            "automationReportsRelativePath", "containerMetadataIdentifier",
            "containerMetadataCreator", "issues",
        )
        self.assertEqual(set(top_level_fields), set(attestation))
        for field in top_level_fields:
            with self.subTest(mutation="delete", field=field):
                changed = copy.deepcopy(attestation)
                changed.pop(field)
                self._assert_sandbox_attestation_issue(
                    changed, "INVALID_SCHEMA", f"/{field}")

        type_cases = (
            ("schemaVersion", "1", "INVALID_SCHEMA", "/schemaVersion"),
            ("reportType", [], "INVALID_SCHEMA", "/reportType"),
            ("status", [], "INVALID_SCHEMA", "/status"),
            ("transactionId", [], "INVALID_IDENTITY", "/transactionId"),
            ("sourceHead", [], "INVALID_IDENTITY", "/sourceHead"),
            ("appBundlePath", [], "INVALID_FILE", "/appBundlePath"),
            ("bundleIdentifier", [], "INVALID_IDENTITY", "/bundleIdentifier"),
            ("signingIdentifier", [], "INVALID_IDENTITY", "/signingIdentifier"),
            ("infoPlist", [], "INVALID_SCHEMA", "/infoPlist"),
            ("entitlementsPlist", [], "INVALID_SCHEMA", "/entitlementsPlist"),
            ("codesignVerified", 1, "INVALID_SCHEMA", "/codesignVerified"),
            ("appSandbox", 1, "INVALID_SCHEMA", "/appSandbox"),
            ("containerDataLeaf", [], "INVALID_SCHEMA", "/containerDataLeaf"),
            ("automationReportsRelativePath", [], "INVALID_FILE",
             "/automationReportsRelativePath"),
            ("containerMetadataIdentifier", [], "INVALID_IDENTITY",
             "/containerMetadataIdentifier"),
            ("containerMetadataCreator", [], "INVALID_IDENTITY",
             "/containerMetadataCreator"),
            ("issues", {}, "INVALID_SCHEMA", "/issues"),
        )
        for field, replacement, code, pointer in type_cases:
            with self.subTest(mutation="mistype", field=field):
                changed = copy.deepcopy(attestation)
                changed[field] = replacement
                self._assert_sandbox_attestation_issue(changed, code, pointer)

        changed = copy.deepcopy(attestation)
        changed["unexpected"] = True
        self._assert_sandbox_attestation_issue(
            changed, "INVALID_SCHEMA", "/unexpected")

        relation_cases = (
            ("schema-version", "schemaVersion", 2,
             "INVALID_SCHEMA", "/schemaVersion"),
            ("report-type", "reportType", "other",
             "INVALID_SCHEMA", "/reportType"),
            ("status", "status", "FAIL", "INVALID_SCHEMA", "/status"),
            ("transaction-shape", "transactionId", "1" * 31,
             "INVALID_IDENTITY", "/transactionId"),
            ("head-shape", "sourceHead", "2" * 39,
             "INVALID_IDENTITY", "/sourceHead"),
            ("app-path", "appBundlePath", "/private/Actual.app",
             "INVALID_FILE", "/appBundlePath"),
            ("bundle-shape", "bundleIdentifier", "not_a_bundle",
             "INVALID_IDENTITY", "/bundleIdentifier"),
            ("bundle-relation", "bundleIdentifier", "com.example.Other",
             "INVALID_IDENTITY", "/signingIdentifier"),
            ("signing-relation", "signingIdentifier", "com.example.Other",
             "INVALID_IDENTITY", "/signingIdentifier"),
            ("codesign", "codesignVerified", False,
             "INVALID_SCHEMA", "/codesignVerified"),
            ("sandbox", "appSandbox", False,
             "INVALID_SCHEMA", "/appSandbox"),
            ("data-leaf", "containerDataLeaf", "data",
             "INVALID_SCHEMA", "/containerDataLeaf"),
            ("absolute-reports", "automationReportsRelativePath", "/private/Reports",
             "INVALID_FILE", "/automationReportsRelativePath"),
            ("metadata-id", "containerMetadataIdentifier", "com.example.Other",
             "INVALID_IDENTITY", "/containerMetadataIdentifier"),
            ("metadata-creator", "containerMetadataCreator", "com.example.Other",
             "INVALID_IDENTITY", "/containerMetadataCreator"),
            ("pass-issues", "issues", [{
                "code": "TEST", "field": "/test", "detail": "failure",
            }], "INVALID_STATUS", "/issues"),
        )
        for name, field, replacement, code, pointer in relation_cases:
            with self.subTest(mutation="relation", case=name):
                changed = copy.deepcopy(attestation)
                changed[field] = replacement
                self._assert_sandbox_attestation_issue(changed, code, pointer)

    def test_sandbox_attestation_file_evidence_mutation_matrix(self):
        attestation, _ = self._sandbox_attestation_fixture()
        for evidence_key in ("infoPlist", "entitlementsPlist"):
            for field in ("path", "sha256", "sizeBytes"):
                with self.subTest(
                        evidence=evidence_key, mutation="delete", field=field):
                    changed = copy.deepcopy(attestation)
                    changed[evidence_key].pop(field)
                    self._assert_sandbox_attestation_issue(
                        changed, "INVALID_SCHEMA", f"/{evidence_key}/{field}")

            type_cases = (
                ("path", [], "INVALID_FILE"),
                ("sha256", [], "INVALID_HASH"),
                ("sizeBytes", "1", "INVALID_FILE"),
            )
            for field, replacement, code in type_cases:
                with self.subTest(
                        evidence=evidence_key, mutation="mistype", field=field):
                    changed = copy.deepcopy(attestation)
                    changed[evidence_key][field] = replacement
                    self._assert_sandbox_attestation_issue(
                        changed, code, f"/{evidence_key}/{field}")

            changed = copy.deepcopy(attestation)
            changed[evidence_key]["unexpected"] = True
            self._assert_sandbox_attestation_issue(
                changed, "INVALID_SCHEMA", f"/{evidence_key}/unexpected")

            cases = (
                ("absolute-path", "path", "/private/input.plist", "INVALID_FILE"),
                ("traversal-path", "path", "../input.plist", "INVALID_FILE"),
                ("malformed-hash", "sha256", "x", "INVALID_HASH"),
                ("wrong-hash", "sha256", "0" * 64, "INVALID_HASH"),
                ("zero-size", "sizeBytes", 0, "INVALID_FILE"),
                ("wrong-size", "sizeBytes",
                 attestation[evidence_key]["sizeBytes"] + 1, "INVALID_FILE"),
            )
            for name, field, replacement, code in cases:
                with self.subTest(evidence=evidence_key, mutation=name):
                    changed = copy.deepcopy(attestation)
                    changed[evidence_key][field] = replacement
                    self._assert_sandbox_attestation_issue(
                        changed, code, f"/{evidence_key}/{field}")

        wrong_info = copy.deepcopy(attestation)
        wrong_info["infoPlist"] = copy.deepcopy(attestation["entitlementsPlist"])
        self._assert_sandbox_attestation_issue(
            wrong_info, "INVALID_FILE", "/infoPlist/path")
        wrong_entitlements = copy.deepcopy(attestation)
        wrong_entitlements["entitlementsPlist"] = copy.deepcopy(
            attestation["infoPlist"])
        self._assert_sandbox_attestation_issue(
            wrong_entitlements, "INVALID_FILE", "/entitlementsPlist/path")

    def test_sandbox_attestation_rehashes_physical_file_mutation_matrix(self):
        attestation, paths = self._sandbox_attestation_fixture()
        for evidence_key, path_key, payload_key in (
                ("infoPlist", "info", "infoPayload"),
                ("entitlementsPlist", "entitlements", "entitlementsPayload")):
            path = paths[path_key]
            payload = paths[payload_key]
            auxiliary = path.with_name(path.name + ".mutation-source")
            for mutation, code, field in (
                    ("missing", "INVALID_FILE", "path"),
                    ("tampered", "INVALID_HASH", "sha256"),
                    ("symlink", "INVALID_FILE", "path"),
                    ("hardlink", "INVALID_FILE", "path")):
                with self.subTest(evidence=evidence_key, mutation=mutation):
                    if auxiliary.exists() or auxiliary.is_symlink():
                        auxiliary.unlink()
                    if path.exists() or path.is_symlink():
                        path.unlink()
                    path.write_bytes(payload)
                    try:
                        if mutation == "missing":
                            path.unlink()
                        elif mutation == "tampered":
                            path.write_bytes(b"x" * len(payload))
                        elif mutation == "symlink":
                            path.rename(auxiliary)
                            path.symlink_to(auxiliary.name)
                        else:
                            os.link(path, auxiliary)
                        self._assert_sandbox_attestation_issue(
                            copy.deepcopy(attestation), code,
                            f"/{evidence_key}/{field}")
                    finally:
                        if path.exists() or path.is_symlink():
                            path.unlink()
                        if auxiliary.exists() or auxiliary.is_symlink():
                            auxiliary.unlink()
                        path.write_bytes(payload)

    def test_sandbox_attestation_plist_content_mutation_matrix(self):
        attestation, paths = self._sandbox_attestation_fixture()
        changed = copy.deepcopy(attestation)
        paths["info"].write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "com.example.Other",
        }, fmt=plistlib.FMT_XML, sort_keys=True))
        changed["infoPlist"] = self.fixture.file_evidence(paths["info"])
        self._assert_sandbox_attestation_issue(
            changed, "INVALID_IDENTITY", "/infoPlist")
        paths["info"].write_bytes(paths["infoPayload"])

        entitlement_cases = (
            ("malformed", b"not-a-plist", "INVALID_REPORT"),
            ("non-dict", plistlib.dumps(
                ["com.apple.security.app-sandbox"],
                fmt=plistlib.FMT_XML, sort_keys=True), "INVALID_STATUS"),
            ("noncanonical", plistlib.dumps({
                "com.apple.security.app-sandbox": True,
            }, fmt=plistlib.FMT_BINARY, sort_keys=False), "INVALID_REPORT"),
            ("missing-sandbox", plistlib.dumps({
                "com.apple.security.get-task-allow": True,
            }, fmt=plistlib.FMT_XML, sort_keys=True), "INVALID_STATUS"),
            ("false-sandbox", plistlib.dumps({
                "com.apple.security.app-sandbox": False,
            }, fmt=plistlib.FMT_XML, sort_keys=True), "INVALID_STATUS"),
            ("mistyped-sandbox", plistlib.dumps({
                "com.apple.security.app-sandbox": "true",
            }, fmt=plistlib.FMT_XML, sort_keys=True), "INVALID_STATUS"),
        )
        for name, payload, code in entitlement_cases:
            with self.subTest(mutation=name):
                paths["entitlements"].write_bytes(payload)
                changed = copy.deepcopy(attestation)
                changed["entitlementsPlist"] = self.fixture.file_evidence(
                    paths["entitlements"])
                self._assert_sandbox_attestation_issue(
                    changed, code, "/entitlementsPlist")
        paths["entitlements"].write_bytes(paths["entitlementsPayload"])

    def test_cook_validator_prefixes_nested_sandbox_attestation_failures(self):
        attestation, paths = self._sandbox_attestation_fixture()
        outer_evidence = {}
        for name in ("receipt", "stage", "archive", "executable"):
            path = paths["packageReports"] / f"outer-{name}.bin"
            path.write_bytes(name.encode("ascii"))
            outer_evidence[name] = self.fixture.file_evidence(path)

        def cook_report(sandbox_evidence):
            return {
                "archiveManifest": outer_evidence["archive"],
                "containerLists": [],
                "containers": [],
                "corsairsImportLeaks": [],
                "issues": [],
                "packagedExecutable": outer_evidence["executable"],
                "reportType": "garner-terrain-cook-package",
                "runtimeFiles": [],
                "sandboxAttestation": sandbox_evidence,
                "schemaVersion": 1,
                "sourceHead": HEAD,
                "stageManifest": outer_evidence["stage"],
                "status": "PASS",
                "targetReceipt": outer_evidence["receipt"],
                "transactionId": TXN,
            }

        for name, mutate, code, field in (
                ("nested-field", lambda value: value.__setitem__(
                    "appSandbox", False), "INVALID_SCHEMA",
                 "/sandboxAttestation/appSandbox"),
                ("nested-identity", lambda value: value.__setitem__(
                    "sourceHead", "3" * 40), "INVALID_IDENTITY",
                 "/sandboxAttestation")):
            with self.subTest(mutation=name):
                changed = copy.deepcopy(attestation)
                mutate(changed)
                evidence = self._write_sandbox_attestation_evidence(
                    changed, paths, f"{name}.json")
                issues = rules.validate_cook_package_report(
                    cook_report(evidence), self.fixture.root)
                self.assertTrue(issues)
                self.assertEqual(
                    (issues[0]["code"], issues[0]["field"]), (code, field))

    def test_base_bundle_rejects_mutable_report_receipt_or_binary_path(self):
        run_root = (
            self.fixture.root / f"artifacts/maps/reports/runs/{TXN}")
        automation_root = run_root / "editor-automation"
        automation_root.mkdir(parents=True)
        automation_file = automation_root / "index.json"
        automation_file.write_bytes(b"{}")
        evidence_set = {
            "transactionId": TXN,
            "sourceHead": HEAD,
            "root": automation_root.relative_to(self.fixture.root).as_posix(),
            "files": [self.fixture.file_evidence(automation_file)],
        }
        self.assertEqual(rules.validate_evidence_set(
            evidence_set, "/reports/editorAutomation", self.fixture.root,
            TXN, HEAD), [])

        mutable = self.fixture.root / "CorsairsUE/Saved/index.json"
        mutable.parent.mkdir(parents=True)
        mutable.write_bytes(b"{}")
        changed = copy.deepcopy(evidence_set)
        changed["files"] = [self.fixture.file_evidence(mutable)]
        issues = rules.validate_evidence_set(
            changed, "/reports/editorAutomation", self.fixture.root, TXN, HEAD)
        self.assertTrue(issues)
        self.assertEqual(
            issues[0]["field"], "/reports/editorAutomation/files/0/path")

        build_root = run_root / "builds/CorsairsUEEditor"
        build_root.mkdir(parents=True)
        receipt = build_root / "CorsairsUEEditor.target"
        product = build_root / "products/Binaries/Mac/CorsairsUEEditor"
        product.parent.mkdir(parents=True)
        receipt.write_bytes(b'{"TargetName":"CorsairsUEEditor"}')
        product.write_bytes(b"editor")
        build = {
            "transactionId": TXN,
            "target": "CorsairsUEEditor",
            "platform": "Mac",
            "configuration": "Development",
            "sourceHead": HEAD,
            "receipt": self.fixture.file_evidence(receipt),
            "products": [self.fixture.file_evidence(product)],
        }
        self.assertEqual(rules.validate_build_evidence(
            build, "/builds/editor", self.fixture.root, TXN, HEAD,
            "CorsairsUEEditor"), [])
        build["products"] = [self.fixture.file_evidence(mutable)]
        issues = rules.validate_build_evidence(
            build, "/builds/editor", self.fixture.root, TXN, HEAD,
            "CorsairsUEEditor")
        self.assertTrue(issues)
        self.assertEqual(issues[0]["field"], "/builds/editor/products/0/path")

    def test_editor_build_allows_editor_module_but_game_build_rejects_it(self):
        run_root = (
            self.fixture.root / f"artifacts/maps/reports/runs/{TXN}/builds")

        def build_evidence(target, product_leaf):
            build_root = run_root / target
            build_root.mkdir(parents=True)
            receipt = build_root / f"{target}.target"
            product = build_root / "products/Binaries/Mac" / product_leaf
            product.parent.mkdir(parents=True)
            receipt.write_text(json.dumps({
                "TargetName": target,
                "BuildProducts": [{
                    "Path": f"$(ProjectDir)/Binaries/Mac/{product_leaf}",
                    "Type": "DynamicLibrary",
                }],
            }, sort_keys=True), encoding="utf-8")
            product.write_bytes(b"module")
            return {
                "transactionId": TXN,
                "target": target,
                "platform": "Mac",
                "configuration": "Development",
                "sourceHead": HEAD,
                "receipt": self.fixture.file_evidence(receipt),
                "products": [self.fixture.file_evidence(product)],
            }

        editor = build_evidence(
            "CorsairsUEEditor", "libUnrealEditor-CorsairsImport.dylib")
        self.assertEqual(rules.validate_build_evidence(
            editor, "/builds/editor", self.fixture.root, TXN, HEAD,
            "CorsairsUEEditor"), [])

        game = build_evidence("CorsairsUE", "libCorsairsImport.dylib")
        issues = rules.validate_build_evidence(
            game, "/builds/game", self.fixture.root, TXN, HEAD, "CorsairsUE")
        self.assertTrue(issues)
        self.assertEqual(
            (issues[0]["code"], issues[0]["field"]),
            ("EDITOR_MODULE_LEAK", "/builds/game/receipt/path"))

        game_receipt = self.fixture.root / game["receipt"]["path"]
        game_receipt.write_text(json.dumps({
            "TargetName": "CorsairsUE",
            "BuildProducts": [{
                "Path": "$(ProjectDir)/Binaries/Mac/CorsairsUE",
                "Type": "Executable",
            }],
        }, sort_keys=True), encoding="utf-8")
        game["receipt"] = self.fixture.file_evidence(game_receipt)
        issues = rules.validate_build_evidence(
            game, "/builds/game", self.fixture.root, TXN, HEAD, "CorsairsUE")
        self.assertTrue(issues)
        self.assertEqual(
            (issues[0]["code"], issues[0]["field"]),
            ("EDITOR_MODULE_LEAK", "/builds/game/products/0/path"))

    def test_package_family_hashes_include_every_sidecar(self):
        stem = self.fixture.root / "CorsairsUE/Content/Terrain/Reference/Garner/T_Garner_17_21"
        stem.parent.mkdir(parents=True, exist_ok=True)
        primary = stem.with_suffix(".uasset")
        bulk = stem.with_suffix(".ubulk")
        primary.write_bytes(b"x")
        bulk.write_bytes(b"y")
        family = rules.package_family_evidence(
            "texture", "/Game/Terrain/Reference/Garner/T_Garner_17_21",
            stem, self.fixture.root)
        self.assertEqual(
            [Path(item["path"]).suffix for item in family["files"]],
            [".uasset", ".ubulk"])
        bulk.write_bytes(b"changed")
        self.assertNotEqual(
            rules.package_family_evidence(
                "texture", "/Game/Terrain/Reference/Garner/T_Garner_17_21",
                stem, self.fixture.root)["familySha256"],
            family["familySha256"])

    def test_texture_sampling_contract_is_complete(self):
        state = reference_state()
        self.assertEqual(rules.validate_reference_state(state), [])
        for field, value in (
            ("filter", "TF_NEAREST"),
            ("addressX", "TA_WRAP"),
            ("neverStream", True),
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(state)
                changed["texture"][field] = value
                issues = rules.validate_reference_state(changed)
                self.assertTrue(issues)
                self.assertEqual(issues[0]["field"], f"/referenceState/texture/{field}")

    def test_second_import_is_zero_mutation_with_identical_hashes(self):
        families = []
        specs = (
            ("mesh", "/Game/Terrain/Reference/Garner/SM_Garner_17_21", ".uasset"),
            ("texture", "/Game/Terrain/Reference/Garner/T_Garner_17_21", ".uasset"),
            ("material", "/Game/Terrain/Reference/M_TerrainReference", ".uasset"),
            ("instance", "/Game/Terrain/Reference/Garner/MI_Garner_17_21", ".uasset"),
            ("map", "/Game/Maps/Garner", ".umap"),
        )
        for name, package, suffix in specs:
            primary = self.fixture.root / ("families/" + name + suffix)
            primary.parent.mkdir(parents=True, exist_ok=True)
            primary.write_bytes(b"x")
            families.append(package_family(
                name, package, [primary], self.fixture.root))
        first = {
            "referenceState": reference_state(),
            "finalPackageHashes": families,
            "created": [{"objectPath": "/Game/X", "className": "/Script/Engine.Object", "reason": "CREATED"}],
            "updated": [], "deleted": [], "savedPackages": ["/Game/X"],
        }
        second = copy.deepcopy(first)
        for key in ("created", "updated", "deleted", "savedPackages"):
            second[key] = []
        self.assertEqual(rules.validate_idempotent_import_reports(first, second), [])
        second["finalPackageHashes"][0]["files"][0]["sha256"] = HASH_Y
        issues = rules.validate_idempotent_import_reports(first, second)
        self.assertTrue(issues)
        self.assertEqual(issues[0]["field"], "/second/finalPackageHashes")

    def test_sidecar_add_remove_or_byte_change_breaks_idempotence(self):
        first = {"referenceState": reference_state(), "finalPackageHashes": [],
                 "created": [], "updated": [], "deleted": [], "savedPackages": []}
        second = copy.deepcopy(first)
        base = {
            "family": "mesh", "package": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
            "files": [{"path": "a.uasset", "sha256": HASH_X, "sizeBytes": 1}],
            "familySha256": HASH_X,
        }
        first["finalPackageHashes"] = [base]
        for mutation in ("add", "remove", "change"):
            with self.subTest(mutation=mutation):
                second = copy.deepcopy(first)
                if mutation == "add":
                    second["finalPackageHashes"][0]["files"].append(
                        {"path": "a.ubulk", "sha256": HASH_X, "sizeBytes": 1})
                elif mutation == "remove":
                    second["finalPackageHashes"][0]["files"] = []
                else:
                    second["finalPackageHashes"][0]["files"][0]["sha256"] = HASH_Y
                self.assertTrue(
                    rules.validate_idempotent_import_reports(first, second))


if __name__ == "__main__":
    unittest.main()
