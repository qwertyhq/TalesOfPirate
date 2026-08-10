"""Чистая проверка native PNG прогресс-кадра без зависимости от Unreal."""

from pathlib import Path
import struct
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
IHDR_LENGTH = 13
RGBA_COLOR_TYPE = 6


def validate_scene_progress_png(path, expected_width, expected_height):
    path = Path(path)
    try:
        header = path.read_bytes()[:33]
    except OSError as error:
        raise RuntimeError(f"PNG не читается: {path}: {error}") from error

    if len(header) != 33 or header[:8] != PNG_SIGNATURE:
        raise RuntimeError(f"неверная PNG signature/IHDR: {path}")
    length = struct.unpack(">I", header[8:12])[0]
    kind = header[12:16]
    payload = header[16:29]
    recorded_crc = struct.unpack(">I", header[29:33])[0]
    actual_crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
    if length != IHDR_LENGTH or kind != b"IHDR":
        raise RuntimeError(f"неверный PNG IHDR: {path}")
    if actual_crc != recorded_crc:
        raise RuntimeError(
            f"неверный PNG IHDR CRC: {recorded_crc:#x}/{actual_crc:#x}")

    width, height, bit_depth, color_type, compression, filter_kind, interlace = (
        struct.unpack(">IIBBBBB", payload)
    )
    if (width != expected_width or height != expected_height):
        raise RuntimeError(
            f"неверный размер PNG: {width}x{height}, "
            f"ожидался {expected_width}x{expected_height}")
    if (bit_depth != 8 or color_type != RGBA_COLOR_TYPE
            or compression != 0 or filter_kind != 0 or interlace not in (0, 1)):
        raise RuntimeError(
            "неверный PNG формат: "
            f"bitDepth={bit_depth} colorType={color_type} "
            f"compression={compression} filter={filter_kind} "
            f"interlace={interlace}")
    return width, height
