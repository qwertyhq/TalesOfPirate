from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "CorsairsUE" / "Scripts"))

import build_character_map  # noqa: E402


class CharacterMapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = build_character_map.build_catalog(
            str(ROOT / "databases" / "gamedata.sqlite"))

    def test_lambert_has_five_default_items_and_driver(self):
        lambert = self.catalog["characters"]["1"]
        self.assertEqual([2000, 255, 464, 640, 816],
                         lambert["defaultItemIds"])
        self.assertEqual(1, lambert["moduleIndex"])
        self.assertEqual(
            "/Game/Animations/0000/SkeletalMeshes/0000",
            lambert["driverMesh"])
        self.assertEqual(
            "/Game/Animations/0000/SkeletalMeshes/0000_Anim",
            lambert["animation"])

    def test_items_use_model_specific_module_columns(self):
        self.assertEqual(
            "/Game/All/0000000001/SkeletalMeshes/0000000001",
            self.catalog["items"]["2000"]["meshesByModule"]["1"])
        self.assertEqual(
            "/Game/All/0000610002/SkeletalMeshes/0000610002",
            self.catalog["items"]["464"]["meshesByModule"]["1"])

    def test_zero_modules_are_absent(self):
        self.assertNotIn("1",
                         self.catalog["items"]["200"]["meshesByModule"])

    def test_module_suffix_is_preserved_verbatim(self):
        self.assertEqual(
            "/Game/All/02060001_/SkeletalMeshes/02060001_",
            self.catalog["items"]["200"]["meshesByModule"]["2"])

    def test_non_player_keeps_static_mesh_fallback(self):
        warrior = self.catalog["characters"]["5"]
        self.assertEqual(4, warrior["modalType"])
        self.assertEqual(
            "/Game/All/0005000000/SkeletalMeshes/0005000000",
            warrior["staticMesh"])


if __name__ == "__main__":
    unittest.main()
