import unittest
from pathlib import Path

from CorsairsUE.Scripts.scene_lighting import resolve_reference_lighting


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
DATABASE_PATH = REPOSITORY_ROOT / "databases" / "gamedata.sqlite"


def source_record(model_id, island, tile_color, source_index):
    return {
        "sourceKey": {
            "sectionIndex": source_index,
            "slotIndex": 0,
            "byteOffset": source_index * 16,
        },
        "modelId": model_id,
        "type": 0,
        "disposition": "scene-model",
        "terrainSectionPresent": island != 0,
        "island": island,
        "tileColor565": tile_color,
        "inReferenceSet": True,
    }


def source_manifest(*records):
    return {
        "schemaVersion": 2,
        "stats": {
            "referenceObjectCount": len(records),
        },
        "records": list(records),
    }


class SceneLightingTests(unittest.TestCase):
    def test_real_golden_ids_encode_unlit_and_area_lighting(self):
        manifest = source_manifest(
            source_record(22, 1, 65535, 1),
            source_record(314, 1, 31727, 2),
            source_record(323, 1, 65535, 3),
        )

        resolved = resolve_reference_lighting(manifest, DATABASE_PATH)
        payloads = [item["payload"] for item in resolved]

        self.assertEqual(3, len(resolved))
        self.assertTrue(all(len(payload) == 33 for payload in payloads))
        self.assertTrue(all(
            isinstance(value, float)
            for payload in payloads
            for value in payload
        ))
        self.assertEqual(
            (
                0.0, 0.0, 0.0, 0.0,
                1.0, 1.0, 1.0,
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0,
                0.0,
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0,
                0.0,
                0.0, 0.0, 0.0,
            ),
            payloads[0],
        )
        self.assertEqual("legacy-unlit", resolved[0]["mode"])
        self.assertEqual((0, 0, 0), resolved[0]["diagnostics"]["flags"])
        self.assertIsNone(resolved[0]["diagnostics"]["areaId"])

        # Направление света в данных — (-1, -1, -1) после нормировки. Поворот
        # Q = (-y, x, z) переводит его в (+a, -a, -a). Прежде здесь стояло
        # (-a, +a, -a) — результат зеркала (x, -y, z), от которого сцена уже
        # отказалась; свет оставался развёрнутым на девяносто градусов
        # относительно всего остального.
        inverse_sqrt_three = 0.5773502691896258
        area_lit = (
            1.0, 1.0, 0.0, 0.0,
            114.0 / 255.0, 148.0 / 255.0, 155.0 / 255.0,
            inverse_sqrt_three, -inverse_sqrt_three, -inverse_sqrt_three,
            1.0, 1.0, 1.0,
            0.0, 0.0, 0.0,
            0.0, 0.0, 0.0,
            0.0,
            0.0, 0.0, 0.0,
            0.0, 0.0, 0.0,
            0.0, 0.0, 0.0,
            0.0,
            0.0, 0.0, 0.0,
        )
        self.assertEqual(area_lit, payloads[1])
        self.assertEqual(area_lit, payloads[2])
        self.assertEqual("legacy-area", resolved[1]["mode"])
        self.assertEqual("legacy-area", resolved[2]["mode"])
        self.assertEqual((1, 0, 0), resolved[1]["diagnostics"]["flags"])
        self.assertEqual(1, resolved[1]["diagnostics"]["areaId"])

    def test_shade_uses_legacy_bgra565_and_size_flag(self):
        manifest = source_manifest(
            source_record(268, 1, 0xF800, 1),
            source_record(148, 1, 0xF800, 2),
        )

        shade_resolution, env_shade_resolution = resolve_reference_lighting(
            manifest, DATABASE_PATH)
        shade_only = shade_resolution["payload"]
        env_and_shade = env_shade_resolution["payload"]

        self.assertEqual("legacy-shade", shade_resolution["mode"])
        self.assertEqual((1.0, 0.0, 0.0, 1.0), shade_only[:4])
        self.assertEqual((0.0, 0.0, 248.0 / 255.0), shade_only[4:7])
        self.assertEqual((0.0,) * 6, shade_only[7:13])

        inverse_sqrt_three = 0.5773502691896258
        self.assertEqual("legacy-area-shade", env_shade_resolution["mode"])
        self.assertEqual((1.0, 1.0, 0.0, 1.0), env_and_shade[:4])
        self.assertEqual((0.0, 0.0, 150.0 / 255.0), env_and_shade[4:7])
        # То же направление и тот же поворот Q, что и в проверке выше.
        self.assertEqual(
            (inverse_sqrt_three, -inverse_sqrt_three, -inverse_sqrt_three),
            env_and_shade[7:10],
        )
        self.assertEqual((1.0, 1.0, 1.0), env_and_shade[10:13])

    def test_island_zero_uses_neutral_payload_not_area_one(self):
        manifest = source_manifest(source_record(314, 0, 0, 1))

        resolution = resolve_reference_lighting(manifest, DATABASE_PATH)[0]
        payload = resolution["payload"]

        self.assertEqual("legacy-inherited-outside-capture", resolution["mode"])
        self.assertIsNone(resolution["diagnostics"]["areaId"])
        self.assertEqual((1.0, 1.0, 0.0, 0.0), payload[:4])
        self.assertEqual((1.0, 1.0, 1.0), payload[4:7])
        self.assertEqual((0.0,) * 6, payload[7:13])
        self.assertEqual((0.0,) * 20, payload[13:33])


if __name__ == "__main__":
    unittest.main()
