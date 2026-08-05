"""Строит таблицу «тип персонажа -> модель тела» для клиента.

    python3 Scripts/build_character_map.py <gamedata.sqlite> <выход.json>

Пример:
    python3 Scripts/build_character_map.py ../databases/gamedata.sqlite \\
        ../CorsairsUE/Data/character_map.json

Ответ на вход содержит числовой тип персонажа. Имя файла модели выводится
через таблицу `characters`: тип — это идентификатор записи, а поле `model`
разворачивается в четырёхзначный номер. Скины при этом называются
`<модель><вариант>.lgo`, где вариант шестизначный; нулевой вариант — базовое
тело.

Таблица считается заранее и кладётся обычным JSON: внутри игры нет ни sqlite3,
ни доступа к исходным данным.
"""

import json
import os
import sqlite3
import sys

# Куда import_assets.py кладёт ассеты.
CONTENT_ROOT = "/Game/All"
ANIMATION_ROOT = "/Game/Animations"

# Нулевой вариант скина — базовое тело без снаряжения.
BASE_SKIN_VARIANT = "000000"


def main():
    if len(sys.argv) < 3:
        print("нужны аргументы: <gamedata.sqlite> <выход.json>", file=sys.stderr)
        return 2

    db_path, out_path = sys.argv[1], sys.argv[2]

    db = sqlite3.connect(db_path)
    rows = list(db.execute("SELECT id, name, model FROM characters"))
    db.close()

    mapping = {}
    for cha_id, name, model in rows:
        if model is None:
            continue
        bone = f"{int(model):04d}"
        asset_name = f"{bone}{BASE_SKIN_VARIANT}"
        mapping[str(cha_id)] = {
            "name": name,
            "mesh": f"{CONTENT_ROOT}/{asset_name}/SkeletalMeshes/{asset_name}",
            # Скелет и дорожка приходят из одного .lab, поэтому анимация
            # адресуется тем же четырёхзначным номером.
            "animation": f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}_Anim",
        }

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump({"characters": mapping}, handle,
                  ensure_ascii=False, indent=1, sort_keys=True)

    print(f"записей в characters: {len(rows)}")
    print(f"  сопоставлено: {len(mapping)}")
    for cha_id in list(sorted(mapping, key=int))[:4]:
        print(f"    тип {cha_id}: {mapping[cha_id]['name']} -> {mapping[cha_id]['mesh']}")
    print(f"таблица записана: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
