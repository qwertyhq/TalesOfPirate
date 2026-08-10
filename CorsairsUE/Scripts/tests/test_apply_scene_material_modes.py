from pathlib import Path
import unittest

from CorsairsUE.Scripts import apply_scene_material_modes as material_modes
from CorsairsUE.Scripts.apply_scene_material_modes import (
    ImportedMaterial,
    MaterialMappingError,
    SourceMaterial,
    map_imported_materials,
)


class SceneMaterialModeMappingTests(unittest.TestCase):
    def test_maps_indexed_source_name_inside_matching_model_root(self):
        source = SourceMaterial(
            model_name="by-bd001_0",
            material_name="010038.bmp5",
            mode="subtractive",
            gltf_path="models/by-bd001_0.gltf",
        )
        expected = ImportedMaterial(
            package_name=(
                "/Game/SceneParityCityMaterialV2/by-bd001_0/Materials/"
                "010038_bmp5"
            ),
            asset_name="010038_bmp5",
        )
        same_name_in_other_model = ImportedMaterial(
            package_name=(
                "/Game/SceneParityCityMaterialV2/by-bd001_2/Materials/"
                "010038_bmp5"
            ),
            asset_name="010038_bmp5",
        )

        mapped = map_imported_materials(
            [source],
            [same_name_in_other_model, expected],
            "/Game/SceneParityCityMaterialV2",
        )

        self.assertEqual(expected, mapped[source])

    def test_rejects_missing_or_ambiguous_materials(self):
        missing = SourceMaterial(
            model_name="by-bd010_9",
            material_name="010026.bmp0",
            mode="additive",
            gltf_path="models/by-bd010_9.gltf",
        )
        ambiguous = SourceMaterial(
            model_name="by-bd001_0",
            material_name="010038.bmp5",
            mode="subtractive",
            gltf_path="models/by-bd001_0.gltf",
        )
        duplicates = [
            ImportedMaterial(
                package_name=(
                    "/Game/SceneParityCityMaterialV2/by-bd001_0/Materials/"
                    "010038_bmp5"
                ),
                asset_name="010038_bmp5",
            ),
            ImportedMaterial(
                package_name=(
                    "/Game/SceneParityCityMaterialV2/by-bd001_0/Extra/"
                    "010038_bmp5"
                ),
                asset_name="010038_bmp5",
            ),
        ]

        with self.assertRaises(MaterialMappingError) as raised:
            map_imported_materials(
                [missing, ambiguous],
                duplicates,
                "/Game/SceneParityCityMaterialV2",
            )

        self.assertIn("missing", raised.exception.problems)
        self.assertIn("ambiguous", raised.exception.problems)


