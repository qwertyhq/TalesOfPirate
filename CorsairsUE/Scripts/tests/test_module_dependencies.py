from pathlib import Path
import subprocess
import sys
import unittest

from CorsairsUE.Scripts import reference_terrain_rules as rules


ROOT = Path(__file__).resolve().parents[3]


class ModuleDependencyTests(unittest.TestCase):
    def test_ue_python_entrypoints_bootstrap_sibling_imports(self):
        scripts = ROOT / "CorsairsUE/Scripts"
        for name in (
            "build_reference_terrain_level.py",
            "import_reference_terrain.py",
            "check_reference_terrain.py",
        ):
            entrypoint = scripts / name
            probe = (
                "import runpy,sys,types;"
                "sys.dont_write_bytecode=True;"
                "sys.modules['unreal']=types.ModuleType('unreal');"
                f"runpy.run_path({str(entrypoint)!r},run_name='ue_entrypoint_probe')"
            )
            completed = subprocess.run(
                [sys.executable, "-I", "-c", probe],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_module_graph_has_no_editor_runtime_cycle(self):
        issues = rules.validate_module_contract(ROOT)
        self.assertEqual(issues, [])

    def test_runtime_data_staging_is_exact(self):
        game_rules = (
            ROOT / "CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs"
        ).read_text(encoding="utf-8")
        for relative in rules.RUNTIME_INPUT_PATHS:
            self.assertEqual(game_rules.count(relative), 1)
        self.assertEqual(game_rules.count("StagedFileType.UFS"), 3)

    def test_editor_and_runtime_automation_contracts_exist(self):
        editor = (ROOT / "CorsairsUE/Source/CorsairsImport/Private/Tests/"
                  "ReferenceTerrainAssetTests.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(encoding="utf-8")
        self.assertIn("Corsairs.Terrain.ReferenceAssets.DefaultGameMode", editor)
        self.assertIn("Corsairs.Terrain.ReferenceAssets.ReferenceActor", editor)
        self.assertIn("/Script/CorsairsGame.CorsairsGameMode", editor)
        self.assertNotIn('#include "CorsairsGame', editor)
        self.assertIn("Corsairs.Terrain.ReferenceRuntime.CookedWorld", runtime)
        self.assertIn("CORSAIRS_TERRAIN_RUNTIME_JSON=", runtime)
        for relative in rules.RUNTIME_INPUT_PATHS:
            self.assertIn(relative, runtime)
        self.assertNotIn("CorsairsImport", runtime)
        self.assertNotIn("UnrealEd", runtime)


if __name__ == "__main__":
    unittest.main()
