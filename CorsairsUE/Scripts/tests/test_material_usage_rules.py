import unittest

from CorsairsUE.Scripts.material_usage_rules import (
    required_usages,
    resolve_blend_mode,
)


class MaterialUsageRulesTests(unittest.TestCase):
    def test_opaque_nanite_hism_needs_both_usages(self):
        self.assertEqual(
            {"instanced_static_meshes", "nanite"},
            required_usages("hism", True, "opaque"))

    def test_translucent_hism_disallows_nanite(self):
        self.assertEqual(
            {"instanced_static_meshes", "disallow_nanite"},
            required_usages("hism", True, "translucent"))

    def test_additive_and_modulate_meshes_disallow_nanite(self):
        for blend_mode in ("additive", "modulate"):
            with self.subTest(blend_mode=blend_mode):
                self.assertEqual(
                    {"disallow_nanite"},
                    required_usages("mesh", True, blend_mode))

    def test_opaque_non_nanite_hism_needs_instancing_only(self):
        self.assertEqual(
            {"instanced_static_meshes"},
            required_usages("hism", False, "opaque"))

    def test_nanite_terrain_needs_nanite_usage(self):
        self.assertEqual(
            {"nanite"},
            required_usages("terrain", True, "opaque"))

    def test_effective_translucent_override_wins_over_opaque_base(self):
        self.assertEqual(
            "translucent",
            resolve_blend_mode("opaque", "translucent"))


if __name__ == "__main__":
    unittest.main()
