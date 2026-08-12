import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from CorsairsUE.Scripts import prepare_playable_scene_progress as playable


class PreparePlayableSceneProgressContractTests(unittest.TestCase):
    def test_contract_is_exact_and_owned(self):
        self.assertEqual(
            "/Game/Maps/GarnerSceneProgressCity",
            playable.SOURCE_LEVEL,
        )
        self.assertEqual(
            "/Game/Maps/GarnerSceneProgressPlay",
            playable.TARGET_LEVEL,
        )
        self.assertEqual(
            "SceneProgressCity_Test195126",
            playable.BAKED_CHARACTER_LABEL,
        )
        self.assertEqual("SceneProgressCity_NPC_", playable.BAKED_NPC_PREFIX)
        self.assertEqual(5, playable.BAKED_NPC_COUNT)
        self.assertEqual(
            (
                "/Game/Maps/GarnerSceneProgressPlay__TxnTemp",
                "/Game/Maps/GarnerSceneProgressPlay__TxnBackup",
            ),
            playable.owned_transaction_paths(playable.TARGET_LEVEL),
        )

        with self.assertRaisesRegex(RuntimeError, playable.SOURCE_LEVEL):
            playable.validate_contract("/Game/Maps/Garner", playable.TARGET_LEVEL)
        with self.assertRaisesRegex(RuntimeError, playable.TARGET_LEVEL):
            playable.validate_contract(playable.SOURCE_LEVEL, "/Game/Maps/Garner")
        with self.assertRaisesRegex(RuntimeError, "совпадают"):
            playable.validate_contract(playable.SOURCE_LEVEL, playable.SOURCE_LEVEL)
        with self.assertRaisesRegex(RuntimeError, playable.TARGET_LEVEL):
            playable.owned_transaction_paths("/Game/Maps/Garner")

    def test_owned_backup_falls_back_to_exact_package_removal(self):
        backup = playable.TARGET_LEVEL + playable.TRANSACTION_BACKUP_SUFFIX

        with tempfile.TemporaryDirectory() as temporary_directory:
            content = Path(temporary_directory)
            package = content / "Maps/GarnerSceneProgressPlay__TxnBackup.umap"
            package.parent.mkdir(parents=True)
            package.write_bytes(b"owned backup")

            class Library:
                @staticmethod
                def delete_asset(_path):
                    return True

                @staticmethod
                def does_asset_exist(_path):
                    return package.is_file()

            class Registry:
                calls = []

                @classmethod
                def scan_paths_synchronous(cls, paths, force_rescan=False):
                    cls.calls.append((paths, force_rescan))

            fake_unreal = SimpleNamespace(
                AssetRegistryHelpers=SimpleNamespace(
                    get_asset_registry=lambda: Registry
                )
            )
            with (
                mock.patch.object(playable, "CONTENT_DIR", content),
            ):
                self.assertTrue(
                    playable.delete_owned_map(Library, backup, fake_unreal)
                )

            self.assertFalse(package.exists())
            self.assertEqual([(["/Game/Maps"], True)], Registry.calls)

    def test_owned_removal_refuses_canonical_target(self):
        with self.assertRaisesRegex(RuntimeError, "не служебной"):
            playable.owned_map_package_file(playable.TARGET_LEVEL)

    def test_recovery_commits_validated_target_when_backup_remains(self):
        target = playable.TARGET_LEVEL
        temporary, backup = playable.owned_transaction_paths(target)

        class Library:
            assets = {target: "candidate", backup: "previous"}

            @classmethod
            def does_asset_exist(cls, path):
                return path in cls.assets

            @classmethod
            def rename_asset(cls, source, destination):
                if source not in cls.assets or destination in cls.assets:
                    return False
                cls.assets[destination] = cls.assets.pop(source)
                return True

            @classmethod
            def delete_asset(cls, path):
                cls.assets.pop(path, None)
                return True

        validations = []
        playable.recover_owned_transaction(
            Library,
            target,
            lambda: validations.append("validated"),
        )

        self.assertEqual(["validated"], validations)
        self.assertEqual({target: "candidate"}, Library.assets)
        self.assertNotIn(temporary, Library.assets)
        self.assertNotIn(backup, Library.assets)


if __name__ == "__main__":
    unittest.main()
