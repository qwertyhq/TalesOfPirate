import unittest

from CorsairsUE.Scripts.tests.test_scene_progress_animation_contract import (
    CITY_SCRIPT,
    fake_unreal_module,
    load_script,
)


SOURCE = "/Game/Maps/Garner"
TARGET = "/Game/Maps/GarnerSceneProgressCity"
TEMP = "/Game/Maps/GarnerSceneProgressCity__TxnTemp"
BACKUP = "/Game/Maps/GarnerSceneProgressCity__TxnBackup"
UNRELATED = "/Game/Maps/Unrelated"


class FakeReport:
    def __init__(self):
        self.lines = []

    def line(self, message):
        self.lines.append(message)


class FakeAssetLibrary:
    def __init__(self):
        self.assets = {
            SOURCE: "source",
            TARGET: "old-target",
            UNRELATED: "unrelated",
        }
        self.fail_duplicate = False
        self.fail_renames = set()
        self.fail_deletes = set()

    def does_asset_exist(self, path):
        return path in self.assets

    def duplicate_asset(self, source, destination):
        if self.fail_duplicate or source not in self.assets:
            return None
        self.assets[destination] = self.assets[source]
        return object()

    def rename_asset(self, source, destination):
        if ((source, destination) in self.fail_renames
                or source not in self.assets or destination in self.assets):
            return False
        self.assets[destination] = self.assets.pop(source)
        return True

    def delete_asset(self, path):
        if path in self.fail_deletes or path not in self.assets:
            return False
        del self.assets[path]
        return True


class FakeLevels:
    def __init__(self, library):
        self.library = library
        self.current = None
        self.fail_loads = set()
        self.fail_save = False

    def load_level(self, path):
        if path in self.fail_loads or path not in self.library.assets:
            return False
        self.current = path
        return True

    def save_current_level(self):
        return not self.fail_save and self.current in self.library.assets


class CityMapTransactionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = load_script(CITY_SCRIPT, fake_unreal_module())

    def fixture(self):
        library = FakeAssetLibrary()
        return FakeReport(), library, FakeLevels(library)

    def run_transaction(self, report, library, levels, build=None):
        def default_build(_levels):
            library.assets[TEMP] = "new-target"

        def readback():
            if library.assets.get(TEMP) != "new-target":
                raise RuntimeError("saved readback mismatch")

        return self.script.run_owned_map_transaction(
            report,
            SOURCE,
            TARGET,
            default_build if build is None else build,
            readback,
            library=library,
            levels=levels,
        )

    def assert_old_target_is_only_owned_result(self, library):
        self.assertEqual("old-target", library.assets[TARGET])
        self.assertNotIn(TEMP, library.assets)
        self.assertNotIn(BACKUP, library.assets)
        self.assertEqual("unrelated", library.assets[UNRELATED])

    def test_source_duplicate_and_build_failures_preserve_old_target(self):
        for failure in ("source-load", "duplicate", "build"):
            with self.subTest(failure=failure):
                report, library, levels = self.fixture()
                if failure == "source-load":
                    levels.fail_loads.add(SOURCE)
                elif failure == "duplicate":
                    library.fail_duplicate = True

                def build(_levels):
                    if failure == "build":
                        raise RuntimeError("build failed")
                    library.assets[TEMP] = "new-target"

                with self.assertRaises(RuntimeError):
                    self.run_transaction(
                        report, library, levels, build=build)
                self.assert_old_target_is_only_owned_result(library)
                if failure != "source-load":
                    self.assertEqual(
                        SOURCE,
                        levels.current,
                        "failed temp map must be unloaded before cleanup",
                    )

    def test_publish_rename_failure_rolls_back_old_target(self):
        report, library, levels = self.fixture()
        library.fail_renames.add((TEMP, TARGET))

        with self.assertRaisesRegex(RuntimeError, "publish"):
            self.run_transaction(report, library, levels)

        self.assert_old_target_is_only_owned_result(library)

    def test_success_publishes_new_target_and_removes_transaction_assets(self):
        report, library, levels = self.fixture()

        self.run_transaction(report, library, levels)

        self.assertEqual("new-target", library.assets[TARGET])
        self.assertNotIn(TEMP, library.assets)
        self.assertNotIn(BACKUP, library.assets)
        self.assertEqual("unrelated", library.assets[UNRELATED])
        self.assertEqual(TARGET, levels.current)

    def test_stale_owned_paths_recover_deterministically_or_fail_closed(self):
        report, library, _levels = self.fixture()
        library.assets[TEMP] = "stale-temp"
        self.script.recover_owned_map_transaction(report, library, TARGET)
        self.assert_old_target_is_only_owned_result(library)

        report, library, _levels = self.fixture()
        del library.assets[TARGET]
        library.assets[TEMP] = "interrupted-new"
        library.assets[BACKUP] = "old-target"
        self.script.recover_owned_map_transaction(report, library, TARGET)
        self.assert_old_target_is_only_owned_result(library)

        report, library, _levels = self.fixture()
        library.assets[BACKUP] = "ambiguous-backup"
        before = dict(library.assets)
        with self.assertRaisesRegex(RuntimeError, "неоднозначное"):
            self.script.recover_owned_map_transaction(report, library, TARGET)
        self.assertEqual(before, library.assets)

        report, library, _levels = self.fixture()
        library.assets[TEMP] = "ambiguous-temp"
        library.assets[BACKUP] = "ambiguous-backup"
        before = dict(library.assets)
        with self.assertRaisesRegex(RuntimeError, "неоднозначное"):
            self.script.recover_owned_map_transaction(report, library, TARGET)
        self.assertEqual(before, library.assets)


if __name__ == "__main__":
    unittest.main()
