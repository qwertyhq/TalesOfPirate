"""Read-only independent checker for the imported Garner reference page."""

from __future__ import annotations

from pathlib import Path
import sys

import unreal

import import_reference_terrain as importer
import reference_terrain_rules as rules


def _load_valid(path, validator, repo_root):
    data, _ = rules.strict_json_load(path)
    issues = validator(data, repo_root)
    if issues:
        raise RuntimeError(f"invalid linked report {path}: {issues[0]}")
    return data, rules.file_evidence(path, repo_root)


def check_reference(
    manifest_path: Path, map_package: str, level_report_path: Path,
    pass1_path: Path, pass2_path: Path, report_path: Path,
    transaction_id: str, source_head: str, repo_root: Path,
):
    target_issues = rules.validate_editor_identity_target(
        report_path, transaction_id, source_head, repo_root)
    if target_issues:
        raise RuntimeError(target_issues[0])
    if map_package != rules.REFERENCE_MAP_PACKAGE:
        raise RuntimeError("only /Game/Maps/Garner is an allowed check target")
    manifest, _ = rules.strict_json_load(manifest_path)
    manifest_issues = rules.validate_manifest(manifest, manifest_path)
    if manifest_issues:
        raise RuntimeError(manifest_issues[0])
    manifest_evidence = rules.file_evidence(manifest_path, repo_root)
    level, level_evidence = _load_valid(
        level_report_path, rules.validate_level_build_report, repo_root)
    pass1, pass1_evidence = _load_valid(
        pass1_path, rules.validate_import_report, repo_root)
    pass2, pass2_evidence = _load_valid(
        pass2_path, rules.validate_import_report, repo_root)
    for name, linked in (("level", level), ("pass1", pass1), ("pass2", pass2)):
        if (linked["transactionId"] != transaction_id or
                linked["sourceHead"] != source_head or
                linked["manifest"] != manifest_evidence):
            raise RuntimeError(f"{name} report identity/hash chain differs")
    idempotence = rules.validate_idempotent_import_reports(pass1, pass2)
    if idempotence:
        raise RuntimeError(idempotence[0])
    if pass1["beforeMapPackageHash"] != level["mapPackageHash"]:
        raise RuntimeError("pass 1 does not chain to level-build map family")
    if pass2["beforeMapPackageHash"] != pass1["finalPackageHashes"][4]:
        raise RuntimeError("pass 2 does not chain to pass 1 map family")

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(map_package):
        raise RuntimeError("checker could not load Garner")
    world = unreal.EditorLevelLibrary.get_editor_world()
    if world is None or world.get_path_name() != rules.REFERENCE_WORLD_OBJECT:
        raise RuntimeError("checker observed a noncanonical world")
    settings = world.get_world_settings()
    game_mode = settings.get_editor_property("default_game_mode")
    if game_mode is None or game_mode.get_path_name() != rules.REFERENCE_GAME_MODE:
        raise RuntimeError("checker observed a noncanonical GameMode")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    markers = [actor for actor in actor_subsystem.get_all_level_actors()
               if actor.get_path_name() == rules.REFERENCE_BUILD_MARKER]
    if len(markers) != 1:
        raise RuntimeError("checker observed a non-unique build marker")

    state = importer.inspect_reference_state(manifest)
    state_issues = rules.validate_reference_state(state)
    if state_issues:
        raise RuntimeError(state_issues[0])
    source_links = (
        state["mesh"]["sourceGltfSha256"] == manifest["files"]["meshGltf"]["sha256"],
        state["mesh"]["sourceBinSha256"] == manifest["files"]["meshBin"]["sha256"],
        state["texture"]["sourceSha256"] == manifest["files"]["albedo"]["sha256"],
    )
    if not all(source_links):
        raise RuntimeError("checker source metadata differs from Task 7 manifest")
    reference_actors = importer._find_reference_actor(actor_subsystem)
    if len(reference_actors) != 1:
        raise RuntimeError("checker observed non-unique reference actor")
    reference_actor = reference_actors[0]
    page_bounds = importer._actor_bounds_xy(reference_actor)
    legacy = []
    legacy_by_path = {}
    for actor in actor_subsystem.get_all_level_actors():
        if actor == reference_actor or not actor.get_actor_label().startswith("Terrain_"):
            continue
        path = actor.get_path_name()
        legacy.append((path, *importer._actor_bounds_xy(actor)))
        legacy_by_path[path] = actor
    overlaps = rules.overlapping_legacy_tiles(page_bounds, legacy)
    visible = []
    for path in overlaps:
        actor = legacy_by_path[path]
        tags = {str(tag) for tag in actor.tags}
        if (importer.HIDDEN_OVERLAP_TAG not in tags or
                not actor.is_temporarily_hidden_in_editor() or
                not actor.is_hidden()):
            visible.append(path)
    component_material = reference_actor.static_mesh_component.get_material(0)
    grass = []
    if component_material and "MI_grass05" in component_material.get_path_name():
        grass.append(component_material.get_path_name())
    material_fallback = 0 if component_material and not any(
        token in component_material.get_path_name()
        for token in ("WorldGridMaterial", "DefaultMaterial", "White")) else 1
    usage_errors = 0 if state["material"]["usageFlags"] == [
        "MATUSAGE_NANITE", "MATUSAGE_STATIC_MESH"] else 1
    translucent_nanite = int(
        state["mesh"]["naniteEnabled"] and
        state["material"]["blendMode"] not in ("BLEND_OPAQUE", "BLEND_MASKED"))
    families = rules.enumerate_package_families(repo_root)
    if families != pass2["finalPackageHashes"]:
        raise RuntimeError("checker physical families differ from import pass 2")
    report = {
        "schemaVersion": 1,
        "reportType": "garner-reference-terrain-check",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "manifest": manifest_evidence,
        "levelBuildReport": level_evidence,
        "importPass1Report": pass1_evidence,
        "importPass2Report": pass2_evidence,
        "mapPackage": rules.REFERENCE_MAP_PACKAGE,
        "worldObject": rules.REFERENCE_WORLD_OBJECT,
        "markerObject": rules.REFERENCE_BUILD_MARKER,
        "gameModeClass": rules.REFERENCE_GAME_MODE,
        "referenceState": state,
        "finalPackageHashes": families,
        "overlappingLegacyActors": overlaps,
        "visibleLegacyOverlaps": sorted(visible),
        "grassOverrides": sorted(grass),
        "materialFallbackCount": material_fallback,
        "materialUsageErrorCount": usage_errors,
        "translucentNaniteCount": translucent_nanite,
        "issues": [],
    }
    for key in (
        "visibleLegacyOverlaps", "grassOverrides", "materialFallbackCount",
        "materialUsageErrorCount", "translucentNaniteCount",
    ):
        if report[key] not in ([], 0):
            raise RuntimeError(f"checker gate failed: {key}={report[key]}")
    rules.atomic_write_json(report_path, report)
    issues = rules.validate_check_report(report, repo_root)
    if issues:
        raise RuntimeError(f"checker report self-validation failed: {issues[0]}")
    return report


