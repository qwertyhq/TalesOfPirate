"""Create the deterministic marker-only Garner world for Task 8."""

from __future__ import annotations

import json
from pathlib import Path
import sys

import unreal

import reference_terrain_rules as rules


def _failure(
    manifest_evidence, transaction_id, source_head, detail, field="/",
):
    return {
        "schemaVersion": 1,
        "reportType": "garner-reference-terrain-level-build",
        "status": "FAIL",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "manifest": manifest_evidence,
        "mapPackage": rules.REFERENCE_MAP_PACKAGE,
        "worldObject": rules.REFERENCE_WORLD_OBJECT,
        "mapPackageHash": None,
        "markerObject": rules.REFERENCE_BUILD_MARKER,
        "gameModeClass": rules.REFERENCE_GAME_MODE,
        "replacedExistingMap": False,
        "issues": [{"code": "LEVEL_BUILD_FAILED", "field": field,
                    "detail": str(detail)}],
    }


def _load_manifest(path: Path, repo_root: Path):
    data, _ = rules.strict_json_load(path)
    issues = rules.validate_manifest(data, path)
    if issues:
        raise RuntimeError(json.dumps(issues[0], ensure_ascii=False, sort_keys=True))
    return data, rules.file_evidence(path, repo_root)


def _class_path(value) -> str:
    if value is None:
        return ""
    getter = getattr(value, "get_path_name", None)
    return str(getter()) if getter is not None else str(value)


def build_level(
    manifest_path: Path,
    map_package: str,
    report_path: Path,
    transaction_id: str,
    source_head: str,
    repo_root: Path,
):
    target_issues = rules.validate_editor_identity_target(
        report_path, transaction_id, source_head, repo_root)
    if target_issues:
        raise RuntimeError(json.dumps(target_issues[0], sort_keys=True))
    if map_package != rules.REFERENCE_MAP_PACKAGE:
        raise RuntimeError("only /Game/Maps/Garner is an allowed build target")
    _, manifest_evidence = _load_manifest(manifest_path, repo_root)
    replaced = unreal.EditorAssetLibrary.does_asset_exist(map_package)
    if replaced and not unreal.EditorAssetLibrary.delete_asset(map_package):
        raise RuntimeError("existing generated Garner map could not be deleted")

    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_subsystem.new_level(map_package):
        raise RuntimeError("LevelEditorSubsystem.new_level failed")
    world = unreal.EditorLevelLibrary.get_editor_world()
    if world is None or str(world.get_path_name()) != rules.REFERENCE_WORLD_OBJECT:
        raise RuntimeError("new level has a noncanonical world object")
    game_mode = unreal.load_class(None, rules.REFERENCE_GAME_MODE)
    if game_mode is None or _class_path(game_mode) != rules.REFERENCE_GAME_MODE:
        raise RuntimeError("exact CorsairsGameMode class did not load")
    settings = world.get_world_settings()
    settings.set_editor_property("default_game_mode", game_mode)

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    marker = actor_subsystem.spawn_actor_from_class(
        unreal.Actor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if marker is None:
        raise RuntimeError("reference build marker did not spawn")
    if not marker.rename(
            "ReferenceTerrainBuildRoot", level_subsystem.get_current_level()):
        raise RuntimeError("reference build marker rename failed")
    marker.set_actor_label("ReferenceTerrainBuildRoot", mark_dirty=True)
    marker.set_editor_property("tags", [unreal.Name(rules.REFERENCE_BUILD_TAG)])
    if marker.get_path_name() != rules.REFERENCE_BUILD_MARKER:
        raise RuntimeError(
            f"marker object path differs: {marker.get_path_name()}")
    if not level_subsystem.save_current_level():
        raise RuntimeError("Garner level save failed")
    if not level_subsystem.load_level(map_package):
        raise RuntimeError("Garner level reload failed")

    world = unreal.EditorLevelLibrary.get_editor_world()
    if world is None or str(world.get_path_name()) != rules.REFERENCE_WORLD_OBJECT:
        raise RuntimeError("saved Garner world object differs")
    settings = world.get_world_settings()
    if _class_path(settings.get_editor_property("default_game_mode")) != rules.REFERENCE_GAME_MODE:
        raise RuntimeError("saved Garner default GameMode differs")
    markers = [actor for actor in actor_subsystem.get_all_level_actors()
               if actor.get_path_name() == rules.REFERENCE_BUILD_MARKER]
    if len(markers) != 1:
        raise RuntimeError("saved Garner marker is not unique")

    map_family = rules.package_family_evidence(
        "map", rules.REFERENCE_MAP_PACKAGE,
        repo_root / rules.PACKAGE_STEMS["map"], repo_root)
    report = {
        "schemaVersion": 1,
        "reportType": "garner-reference-terrain-level-build",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "manifest": manifest_evidence,
        "mapPackage": rules.REFERENCE_MAP_PACKAGE,
        "worldObject": rules.REFERENCE_WORLD_OBJECT,
        "mapPackageHash": map_family,
        "markerObject": rules.REFERENCE_BUILD_MARKER,
        "gameModeClass": rules.REFERENCE_GAME_MODE,
        "replacedExistingMap": bool(replaced),
        "issues": [],
    }
    rules.atomic_write_json(report_path, report)
    issues = rules.validate_level_build_report(report, repo_root)
    if issues:
        raise RuntimeError(f"self-validation failed: {issues[0]}")
    return report


def main(argv=None) -> int:
    arguments = list(sys.argv if argv is None else argv)
    if len(arguments) != 6:
        raise RuntimeError(
            "usage: build_reference_terrain_level.py manifest map report "
            "transaction-id source-head")
    repo_root = Path(__file__).resolve().parents[2]
    manifest_path = Path(arguments[1]).resolve()
    report_path = Path(arguments[3]).resolve()
    manifest_evidence = None
    try:
        _, manifest_evidence = _load_manifest(manifest_path, repo_root)
        build_level(
            manifest_path, arguments[2], report_path, arguments[4], arguments[5],
            repo_root)
    except Exception as exc:
        identity_issues = rules.validate_editor_identity_target(
            report_path, arguments[4], arguments[5], repo_root)
        if not identity_issues:
            if manifest_evidence is None:
                manifest_evidence = {
                    "path": manifest_path.relative_to(repo_root).as_posix(),
                    "sha256": "0" * 64,
                    "sizeBytes": 0,
                }
            failure = _failure(
                manifest_evidence, arguments[4], arguments[5], exc)
            rules.atomic_write_json(report_path, failure)
        raise RuntimeError(str(exc)) from exc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
