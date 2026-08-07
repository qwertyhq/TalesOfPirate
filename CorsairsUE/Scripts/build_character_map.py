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

# Сколько частей у персонажа. Значение зафиксировано форматом: пять слотов
# внешности на модель (MPChaLoadInfo в оригинальном клиенте).
PART_NUM = 5


def main():
    if len(sys.argv) < 3:
        print("нужны аргументы: <gamedata.sqlite> <выход.json>", file=sys.stderr)
        return 2

    db_path, out_path = sys.argv[1], sys.argv[2]

    db = sqlite3.connect(db_path)
    rows = list(db.execute(
        "SELECT id, name, model, suit_id, skin_info FROM characters"))
    db.close()

    mapping = {}
    for cha_id, name, model, suit_id, skin_info in rows:
        if model is None:
            continue
        bone = f"{int(model):04d}"

        # Персонаж собирается из пяти частей, а не из одной модели. Номер
        # файла каждой — `модель * 1000000 + костюм * 10000 + номер части`;
        # формула взята из CharacterModel.cpp оригинального клиента. Часть
        # существует, если ненулевой соответствующий элемент skin_info.
        #
        # Без этого у человеческих персонажей загружалась только первая часть
        # — голова, — и в кадре висело лицо без тела.
        parts = []
        skins = [int(v) for v in str(skin_info or "").split(",") if v.strip().lstrip("-").isdigit()]
        for index in range(PART_NUM):
            if index < len(skins) and skins[index] == 0:
                continue
            file_id = int(model) * 1000000 + int(suit_id or 0) * 10000 + index
            asset = f"{file_id:010d}"
            parts.append(f"{CONTENT_ROOT}/{asset}/SkeletalMeshes/{asset}")

        if not parts:
            asset = f"{bone}{BASE_SKIN_VARIANT}"
            parts.append(f"{CONTENT_ROOT}/{asset}/SkeletalMeshes/{asset}")

        mapping[str(cha_id)] = {
            "name": name,
            # Первая часть — основная: к ней крепятся остальные, и её скелет
            # задаёт позу всей сборке.
            "mesh": parts[0],
            "parts": parts,
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
