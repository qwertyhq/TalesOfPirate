from pathlib import Path
import subprocess
import sys
import unittest

from CorsairsUE.Scripts import reference_terrain_rules as rules


ROOT = Path(__file__).resolve().parents[3]


class ModuleDependencyTests(unittest.TestCase):
    def test_runtime_terrain_nanite_uses_disk_asset_registry_metadata(self):
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(
                       encoding="utf-8")
        game_rules = (
            ROOT / "CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs"
        ).read_text(encoding="utf-8")
        public_dependencies = game_rules.split(
            "PublicDependencyModuleNames.AddRange", 1)[1].split("});", 1)[0]

        self.assertIn('#include "AssetRegistry/AssetRegistryModule.h"', runtime)
        self.assertIn("GetAssetByObjectPath(", runtime)
        self.assertIn("/*bIncludeOnlyOnDiskAssets=*/ true", runtime)
        self.assertIn('GetTagValue(TEXT("NaniteEnabled")', runtime)
        self.assertIn('FString(TEXT("True"))', runtime)
        self.assertNotIn("HasValidNaniteData()", runtime)
        self.assertEqual(
            game_rules.count(
                'PrivateDependencyModuleNames.Add("AssetRegistry");'),
            1,
        )
        self.assertNotIn('"AssetRegistry"', public_dependencies)

    def test_runtime_terrain_transform_uses_serialized_root_state(self):
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(
                       encoding="utf-8")
        self.assertIn("GetRootComponent() == Component", runtime)
        self.assertIn("GetAttachParent() == nullptr", runtime)
        self.assertIn("const FTransform RelativeTransform =", runtime)
        self.assertIn("Component->GetRelativeTransform()", runtime)
        self.assertIn("Component->CalcBounds(RelativeTransform).GetBox()", runtime)
        for forbidden in (
            "GetActorLocation()",
            "GetActorBounds(",
            "GetActorTransform()",
            "GetComponentTransform()",
            "ConditionalUpdateComponentToWorld",
            "RegisterComponent",
            "InitializeActorsForPlay",
            "BeginPlay",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, runtime)

    def test_runtime_terrain_sha256_uses_commoncrypto_on_mac(self):
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(
                       encoding="utf-8")
        self.assertIn(
            "#if PLATFORM_MAC\n#include <CommonCrypto/CommonDigest.h>\n#endif",
            runtime,
        )
        self.assertIn("Data == nullptr || SizeBytes <= 0", runtime)
        self.assertIn("SizeBytes > static_cast<int64>(MAX_uint32)", runtime)
        self.assertIn("CC_SHA256(", runtime)
        self.assertIn("static_cast<CC_LONG>(SizeBytes)", runtime)
        self.assertIn("Signature.Signature) != nullptr", runtime)
        self.assertIn("FPlatformMisc::GetSHA256Signature(", runtime)
        self.assertIn("if (!bComputed)", runtime)
        self.assertIn("Signature.ToString().ToLower()", runtime)
        self.assertIn("IsLowerHex(OutSha256, 64)", runtime)

    def test_runtime_sandbox_probe_uses_supported_in_process_paths_before_world_load(self):
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(
                       encoding="utf-8")
        self.assertIn('#include "HAL/PlatformProcess.h"', runtime)
        self.assertIn("FParse::Param(", runtime)
        self.assertIn("CorsairsTerrainSandboxProbe", runtime)
        self.assertIn("FPlatformProcess::GetGameBundleId()", runtime)
        self.assertIn("FPlatformProcess::UserHomeDir()", runtime)
        self.assertIn("FPaths::AutomationReportsDir()", runtime)
        self.assertIn("EscapeJsonString(", runtime)
        self.assertEqual(runtime.count("CORSAIRS_TERRAIN_SANDBOX_JSON="), 1)
        self.assertLess(
            runtime.index("CORSAIRS_TERRAIN_SANDBOX_JSON="),
            runtime.index("LoadObject<UWorld>"),
        )
        sandbox_start = runtime.index("if (bSandboxProbe)")
        world_else = runtime.index("\n\telse\n\t{", sandbox_start)
        sandbox_branch = runtime[sandbox_start:world_else]
        world_branch = runtime[world_else:]
        self.assertTrue(sandbox_branch.rstrip().endswith("return true;\n\t}"))
        self.assertNotIn("CORSAIRS_TERRAIN_RUNTIME_JSON=", sandbox_branch)
        self.assertIn("LoadObject<UWorld>", world_branch)
        self.assertIn("CORSAIRS_TERRAIN_RUNTIME_JSON=", world_branch)
        self.assertNotIn("CORSAIRS_TERRAIN_SANDBOX_JSON=", world_branch)

    def test_runtime_sandbox_probe_bootstraps_only_its_strict_reports_suffix(self):
        runtime = (ROOT / "CorsairsUE/Source/CorsairsGame/Private/Tests/"
                   "CorsairsReferenceTerrainRuntimeTests.cpp").read_text(
                       encoding="utf-8")
        for token in (
            '#include "HAL/PlatformFile.h"',
            "bool BootstrapSandboxAutomationReportsDirectory(",
            "bool VerifySandboxDirectoryChain(",
            "::open(",
            "::openat(",
            "::mkdirat(",
            "O_NOFOLLOW_ANY",
            "O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC",
            "OpenError != ENOENT",
            "::fstat(",
            "::geteuid()",
            "S_IWGRP | S_IWOTH",
            "static_cast<mode_t>(ALLPERMS)",
            "bRequireExactOwnerMode &&",
            "static_cast<mode_t>(0700)",
            "::fchmod(ChildDescriptor, static_cast<mode_t>(0700))",
            "ChildDescriptor, false, &ChildIdentity",
            "ParentDescriptor, Component, false, &ChildIdentity",
            "const bool bChildSynced = ::fsync(ChildDescriptor) == 0;",
            "const bool bParentSynced = ::fsync(ParentDescriptor) == 0;",
            "if (!bChildSynced || !bParentSynced)",
            "::fsync(",
            "::close(",
            "ReconstructedRoot != AutomationReportsRoot",
            "ExpectedIdentities",
            "SameSandboxDirectoryIdentity(",
            "IPlatformFile::GetPlatformPhysical()",
            "CreateDirectoryTree(*AutomationReportsRoot)",
            "DirectoryExists(*AutomationReportsRoot)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, runtime)
        self.assertNotIn("CreateDirectoryTree(*ContainerDataRoot)", runtime)
        self.assertEqual(
            runtime.count("BootstrapSandboxAutomationReportsDirectory("), 2)

        helper_start = runtime.index(
            "bool BootstrapSandboxAutomationReportsDirectory(")
        absolute_open_start = runtime.index(
            "int OpenVerifiedSandboxDirectory(")
        absolute_open_end = runtime.index(
            "int OpenVerifiedSandboxDirectoryAt(", absolute_open_start)
        self.assertIn(
            "O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC",
            runtime[absolute_open_start:absolute_open_end])
        components = runtime.index(
            "if (!SandboxDirectoryComponents(", helper_start)
        data_open = runtime.index(
            "int ParentDescriptor = OpenVerifiedSandboxDirectory(", helper_start)
        suffix_mkdir = runtime.index("::mkdirat(", helper_start)
        created_normalize = runtime.index(
            "::fchmod(ChildDescriptor, static_cast<mode_t>(0700))",
            suffix_mkdir)
        child_barrier = runtime.index("const bool bChildSynced", suffix_mkdir)
        parent_barrier = runtime.index("const bool bParentSynced", child_barrier)
        barrier_gate = runtime.index(
            "if (!bChildSynced || !bParentSynced)", parent_barrier)
        component_helper = runtime.index("bool SandboxDirectoryComponents(")
        reconstruction = runtime.index(
            "ReconstructedRoot != AutomationReportsRoot", component_helper)
        second_walk = runtime.index(
            "return VerifySandboxDirectoryChain(", helper_start)
        self.assertLess(component_helper, reconstruction)
        self.assertLess(components, data_open)
        self.assertLess(data_open, suffix_mkdir)
        self.assertLess(suffix_mkdir, created_normalize)
        self.assertLess(created_normalize, child_barrier)
        self.assertLess(child_barrier, parent_barrier)
        self.assertLess(parent_barrier, barrier_gate)
        self.assertLess(suffix_mkdir, second_walk)
        bootstrap_mac = runtime[helper_start:runtime.index("#else", helper_start)]
        self.assertNotIn("::fsync(ChildDescriptor) != 0 ||", bootstrap_mac)
        self.assertNotIn("static_cast<mode_t>(0777)", bootstrap_mac)

        sandbox_start = runtime.index("if (bSandboxProbe)")
        world_else = runtime.index("\n\telse\n\t{", sandbox_start)
        sandbox_branch = runtime[sandbox_start:world_else]
        world_branch = runtime[world_else:]
        containment = sandbox_branch.index("FPaths::IsUnderDirectory")
        containment_gate = sandbox_branch.index(
            "if (HasAnyErrors())", containment)
        bootstrap = sandbox_branch.index(
            "if (!BootstrapSandboxAutomationReportsDirectory(",
            containment_gate)
        failure = sandbox_branch.index("AddError(", bootstrap)
        failure_return = sandbox_branch.index("return false;", failure)
        json_build = sandbox_branch.index("const FString Json", failure_return)
        event = sandbox_branch.index(
            "CORSAIRS_TERRAIN_SANDBOX_JSON=", json_build)
        self.assertLess(containment, containment_gate)
        self.assertLess(containment_gate, bootstrap)
        self.assertLess(bootstrap, failure)
        self.assertLess(failure, failure_return)
        self.assertLess(failure_return, json_build)
        self.assertLess(json_build, event)
        self.assertNotIn(
            "BootstrapSandboxAutomationReportsDirectory(", world_branch)

    def test_contract_enum_uses_equality_not_python_alias_spelling(self):
        entrypoint = ROOT / "CorsairsUE/Scripts/import_reference_terrain.py"
        probe = f"""
import runpy
import sys
import types

class EnumValue:
    def __init__(self, spelling, identity):
        self.spelling = spelling
        self.identity = identity

    def __eq__(self, other):
        return isinstance(other, EnumValue) and self.identity == other.identity

    def __str__(self):
        return self.spelling

sys.modules["unreal"] = types.ModuleType("unreal")
namespace = runpy.run_path({str(entrypoint)!r}, run_name="ue_enum_probe")
contract_enum = namespace["_contract_enum"]
actual_alias = EnumValue("TextureCompressionSettings.DEFAULT", 0)
expected_alias = EnumValue("TextureCompressionSettings.TC_DEFAULT", 0)
mismatch = EnumValue("TextureCompressionSettings.NORMALMAP", 1)
assert contract_enum(actual_alias, expected_alias, "TC_DEFAULT") == "TC_DEFAULT"
assert contract_enum(mismatch, expected_alias, "TC_DEFAULT") == "NORMALMAP"
"""
        completed = subprocess.run(
            [sys.executable, "-I", "-B", "-c", probe],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_vector_tolerance_is_finite_componentwise_and_inclusive(self):
        entrypoint = ROOT / "CorsairsUE/Scripts/import_reference_terrain.py"
        probe = f"""
import runpy
import sys
import types

sys.modules["unreal"] = types.ModuleType("unreal")
namespace = runpy.run_path({str(entrypoint)!r}, run_name="ue_vector_probe")
within = namespace["_vector_within_tolerance"]
vector = types.SimpleNamespace
expected = vector(x=1.0, y=2.0, z=3.0)
assert within(vector(x=1.001, y=1.999, z=3.001), expected, 0.001)
assert not within(vector(x=1.001001, y=2.0, z=3.0), expected, 0.001)
assert not within(vector(x=float("nan"), y=2.0, z=3.0), expected, 0.001)
assert not within(vector(x=1.0, y=float("inf"), z=3.0), expected, 0.001)
assert not within(expected, expected, float("nan"))
assert not within(expected, expected, -0.001)
"""
        completed = subprocess.run(
            [sys.executable, "-I", "-B", "-c", probe],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_interchange_nested_static_mesh_is_adopted_at_canonical_path(self):
        entrypoint = ROOT / "CorsairsUE/Scripts/import_reference_terrain.py"
        probe = f"""
import runpy
import sys
import types

canonical = "/Game/Terrain/Reference/Garner/SM_Garner_17_21"
nested = canonical.rsplit("/", 1)[0] + "/source/StaticMeshes/SM_Garner_17_21.SM_Garner_17_21"
extra = canonical.rsplit("/", 1)[0] + "/source/Materials/M_Unexpected.M_Unexpected"

class StaticMesh:
    def __init__(self, path):
        self.path = path

    def get_path_name(self):
        return self.path

class OtherAsset:
    pass

mesh = StaticMesh(nested)
state = {{nested: mesh, extra: OtherAsset()}}
renamed = []
deleted = []

class EditorAssetLibrary:
    @staticmethod
    def does_asset_exist(path):
        return path in state

    @staticmethod
    def delete_asset(path):
        deleted.append(path)
        return state.pop(path, None) is not None

    @staticmethod
    def rename_loaded_asset(asset, destination):
        renamed.append((asset.get_path_name(), destination))
        state.pop(asset.get_path_name())
        asset.path = destination + ".SM_Garner_17_21"
        state[destination] = asset
        return True

unreal = types.ModuleType("unreal")
unreal.StaticMesh = StaticMesh
unreal.EditorAssetLibrary = EditorAssetLibrary
unreal.load_asset = lambda path: state.get(path)
sys.modules["unreal"] = unreal
namespace = runpy.run_path({str(entrypoint)!r}, run_name="ue_import_probe")
asset, removed = namespace["_adopt_imported_asset"](
    [nested, extra], canonical, StaticMesh)
assert asset is mesh
assert renamed == [(nested, canonical)]
assert deleted == [extra]
assert removed == [extra]
assert state == {{canonical: mesh}}
"""
        completed = subprocess.run(
            [sys.executable, "-I", "-B", "-c", probe],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_ue_python_entrypoints_disable_bytecode_before_sibling_imports(self):
        scripts = ROOT / "CorsairsUE/Scripts"
        sibling_imports = {
            "build_reference_terrain_level.py": "import reference_terrain_rules",
            "import_reference_terrain.py": "import reference_terrain_rules",
            "check_reference_terrain.py": "import import_reference_terrain",
        }
        for name, sibling_import in sibling_imports.items():
            source = (scripts / name).read_text(encoding="utf-8")
            marker = "sys.dont_write_bytecode = True"
            self.assertIn(marker, source)
            self.assertLess(source.index(marker), source.index(sibling_import))

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
