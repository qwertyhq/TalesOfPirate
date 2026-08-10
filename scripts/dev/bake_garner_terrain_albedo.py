"""Печёт альбедо-текстуры рельефа garner по страницам из исходных данных.

Повторяет правила бейки reference-страницы из конвертера
(`tools/AssetConverter/src/TerrainPageBaker.cpp`):
- слои клетки из `garner.layers.raw`: base + до трёх верхних
  (ид тестуры, 4-битный индекс alpha-маски);
- текстуры тайлятся окном 4×4 клетки: u = ((cell % 4) + local) / 4, wrap;
- alpha верхнего слоя берётся из атласа `Client/texture/terrain/alpha/total.png`:
  4×4-подштакета по индексу mask-1, отступы 0.01/0.23, отражение адресации;
- композит: RGB = base*(1-a) + upper*a (по альфе атласа), сверху вниз.

    python3 scripts/dev/bake_garner_terrain_albedo.py <выходной каталог>

Результат: garner_page_XX_YY.png 2048×2048 RGBA (512 клеток × 4 пикселя).
"""

import json
import os
import sqlite3
import sys

import numpy as np
from PIL import Image

CELLS_PER_PAGE = 512
PX_PER_CELL = 4
GRID = 4096
PAGES = GRID // CELLS_PER_PAGE
REPO = os.path.dirname(os.path.abspath(os.path.dirname(os.path.dirname(__file__))))

LAYERS_RAW = os.path.join(REPO, "tmp-terrain-garner", "garner.layers.raw")
# layers.raw лежит в выходе каталожной конвертации рельефа
LAYERS_CANDIDATES = [
    os.path.join(REPO, "artifacts", "terrain", "garner.layers.raw"),
    os.path.join(REPO, "artifacts", "terrain", "garner", "garner.layers.raw"),
]
ATLAS_PATH = os.path.join(REPO, "Client", "texture", "terrain", "alpha", "total.png")
DB_PATH = os.path.join(REPO, "databases", "gamedata.sqlite")


def resolve_layers_path():
    for candidate in LAYERS_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    raise SystemExit(f"нет garner.layers.raw ни в одном из: {LAYERS_CANDIDATES}")


def load_textures():
    """Ид слоя → RGBA-массив текстуры. Источник — BMP из Client/texture/terrain."""
    db = sqlite3.connect(DB_PATH)
    rows = list(db.execute("SELECT id, name FROM terrains WHERE name IS NOT NULL AND name <> ''"))
    db.close()
    textures = {}
    for terrain_id, name in rows:
        stem = os.path.splitext(os.path.basename(name))[0]
        for ext in (".bmp", ".png", ".tga"):
            path = os.path.join(REPO, "Client", "texture", "terrain", stem + ext)
            if os.path.isfile(path):
                arr = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
                textures[int(terrain_id)] = arr
                break
        else:
            print(f"ПРОПУСК текстуры слоя {terrain_id}: {name}")
    return textures


def sample_wrap(texture, u, v):
    """Ближайший сосед с wrap; u/v в [0,1)."""
    h, w = texture.shape[:2]
    idx_u = np.minimum((u * w).astype(np.int64), w - 1)
    idx_v = np.minimum((v * h).astype(np.int64), h - 1)
    return texture[idx_v, idx_u]