def main(argv=None):
    arguments = list(sys.argv if argv is None else argv)
    if len(arguments) != 9:
        raise RuntimeError(
            "usage: check_reference_terrain.py manifest map level-report "
            "pass1-report pass2-report check-report transaction-id source-head")
    repo_root = Path(__file__).resolve().parents[2]
    try:
        check_reference(
            Path(arguments[1]).resolve(), arguments[2], Path(arguments[3]).resolve(),
            Path(arguments[4]).resolve(), Path(arguments[5]).resolve(),
            Path(arguments[6]).resolve(), arguments[7], arguments[8], repo_root)
    except Exception as exc:
        report_path = Path(arguments[6]).resolve()
        if not rules.validate_editor_identity_target(
                report_path, arguments[7], arguments[8], repo_root):
            failure = {
                "schemaVersion": 1,
                "reportType": "garner-reference-terrain-check",
                "status": "FAIL", "transactionId": arguments[7],
                "sourceHead": arguments[8], "manifest": None,
                "levelBuildReport": None, "importPass1Report": None,
                "importPass2Report": None, "mapPackage": rules.REFERENCE_MAP_PACKAGE,
                "worldObject": rules.REFERENCE_WORLD_OBJECT,
                "markerObject": rules.REFERENCE_BUILD_MARKER,
                "gameModeClass": rules.REFERENCE_GAME_MODE,
                "referenceState": None, "finalPackageHashes": None,
                "overlappingLegacyActors": [], "visibleLegacyOverlaps": [],
                "grassOverrides": [], "materialFallbackCount": 0,
                "materialUsageErrorCount": 0, "translucentNaniteCount": 0,
                "issues": [{"code": "CHECK_FAILED", "field": "/",
                            "detail": str(exc)}],
            }
            rules.atomic_write_json(report_path, failure)
        raise RuntimeError(str(exc)) from exc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
