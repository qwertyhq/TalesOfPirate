"""Строит таблицу «номер слоя рельефа -> текстура» для материала в UE.

    python3 Scripts/build_terrain_map.py <gamedata.sqlite> <выход.json>

Пример:
    python3 Scripts/build_terrain_map.py ../databases/gamedata.sqlite \\
        ../CorsairsUE/Data/terrain_map.json

Слои рельефа хранятся номерами: три верхних упакованы в `TileInfo` по шесть
бит, базовый лежит отдельным полем. Конвертер выгружает их в
`<карта>.layers.raw` по восемь байт на клетку, а во что разворачивается номер,
знает только таблица `terrains` игровых данных.

Путь текстуры приводится к тому виду, в котором ассет лежит в Content: имя
файла без расширения и без каталога, потому что импорт кладёт текстуры плоско
по имени модели.
"""

import json
import os
import sqlite3
import sys

CONTENT_ROOT = "/Game/Terrain/Textures"


def main():
    if len(sys.argv) < 3:
        print("нужны аргументы: <gamedata.sqlite> <выход.json>", file=sys.stderr)
        return 2

    db_path, out_path = sys.argv[1], sys.argv[2]

    db = sqlite3.connect(db_path)
    rows = list(db.execute("SELECT id, name, type, attr FROM terrains"))
    db.close()

    layers = {}
    for terrain_id, name, type_id, attr in rows:
        if not name:
            continue
        stem = os.path.splitext(os.path.basename(name))[0]
        layers[str(terrain_id)] = {
            "source": name,
            "texture": f"{CONTENT_ROOT}/{stem}",
            "type": type_id,
            "attr": attr,
        }

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump({"layers": layers}, handle,
                  ensure_ascii=False, indent=1, sort_keys=True)

    print(f"записей в terrains: {len(rows)}")
    print(f"  сопоставлено:     {len(layers)}")
    for key in list(sorted(layers, key=int))[:3]:
        print(f"    слой {key}: {layers[key]['source']}")
    print(f"таблица записана: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
