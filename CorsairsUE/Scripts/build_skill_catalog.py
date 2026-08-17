"""Экспорт детерминированного каталога навыков из SQLite."""

import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path


QUERY = (
    "SELECT id, name, apply_distance, apply_target, apply_type, helpful, "
    "tar_type, radii, range_val FROM skills ORDER BY id"
)
ROW_FIELDS = (
    "id",
    "name",
    "apply_distance",
    "apply_target",
    "apply_type",
    "helpful",
    "tar_type",
    "radii",
    "range_val",
)
MAX_SKILL_ID = 0xFFFFFFFF


def target_mode(apply_type):
    if apply_type in (1, 3):
        return "entity"
    if apply_type == 2:
        return "ground"
    return "unsupported"


def build_catalog_from_rows(rows):
    skills = []
    skill_ids = set()
    for row in rows:
        values = {field: row[field] for field in ROW_FIELDS}
        skill_id = values["id"]
        if isinstance(skill_id, bool) or not isinstance(skill_id, int):
            raise ValueError("skill id must be an integer")
        if skill_id <= 0 or skill_id > MAX_SKILL_ID:
            raise ValueError("skill id must fit the uint32 wire range")
        if skill_id in skill_ids:
            raise ValueError(f"duplicate skill id: {skill_id}")
        skill_ids.add(skill_id)
        if not isinstance(values["name"], str) or not values["name"]:
            raise ValueError(f"skills.{skill_id}.name must be a non-empty string")
        integer_fields = ROW_FIELDS[2:]
        if any(
            isinstance(values[field], bool) or not isinstance(values[field], int)
            for field in integer_fields
        ):
            raise ValueError(f"skills.{skill_id} contains a non-integer value")
        if any(values[field] < 0 for field in integer_fields):
            raise ValueError(f"skills.{skill_id} contains a negative value")
        if values["helpful"] not in (0, 1):
            raise ValueError(f"skills.{skill_id}.helpful must be 0 or 1")
        skills.append(
            {
                "skillId": skill_id,
                "name": values["name"],
                "applyDistance": values["apply_distance"],
                "applyTarget": values["apply_target"],
                "applyType": values["apply_type"],
                "helpful": bool(values["helpful"]),
                "habitatMask": values["tar_type"],
                "radius": values["radii"],
                "shape": values["range_val"],
                "targetMode": target_mode(values["apply_type"]),
            }
        )
    skills.sort(key=lambda skill: skill["skillId"])
    return {"schemaVersion": 1, "skills": skills}


def build_catalog(database_path):
    path = Path(database_path).resolve()
    connection = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    try:
        rows = connection.execute(QUERY).fetchall()
    finally:
        connection.close()
    return build_catalog_from_rows(rows)


def build_catalog_bytes(database_path):
    catalog = build_catalog(database_path)
    text = json.dumps(
        catalog,
        ensure_ascii=False,
        indent=2,
        separators=(",", ": "),
    )
    return (text + "\n").encode("utf-8")


def publish_catalog(database_path, output_path):
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    data = build_catalog_bytes(database_path)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", dir=output.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, output)
    finally:
        if os.path.exists(temporary_name):
            os.unlink(temporary_name)


def main():
    if len(sys.argv) != 3:
        print("нужны аргументы: <skills.sqlite> <выход.json>", file=sys.stderr)
        return 2
    publish_catalog(sys.argv[1], sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
