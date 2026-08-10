"""Place the five NPCs visible in the Garner reference capture.

``--dry-run`` is a pure-Python, fail-closed provenance and asset preflight.
The apply path owns only actors whose labels start with ``ACTOR_PREFIX``;
it never rebuilds the city or edits imported content.
"""

import argparse
import json
import math
from pathlib import Path, PurePosixPath
import re
import sqlite3
import struct
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from scene_coordinate_basis import (  # noqa: E402
    source_location_to_ue,
    standalone_character_yaw_to_ue,
)


SCHEMA_VERSION = 1
TARGET_LEVEL = "/Game/Maps/GarnerSceneProgressCity"
ACTOR_PREFIX = "SceneProgressCity_NPC_"
CHARACTER_HEIGHT_CM = 176.0

REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_NPC_SOURCE = (
    REPO_ROOT / "server" / "GameServer" / "resource" / "garner"
    / "garnernpc.txt"
)
DEFAULT_DATABASE = REPO_ROOT / "databases" / "gamedata.sqlite"
DEFAULT_CHARACTER_MAP = REPO_ROOT / "CorsairsUE" / "Data" / "character_map.json"
DEFAULT_HEIGHT_MAP = (
    REPO_ROOT / "CorsairsUE" / "Data" / "Heights" / "garner.height.r16"
)
DEFAULT_CONTENT_ROOT = REPO_ROOT / "CorsairsUE" / "Content"

# The screenshot contains these five source records and no sixth NPC.  In
# particular, nearby source serial 141 (Cleaner / character 724) is outside
# the visible reference population and is intentionally not part of this
# bounded slice.
REFERENCE_RECORDS = {
    8: ("Physican - Ditto", 17, 225075, 277025, 180.0),
    14: ("Nurse - Gina", 29, 224487, 277032, 180.0),
    21: ("Newbie Guide - Senna", 11, 222375, 278525, 180.0),
    142: ("Event NPC - Pappa", 260, 222275, 276827, 180.0),
    152: ("Weird Hat Seller", 449, 224000, 276932, 180.0),
}

ANIMATION_POLICIES = {
    8: "loop",
    14: "loop",
    21: "loop",
    # The imported 0223 animation has incompatible far pivots and explodes
    # King Penguin.  A newly spawned component stays in its reference pose
    # when no animation override is applied.
    142: "staticReferencePose",
    152: "loop",
}
EXPECTED_ANIMATION_POLICY_CENSUS = {
    "loop": 4,
    "staticReferencePose": 1,
}


class NpcPreflightError(RuntimeError):
    pass


def require_target_level(target):
    if target != TARGET_LEVEL:
        raise NpcPreflightError(
            f"target level must be exactly {TARGET_LEVEL}, got {target!r}"
        )
    return target


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Place the exact NPC slice visible in the Garner reference"
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--target", default=TARGET_LEVEL)
    parser.add_argument("--npc-source", type=Path, default=DEFAULT_NPC_SOURCE)
    parser.add_argument("--database", type=Path, default=DEFAULT_DATABASE)
    parser.add_argument(
        "--character-map", type=Path, default=DEFAULT_CHARACTER_MAP
    )
    parser.add_argument("--height-map", type=Path, default=DEFAULT_HEIGHT_MAP)
    parser.add_argument("--content-root", type=Path, default=DEFAULT_CONTENT_ROOT)
    return parser.parse_args(argv)


def parse_position(value, source_serial, field):
    pieces = value.split(",")
    if len(pieces) != 2:
        raise NpcPreflightError(
            f"source serial {source_serial}: malformed {field}={value!r}"
        )
    try:
        return int(pieces[0]), int(pieces[1])
    except ValueError as error:
        raise NpcPreflightError(
            f"source serial {source_serial}: malformed {field}={value!r}"
        ) from error


