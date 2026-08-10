import ast
import json
import math
from pathlib import Path
import sqlite3
import sys
import tempfile
import types
import unittest
from unittest import mock

from CorsairsUE.Scripts import scene_coordinate_basis


ROOT = Path(__file__).resolve().parents[3]
IMPORT_SCRIPT = ROOT / "CorsairsUE" / "Scripts" / "import_assets.py"
CITY_SCRIPT = ROOT / "CorsairsUE" / "Scripts" / "place_scene_progress_city.py"
LIGHTING_VERIFY_SCRIPT = (
    ROOT / "CorsairsUE" / "Scripts" / "verify_scene_progress_lighting.py"
)


def load_script(path, fake_unreal):
    """Загружает определения скрипта без его нижнего UE entry point."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    definitions = []
    for node in tree.body:
        if not isinstance(
            node,
            (ast.Import, ast.ImportFrom, ast.Assign, ast.AnnAssign,
             ast.FunctionDef, ast.ClassDef),
        ):
            continue
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "report"
            for target in node.targets
        ):
            continue
        definitions.append(node)

    report = types.ModuleType("report")
    report.Reporter = object
    report.RefuseIfEditorOpen = lambda _report: False
    scene_lighting = types.ModuleType("scene_lighting")
    scene_lighting.resolve_reference_lighting = lambda *_args: []
    namespace = {"__file__": str(path), "__name__": "animation_contract_test"}
    with mock.patch.dict(
        sys.modules,
        {
            "unreal": fake_unreal,
            "report": report,
            "scene_coordinate_basis": scene_coordinate_basis,
            "scene_lighting": scene_lighting,
        },
    ):
        exec(compile(ast.Module(definitions, []), str(path), "exec"), namespace)
    return types.SimpleNamespace(**namespace)


class FakeStaticMesh:
    pass


class FakeSkeletalMesh:
    def __init__(self, skeleton=None):
        self.skeleton = skeleton

    def get_editor_property(self, name):
        if name == "skeleton":
            return self.skeleton
        raise AttributeError(name)


class FakeSkeleton:
    pass


class FakePhysicsAsset:
    pass


class FakeFrameRate:
    def __init__(self, numerator, denominator):
        self.numerator = numerator
        self.denominator = denominator


class FakeDataModel:
    def __init__(self, keys, frames, frame_rate):
        self.keys = keys
        self.frames = frames
        self.frame_rate = frame_rate

    def get_number_of_keys(self):
        return self.keys

    def get_number_of_frames(self):
        return self.frames

    def get_frame_rate(self):
        return self.frame_rate


class FakeAnimSequence:
    def __init__(self, skeleton, keys, play_length):
        self.skeleton = skeleton
        self.data_model = FakeDataModel(
            keys, keys - 1, FakeFrameRate(30, 1)
        )
        self.play_length = play_length

    def get_editor_property(self, name):
        if name == "skeleton":
            return self.skeleton
        if name == "data_model_interface":
            return self.data_model
        raise AttributeError(name)

    def get_play_length(self):
        return self.play_length


def fake_unreal_module(assets=None):
    module = types.ModuleType("unreal")
    module.StaticMesh = FakeStaticMesh
    module.SkeletalMesh = FakeSkeletalMesh
    module.Skeleton = FakeSkeleton
    module.PhysicsAsset = FakePhysicsAsset
    module.AnimSequence = FakeAnimSequence
    module.load_asset = lambda path: (assets or {}).get(path)
    return module


class PreserveAnimatedImportTests(unittest.TestCase):
    def setUp(self):
        self.unreal = fake_unreal_module()
        self.script = load_script(IMPORT_SCRIPT, self.unreal)

    def write_gltf(self, directory, sample_frame=25):
        path = Path(directory) / "by-bd002_8.gltf"
        path.write_text(
            json.dumps(
                {
                    "nodes": [
                        {
                            "name": "part_root",
                            "extras": {
                                "corsairsLegacyCapture": {
                                    "schemaVersion": 1,
                                    "captureTick": 120,
                                    "bone": {
                                        "policy": "preserveAnimated",
                                        "frameCount": 47,
                                        "sampleFrame": sample_frame,
                                    },
                                }
                            },
                        }
                    ],
                    "skins": [{"joints": [1]}],
                    "animations": [{"name": "legacy_bone"}],
                }
            ),
            encoding="utf-8",
        )
        return path

    def test_metadata_requires_owned_tick119_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            good = self.write_gltf(directory)
            self.assertEqual(
                {
                    "captureTick": 120,
                    "frameCount": 47,
                    "sampleFrame": 25,
                },
                self.script.read_preserve_animated_metadata(str(good)),
            )
            bad = self.write_gltf(directory, sample_frame=26)
            with self.assertRaisesRegex(RuntimeError, "sampleFrame"):
                self.script.read_preserve_animated_metadata(str(bad))

    def test_cli_exposes_exact_preserve_animated_count_gate(self):
        args = self.script.parse_args(
            ["input", "/Game/SceneParityCityTick120V4",
             "--expected-preserve-animated", "10"]
        )
        self.assertEqual(10, args.expected_preserve_animated)
        with self.assertRaisesRegex(RuntimeError, "PreserveAnimated"):
            self.script.require_preserve_animated_count(9, 10)

    def test_import_gate_resolves_exact_interchange_assets_and_key_count(self):
        destination = "/Game/SceneParityCityTick120V4"
        source = "/tmp/by-bd002_8.gltf"
        paths = self.script.expected_interchange_paths(source, destination)
        self.assertEqual(
            {
                "animation": (
                    "/Game/SceneParityCityTick120V4/by-bd002_8/"
                    "SkeletalMeshes/by-bd002_8_Anim"
                ),
                "physics_asset": (
                    "/Game/SceneParityCityTick120V4/by-bd002_8/"
                    "SkeletalMeshes/by-bd002_8_PhysicsAsset"
                ),
                "skeletal_mesh": (
                    "/Game/SceneParityCityTick120V4/by-bd002_8/"
                    "SkeletalMeshes/by-bd002_8"
                ),
                "skeleton": (
                    "/Game/SceneParityCityTick120V4/by-bd002_8/"
                    "SkeletalMeshes/by-bd002_8_Skeleton"
                ),
                "static_mesh": (
                    "/Game/SceneParityCityTick120V4/by-bd002_8/"
                    "StaticMeshes/by-bd002_8"
                ),
            },
            paths,
        )

        skeleton = FakeSkeleton()
        assets = {
            paths["skeletal_mesh"]: FakeSkeletalMesh(skeleton),
            paths["skeleton"]: skeleton,
            paths["physics_asset"]: FakePhysicsAsset(),
            paths["animation"]: FakeAnimSequence(
                skeleton, 47, 46.0 / 30.0
            ),
        }
        self.script.unreal.load_asset = assets.get
        validated = self.script.validate_preserve_animated_import(
            source,
            destination,
            {"captureTick": 120, "frameCount": 47, "sampleFrame": 25},
        )
        self.assertEqual(paths, validated)
        del assets[paths["animation"]]
        with self.assertRaisesRegex(RuntimeError, "AnimSequence"):
            self.script.validate_preserve_animated_import(
                source,
                destination,
                {"captureTick": 120, "frameCount": 47, "sampleFrame": 25},
            )


class CityAnimationPlacementTests(unittest.TestCase):
    CASES = (
        (2, "by-bd002_8", 47, 25, 1),
        (325, "nml-bd025", 101, 18, 5),
        (475, "by-bd032_1", 101, 18, 1),
        (476, "by-bd033_1", 97, 22, 2),
        (478, "by-bd035_1", 97, 22, 1),
        (513, "nml-bd199_0", 201, 119, 5),
        (513, "nml-bd199_1", 201, 119, 5),
        (513, "nml-bd199_2", 201, 119, 5),
        (513, "nml-bd199_3", 201, 119, 5),
        (513, "nml-bd199_4", 201, 119, 5),
    )

    def setUp(self):
        self.unreal = fake_unreal_module()
        self.script = load_script(CITY_SCRIPT, self.unreal)

    def build_census(self):
        resolved = {}
        references = []
        for model_id, stem, frame_count, sample_frame, placements in self.CASES:
            descriptor = {
                "animation": object(),
                "animation_path": (
                    f"/Game/SceneParityCityTick120V4/{stem}/"
                    f"SkeletalMeshes/{stem}_Anim"
                ),
                "frame_count": frame_count,
                "kind": "skeletal",
                "path": (
                    f"/Game/SceneParityCityTick120V4/{stem}/"
                    f"SkeletalMeshes/{stem}"
                ),
                "sample_frame": sample_frame,
                "stem": stem,
            }
            resolved.setdefault(model_id, []).append(descriptor)
            if not any(record["modelId"] == model_id for record in references):
                references.extend({"modelId": model_id} for _ in range(placements))
        return references, resolved

    def test_exact_animated_census_is_10_unique_parts_and_35_placements(self):
        references, resolved = self.build_census()
        self.assertEqual(
            {"placements": 35, "uniqueParts": 10},
            self.script.validate_animated_city_census(references, resolved),
        )
        resolved[2][0]["sample_frame"] = 26
        with self.assertRaisesRegex(RuntimeError, "sampleFrame"):
            self.script.validate_animated_city_census(references, resolved)

    def test_scene_mesh_resolution_prefers_skeletal_sibling(self):
        static_path = "/Game/V4/example/StaticMeshes/example"
        skeletal_path = "/Game/V4/example/SkeletalMeshes/example"
        static = FakeStaticMesh()
        skeletal = FakeSkeletalMesh()
        self.script.unreal.load_asset = {
            static_path: static,
            skeletal_path: skeletal,
        }.get

        self.assertEqual(
            (skeletal, "skeletal", skeletal_path),
            self.script.load_scene_mesh_asset(static_path, skeletal_path),
        )

    def test_skeletal_pose_uses_serialized_tick119_time_and_pauses(self):
        animation = object()

        class AnimationData:
            def __init__(self):
                self.values = {}

            def get_editor_property(self, name):
                return self.values[name]

        class Component:
            def __init__(self):
                self.animation_data = AnimationData()
                self.modified = False
                self.override_args = None
                self.pause_anims = False

            def modify(self):
                self.modified = True

            def override_animation_data(
                self, anim, looping, playing, position, play_rate
            ):
                self.override_args = (
                    anim, looping, playing, position, play_rate
                )
                self.animation_data.values = {
                    "anim_to_play": anim,
                    "saved_looping": looping,
                    "saved_playing": playing,
                    "saved_position": position,
                    "saved_play_rate": play_rate,
                }

            def set_editor_property(self, name, value):
                setattr(self, name, value)

            def get_editor_property(self, name):
                return getattr(self, name)

            def is_playing(self):
                return self.animation_data.values["saved_playing"]

            def get_position(self):
                return self.animation_data.values["saved_position"]

        component = Component()
        descriptor = {
            "animation": animation,
            "frame_count": 47,
            "sample_frame": 25,
        }
        self.script.freeze_skeletal_capture_pose(component, descriptor)
        self.assertTrue(component.modified)
        self.assertEqual((animation, False, False), component.override_args[:3])
        self.assertTrue(math.isclose(25.0 / 30.0, component.override_args[3]))
        self.assertEqual(1.0, component.override_args[4])
        self.assertTrue(component.pause_anims)

    def test_city_scope_is_exact_before_owned_map_recreation(self):
        validator = getattr(self.script, "validate_city_scope_counts", None)
        self.assertIsNotNone(
            validator,
            "production должен иметь fail-closed exact scope gate",
        )
        references = [{"modelId": 1}] * 1370
        helpers = [{"modelId": 351}] * 264
        validator(references, helpers, 1617)

        with self.assertRaisesRegex(RuntimeError, "visible anchors"):
            validator(references[:-1], helpers, 1617)
        with self.assertRaisesRegex(RuntimeError, "part placements"):
            validator(references, helpers, 1616)


class LightingVerifierSafetyTests(unittest.TestCase):
    def setUp(self):
        self.script = load_script(
            LIGHTING_VERIFY_SCRIPT,
            fake_unreal_module(),
        )

    def test_unknown_manifest_model_id_is_not_silently_filtered(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "gamedata.sqlite"
            connection = sqlite3.connect(database)
            try:
                connection.execute(
                    'CREATE TABLE scene_objects (id INTEGER PRIMARY KEY, "type" INTEGER)'
                )
                connection.execute(
                    'INSERT INTO scene_objects (id, "type") VALUES (22, 0)'
                )
                connection.commit()
            finally:
                connection.close()

            manifest = {
                "records": [
                    {"modelId": 22, "inReferenceSet": True},
                    {"modelId": 999999, "inReferenceSet": True},
                ]
            }
            with self.assertRaisesRegex(RuntimeError, "missing model IDs"):
                self.script.visual_manifest(manifest, database)

    def test_cold_verifier_requires_exact_scope_and_successful_level_load(self):
        scope_validator = getattr(
            self.script, "validate_visual_manifest_scope", None
        )
        self.assertIsNotNone(
            scope_validator,
            "cold verifier должен иметь exact 1370-anchor gate",
        )
        scope_validator({"records": [{}] * 1370})
        with self.assertRaisesRegex(RuntimeError, "anchors"):
            scope_validator({"records": [{}] * 1369})

        level_loader = getattr(self.script, "load_verified_level", None)
        self.assertIsNotNone(
            level_loader,
            "cold verifier должен проверять результат load_level",
        )

        class Levels:
            @staticmethod
            def load_level(_path):
                return False

        with self.assertRaisesRegex(RuntimeError, "не загружена карта"):
            level_loader(Levels(), "/Game/Maps/GarnerSceneProgressCity")


if __name__ == "__main__":
    unittest.main()
