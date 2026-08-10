import unittest

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


if __name__ == "__main__":
    unittest.main()