def load_source_records(path):
    selected = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise NpcPreflightError(f"NPC source is unreadable: {path}: {error}") from error

    for line_number, line in enumerate(lines, start=1):
        if not line or line.startswith("//"):
            continue
        fields = line.split("\t")
        if len(fields) != 13:
            raise NpcPreflightError(
                f"NPC source line {line_number}: expected 13 fields, got {len(fields)}"
            )
        try:
            source_serial = int(fields[0])
        except ValueError as error:
            raise NpcPreflightError(
                f"NPC source line {line_number}: invalid serial {fields[0]!r}"
            ) from error
        if source_serial not in REFERENCE_RECORDS:
            continue
        if source_serial in selected:
            raise NpcPreflightError(f"duplicate NPC source serial {source_serial}")

        try:
            npc_type = int(fields[2])
            character_type = int(fields[3])
            direction = float(fields[7])
        except ValueError as error:
            raise NpcPreflightError(
                f"source serial {source_serial}: invalid numeric field"
            ) from error
        position = parse_position(fields[5], source_serial, "start position")
        goal = parse_position(fields[6], source_serial, "goal position")
        selected[source_serial] = {
            "sourceSerial": source_serial,
            "name": fields[1],
            "npcType": npc_type,
            "characterType": character_type,
            "sourcePosition": position,
            "goalPosition": goal,
            "sourceDirectionDegrees": direction,
            "area": fields[8],
            "regionId": fields[9],
        }

    missing = sorted(set(REFERENCE_RECORDS) - set(selected))
    if missing:
        raise NpcPreflightError(f"NPC source is missing serials {missing}")

    for source_serial, expected in REFERENCE_RECORDS.items():
        record = selected[source_serial]
        actual = (
            record["name"],
            record["characterType"],
            record["sourcePosition"][0],
            record["sourcePosition"][1],
            record["sourceDirectionDegrees"],
        )
        if actual != expected:
            raise NpcPreflightError(
                f"source serial {source_serial}: expected {expected}, got {actual}"
            )
        if record["npcType"] != 1 or record["goalPosition"] != record["sourcePosition"]:
            raise NpcPreflightError(
                f"source serial {source_serial}: expected fixed NPC type 1"
            )
        if record["area"] != "Argent City" or record["regionId"] != "1":
            raise NpcPreflightError(
                f"source serial {source_serial}: not in Argent City region 1"
            )
    return selected


def load_catalog(path):
    try:
        catalog = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise NpcPreflightError(
            f"character catalog is unreadable: {path}: {error}"
        ) from error
    if not isinstance(catalog, dict) or not isinstance(catalog.get("characters"), dict):
        raise NpcPreflightError(f"character catalog has no characters object: {path}")
    return catalog


def load_database_rows(path, records):
    try:
        database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
        database.row_factory = sqlite3.Row
    except sqlite3.Error as error:
        raise NpcPreflightError(f"database cannot be opened: {path}: {error}") from error

    result = {}
    try:
        for source_serial, source in records.items():
            db_npc_id = source_serial + 1
            npc = database.execute(
                "SELECT id, name, area, x_pos, y_pos, map_name "
                "FROM npc_list WHERE id = ?",
                (db_npc_id,),
            ).fetchone()
            if npc is None:
                raise NpcPreflightError(f"npc_list.{db_npc_id} is missing")
            expected_npc = (
                source["name"],
                "Argent City",
                source["sourcePosition"][0] // 100,
                source["sourcePosition"][1] // 100,
                "Ascaron",
            )
            actual_npc = (
                npc["name"],
                npc["area"],
                npc["x_pos"],
                npc["y_pos"],
                npc["map_name"],
            )
            if actual_npc != expected_npc:
                raise NpcPreflightError(
                    f"npc_list.{db_npc_id}: expected {expected_npc}, got {actual_npc}"
                )

            character_type = source["characterType"]
            character = database.execute(
                "SELECT id, name, modal_type, model, suit_id "
                "FROM characters WHERE id = ?",
                (character_type,),
            ).fetchone()
            if character is None:
                raise NpcPreflightError(f"characters.{character_type} is missing")
            if int(character["modal_type"]) == 1:
                raise NpcPreflightError(
                    f"characters.{character_type} is modular, expected single mesh"
                )
            result[source_serial] = {
                "dbNpcId": db_npc_id,
                "characterName": str(character["name"]),
                "modalType": int(character["modal_type"]),
                "modelId": int(character["model"]),
                "suitId": int(character["suit_id"] or 0),
            }
    except sqlite3.Error as error:
        raise NpcPreflightError(f"database query failed: {path}: {error}") from error
    finally:
        database.close()
    return result


def height_grid_width(path):
    try:
        size = path.stat().st_size
    except OSError as error:
        raise NpcPreflightError(f"height map is unreadable: {path}: {error}") from error
    if size == 0 or size % 2:
        raise NpcPreflightError(f"height map has invalid byte size {size}: {path}")
    sample_count = size // 2
    width = math.isqrt(sample_count)
    if width * width != sample_count:
        raise NpcPreflightError(
            f"height map is not a square r16 grid: samples={sample_count}"
        )
    return width


