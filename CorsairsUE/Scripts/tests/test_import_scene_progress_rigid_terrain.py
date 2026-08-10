import ast
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "CorsairsUE" / "Scripts" / "import_scene_progress_rigid_terrain.py"


class FakeStaticMesh:
    pass


class FakeMaterialInstanceConstant:
    pass


def load_script():
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"), filename=str(SCRIPT))
    definitions = [
        node for node in tree.body
        if isinstance(
            node,
            (ast.Import, ast.ImportFrom, ast.Assign, ast.AnnAssign,
             ast.FunctionDef, ast.ClassDef),
        )
        and not (
            isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name) and target.id == "report"
                for target in node.targets
            )
        )
    ]
    unreal = types.ModuleType("unreal")
    unreal.StaticMesh = FakeStaticMesh
    unreal.MaterialInstanceConstant = FakeMaterialInstanceConstant
    report = types.ModuleType("report")
    report.Reporter = object
    report.RefuseIfEditorOpen = lambda _report: False
    namespace = {"__file__": str(SCRIPT), "__name__": "rigid_terrain_test"}
    with mock.patch.dict(sys.modules, {"unreal": unreal, "report": report}):
        exec(compile(ast.Module(definitions, []), str(SCRIPT), "exec"), namespace)
    return types.SimpleNamespace(**namespace)


class ContractMesh(FakeStaticMesh):
    def __init__(self, path, material, vertices=16641, bounds=None):
        self.path = path
        self.material = material
        self.vertices = vertices
        minimum, maximum = bounds or (
            (-12800.0, 0.0, -270.0),
            (0.0, 12800.0, 300.0),
        )
        self.bounds = types.SimpleNamespace(
            is_valid=True,
            min=types.SimpleNamespace(
                x=minimum[0], y=minimum[1], z=minimum[2]),
            max=types.SimpleNamespace(
                x=maximum[0], y=maximum[1], z=maximum[2]),
        )
        self.nanite = types.SimpleNamespace(enabled=True)
        self.metadata = {}
        self.reject_vertex_query = False

    def get_path_name(self):
        return self.path

    def get_material(self, index):
        return self.material if index == 0 else None

    def set_material(self, index, material):
        if index != 0:
            raise AssertionError(index)
        self.material = material

    def get_editor_property(self, name):
        if name == "nanite_settings":
            return self.nanite
        raise AttributeError(name)

    def set_editor_property(self, name, value):
        if name != "nanite_settings":
            raise AttributeError(name)
        self.nanite = value

    def get_num_lods(self):
        return 1

    def get_num_vertices(self, lod):
        if self.reject_vertex_query:
            raise AssertionError("render vertex count is not a source contract")
        if lod != 0:
            raise AssertionError(lod)
        return self.vertices

    def get_bounding_box(self):
        return self.bounds


class MetadataLibrary:
    @staticmethod
    def get_metadata_tag(asset, key):
        return asset.metadata.get(key, "")

    @staticmethod
    def set_metadata_tag(asset, key, value):
        asset.metadata[key] = value


class FakeReport:
    def __init__(self):
        self.lines = []

    def line(self, message):
        self.lines.append(message)