def atlas_alpha(atlas, mask, local_u, local_v):
    """Альфа верхнего слоя из подштакеты атласа, с mirror-адресацией."""
    if mask <= 0:
        return None
    index = mask - 1
    col = (index % 4) / 4.0
    row = (index // 4) / 4.0
    au = col + 0.01 + local_u * 0.23
    av = row + 0.01 + local_v * 0.23
    h, w = atlas.shape[:2]
    # mirror: отражаем периодом 2
    fu = (au * w).astype(np.int64)
    fv = (av * h).astype(np.int64)
    fu = np.where((fu // w) % 2 == 1, w - 1 - (fu % w), fu % w)
    fv = np.where((fv // h) % 2 == 1, h - 1 - (fv % h), fv % h)
    return atlas[fv, fu]


def bake_page(layers, textures, atlas, page_x, page_y, out_dir):
    page_cells = CELLS_PER_PAGE
    px = page_cells * PX_PER_CELL
    x0 = page_x * page_cells
    y0 = page_y * page_cells

    cell_lx = np.arange(px) // PX_PER_CELL
    local_u = ((np.arange(px) % PX_PER_CELL) + 0.5) / PX_PER_CELL
    # текстурные координаты: период 4 клетки
    tex_u = ((cell_lx % 4) + local_u) / 4.0

    page = layers[y0:y0 + page_cells, x0:x0 + page_cells]  # (512,512,8) bytes
    # локальный слой-стек на пиксел: base + до 3 верхних
    base_tex = page[:, :, 0]
    up_tex = [page[:, :, 2], page[:, :, 4], page[:, :, 6]]
    up_mask = [page[:, :, 3], page[:, :, 5], page[:, :, 7]]

    empty = base_tex == 0
    out = np.zeros((px, px, 4), dtype=np.uint8)

    active_base = ~empty
    if np.any(active_base):
        # для каждого слоя с ненулевым base собираем свою зону
        for tex_id, texture in textures.items():
            zone = (base_tex == tex_id)
            if not np.any(zone):
                continue
            zone_px = np.repeat(np.repeat(zone, PX_PER_CELL, axis=0),
                                PX_PER_CELL, axis=1)
            sampled = sample_wrap(texture, tex_u[None, :], tex_u[:, None])
            out[zone_px, :3] = np.clip(sampled[zone_px], 0, 255).astype(np.uint8)

        out[:, :, 3] = 255

        for layer_tex_arr, layer_mask_arr in zip(up_tex, up_mask):
            active = (layer_tex_arr > 0) & (layer_mask_arr > 0) & active_base
            if not np.any(active):
                continue
            for tex_id, texture in textures.items():
                zone = active & (layer_tex_arr == tex_id)
                if not np.any(zone):
                    continue
                zone_px = np.repeat(np.repeat(zone, PX_PER_CELL, axis=0),
                                    PX_PER_CELL, axis=1)
                mask_zone = np.repeat(np.repeat(layer_mask_arr, PX_PER_CELL, axis=0),
                                      PX_PER_CELL, axis=1)
                vals = np.unique(layer_mask_arr[zone])
                for mask_value in vals:
                    sub = zone_px & (mask_zone == mask_value)
                    if not np.any(sub):
                        continue
                    alpha = atlas_alpha(atlas, int(mask_value),
                                        local_u[None, :], local_u[:, None])
                    if alpha is None:
                        continue
                    a = (alpha[sub].astype(np.float32) / 255.0)[:, None]
                    upper = sample_wrap(texture, tex_u[None, :], tex_u[:, None])[sub]
                    base_rgb = out[sub, :3].astype(np.float32)
                    out[sub, :3] = np.clip(
                        upper * a + base_rgb * (1.0 - a), 0, 255).astype(np.uint8)

    path = os.path.join(out_dir, f"garner_page_{page_x:02d}_{page_y:02d}.png")
    Image.fromarray(out, "RGBA").save(path)
    return path


_SHARED = None


def _worker_init():
    layers_path = resolve_layers_path()
    raw = np.fromfile(layers_path, dtype=np.uint8)
    layers = raw.reshape(GRID, GRID, 8)
    textures = load_textures()
    atlas = np.asarray(
        Image.open(ATLAS_PATH).convert("RGBA"), dtype=np.uint8)[:, :, 3]
    global _SHARED
    _SHARED = (layers, textures, atlas)


def _worker_page(task):
    page_x, page_y, out_dir = task
    layers, textures, atlas = _SHARED
    return bake_page(layers, textures, atlas, page_x, page_y, out_dir)


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    raw_probe = np.fromfile(resolve_layers_path(), dtype=np.uint8)
    if raw_probe.size != GRID * GRID * 8:
        raise SystemExit(
            f"layers.raw: {raw_probe.size} байт, ожидалось {GRID * GRID * 8}")
    del raw_probe

    workers = max(2, (os.cpu_count() or 4) - 3)
    tasks = [(px_i, py, out_dir) for py in range(PAGES) for px_i in range(PAGES)]
    import multiprocessing
    with multiprocessing.get_context("spawn").Pool(
            workers, initializer=_worker_init) as pool:
        for index, path in enumerate(pool.imap_unordered(_worker_page, tasks)):
            print(f"page {index + 1}/{len(tasks)}: {path}", flush=True)


if __name__ == "__main__":
    main()
