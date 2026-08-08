"""Idempotently import the one canonical Garner reference terrain page."""

from __future__ import annotations

import copy
import json
import math
from pathlib import Path
import sys

sys.dont_write_bytecode = True
_SCRIPT_DIR = str(Path(__file__).resolve().parent)
if not sys.path or sys.path[0] != _SCRIPT_DIR:
    sys.path.insert(0, _SCRIPT_DIR)

import unreal

import reference_terrain_rules as rules


HIDDEN_OVERLAP_TAG = "CorsairsReferenceTerrainHiddenOverlap"


def _enum_leaf(value) -> str:
    return str(value).rsplit(".", 1)[-1]


def _class_path(value) -> str:
    if value is None:
        return ""
    getter = getattr(value, "get_path_name", None)
    return str(getter()) if getter is not None else str(value)


def _asset_package_path(value) -> str:
    """Return the canonical Unreal package path used by the Task 8 DTOs."""
    if value is None:
        return ""
    object_path = str(value.get_path_name())
    package_path, separator, object_name = object_path.rpartition(".")
    if separator and package_path.rsplit("/", 1)[-1] == object_name:
        return package_path
    return object_path


def _metadata(asset, key: str) -> str:
    return str(unreal.EditorAssetLibrary.get_metadata_tag(asset, key) or "")


def _set_metadata(asset, key: str, value: str) -> bool:
    if _metadata(asset, key) == value:
        return False
    unreal.EditorAssetLibrary.set_metadata_tag(asset, key, value)
    return True


def _find_reference_actor(actor_subsystem):
    expected = rules.reference_actor_object_path(
        rules.REFERENCE_MAP_PACKAGE, "garner", 17, 21)
    return [actor for actor in actor_subsystem.get_all_level_actors()
            if actor.get_path_name() == expected or
            rules.REFERENCE_TAG in {str(tag) for tag in actor.tags}]


def _actor_bounds_xy(actor):
    origin, extent = actor.get_actor_bounds(False)
    return (
        float(origin.x - extent.x), float(origin.y - extent.y),
        float(origin.x + extent.x), float(origin.y + extent.y),
    )


def _vector_within_tolerance(actual, expected, tolerance):
    try:
        limit = float(tolerance)
        pairs = tuple(
            (float(getattr(actual, axis)), float(getattr(expected, axis)))
            for axis in ("x", "y", "z"))
    except (AttributeError, TypeError, ValueError):
        return False
    return (math.isfinite(limit) and limit >= 0.0 and
            all(math.isfinite(observed) and math.isfinite(wanted) and
                abs(observed - wanted) <= limit
                for observed, wanted in pairs))