class LegacyUnlitMaterialTests(unittest.TestCase):
    @staticmethod
    def _source_material(name):
        return {
            "name": name,
            "alphaMode": "OPAQUE",
            "extras": {
                "corsairsLegacyMaterial": {
                    "schemaVersion": 1,
                    "mode": "opaque",
                    "opacity": 1.0,
                },
            },
        }

    def test_parser_marks_vertex_color_per_referenced_material(self):
        document = {
            "materials": [
                self._source_material("010001.bmp0"),
                self._source_material("010002.bmp0"),
            ],
            "meshes": [{
                "primitives": [
                    {
                        "material": 0,
                        "attributes": {"POSITION": 0, "COLOR_0": 1},
                    },
                    {
                        "material": 1,
                        "attributes": {"POSITION": 2},
                    },
                ],
            }],
        }

        parsed = material_modes._parse_source_materials(
            document, "/fixture/by-bd015_0.gltf")

        self.assertEqual(
            [("010001.bmp0", True), ("010002.bmp0", False)],
            [(item.material_name, item.use_vertex_color) for item in parsed],
        )

    def test_parser_rejects_mixed_color_references_for_one_material(self):
        document = {
            "materials": [self._source_material("010001.bmp0")],
            "meshes": [{
                "primitives": [
                    {
                        "material": 0,
                        "attributes": {"POSITION": 0, "COLOR_0": 1},
                    },
                    {
                        "material": 0,
                        "attributes": {"POSITION": 2},
                    },
                ],
            }],
        }

        with self.assertRaisesRegex(
                material_modes.SourceMaterialError,
                "COLOR_0.*010001.bmp0"):
            material_modes._parse_source_materials(
                document, "/fixture/by-bd015_0.gltf")

    def test_parser_rejects_material_without_primitive_reference(self):
        document = {
            "materials": [self._source_material("010001.bmp0")],
            "meshes": [{
                "primitives": [{
                    "attributes": {"POSITION": 0},
                }],
            }],
        }

        with self.assertRaisesRegex(
                material_modes.SourceMaterialError,
                "не используется.*010001.bmp0"):
            material_modes._parse_source_materials(
                document, "/fixture/by-bd015_0.gltf")

    def test_graph_plan_has_literal_ue_expression_count_for_every_mode(self):
        expected_counts = {
            "opaque": 110,
            "masked": 113,
            "alpha": 110,
            "additive": 110,
            "subtractive": 112,
        }

        for mode, expected_count in expected_counts.items():
            with self.subTest(mode=mode):
                plan = material_modes.plan_material_graph(mode)
                self.assertEqual(
                    expected_count,
                    getattr(plan, "expected_expression_count", None),
                )

    def test_expression_cleanup_snapshots_before_mutating_ue_collection(self):
        expressions = [object() for _ in range(104)]

        class FakeMaterial:
            def __init__(self):
                self.expressions = list(expressions)

        class FakeLibrary:
            @staticmethod
            def get_material_expressions(material):
                # UE returns its collection; deleting while iterating this live
                # list would skip every other expression.
                return material.expressions

            @staticmethod
            def delete_material_expression(material, expression):
                material.expressions.remove(expression)

            @staticmethod
            def get_num_material_expressions(material):
                return len(material.expressions)

        material = FakeMaterial()
        cleanup = getattr(material_modes, "_delete_material_expressions", None)
        self.assertIsNotNone(cleanup, "нет mutation-safe очистки UE graph")

        cleanup(FakeLibrary, material, "/Game/Test/M_Master")

        self.assertEqual([], material.expressions)

    def test_all_modes_modulate_vertex_color_before_lighting_and_alpha(self):
        planner = getattr(material_modes, "plan_material_graph", None)
        self.assertIsNotNone(
            planner,
            "нет pure graph-плана для VertexColor modulation",
        )

        expected_blends = {
            "opaque": "opaque",
            "masked": "masked",
            "alpha": "translucent",
            "additive": "additive",
            "subtractive": "modulate",
        }
        for mode, blend in expected_blends.items():
            with self.subTest(mode=mode):
                plan = planner(mode)
                self.assertEqual(mode, plan.mode)
                self.assertEqual(blend, plan.blend)
                self.assertEqual(
                    ("one", "vertex_color_rgb", "use_vertex_color"),
                    plan.effective_vertex_rgb_inputs,
                )
                self.assertEqual(
                    ("one", "vertex_color_alpha", "use_vertex_color"),
                    plan.effective_vertex_alpha_inputs,
                )
                self.assertEqual(
                    ("texture_rgb", "effective_vertex_rgb"),
                    plan.raw_rgb_inputs,
                )
                self.assertEqual(
                    ("texture_vertex_rgb", "legacy_lighting"),
                    plan.lit_rgb_inputs,
                )
                self.assertEqual(
                    ("texture_vertex_rgb", "lit_rgb", "legacy_lit_flag"),
                    plan.selected_rgb_inputs,
                )
                self.assertEqual(
                    (
                        "texture_alpha",
                        "effective_vertex_alpha",
                        "source_opacity",
                    ),
                    plan.output_alpha_inputs,
                )
                self.assertEqual(mode == "subtractive", plan.invert_selected_rgb)

    def test_apply_arguments_require_explicit_source_and_content_root(self):
        for argv in ([], ["/tmp/scene-corpus"]):
            with self.subTest(argv=argv):
                with self.assertRaises(RuntimeError):
                    material_modes._script_arguments(argv)

        parsed = material_modes._script_arguments([
            "/tmp/scene-corpus",
            "/Game/SceneParityCityTick120V4",
        ])

        self.assertEqual(
            Path("/tmp/scene-corpus").resolve(),
            parsed[0],
        )
        self.assertEqual("/Game/SceneParityCityTick120V4", parsed[1])
        self.assertFalse(parsed[2])

    def test_verify_arguments_require_explicit_source_and_content_root(self):
        for argv in (["--verify-only"], ["--verify-only", "/tmp/scene-corpus"]):
            with self.subTest(argv=argv):
                with self.assertRaises(RuntimeError):
                    material_modes._script_arguments(argv)

        parsed = material_modes._script_arguments([
            "--verify-only",
            "/tmp/scene-corpus",
            "/Game/SceneParityCityTick120V4",
        ])

        self.assertEqual(3, len(parsed))
        self.assertEqual(Path("/tmp/scene-corpus").resolve(), parsed[0])
        self.assertEqual("/Game/SceneParityCityTick120V4", parsed[1])
        self.assertTrue(parsed[2])

    def test_all_five_modes_use_owned_unlit_parent_and_preserve_parameters(self):
        planner = getattr(material_modes, "plan_material_application", None)
        self.assertIsNotNone(
            planner,
            "нет production-плана перепривязки всех пяти legacy modes",
        )

        applications = [
            planner("opaque", 1.0, None, False),
            planner("masked", 0.75, 130.0 / 255.0, True),
            planner("alpha", 0.5, None, False),
            planner("additive", 0.25, None, True),
            planner("subtractive", 1.0, None, False),
        ]

        self.assertEqual(
            [
                "/Game/SceneParityMaterials/M_Legacy_Opaque",
                "/Game/SceneParityMaterials/M_Legacy_Masked",
                "/Game/SceneParityMaterials/M_Legacy_Alpha",
                "/Game/SceneParityMaterials/M_Legacy_Additive",
                "/Game/SceneParityMaterials/M_Legacy_Subtractive",
            ],
            [item.parent_path for item in applications],
        )
        self.assertEqual(
            ["opaque", "masked", "translucent", "additive", "modulate"],
            [item.blend for item in applications],
        )
        self.assertTrue(all(item.unlit for item in applications))
        self.assertEqual(
            [1.0, 0.75, 0.5, 0.25, 1.0],
            [item.source_opacity for item in applications],
        )
        self.assertEqual(
            [0.0, 1.0, 0.0, 1.0, 0.0],
            [item.use_vertex_color for item in applications],
        )
        self.assertAlmostEqual(130.0 / 255.0, applications[1].alpha_cutoff)
        self.assertTrue(applications[-1].invert_lit_rgb)
        self.assertFalse(any(
            item.invert_lit_rgb for item in applications[:-1]
        ))
        with self.assertRaisesRegex(ValueError, "use vertex color"):
            planner("opaque", 1.0, None, 1)

    def test_use_vertex_color_readback_requires_exact_binary_value(self):
        validator = getattr(
            material_modes, "validate_use_vertex_color_readback", None)
        self.assertIsNotNone(validator, "нет pure MIC readback validator")

        validator(0.0, 0.0, "/Game/Test/M_WithoutColor")
        validator(1.0, 1.0, "/Game/Test/M_WithColor")
        for expected, actual in ((0.0, 1.0), (1.0, 0.0), (1.0, 0.5)):
            with self.subTest(expected=expected, actual=actual):
                with self.assertRaisesRegex(
                        RuntimeError, "UseVertexColor"):
                    validator(expected, actual, "/Game/Test/M_Bad")

    def test_33_float_payload_selects_raw_unlit_or_legacy_lighting(self):
        evaluator = getattr(material_modes, "evaluate_legacy_lighting", None)
        self.assertIsNotNone(
            evaluator,
            "нет эталонной реализации 33-float lighting equation",
        )

        texture = (0.8, 0.5, 0.25)
        unlit = [0.0] * 33
        unlit[4:7] = [0.9, 0.9, 0.9]
        self.assertEqual(
            texture,
            evaluator(
                texture,
                unlit,
                normal=(0.0, 0.0, 1.0),
                actor_position=(0.0, 0.0, 0.0),
                world_position=(0.0, 0.0, 0.0),
            ),
        )

        lit = [0.0] * 33
        lit[0:4] = [1.0, 1.0, 1.0, 0.0]
        lit[4:7] = [0.1, 0.2, 0.3]
        lit[7:10] = [0.0, 0.0, -1.0]
        lit[10:13] = [0.2, 0.1, 0.0]
        lit[13:16] = [0.0, 0.0, 10.0]
        lit[16:19] = [0.3, 0.2, 0.1]
        lit[19:23] = [20.0, 1.0, 0.0, 0.0]
        lit[23:26] = [0.0, 0.0, -10.0]
        lit[26:29] = [1.0, 1.0, 1.0]
        lit[29:33] = [20.0, 1.0, 0.0, 0.0]

        result = evaluator(
            texture,
            lit,
            normal=(0.0, 0.0, 1.0),
            actor_position=(0.0, 0.0, 0.0),
            world_position=(0.0, 0.0, 0.0),
        )

        for expected, actual in zip((0.48, 0.25, 0.1), result):
            self.assertAlmostEqual(expected, actual)

        with self.assertRaises(ValueError):
            evaluator(
                texture,
                [0.0] * 32,
                normal=(0.0, 0.0, 1.0),
                actor_position=(0.0, 0.0, 0.0),
                world_position=(0.0, 0.0, 0.0),
            )


if __name__ == "__main__":
    unittest.main()
