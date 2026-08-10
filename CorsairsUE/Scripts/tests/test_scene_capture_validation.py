import struct
from pathlib import Path
import tempfile
import unittest
import zlib


def png_chunk(kind, payload):
    body = kind + payload
    return (
        struct.pack(">I", len(payload))
        + body
        + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    )


def write_png_header(path, width, height):
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IEND", b"")
    )


class SceneCaptureValidationTests(unittest.TestCase):
    def production_validator(self):
        try:
            from CorsairsUE.Scripts.scene_capture_validation import (
                validate_scene_progress_png,
            )
        except ModuleNotFoundError as error:
            self.fail(f"нет production PNG validator: {error}")
        return validate_scene_progress_png

    def test_accepts_only_crc_valid_1920x1080_rgba_png(self):
        validator = self.production_validator()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.png"
            write_png_header(path, 1920, 1080)
            self.assertEqual((1920, 1080), validator(path, 1920, 1080))

            write_png_header(path, 1919, 1080)
            with self.assertRaisesRegex(RuntimeError, "размер"):
                validator(path, 1920, 1080)

            broken = bytearray(path.read_bytes())
            broken[29] ^= 0x01
            path.write_bytes(broken)
            with self.assertRaisesRegex(RuntimeError, "CRC"):
                validator(path, 1919, 1080)

    def test_rejects_non_png_even_when_file_is_nonempty(self):
        validator = self.production_validator()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.png"
            path.write_bytes(b"not a png but non-empty")
            with self.assertRaisesRegex(RuntimeError, "PNG"):
                validator(path, 1920, 1080)

    def test_gui_capture_bootstraps_sibling_imports(self):
        capture_path = Path(__file__).resolve().parents[1] / (
            "capture_scene_progress_city.py"
        )
        source = capture_path.read_text(encoding="utf-8")
        bootstrap = "sys.path.insert(0, str(SCRIPT_DIR))"
        sibling_import = (
            "from scene_capture_validation import validate_scene_progress_png"
        )
        self.assertIn(bootstrap, source)
        self.assertLess(source.index(bootstrap), source.index(sibling_import))


if __name__ == "__main__":
    unittest.main()
