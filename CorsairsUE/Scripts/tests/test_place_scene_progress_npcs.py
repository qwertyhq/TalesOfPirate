import json
from pathlib import Path
import subprocess
import sys
import unittest

from CorsairsUE.Scripts import place_scene_progress_npcs as npc_placement


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "CorsairsUE" / "Scripts" / "place_scene_progress_npcs.py"


class SceneProgressNpcPlacementTests(unittest.TestCase):
    def test_real_dry_run_resolves_exact_reference_npc_slice(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--dry-run"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            {
                "actorPrefix": "SceneProgressCity_NPC_",
                "actors": [
                    {
                        "animation": (
                            "/Game/Animations/0090/SkeletalMeshes/0090_Anim"
                        ),
                        "animationImported": True,
                        "animationPolicy": "loop",
                        "characterName": "Trader",
                        "characterType": 17,
                        "dbNpcId": 9,
                        "label": "SceneProgressCity_NPC_008_Physican_Ditto",
                        "locationCm": [-277025.0, 225075.0, 60.0],
                        "mesh": (
                            "/Game/All/0090000000/SkeletalMeshes/0090000000"
                        ),
                        "meshImported": True,
                        "modelId": 90,
                        "name": "Physican - Ditto",
                        "sourceDirectionDegrees": 180.0,
                        "sourcePosition": [225075, 277025],
                        "sourceSerial": 8,
                        "yawDegrees": 90.0,
                    },
                    {
                        "animation": (
                            "/Game/Animations/0126/SkeletalMeshes/0126_Anim"
                        ),
                        "animationImported": True,
                        "animationPolicy": "loop",
                        "characterName": "Nurse",
                        "characterType": 29,
                        "dbNpcId": 15,
                        "label": "SceneProgressCity_NPC_014_Nurse_Gina",
                        "locationCm": [-277032.0, 224487.0, 60.0],
                        "mesh": (
                            "/Game/All/0126000000/SkeletalMeshes/0126000000"
                        ),
                        "meshImported": True,
                        "modelId": 126,
                        "name": "Nurse - Gina",
                        "sourceDirectionDegrees": 180.0,
                        "sourcePosition": [224487, 277032],
                        "sourceSerial": 14,
                        "yawDegrees": 90.0,
                    },
                    {
                        "animation": (
                            "/Game/Animations/0011/SkeletalMeshes/0011_Anim"
                        ),
                        "animationImported": True,
                        "animationPolicy": "loop",
                        "characterName": "Occult Merchant",
                        "characterType": 11,
                        "dbNpcId": 22,
                        "label": "SceneProgressCity_NPC_021_Newbie_Guide_Senna",
                        "locationCm": [-278525.0, 222375.0, 60.0],
                        "mesh": (
                            "/Game/All/0011000000/SkeletalMeshes/0011000000"
                        ),
                        "meshImported": True,
                        "modelId": 11,
                        "name": "Newbie Guide - Senna",
                        "sourceDirectionDegrees": 180.0,
                        "sourcePosition": [222375, 278525],
                        "sourceSerial": 21,
                        "yawDegrees": 90.0,
                    },
                    {
                        "animation": (
                            "/Game/Animations/0223/SkeletalMeshes/0223_Anim"
                        ),
                        "animationImported": True,
                        "animationPolicy": "staticReferencePose",
                        "characterName": "King Penguin",
                        "characterType": 260,
                        "dbNpcId": 143,
                        "label": "SceneProgressCity_NPC_142_Event_NPC_Pappa",
                        "locationCm": [-276827.0, 222275.0, 60.0],
                        "mesh": (
                            "/Game/All/0223000000/SkeletalMeshes/0223000000"
                        ),
                        "meshImported": True,
                        "modelId": 223,
                        "name": "Event NPC - Pappa",
                        "sourceDirectionDegrees": 180.0,
                        "sourcePosition": [222275, 276827],
                        "sourceSerial": 142,
                        "yawDegrees": 90.0,
                    },
                    {
                        "animation": (
                            "/Game/Animations/0261/SkeletalMeshes/0261_Anim"
                        ),
                        "animationImported": True,
                        "animationPolicy": "loop",
                        "characterName": "Trader 3",
                        "characterType": 449,
                        "dbNpcId": 153,
                        "label": "SceneProgressCity_NPC_152_Weird_Hat_Seller",
                        "locationCm": [-276932.0, 224000.0, 60.0],
                        "mesh": (
                            "/Game/All/0261000000/SkeletalMeshes/0261000000"
                        ),
                        "meshImported": True,
                        "modelId": 261,
                        "name": "Weird Hat Seller",
                        "sourceDirectionDegrees": 180.0,
                        "sourcePosition": [224000, 276932],
                        "sourceSerial": 152,
                        "yawDegrees": 90.0,
                    },
                ],
                "animationPolicyCensus": {
                    "loop": 4,
                    "staticReferencePose": 1,
                },
                "applyReady": True,
                "count": 5,
                "schemaVersion": 1,
                "status": "PASS",
                "target": "/Game/Maps/GarnerSceneProgressCity",
            },
            json.loads(result.stdout),
        )

    def test_dry_run_rejects_arbitrary_target_level(self):
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--dry-run",
                "--target",
                "/Game/Maps/Garner",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(2, result.returncode, result.stdout)
        self.assertEqual("", result.stdout)
        self.assertIn(npc_placement.TARGET_LEVEL, result.stderr)

    def test_apply_plan_rejects_arbitrary_target_before_unreal(self):
        actors = [
            {"sourceSerial": serial, "animationPolicy": policy}
            for serial, policy in (
                (8, "loop"),
                (14, "loop"),
                (21, "loop"),
                (142, "staticReferencePose"),
                (152, "loop"),
            )
        ]
        plan = {
            "actors": actors,
            "animationPolicyCensus": {
                "loop": 4,
                "staticReferencePose": 1,
            },
            "target": "/Game/Maps/Garner",
        }

        with self.assertRaisesRegex(
                npc_placement.NpcPreflightError,
                npc_placement.TARGET_LEVEL):
            npc_placement.apply_plan(plan)

    def test_static_reference_pose_skips_animation_override(self):
        class Component:
            def __init__(self):
                self.override_calls = []

            def override_animation_data(self, *args):
                self.override_calls.append(args)

        component = Component()
        animation = object()
        npc_placement.apply_animation_policy(
            component,
            {"sourceSerial": 142, "animationPolicy": "staticReferencePose"},
            None,
        )
        self.assertEqual([], component.override_calls)

        npc_placement.apply_animation_policy(
            component,
            {"sourceSerial": 8, "animationPolicy": "loop"},
            animation,
        )
        self.assertEqual(
            [(animation, True, True, 0.0, 1.0)],
            component.override_calls,
        )

    def test_animation_policy_census_is_fail_closed(self):
        actors = [
            {"sourceSerial": serial, "animationPolicy": policy}
            for serial, policy in (
                (8, "loop"),
                (14, "loop"),
                (21, "loop"),
                (142, "staticReferencePose"),
                (152, "loop"),
            )
        ]
        self.assertEqual(
            {"loop": 4, "staticReferencePose": 1},
            npc_placement.validate_animation_policy_census(actors),
        )
        actors[3]["animationPolicy"] = "loop"
        with self.assertRaises(npc_placement.NpcPreflightError):
            npc_placement.validate_animation_policy_census(actors)


if __name__ == "__main__":
    unittest.main()