def inspect_reference_state(manifest: dict):
    paths = rules.asset_paths("garner", 17, 21)
    mesh = unreal.load_asset(paths["mesh"])
    texture = unreal.load_asset(paths["texture"])
    material = unreal.load_asset(paths["material"])
    instance = unreal.load_asset(paths["instance"])
    if not isinstance(mesh, unreal.StaticMesh):
        raise RuntimeError("canonical reference StaticMesh is missing")
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError("canonical reference Texture2D is missing")
    if not isinstance(material, unreal.Material):
        raise RuntimeError("canonical reference Material is missing")
    if not isinstance(instance, unreal.MaterialInstanceConstant):
        raise RuntimeError("canonical reference MaterialInstanceConstant is missing")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = _find_reference_actor(actor_subsystem)
    if len(actors) != 1:
        raise RuntimeError("reference terrain actor is not unique")
    actor = actors[0]
    bounds = _actor_bounds_xy(actor)
    component = actor.static_mesh_component
    component_material = component.get_material(0)
    mesh_material = mesh.get_material(0)
    parent = instance.get_editor_property("parent")
    parameter = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
        instance, unreal.Name("BaseColorTexture"))

    expressions = list(
        unreal.MaterialEditingLibrary.get_material_expressions(material))
    samplers = [item for item in expressions if isinstance(
        item, unreal.MaterialExpressionTextureSampleParameter2D) and
        str(item.get_editor_property("parameter_name")) == "BaseColorTexture"]
    if len(samplers) != 1:
        raise RuntimeError("material BaseColorTexture sampler is not unique")
    sampler = samplers[0]
    emissive = unreal.MaterialEditingLibrary.get_material_property_input_node(
        material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity = unreal.MaterialEditingLibrary.get_material_property_input_node(
        material, unreal.MaterialProperty.MP_OPACITY_MASK)
    if emissive != sampler or opacity != sampler:
        raise RuntimeError("material sampler outputs are not fully connected")
    nanite = mesh.get_editor_property("nanite_settings")
    location = actor.get_actor_location()
    return {
        "mesh": {
            "objectPath": _asset_package_path(mesh),
            "sourceGltfSha256": _metadata(mesh, "Corsairs.SourceGltfSha256"),
            "sourceBinSha256": _metadata(mesh, "Corsairs.SourceBinSha256"),
            "naniteEnabled": bool(nanite.enabled),
            "materialSlot0": _asset_package_path(mesh_material),
        },
        "texture": {
            "objectPath": _asset_package_path(texture),
            "sourceWidth": int(texture.blueprint_get_size_x()),
            "sourceHeight": int(texture.blueprint_get_size_y()),
            "sourceFormat": _metadata(texture, "Corsairs.SourceFormat"),
            "sourceSha256": _metadata(texture, "Corsairs.SourceSha256"),
            "srgb": bool(texture.get_editor_property("srgb")),
            "compression": _enum_leaf(texture.get_editor_property("compression_settings")),
            "filter": _enum_leaf(texture.get_editor_property("filter")),
            "addressX": _enum_leaf(texture.get_editor_property("address_x")),
            "addressY": _enum_leaf(texture.get_editor_property("address_y")),
            "mipGenSettings": _enum_leaf(texture.get_editor_property("mip_gen_settings")),
            "lodGroup": _enum_leaf(texture.get_editor_property("lod_group")),
            "lodBias": int(texture.get_editor_property("lod_bias")),
            "neverStream": bool(texture.get_editor_property("never_stream")),
        },
        "material": {
            "objectPath": _asset_package_path(material),
            "blendMode": _enum_leaf(material.get_editor_property("blend_mode")),
            "shadingModel": _enum_leaf(material.get_editor_property("shading_model")),
            "parameterName": str(sampler.get_editor_property("parameter_name")),
            "samplerType": _enum_leaf(sampler.get_editor_property("sampler_type")),
            "samplerSource": _enum_leaf(sampler.get_editor_property("sampler_source")),
            "rgbOutput": "MP_EMISSIVE_COLOR",
            "alphaOutput": "MP_OPACITY_MASK",
            "usageFlags": sorted([
                name for name, enabled in (
                    ("MATUSAGE_NANITE", material.get_editor_property("used_with_nanite")),
                    ("MATUSAGE_STATIC_MESH",
                    material.get_editor_property("used_with_static_mesh")),
                ) if enabled]),
        },
        "instance": {
            "objectPath": _asset_package_path(instance),
            "parent": _asset_package_path(parent),
            "baseColorTexture": _asset_package_path(parameter),
        },
        "actor": {
            "objectPath": actor.get_path_name(),
            "className": actor.get_class().get_path_name(),
            "label": actor.get_actor_label(),
            "tag": rules.REFERENCE_TAG if rules.REFERENCE_TAG in
                {str(tag) for tag in actor.tags} else "",
            "locationCm": [float(location.x), float(location.y), float(location.z)],
            "staticMesh": _asset_package_path(component.get_editor_property(
                "static_mesh")),
            "boundsMinCm": [bounds[0], bounds[1]],
            "boundsMaxCm": [bounds[2], bounds[3]],
            "componentMaterialSlot0": (
                _asset_package_path(component_material)),
        },
    }


def _change(path, class_name, reason):
    return {"objectPath": path, "className": class_name, "reason": reason}


def _import_asset(source: Path, destination_path: str, destination_name: str):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", str(source))
    task.set_editor_property("destination_path", destination_path)
    task.set_editor_property("destination_name", destination_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("replace_existing_settings", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return [str(item) for item in task.get_editor_property("imported_object_paths")]


def _imported_package_path(object_path: str) -> str:
    package_path, separator, object_name = str(object_path).rpartition(".")
    if separator and package_path.rsplit("/", 1)[-1] == object_name:
        return package_path
    return str(object_path)


def _delete_attempt_owned_imports(imported_paths, keep_packages=()):
    keep = set(keep_packages)
    removed = []
    for imported_path in dict.fromkeys(str(item) for item in imported_paths):
        if _imported_package_path(imported_path) in keep:
            continue
        if unreal.EditorAssetLibrary.does_asset_exist(imported_path):
            if not unreal.EditorAssetLibrary.delete_asset(imported_path):
                raise RuntimeError(
                    f"attempt-owned imported asset could not be deleted: {imported_path}")
            removed.append(imported_path)
    return removed


def _adopt_imported_asset(imported_paths, canonical_path, expected_type):
    returned = list(dict.fromkeys(str(item) for item in imported_paths))
    candidates = []
    for imported_path in returned:
        asset = unreal.load_asset(imported_path)
        if isinstance(asset, expected_type):
            candidates.append((imported_path, asset))
    if len(candidates) != 1:
        _delete_attempt_owned_imports(returned)
        raise RuntimeError(
            f"import returned {len(candidates)} expected assets: {returned}")

    source_path, asset = candidates[0]
    source_package = _asset_package_path(asset)
    if source_package != canonical_path:
        if unreal.EditorAssetLibrary.does_asset_exist(canonical_path):
            if not unreal.EditorAssetLibrary.delete_asset(canonical_path):
                _delete_attempt_owned_imports(returned)
                raise RuntimeError(
                    f"canonical import target could not be replaced: {canonical_path}")
        if not unreal.EditorAssetLibrary.rename_loaded_asset(asset, canonical_path):
            _delete_attempt_owned_imports(returned)
            raise RuntimeError(
                f"imported asset could not move from {source_path} to {canonical_path}")

    adopted = unreal.load_asset(canonical_path)
    if (not isinstance(adopted, expected_type) or
            _asset_package_path(adopted) != canonical_path):
        _delete_attempt_owned_imports(returned)
        raise RuntimeError(f"canonical imported asset differs: {canonical_path}")
    removed = _delete_attempt_owned_imports(
        returned, keep_packages=(canonical_path,))
    return adopted, removed


def _configure_reference_assets(manifest, created, updated, deleted):
    paths = rules.asset_paths("garner", 17, 21)
    mesh_package, mesh_name = paths["mesh"].rsplit("/", 1)
    texture_package, texture_name = paths["texture"].rsplit("/", 1)
    source_mesh = manifest["files"]["meshGltf"]
    source_bin = manifest["files"]["meshBin"]
    source_texture = manifest["files"]["albedo"]
    manifest_root = Path(_ACTIVE_MANIFEST_PATH).parent

    mesh = unreal.load_asset(paths["mesh"])
    mesh_existed = isinstance(mesh, unreal.StaticMesh)
    mesh_needs_import = (not mesh_existed or
                         _metadata(mesh, "Corsairs.SourceGltfSha256") !=
                         source_mesh["sha256"] or
                         _metadata(mesh, "Corsairs.SourceBinSha256") !=
                         source_bin["sha256"])
    if mesh_needs_import:
        imported = _import_asset(manifest_root / source_mesh["path"], mesh_package, mesh_name)
        mesh, removed = _adopt_imported_asset(
            imported, paths["mesh"], unreal.StaticMesh)
        (updated if mesh_existed else created).append(_change(
            paths["mesh"], "/Script/Engine.StaticMesh",
            "REIMPORTED" if mesh_existed else "IMPORTED"))
        for imported_path in removed:
            deleted.append(_change(
                imported_path, "/Script/CoreUObject.Object",
                "UNEXPECTED_IMPORT_SIDECAR"))
    texture = unreal.load_asset(paths["texture"])
    texture_existed = isinstance(texture, unreal.Texture2D)
    texture_needs_import = (not texture_existed or
                            _metadata(texture, "Corsairs.SourceSha256") !=
                            source_texture["sha256"])
    if texture_needs_import:
        imported = _import_asset(
            manifest_root / source_texture["path"], texture_package, texture_name)
        texture = unreal.load_asset(paths["texture"])
        if not isinstance(texture, unreal.Texture2D):
            raise RuntimeError(f"texture import failed: {imported}")
        (updated if texture_existed else created).append(_change(
            paths["texture"], "/Script/Engine.Texture2D",
            "REIMPORTED" if texture_existed else "IMPORTED"))
        for imported_path in imported:
            if imported_path not in (
                    paths["texture"], f"{paths['texture']}.{texture_name}") and unreal.EditorAssetLibrary.does_asset_exist(
                    imported_path):
                if not unreal.EditorAssetLibrary.delete_asset(imported_path):
                    raise RuntimeError(
                        f"unexpected texture import sidecar remains: {imported_path}")
                deleted.append(_change(
                    imported_path, "/Script/CoreUObject.Object",
                    "UNEXPECTED_IMPORT_SIDECAR"))

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = unreal.load_asset(paths["material"])
    material_created = not isinstance(material, unreal.Material)
    if material_created:
        material = asset_tools.create_asset(
            "M_TerrainReference", "/Game/Terrain/Reference", unreal.Material,
            unreal.MaterialFactoryNew())
        if material is None:
            raise RuntimeError("reference material creation failed")
        created.append(_change(paths["material"], "/Script/Engine.Material", "CREATED"))
    material_dirty = material_created
    if material.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_MASKED:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        material_dirty = True
    if material.get_editor_property("shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        material_dirty = True
    expressions = list(
        unreal.MaterialEditingLibrary.get_material_expressions(material))
    samplers = [item for item in expressions if isinstance(
        item, unreal.MaterialExpressionTextureSampleParameter2D) and
        str(item.get_editor_property("parameter_name")) == "BaseColorTexture"]
    sampler_ok = False
    if len(samplers) == 1:
        sampler = samplers[0]
        sampler_ok = (
            sampler.get_editor_property("sampler_type") ==
                unreal.MaterialSamplerType.SAMPLERTYPE_COLOR and
            sampler.get_editor_property("sampler_source") ==
                unreal.SamplerSourceMode.SSM_FROM_TEXTURE_ASSET and
            unreal.MaterialEditingLibrary.get_material_property_input_node(
                material, unreal.MaterialProperty.MP_EMISSIVE_COLOR) == sampler and
            unreal.MaterialEditingLibrary.get_material_property_input_node(
                material, unreal.MaterialProperty.MP_OPACITY_MASK) == sampler)
    if not sampler_ok:
        unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
        sampler = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionTextureSampleParameter2D, -300, 0)
        sampler.set_editor_property("parameter_name", unreal.Name("BaseColorTexture"))
        sampler.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        sampler.set_editor_property(
            "sampler_source", unreal.SamplerSourceMode.SSM_FROM_TEXTURE_ASSET)
        unreal.MaterialEditingLibrary.connect_material_property(
            sampler, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        unreal.MaterialEditingLibrary.connect_material_property(
            sampler, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        material_dirty = True
    for usage in (
        unreal.MaterialUsage.MATUSAGE_STATIC_MESH,
        unreal.MaterialUsage.MATUSAGE_NANITE,
    ):
        if unreal.MaterialEditingLibrary.set_material_usage(material, usage):
            material_dirty = True
    if material_dirty:
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not material_created:
            updated.append(_change(
                paths["material"], "/Script/Engine.Material", "CONTRACT"))
    instance = unreal.load_asset(paths["instance"])
    instance_created = not isinstance(instance, unreal.MaterialInstanceConstant)
    if instance_created:
        instance = asset_tools.create_asset(
            "MI_Garner_17_21", "/Game/Terrain/Reference/Garner",
            unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
        if instance is None:
            raise RuntimeError("reference material instance creation failed")
        created.append(_change(
            paths["instance"], "/Script/Engine.MaterialInstanceConstant", "CREATED"))
    instance_dirty = instance_created
    if instance.get_editor_property("parent") != material:
        unreal.MaterialEditingLibrary.set_material_instance_parent(instance, material)
        instance_dirty = True
    current_texture = (
        unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
            instance, unreal.Name("BaseColorTexture")))
    if current_texture != texture:
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
            instance, unreal.Name("BaseColorTexture"), texture)
        instance_dirty = True
    if instance_dirty and not instance_created:
        updated.append(_change(
            paths["instance"], "/Script/Engine.MaterialInstanceConstant", "CONTRACT"))

    mesh_dirty = mesh_needs_import
    for key, value in (
        ("Corsairs.SourceGltfSha256", source_mesh["sha256"]),
        ("Corsairs.SourceBinSha256", source_bin["sha256"]),
    ):
        mesh_dirty = _set_metadata(mesh, key, value) or mesh_dirty
    texture_dirty = texture_needs_import
    for key, value in (
        ("Corsairs.SourceSha256", source_texture["sha256"]),
        ("Corsairs.SourceFormat", "RGBA8"),
    ):
        texture_dirty = _set_metadata(texture, key, value) or texture_dirty
    for property_name, value in (
        ("srgb", True),
        ("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT),
        ("filter", unreal.TextureFilter.TF_BILINEAR),
        ("address_x", unreal.TextureAddress.TA_CLAMP),
        ("address_y", unreal.TextureAddress.TA_CLAMP),
        ("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP),
        ("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD),
        ("lod_bias", 0),
        ("never_stream", False),
    ):
        if texture.get_editor_property(property_name) != value:
            texture.set_editor_property(property_name, value)
            texture_dirty = True
    nanite = mesh.get_editor_property("nanite_settings")
    if not nanite.enabled:
        nanite.enabled = True
        mesh.set_editor_property("nanite_settings", nanite)
        mesh_dirty = True
    if mesh.get_material(0) != instance:
        mesh.set_material(0, instance)
        mesh_dirty = True
    if mesh_dirty and not any(
            item["objectPath"] == paths["mesh"] for item in created + updated):
        updated.append(_change(paths["mesh"], "/Script/Engine.StaticMesh", "CONTRACT"))
    if texture_dirty and not any(
            item["objectPath"] == paths["texture"] for item in created + updated):
        updated.append(_change(paths["texture"], "/Script/Engine.Texture2D", "CONTRACT"))
    return mesh, texture, material, instance


def _reconcile_actor(mesh, instance, created, updated):
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = _find_reference_actor(actor_subsystem)
    if len(actors) > 1:
        raise RuntimeError("multiple tagged reference terrain actors exist")
    if actors:
        actor = actors[0]
    else:
        actor = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(217600.0, -268800.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0))
        if actor is None:
            raise RuntimeError("reference terrain actor creation failed")
        level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not actor.rename(
                "ReferenceTerrain_Garner_17_21",
                level_subsystem.get_current_level()):
            raise RuntimeError("reference terrain actor rename failed")
        actor.set_actor_label("ReferenceTerrain_Garner_17_21", mark_dirty=True)
        actor.set_editor_property("tags", [unreal.Name(rules.REFERENCE_TAG)])
        created.append(_change(
            actor.get_path_name(), "/Script/Engine.StaticMeshActor", "CREATED"))
    changed = False
    expected_location = unreal.Vector(217600.0, -268800.0, 0.0)
    if not _vector_within_tolerance(
            actor.get_actor_location(), expected_location, 0.001):
        actor.set_actor_location(expected_location, False, False)
        changed = True
    component = actor.static_mesh_component
    if component.get_editor_property("static_mesh") != mesh:
        component.set_static_mesh(mesh)
        changed = True
    if component.get_material(0) != instance:
        component.set_material(0, instance)
        changed = True
    if changed:
        updated.append(_change(
            actor.get_path_name(), "/Script/Engine.StaticMeshActor", "CONTRACT"))
    return actor


def _reconcile_legacy_overlaps(reference_actor, updated):
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    page_bounds = _actor_bounds_xy(reference_actor)
    candidates = []
    by_path = {}
    for actor in actor_subsystem.get_all_level_actors():
        if actor == reference_actor or not actor.get_actor_label().startswith("Terrain_"):
            continue
        path = actor.get_path_name()
        by_path[path] = actor
        candidates.append((path, *_actor_bounds_xy(actor)))
    overlaps = set(rules.overlapping_legacy_tiles(page_bounds, candidates))
    for path, actor in by_path.items():
        tags = {str(tag) for tag in actor.tags}
        should_hide = path in overlaps
        tagged = HIDDEN_OVERLAP_TAG in tags
        if should_hide and not tagged:
            actor.set_editor_property("tags", list(actor.tags) + [unreal.Name(HIDDEN_OVERLAP_TAG)])
            actor.set_is_temporarily_hidden_in_editor(True)
            actor.set_actor_hidden_in_game(True)
            updated.append(_change(path, actor.get_class().get_path_name(), "HIDE_OVERLAP"))
        elif not should_hide and tagged:
            actor.set_editor_property(
                "tags", [tag for tag in actor.tags if str(tag) != HIDDEN_OVERLAP_TAG])
            actor.set_is_temporarily_hidden_in_editor(False)
            actor.set_actor_hidden_in_game(False)
            updated.append(_change(path, actor.get_class().get_path_name(), "RESTORE_OVERLAP"))
    return sorted(overlaps)


def _save_dirty_packages(paths, actor_changed):
    saved = []
    for package in (paths["mesh"], paths["texture"], paths["material"], paths["instance"]):
        if unreal.EditorAssetLibrary.save_asset(package, only_if_is_dirty=True):
            saved.append(package)
    if actor_changed:
        subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not subsystem.save_current_level():
            raise RuntimeError("reference Garner level save failed")
        saved.append(rules.REFERENCE_MAP_PACKAGE)
    return sorted(set(saved))


def import_reference(
    manifest_path: Path, map_package: str, report_path: Path,
    transaction_id: str, source_head: str, repo_root: Path,
):
    global _ACTIVE_MANIFEST_PATH
    _ACTIVE_MANIFEST_PATH = manifest_path
    target_issues = rules.validate_editor_identity_target(
        report_path, transaction_id, source_head, repo_root)
    if target_issues:
        raise RuntimeError(target_issues[0])
    if map_package != rules.REFERENCE_MAP_PACKAGE:
        raise RuntimeError("only /Game/Maps/Garner is an allowed import target")
    manifest, _ = rules.strict_json_load(manifest_path)
    manifest_issues = rules.validate_manifest(manifest, manifest_path)
    if manifest_issues:
        raise RuntimeError(manifest_issues[0])
    manifest_evidence = rules.file_evidence(manifest_path, repo_root)
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(map_package):
        raise RuntimeError("canonical Garner level did not load")
    world = unreal.EditorLevelLibrary.get_editor_world()
    if world is None or world.get_path_name() != rules.REFERENCE_WORLD_OBJECT:
        raise RuntimeError("canonical Garner world object differs")
    if _class_path(world.get_world_settings().get_editor_property(
            "default_game_mode")) != rules.REFERENCE_GAME_MODE:
        raise RuntimeError("canonical Garner GameMode differs")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if len([actor for actor in actor_subsystem.get_all_level_actors()
            if actor.get_path_name() == rules.REFERENCE_BUILD_MARKER]) != 1:
        raise RuntimeError("canonical reference build marker differs")

    before_map = rules.package_family_evidence(
        "map", rules.REFERENCE_MAP_PACKAGE,
        repo_root / rules.PACKAGE_STEMS["map"], repo_root)
    created, updated, deleted = [], [], []
    try:
        existing_state = inspect_reference_state(manifest)
        already_valid = not rules.validate_reference_state(existing_state)
        if already_valid:
            links = (
                existing_state["mesh"]["sourceGltfSha256"] ==
                    manifest["files"]["meshGltf"]["sha256"],
                existing_state["mesh"]["sourceBinSha256"] ==
                    manifest["files"]["meshBin"]["sha256"],
                existing_state["texture"]["sourceSha256"] ==
                    manifest["files"]["albedo"]["sha256"],
            )
            already_valid = all(links)
    except RuntimeError:
        already_valid = False
    if already_valid:
        state = existing_state
        saved = []
    else:
        paths = rules.asset_paths("garner", 17, 21)
        mesh, _, _, instance = _configure_reference_assets(
            manifest, created, updated, deleted)
        actor = _reconcile_actor(mesh, instance, created, updated)
        _reconcile_legacy_overlaps(actor, updated)
        saved = _save_dirty_packages(paths, bool(created or updated or deleted))
        if not subsystem.load_level(map_package):
            raise RuntimeError("reference Garner reload after import failed")
        state = inspect_reference_state(manifest)
    state_issues = rules.validate_reference_state(state)
    if state_issues:
        raise RuntimeError(state_issues[0])
    families = rules.enumerate_package_families(repo_root)
    report = {
        "schemaVersion": 1,
        "reportType": "garner-reference-terrain-import",
        "status": "PASS",
        "transactionId": transaction_id,
        "sourceHead": source_head,
        "manifest": manifest_evidence,
        "mapPackage": rules.REFERENCE_MAP_PACKAGE,
        "worldObject": rules.REFERENCE_WORLD_OBJECT,
        "markerObject": rules.REFERENCE_BUILD_MARKER,
        "gameModeClass": rules.REFERENCE_GAME_MODE,
        "beforeMapPackageHash": before_map,
        "referenceState": state,
        "created": sorted(created, key=lambda item: (
            item["objectPath"], item["className"], item["reason"])),
        "updated": sorted(updated, key=lambda item: (
            item["objectPath"], item["className"], item["reason"])),
        "deleted": sorted(deleted, key=lambda item: (
            item["objectPath"], item["className"], item["reason"])),
        "savedPackages": saved,
        "finalPackageHashes": families,
        "issues": [],
    }
    rules.atomic_write_json(report_path, report)
    issues = rules.validate_import_report(report, repo_root)
    if issues:
        raise RuntimeError(f"import report self-validation failed: {issues[0]}")
    return report


def main(argv=None):
    arguments = list(sys.argv if argv is None else argv)
    if len(arguments) != 6:
        raise RuntimeError(
            "usage: import_reference_terrain.py manifest map report "
            "transaction-id source-head")
    repo_root = Path(__file__).resolve().parents[2]
    try:
        import_reference(
            Path(arguments[1]).resolve(), arguments[2], Path(arguments[3]).resolve(),
            arguments[4], arguments[5], repo_root)
    except Exception as exc:
        report_path = Path(arguments[3]).resolve()
        if not rules.validate_editor_identity_target(
                report_path, arguments[4], arguments[5], repo_root):
            failure = {
                "schemaVersion": 1,
                "reportType": "garner-reference-terrain-import",
                "status": "FAIL",
                "transactionId": arguments[4], "sourceHead": arguments[5],
                "manifest": None, "mapPackage": rules.REFERENCE_MAP_PACKAGE,
                "worldObject": rules.REFERENCE_WORLD_OBJECT,
                "markerObject": rules.REFERENCE_BUILD_MARKER,
                "gameModeClass": rules.REFERENCE_GAME_MODE,
                "beforeMapPackageHash": None, "referenceState": None,
                "created": [], "updated": [], "deleted": [], "savedPackages": [],
                "finalPackageHashes": None,
                "issues": [{"code": "IMPORT_FAILED", "field": "/",
                            "detail": str(exc)}],
            }
            rules.atomic_write_json(report_path, failure)
        raise RuntimeError(str(exc)) from exc
    return 0


_ACTIVE_MANIFEST_PATH = ""


if __name__ == "__main__":
    raise SystemExit(main())
