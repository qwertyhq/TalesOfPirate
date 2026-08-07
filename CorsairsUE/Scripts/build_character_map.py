"""Generate the authoritative character and item appearance catalog."""

import json
import os
import sqlite3
import sys

CONTENT_ROOT = "/Game/All"
ANIMATION_ROOT = "/Game/Animations"
PLAYER_MODAL_TYPE = 1
VISIBLE_PART_COUNT = 5
MODULE_COLUMNS = ("module_1", "module_2", "module_3", "module_4")


def mesh_path(module):
    if module is None:
        return None
    value = str(module)
    if not value or value == "0":
        return None
    return f"{CONTENT_ROOT}/{value}/SkeletalMeshes/{value}"


def parse_item_ids(value):
    fields = [] if value is None else str(value).split(",")
    result = [int(field or 0) for field in fields[:VISIBLE_PART_COUNT]]
    return result + [0] * (VISIBLE_PART_COUNT - len(result))


def static_mesh_path(model, suit_id):
    asset = f"{int(model) * 1_000_000 + int(suit_id or 0) * 10_000:010d}"
    return mesh_path(asset)


def build_catalog(db_path):
    db = sqlite3.connect(db_path)
    db.row_factory = sqlite3.Row
    try:
        character_rows = db.execute(
            "SELECT id, name, modal_type, model, suit_id, skin_info "
            "FROM characters ORDER BY id").fetchall()
        item_rows = db.execute(
            "SELECT id, module_1, module_2, module_3, module_4 "
            "FROM items ORDER BY id").fetchall()
    finally:
        db.close()

    items = {}
    for row in item_rows:
        meshes = {}
        for index, column in enumerate(MODULE_COLUMNS, start=1):
            resolved = mesh_path(row[column])
            if resolved is not None:
                meshes[str(index)] = resolved
        if meshes:
            items[str(row["id"])] = {"meshesByModule": meshes}

    characters = {}
    for row in character_rows:
        model = int(row["model"])
        bone = f"{model:04d}"
        characters[str(row["id"])] = {
            "animation": (
                f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}_Anim"),
            "defaultItemIds": parse_item_ids(row["skin_info"]),
            "driverMesh": (
                f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}"),
            "modalType": int(row["modal_type"]),
            "modelId": model,
            "moduleIndex": model + 1 if 0 <= model < 4 else 0,
            "name": row["name"],
            "staticMesh": static_mesh_path(model, row["suit_id"]),
        }

    return {"characters": characters, "items": items}


def main():
    if len(sys.argv) < 3:
        print("нужны аргументы: <gamedata.sqlite> <выход.json>", file=sys.stderr)
        return 2

    db_path, out_path = sys.argv[1], sys.argv[2]
    catalog = build_catalog(db_path)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(catalog, handle, ensure_ascii=False, indent=1, sort_keys=True)

    print(f"записей в characters: {len(catalog['characters'])}")
    print(f"записей в items: {len(catalog['items'])}")
    print(f"таблица записана: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
