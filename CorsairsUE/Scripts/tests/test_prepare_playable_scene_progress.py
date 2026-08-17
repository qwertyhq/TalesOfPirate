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

    def test_recovery_validates_before_any_backup_mutation(self):
        target = playable.TARGET_LEVEL
        _temporary, backup = playable.owned_transaction_paths(target)

        class Library:
            assets = {target: "validated-target", backup: "old-copy"}
            validation_observed = False

            @classmethod
            def does_asset_exist(cls, path):
                return path in cls.assets

            @classmethod
            def delete_asset(cls, path):
                if path == backup and not cls.validation_observed:
                    raise AssertionError("backup удалён до валидации target")
                cls.assets.pop(path, None)
                return True

        def validate_published():
            self.assertEqual("validated-target", Library.assets[target])
            self.assertEqual("old-copy", Library.assets[backup])
            Library.validation_observed = True

        playable.recover_owned_transaction(
            Library,
            target,
            validate_published,
        )

        self.assertEqual({target: "validated-target"}, Library.assets)

    def test_recovery_validation_failure_does_not_delete_backup(self):
        target = playable.TARGET_LEVEL
        _temporary, backup = playable.owned_transaction_paths(target)

        class Library:
            assets = {target: "candidate", backup: "previous"}
            delete_calls = []

            @classmethod
            def does_asset_exist(cls, path):
                return path in cls.assets

            @classmethod
            def delete_asset(cls, path):
                cls.delete_calls.append(path)
                raise AssertionError("backup удалён при неуспешной валидации")

        def reject_target():
            raise RuntimeError("target не прошёл readback")

        with self.assertRaisesRegex(RuntimeError, "target не прошёл readback"):
            playable.recover_owned_transaction(Library, target, reject_target)

        self.assertEqual([], Library.delete_calls)
        self.assertEqual(
            {target: "candidate", backup: "previous"},
            Library.assets,
        )

    def test_publish_keeps_target_when_backup_delete_started_but_copy_is_gone(self):
        target = playable.TARGET_LEVEL
        temporary, backup = playable.owned_transaction_paths(target)

        class Library:
            # Asset registry намеренно сообщает stale backup после фактического
            # удаления: это mutation-sensitive граница UE 5.8.
            assets = {target: "old-target", temporary: "new-target"}
            stale_backup = False

            @classmethod
            def does_asset_exist(cls, path):
                return path in cls.assets or (path == backup and cls.stale_backup)

            @classmethod
            def rename_asset(cls, source, destination):
                if source not in cls.assets or destination in cls.assets:
                    return False
                cls.assets[destination] = cls.assets.pop(source)
                return True

            @classmethod
            def delete_asset(cls, path):
                cls.assets.pop(path, None)
                cls.stale_backup = path == backup
                return True

        class Levels:
            loaded = []

            @classmethod
            def load_level(cls, path):
                cls.loaded.append(path)
                return True

        def delete_backup(library, path):
            self.assertEqual(backup, path)
            library.delete_asset(path)
            return False

        with (
            mock.patch.object(playable, "delete_owned_map", delete_backup),
            self.assertRaisesRegex(RuntimeError, "backup не удалён"),
        ):
            playable.publish_transaction(
                mock.Mock(), Library, Levels, playable.SOURCE_LEVEL, target
            )

        # Старый target уже заменён и backup физически исчез. Он не должен
        # перемещаться во временный путь в попытке восстановить невозможное.
        self.assertEqual("new-target", Library.assets[target])
        self.assertNotIn(temporary, Library.assets)
        self.assertEqual(target, Levels.loaded[-1])

    def test_remove_baked_npcs_validates_all_classes_before_mutating(self):
        class Actor:
            def __init__(self, label, class_path):
                self.label = label
                self.class_path = class_path

            def get_actor_label(self):
                return self.label

            def get_class(self):
                return SimpleNamespace(get_path_name=lambda: self.class_path)

        actors = [
            Actor(f"{playable.BAKED_NPC_PREFIX}{index}", playable.CHARACTER_CLASS)
            for index in range(playable.BAKED_NPC_COUNT - 1)
        ]
        actors.append(
            Actor(
                f"{playable.BAKED_NPC_PREFIX}bad",
                "/Script/Engine.StaticMeshActor",
            )
        )

        class Subsystem:
            destroyed = []

            @classmethod
            def get_all_level_actors(cls):
                return actors

            @classmethod
            def destroy_actor(cls, actor):
                cls.destroyed.append(actor)
                return True

        with self.assertRaisesRegex(RuntimeError, "отказ удаления чужого actor"):
            playable.remove_baked_npcs(mock.Mock(), mock.Mock(), Subsystem)
        self.assertEqual([], Subsystem.destroyed)

    def test_remove_baked_npcs_happy_path_destroys_exact_validated_set(self):
        class Actor:
            def __init__(self, label):
                self.label = label

            def get_actor_label(self):
                return self.label

            def get_class(self):
                return SimpleNamespace(
                    get_path_name=lambda: playable.CHARACTER_CLASS
                )

        actors = [
            Actor(f"{playable.BAKED_NPC_PREFIX}{index}")
            for index in range(playable.BAKED_NPC_COUNT)
        ]

        class Subsystem:
            destroyed = []

            @classmethod
            def get_all_level_actors(cls):
                return [actor for actor in actors if actor not in cls.destroyed]

            @classmethod
            def destroy_actor(cls, actor):
                cls.destroyed.append(actor)
                return True

        playable.remove_baked_npcs(mock.Mock(), mock.Mock(), Subsystem)
        self.assertEqual(actors, Subsystem.destroyed)


if __name__ == "__main__":
    unittest.main()