class RigidTerrainImportTests(unittest.TestCase):
    def setUp(self):
        self.script = load_script()

    def test_script_bootstraps_its_sibling_module_path(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("sys.path.insert(0, _SCRIPT_DIR)", source)
        self.assertLess(
            source.index("sys.path.insert(0, _SCRIPT_DIR)"),
            source.index("from report import Reporter"),
        )

    def write_pair(self, directory, *, profile="RigidQ", bounds=None):
        root = Path(directory)
        binary = root / "garner.terrain_17_21.bin"
        binary.write_bytes(b"\x00\x01\x02\x03")
        position_min, position_max = bounds or (
            [-128.0, -2.7, 0.0],
            [0.0, 3.0, 128.0],
        )
        gltf = root / "garner.terrain_17_21.gltf"
        gltf.write_text(
            json.dumps(
                {
                    "asset": {
                        "version": "2.0",
                        "extras": {
                            "corsairsTerrainCoordinateProfile": profile,
                        },
                    },
                    "buffers": [
                        {
                            "uri": binary.name,
                            "byteLength": binary.stat().st_size,
                        }
                    ],
                    "accessors": [
                        {
                            "componentType": 5126,
                            "count": 16641,
                            "type": "VEC3",
                            "min": position_min,
                            "max": position_max,
                        },
                        {"componentType": 5126, "count": 16641, "type": "VEC3"},
                        {"componentType": 5126, "count": 16641, "type": "VEC2"},
                        {"componentType": 5125, "count": 98304, "type": "SCALAR"},
                    ],
                    "meshes": [
                        {
                            "primitives": [
                                {
                                    "attributes": {
                                        "POSITION": 0,
                                        "NORMAL": 1,
                                        "TEXCOORD_0": 2,
                                    },
                                    "indices": 3,
                                    "mode": 4,
                                }
                            ]
                        }
                    ],
                    "nodes": [{"mesh": 0, "name": "mesh"}],
                    "scenes": [{"nodes": [0]}],
                    "scene": 0,
                }
            ),
            encoding="utf-8",
        )
        return gltf, binary

    def contract(self):
        return {
            "gltfSha256": "a" * 64,
            "binSha256": "b" * 64,
            "coordinateProfile": "RigidQ",
            "vertexCount": 16641,
            "ueBoundsMinCm": (-12800.0, 0.0, -270.0),
            "ueBoundsMaxCm": (0.0, 12800.0, 300.0),
        }

    def install_metadata_library(self):
        self.script.unreal.EditorAssetLibrary = MetadataLibrary

    def test_source_contract_rejects_non_rigid_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            good, _binary = self.write_pair(directory)
            contract = self.script.validate_source_contract(good)
            self.assertEqual("RigidQ", contract.get("coordinateProfile"))
            self.assertEqual(16641, contract.get("vertexCount"))
            self.assertEqual(
                (-12800.0, 0.0, -270.0), contract.get("ueBoundsMinCm"))
            self.assertEqual(
                (0.0, 12800.0, 300.0), contract.get("ueBoundsMaxCm"))

            legacy, _binary = self.write_pair(directory, profile="Task8Legacy")
            with self.assertRaisesRegex(RuntimeError, "coordinate profile"):
                self.script.validate_source_contract(legacy)

    def test_source_contract_rejects_reflected_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            reflected, _binary = self.write_pair(
                directory,
                bounds=([0.0, -2.7, -128.0], [128.0, 3.0, 0.0]),
            )
            with self.assertRaisesRegex(RuntimeError, "POSITION bounds"):
                self.script.validate_source_contract(reflected)

            wrong_height, _binary = self.write_pair(
                directory,
                bounds=([-128.0, -2.6, 0.0], [0.0, 3.0, 128.0]),
            )
            with self.assertRaisesRegex(RuntimeError, "POSITION bounds"):
                self.script.validate_source_contract(wrong_height)

    def test_source_contract_rejects_noncanonical_accessors(self):
        with tempfile.TemporaryDirectory() as directory:
            gltf, _binary = self.write_pair(directory)
            document = json.loads(gltf.read_text(encoding="utf-8"))
            del document["meshes"][0]["primitives"][0]["attributes"]["NORMAL"]
            gltf.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "primitive"):
                self.script.validate_source_contract(gltf)

    def test_import_selection_refuses_paths_outside_attempt_root(self):
        inside = self.script.ATTEMPT_ROOT + "/mesh/StaticMeshes/SM.SM"
        outside = "/Game/Foreign/SM_Foreign.SM_Foreign"
        mesh = FakeStaticMesh()
        assets = {inside: mesh, outside: mesh}

        path, selected = self.script.select_exact_static_mesh(
            [inside], assets.get)
        self.assertEqual(inside, path)
        self.assertIs(mesh, selected)

        with self.assertRaisesRegex(RuntimeError, "outside owned attempt"):
            self.script.select_exact_static_mesh([outside], assets.get)

    def test_import_waits_for_task_completion(self):
        imported_path = self.script.ATTEMPT_ROOT + "/mesh/StaticMeshes/SM.SM"
        events = []

        class Task:
            def __init__(self):
                self.properties = {}
                self.completed = False

            def set_editor_property(self, name, value):
                self.properties[name] = value

            def get_editor_property(self, name):
                if name == "imported_object_paths":
                    return [imported_path] if self.completed else []
                return self.properties[name]

            def get_objects(self):
                self.completed = True
                events.append("interchange")
                return []

        class AssetTools:
            @staticmethod
            def import_asset_tasks(tasks):
                self.assertEqual(1, len(tasks))

        self.script.unreal.AssetImportTask = Task
        self.script.unreal.AssetToolsHelpers = types.SimpleNamespace(
            get_asset_tools=lambda: AssetTools())
        self.script.unreal.AutomationUtilsBlueprintLibrary = types.SimpleNamespace(
            finish_all_asset_compilation=lambda: events.append("compilation"))
        result = self.script.import_candidate(
            {"gltfPath": Path("/tmp/garner.terrain_17_21.gltf")})
        self.assertEqual([imported_path], result)
        self.assertEqual(["interchange", "compilation"], events)

    def test_readback_requires_exact_geometry_and_profile_metadata(self):
        self.install_metadata_library()
        instance = FakeMaterialInstanceConstant()
        mesh = ContractMesh(
            self.script.TARGET_MESH + ".SM_Garner_17_21",
            instance,
            vertices=161,
        )
        mesh.metadata = {
            self.script.SOURCE_GLTF_METADATA: "a" * 64,
            self.script.SOURCE_BIN_METADATA: "b" * 64,
            "Corsairs.TerrainCoordinateProfile": "RigidQ",
        }
        self.script.validate_mesh(mesh, instance, self.contract())

        mesh.bounds.max.x = 1.0
        with self.assertRaisesRegex(RuntimeError, "geometry"):
            self.script.validate_mesh(mesh, instance, self.contract())
        mesh.bounds.max.x = 0.0
        del mesh.metadata["Corsairs.TerrainCoordinateProfile"]
        with self.assertRaisesRegex(RuntimeError, "coordinateProfile"):
            self.script.validate_mesh(mesh, instance, self.contract())

    def test_readback_uses_rigid_bounds_not_legacy_render_vertex_count(self):
        self.install_metadata_library()
        instance = FakeMaterialInstanceConstant()
        mesh = ContractMesh(
            self.script.TARGET_MESH + ".SM_Garner_17_21",
            instance,
            vertices=42,
        )
        mesh.reject_vertex_query = True
        mesh.metadata = {
            self.script.SOURCE_GLTF_METADATA: "a" * 64,
            self.script.SOURCE_BIN_METADATA: "b" * 64,
            self.script.SOURCE_PROFILE_METADATA: "RigidQ",
        }
        try:
            self.script.validate_mesh(mesh, instance, self.contract())
        except (AssertionError, RuntimeError) as error:
            self.fail(f"valid rigid bounds were rejected: {error}")

        mesh.bounds.max.y = 12800.1
        self.script.validate_mesh(mesh, instance, self.contract())
        mesh.bounds.max.y = 12800.1001
        with self.assertRaisesRegex(RuntimeError, "geometry"):
            self.script.validate_mesh(mesh, instance, self.contract())

        mesh.bounds.max.y = 12800.0
        mesh.bounds.is_valid = False
        with self.assertRaisesRegex(RuntimeError, "geometry"):
            self.script.validate_mesh(mesh, instance, self.contract())

        mesh.bounds.is_valid = True
        mesh.bounds.min.z = float("nan")
        with self.assertRaisesRegex(RuntimeError, "geometry"):
            self.script.validate_mesh(mesh, instance, self.contract())

    def test_failed_first_publish_removes_target_and_attempt_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            source, _binary = self.write_pair(directory)
            instance = FakeMaterialInstanceConstant()
            candidate_path = (
                self.script.ATTEMPT_ROOT
                + "/mesh/StaticMeshes/SM_Garner_17_21.SM_Garner_17_21"
            )
            sidecar_path = self.script.ATTEMPT_ROOT + "/Materials/M_Default.M_Default"
            candidate = ContractMesh(candidate_path, None, vertices=161)
            assets = {
                self.script.TERRAIN_INSTANCE: instance,
            }

            class AssetLibrary(MetadataLibrary):
                save_calls = 0

                @classmethod
                def does_directory_exist(cls, path):
                    return any(item.startswith(path + "/") for item in assets)

                @classmethod
                def delete_directory(cls, path):
                    doomed = [item for item in assets if item.startswith(path + "/")]
                    for item in doomed:
                        del assets[item]
                    return True

                @classmethod
                def save_loaded_asset(cls, _asset, only_if_is_dirty=False):
                    self.assertFalse(only_if_is_dirty)
                    cls.save_calls += 1
                    return cls.save_calls == 1

                @classmethod
                def rename_loaded_asset(cls, asset, destination):
                    source_path = next(
                        path for path, value in assets.items() if value is asset)
                    del assets[source_path]
                    asset.path = destination + ".SM_Garner_17_21"
                    assets[destination] = asset
                    return True

                @classmethod
                def delete_asset(cls, path):
                    return assets.pop(path, None) is not None

            def import_candidate(_contract):
                assets[candidate_path] = candidate
                assets[sidecar_path] = object()
                return [candidate_path, sidecar_path]

            self.script.unreal.EditorAssetLibrary = AssetLibrary
            self.script.unreal.load_asset = assets.get
            report = FakeReport()
            globals_patch = {
                "import_candidate": import_candidate,
                "finish_asset_compilation": lambda: None,
                "RefuseIfEditorOpen": lambda _report: False,
            }
            with mock.patch.dict(self.script.main.__globals__, globals_patch):
                with self.assertRaisesRegex(RuntimeError, "не сохранён"):
                    self.script.main(
                        report, types.SimpleNamespace(source_gltf=str(source)))

            self.assertNotIn(self.script.TARGET_MESH, assets)
            self.assertFalse(any(
                path.startswith(self.script.ATTEMPT_ROOT + "/")
                for path in assets
            ))


if __name__ == "__main__":
    unittest.main()
