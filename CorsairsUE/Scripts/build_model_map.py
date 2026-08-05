"""Строит таблицу `modelId` -> ассеты статических мешей в Content.

    python3 Scripts/build_model_map.py <gamedata.sqlite> <каталог-glTF> <выход.json>

Пример:
    python3 Scripts/build_model_map.py ../databases/gamedata.sqlite \\
        ../artifacts/models Scripts/model_map.json

Зачем отдельный шаг. Манифест карты хранит только числовой `modelId`, а имя
файла модели живёт в таблице `scene_objects` игровых данных. Расстановка идёт
внутри редактора UE, где нет ни sqlite3, ни доступа к исходным данным, поэтому
связь вычисляется заранее и кладётся в обычный JSON.

Одному `modelId` может соответствовать несколько мешей: `.lmo` — это дерево, и
конвертер разворачивает его в отдельные glTF с суффиксом `_0`, `_1`, ... Все
части ставятся в одну точку, каждая со своим смещением внутри модели — оно
записано в трансформ узла её glTF и переносится в меш при импорте.

Скрипт запускается обычным Python, а не редактором: он ничего не знает об UE,
кроме соглашения об именовании путей в Content.
"""

import json
import os
import re
import sqlite3
import sys
from collections import defaultdict

# Куда import_assets.py кладёт ассеты и как называет папку с мешами.
CONTENT_ROOT = "/Game/All"
STATIC_MESH_DIR = "StaticMeshes"


def collect_parts(gltf_root):
    """Группирует glTF по базовому имени модели: by-bd001_0 -> by-bd001."""
    parts = defaultdict(list)
    for root, _dirs, files in os.walk(gltf_root):
        for name in files:
            if not name.lower().endswith(".gltf"):
                continue
            stem = os.path.splitext(name)[0]
            base = re.sub(r"_\d+$", "", stem).lower()
            parts[base].append(stem)
    for base in parts:
        parts[base].sort()
    return parts


def asset_path(stem):
    """Путь ассета в Content по имени файла glTF."""
    return f"{CONTENT_ROOT}/{stem}/{STATIC_MESH_DIR}/{stem}"


def main():
    if len(sys.argv) < 4:
        print("нужны аргументы: <gamedata.sqlite> <каталог-glTF> <выход.json>",
              file=sys.stderr)
        return 2

    db_path, gltf_root, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    db = sqlite3.connect(db_path)
    rows = list(db.execute(
        "SELECT id, data_name FROM scene_objects "
        "WHERE data_name IS NOT NULL AND data_name <> ''"))
    db.close()

    parts = collect_parts(gltf_root)

    mapping = {}
    missing_model = []
    for model_id, data_name in rows:
        base = os.path.splitext(data_name)[0].lower()
        stems = parts.get(base)
        if not stems:
            missing_model.append((model_id, data_name))
            continue
        mapping[str(model_id)] = [asset_path(s) for s in stems]

    result = {
        "contentRoot": CONTENT_ROOT,
        "models": mapping,
    }
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=1, sort_keys=True)

    multi = sum(1 for v in mapping.values() if len(v) > 1)
    print(f"записей в scene_objects: {len(rows)}")
    print(f"  сопоставлено:          {len(mapping)}")
    print(f"  из них многочастных:   {multi}")
    print(f"  модель не найдена:     {len(missing_model)}")
    for model_id, name in missing_model[:5]:
        print(f"    id={model_id} {name}")
    print(f"таблица записана: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