def sample_height_cm(handle, width, x_cm, y_cm):
    tile_x = x_cm // 100
    tile_y = y_cm // 100
    if not (0 <= tile_x < width and 0 <= tile_y < width):
        raise NpcPreflightError(
            f"height sample outside {width}x{width}: ({tile_x},{tile_y})"
        )
    handle.seek((tile_y * width + tile_x) * 2)
    payload = handle.read(2)
    if len(payload) != 2:
        raise NpcPreflightError(f"height sample is truncated at ({tile_x},{tile_y})")
    encoded = struct.unpack("<H", payload)[0]
    if encoded % 256:
        raise NpcPreflightError(
            f"height sample is not canonical r16 at ({tile_x},{tile_y}): {encoded}"
        )
    return float((encoded // 256 - 128) * 10)


def asset_file(content_root, asset_path):
    if not asset_path.startswith("/Game/"):
        raise NpcPreflightError(f"asset path is outside /Game: {asset_path}")
    relative = PurePosixPath(asset_path.removeprefix("/Game/"))
    return content_root.joinpath(*relative.parts).with_suffix(".uasset")


def slug(value):
    return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9]+", "_", value)).strip("_")


def validate_animation_policy_census(actors):
    seen = {}
    for descriptor in actors:
        source_serial = descriptor.get("sourceSerial")
        policy = descriptor.get("animationPolicy")
        expected = ANIMATION_POLICIES.get(source_serial)
        if expected is None or policy != expected:
            raise NpcPreflightError(
                f"source serial {source_serial}: animation policy "
                f"expected {expected!r}, got {policy!r}"
            )
        if source_serial in seen:
            raise NpcPreflightError(
                f"duplicate animation policy for source serial {source_serial}"
            )
        seen[source_serial] = policy

    if set(seen) != set(ANIMATION_POLICIES):
        raise NpcPreflightError(
            "animation policy serials mismatch: "
            f"expected={sorted(ANIMATION_POLICIES)}, actual={sorted(seen)}"
        )

    census = {policy: 0 for policy in EXPECTED_ANIMATION_POLICY_CENSUS}
    for policy in seen.values():
        census[policy] += 1
    if census != EXPECTED_ANIMATION_POLICY_CENSUS:
        raise NpcPreflightError(
            "animation policy census mismatch: "
            f"expected={EXPECTED_ANIMATION_POLICY_CENSUS}, actual={census}"
        )
    return census


def apply_animation_policy(component, descriptor, animation):
    source_serial = descriptor.get("sourceSerial")
    policy = descriptor.get("animationPolicy")
    expected = ANIMATION_POLICIES.get(source_serial)
    if policy != expected:
        raise NpcPreflightError(
            f"source serial {source_serial}: animation policy "
            f"expected {expected!r}, got {policy!r}"
        )
    if policy == "staticReferencePose":
        return
    if policy != "loop" or animation is None:
        raise NpcPreflightError(
            f"source serial {source_serial}: loop animation is missing"
        )
    component.override_animation_data(animation, True, True, 0.0, 1.0)


def build_plan(args):
    target = require_target_level(args.target)
    records = load_source_records(args.npc_source.resolve())
    database_rows = load_database_rows(args.database.resolve(), records)
    catalog = load_catalog(args.character_map.resolve())
    width = height_grid_width(args.height_map.resolve())
    actors = []

    try:
        height_handle = args.height_map.resolve().open("rb")
    except OSError as error:
        raise NpcPreflightError(
            f"height map is unreadable: {args.height_map}: {error}"
        ) from error
    with height_handle:
        for source_serial in REFERENCE_RECORDS:
            source = records[source_serial]
            database = database_rows[source_serial]
            character_type = source["characterType"]
            entry = catalog["characters"].get(str(character_type))
            if not isinstance(entry, dict):
                raise NpcPreflightError(
                    f"character catalog is missing characters.{character_type}"
                )

            model_id = database["modelId"]
            asset_stem = model_id * 1_000_000 + database["suitId"] * 10_000
            expected_mesh = (
                f"/Game/All/{asset_stem:010d}/SkeletalMeshes/{asset_stem:010d}"
            )
            bone = f"{model_id:04d}"
            expected_animation = (
                f"/Game/Animations/{bone}/SkeletalMeshes/{bone}_Anim"
            )
            expected_catalog = (
                database["modalType"],
                model_id,
                expected_mesh,
                expected_animation,
            )
            actual_catalog = (
                entry.get("modalType"),
                entry.get("modelId"),
                entry.get("staticMesh"),
                entry.get("animation"),
            )
            if actual_catalog != expected_catalog:
                raise NpcPreflightError(
                    f"characters.{character_type}: expected {expected_catalog}, "
                    f"got {actual_catalog}"
                )

            mesh_file = asset_file(args.content_root.resolve(), expected_mesh)
            animation_file = asset_file(
                args.content_root.resolve(), expected_animation
            )
            mesh_imported = mesh_file.is_file()
            animation_imported = animation_file.is_file()
            if not mesh_imported or not animation_imported:
                missing = [
                    str(path)
                    for present, path in (
                        (mesh_imported, mesh_file),
                        (animation_imported, animation_file),
                    )
                    if not present
                ]
                raise NpcPreflightError(
                    f"character {character_type}: imported assets missing {missing}"
                )

            source_x, source_y = source["sourcePosition"]
            ground_z = sample_height_cm(height_handle, width, source_x, source_y)
            direction = source["sourceDirectionDegrees"]
            location = source_location_to_ue(source_x, source_y, ground_z)
            actors.append(
                {
                    "animation": expected_animation,
                    "animationImported": animation_imported,
                    "animationPolicy": ANIMATION_POLICIES[source_serial],
                    "characterName": database["characterName"],
                    "characterType": character_type,
                    "dbNpcId": database["dbNpcId"],
                    "label": (
                        f"{ACTOR_PREFIX}{source_serial:03d}_{slug(source['name'])}"
                    ),
                    "locationCm": list(location),
                    "mesh": expected_mesh,
                    "meshImported": mesh_imported,
                    "modelId": model_id,
                    "name": source["name"],
                    "sourceDirectionDegrees": direction,
                    "sourcePosition": [source_x, source_y],
                    "sourceSerial": source_serial,
                    # ACorsairsCharacter applies -90 degrees to its mesh
                    # component.  A standalone SkeletalMeshActor folds that
                    # same offset into the actor rotation.
                    "yawDegrees": standalone_character_yaw_to_ue(direction),
                }
            )

    animation_policy_census = validate_animation_policy_census(actors)
    return {
        "actorPrefix": ACTOR_PREFIX,
        "actors": actors,
        "animationPolicyCensus": animation_policy_census,
        "applyReady": True,
        "count": len(actors),
        "schemaVersion": SCHEMA_VERSION,
        "status": "PASS",
        "target": target,
    }


def apply_plan(plan):
    require_target_level(plan.get("target"))
    animation_policy_census = validate_animation_policy_census(plan["actors"])
    if plan.get("animationPolicyCensus") != animation_policy_census:
        raise NpcPreflightError(
            "reported animation policy census mismatch: "
            f"expected={animation_policy_census}, "
            f"actual={plan.get('animationPolicyCensus')!r}"
        )

    try:
        import unreal
    except ImportError as error:
        raise NpcPreflightError(
            "apply requires the Unreal Python environment; use --dry-run outside UE"
        ) from error

    prepared = []
    for descriptor in plan["actors"]:
        mesh = unreal.load_asset(descriptor["mesh"])
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise NpcPreflightError(f"not a SkeletalMesh: {descriptor['mesh']}")
        animation = None
        if descriptor["animationPolicy"] == "loop":
            animation = unreal.load_asset(descriptor["animation"])
            if not isinstance(animation, unreal.AnimSequence):
                raise NpcPreflightError(
                    f"not an AnimSequence: {descriptor['animation']}"
                )
        bounds = mesh.get_bounds()
        height = 2.0 * float(bounds.box_extent.z)
        if not math.isfinite(height) or height <= 0.0:
            raise NpcPreflightError(
                f"invalid bounds height for {descriptor['mesh']}: {height}"
            )
        prepared.append(
            (descriptor, mesh, animation, CHARACTER_HEIGHT_CM / max(height, 1.0))
        )

    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level(plan["target"]):
        raise NpcPreflightError(f"target level did not load: {plan['target']}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in list(actors.get_all_level_actors()):
        if actor.get_actor_label().startswith(ACTOR_PREFIX):
            if not actors.destroy_actor(actor):
                raise NpcPreflightError(
                    f"owned actor did not delete: {actor.get_actor_label()}"
                )

    for descriptor, mesh, animation, scale in prepared:
        location = descriptor["locationCm"]
        actor = actors.spawn_actor_from_class(
            unreal.SkeletalMeshActor,
            unreal.Vector(*location),
            unreal.Rotator(
                roll=0.0, pitch=0.0, yaw=descriptor["yawDegrees"]
            ),
        )
        if actor is None:
            raise NpcPreflightError(f"actor did not spawn: {descriptor['label']}")
        component = actor.skeletal_mesh_component
        component.set_skeletal_mesh(mesh)
        actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))
        component.set_editor_property(
            "visibility_based_anim_tick_option",
            unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES,
        )
        apply_animation_policy(component, descriptor, animation)
        actor.set_actor_label(descriptor["label"], mark_dirty=True)

    expected_labels = {actor["label"] for actor in plan["actors"]}
    actual_labels = {
        actor.get_actor_label()
        for actor in actors.get_all_level_actors()
        if actor.get_actor_label().startswith(ACTOR_PREFIX)
    }
    if actual_labels != expected_labels:
        raise NpcPreflightError(
            f"owned actor labels mismatch: expected={sorted(expected_labels)}, "
            f"actual={sorted(actual_labels)}"
        )
    if not levels.save_current_level():
        raise NpcPreflightError(f"target level did not save: {plan['target']}")


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        plan = build_plan(args)
        if not args.dry_run:
            apply_plan(plan)
    except NpcPreflightError as error:
        print(f"NPC PREFLIGHT FAIL: {error}", file=sys.stderr)
        return 2
    print(json.dumps(plan, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
