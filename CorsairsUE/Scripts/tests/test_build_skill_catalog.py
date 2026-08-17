import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "CorsairsUE" / "Scripts"))

import build_skill_catalog


class SkillCatalogTests(unittest.TestCase):
    def create_database(self, directory):
        path = Path(directory) / "skills.sqlite"
        connection = sqlite3.connect(path)
        connection.execute(
            "CREATE TABLE skills ("
            "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
            "tar_type INTEGER, helpful INTEGER, apply_distance INTEGER, "
            "apply_target INTEGER, apply_type INTEGER, radii INTEGER, "
            "range_val INTEGER)"
        )
        connection.executemany(
            "INSERT INTO skills VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                (4, "Phoenix Slash", 1, 0, 800, 4, 2, 400, 0),
                (1, "Windslash", 1, 0, 400, 4, 1, 0, 0),
            ],
        )
        connection.commit()
        connection.close()
        return path

    def test_export_is_deterministic_and_classifies_literal_fixtures(self):
        with tempfile.TemporaryDirectory() as directory:
            database = self.create_database(directory)
            first = build_skill_catalog.build_catalog_bytes(database)
            second = build_skill_catalog.build_catalog_bytes(database)

        self.assertEqual(first, second)
        self.assertTrue(first.endswith(b"\n"))
        catalog = json.loads(first)
        self.assertEqual(1, catalog["schemaVersion"])
        self.assertEqual([1, 4], [entry["skillId"] for entry in catalog["skills"]])
        self.assertEqual("entity", catalog["skills"][0]["targetMode"])
        self.assertEqual("ground", catalog["skills"][1]["targetMode"])

    def test_duplicate_skill_id_is_rejected(self):
        row = {
            "id": 1,
            "name": "Windslash",
            "tar_type": 1,
            "helpful": 0,
            "apply_distance": 400,
            "apply_target": 4,
            "apply_type": 1,
            "radii": 0,
            "range_val": 0,
        }
        with self.assertRaises(ValueError):
            build_skill_catalog.build_catalog_from_rows([row, dict(row)])

    def test_skill_id_must_fit_uint32_wire_range(self):
        row = {
            "id": build_skill_catalog.MAX_SKILL_ID + 1,
            "name": "Overflow",
            "tar_type": 1,
            "helpful": 0,
            "apply_distance": 1,
            "apply_target": 1,
            "apply_type": 1,
            "radii": 0,
            "range_val": 0,
        }
        with self.assertRaises(ValueError):
            build_skill_catalog.build_catalog_from_rows([row])

    def test_real_database_has_current_skill_census(self):
        catalog = build_skill_catalog.build_catalog(ROOT / "databases" / "gamedata.sqlite")
        self.assertEqual(411, len(catalog["skills"]))
        self.assertEqual("entity", catalog["skills"][0]["targetMode"])
        self.assertEqual("ground", next(
            entry["targetMode"] for entry in catalog["skills"]
            if entry["skillId"] == 4))


if __name__ == "__main__":
    unittest.main()
