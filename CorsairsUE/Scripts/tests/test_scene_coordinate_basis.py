import ast
import math
from pathlib import Path
import unittest

from CorsairsUE.Scripts.scene_coordinate_basis import (
    rotate_source_direction,
    source_camera_to_ue,
    source_location_to_ue,
    source_scene_yaw_to_ue,
    standalone_character_yaw_to_ue,
)


ROOT = Path(__file__).resolve().parents[3]
SCRIPT_DIR = ROOT / "CorsairsUE" / "Scripts"


def script_source(name):
    return (SCRIPT_DIR / name).read_text(encoding="utf-8")


def literal_assignment(source, name):
    tree = ast.parse(source)
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if any(isinstance(target, ast.Name) and target.id == name
               for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(f"assignment {name} not found")


def called_functions(source):
    names = set()
    for node in ast.walk(ast.parse(source)):
        if not isinstance(node, ast.Call):
            continue
        if isinstance(node.func, ast.Name):
            names.add(node.func.id)
        elif isinstance(node.func, ast.Attribute):
            names.add(node.func.attr)
    return names


class SceneCoordinateBasisTests(unittest.TestCase):
    def test_rigid_map_basis_preserves_reference_camera_handedness(self):
        self.assertEqual(
            (-278475.0, 223325.0, 100.0),
            source_location_to_ue(223325.0, 278475.0, 100.0),
        )
        self.assertEqual(
            (-281975.0, 223325.0, 5100.0),
            source_location_to_ue(223325.0, 281975.0, 5100.0),
        )
        self.assertEqual(
            {
                "eye": (-281975.0, 223325.0, 5100.0),
                "target": (-278475.0, 223325.0, 100.0),
                "pitch": -55.00798,
                "yaw": 0.0,
                "right": (0.0, 1.0, 0.0),
            },
            source_camera_to_ue(
                eye=(223325.0, 281975.0, 5100.0),
                target=(223325.0, 278475.0, 100.0),
            ),
        )

    def test_scene_yaw_uses_positive_scale_rigid_basis(self):
        literals = {
            0.0: -90.0,
            90.0: 0.0,
            -180.0: 90.0,
            270.0: 180.0,
        }
        for source_yaw, expected in literals.items():
            with self.subTest(source_yaw=source_yaw):
                self.assertEqual(expected, source_scene_yaw_to_ue(source_yaw))

    def test_standalone_npc_native_forward_matches_rigid_map_direction(self):
        # Generic character meshes face local +Y.  SkeletalMeshActor therefore
        # folds the same -90 degree mesh offset used by ACorsairsCharacter into
        # its actor yaw.
        for source_direction in (0.0, 90.0, 180.0, 270.0):
            with self.subTest(source_direction=source_direction):
                actor_yaw = standalone_character_yaw_to_ue(source_direction)
                radians = math.radians(actor_yaw)
                rotated_native_forward = (-math.sin(radians), math.cos(radians))
                expected = rotate_source_direction(
                    source_direction,
                )
                self.assertAlmostEqual(
                    expected[0], rotated_native_forward[0], places=12
                )
                self.assertAlmostEqual(
                    expected[1], rotated_native_forward[1], places=12
                )


class SceneCoordinateConsumerContractTests(unittest.TestCase):
    def test_city_consumes_rigid_basis_and_v5_terrain_contract(self):
        source = script_source("place_scene_progress_city.py")
        calls = called_functions(source)

        self.assertIn("source_location_to_ue", calls)
        self.assertIn("source_scene_yaw_to_ue", calls)
        self.assertIn("source_camera_to_ue", calls)
        self.assertNotIn("180.0 - float(record[\"sourceYawDegrees\"])", source)
        self.assertEqual(
            90.0,
            literal_assignment(source, "SOURCE_CHARACTER_DIRECTION"),
        )
        # Манекен кадра — обычный SkeletalMeshActor, а не игровой персонаж с
        # его -90 на компоненте меша: тот же сдвиг сложен в поворот актора
        # ровно так же, как у NPC.
        self.assertIn(
            "yaw=standalone_character_yaw_to_ue(SOURCE_CHARACTER_DIRECTION)",
            source)
        self.assertNotIn("set_body_mesh", source)
        self.assertEqual(
            "/Game/Terrain/Reference/GarnerRigid/SM_Garner_17_21",
            literal_assignment(source, "REFERENCE_TERRAIN_MESH"),
        )
        self.assertEqual(
            (-268800.0, 217600.0, 0.0),
            literal_assignment(source, "REFERENCE_TERRAIN_LOCATION"),
        )
        self.assertEqual(
            (-281600.0, 217600.0, -268800.0, 230400.0),
            literal_assignment(source, "REFERENCE_TERRAIN_BOUNDS"),
        )
        self.assertNotIn("if not removed:", source)
        self.assertIn("remaining_overlaps", source)

    def test_terrain_verifier_expects_the_same_page_as_the_placement(self):
        """Проверка земли обязана ждать ту же страницу, что и расстановка.

        Числа в проверке пережили переход с зеркала на поворот незамеченными:
        она подтверждала зеркальную карту, пока расстановка уже ставила
        повёрнутую. Поэтому оба конца сверяются здесь, а сами ожидания
        выводятся поворотом, а не вписываются числами.
        """
        verifier = script_source("verify_scene_progress_terrain.py")
        placement = script_source("place_scene_progress_city.py")

        self.assertIn("source_location_to_ue", called_functions(verifier))
        self.assertEqual(
            "/Game/Terrain/Reference/GarnerRigid/SM_Garner_17_21",
            literal_assignment(verifier, "REFERENCE_MESH"),
        )
        self.assertEqual(
            literal_assignment(placement, "REFERENCE_TERRAIN_MESH"),
            literal_assignment(verifier, "REFERENCE_MESH"),
        )

        origin = literal_assignment(verifier, "SOURCE_PAGE_ORIGIN")
        size = literal_assignment(verifier, "SOURCE_PAGE_SIZE_CM")
        near = source_location_to_ue(origin[0], origin[1], 0.0)
        far = source_location_to_ue(origin[0] + size, origin[1] + size, 0.0)
        self.assertEqual(
            literal_assignment(placement, "REFERENCE_TERRAIN_LOCATION"),
            near,
        )
        self.assertEqual(
            literal_assignment(placement, "REFERENCE_TERRAIN_BOUNDS"),
            (min(near[0], far[0]), min(near[1], far[1]),
             max(near[0], far[0]), max(near[1], far[1])),
        )

    def test_camera_consumers_derive_the_same_q_transform(self):
        for name in (
            "capture_scene_progress_city.py",
            "patch_scene_progress_camera.py",
            "verify_scene_progress_camera.py",
        ):
            with self.subTest(script=name):
                source = script_source(name)
                self.assertIn("source_camera_to_ue", called_functions(source))
                self.assertNotIn("223325.0, -281975.0", source)
                self.assertNotIn("yaw=90.0", source)

    def test_npc_consumer_rotates_location_but_keeps_native_y_yaw_offset(self):
        source = script_source("place_scene_progress_npcs.py")
        calls = called_functions(source)

        self.assertIn("source_location_to_ue", calls)
        self.assertIn("standalone_character_yaw_to_ue", calls)
        self.assertNotIn(
            '"locationCm": [float(source_x), -float(source_y), ground_z]',
            source,
        )


if __name__ == "__main__":
    unittest.main()
