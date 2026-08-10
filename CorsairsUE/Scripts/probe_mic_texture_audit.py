"""Аудит Garner: материалы terrain-акторов + живость BaseColorTexture у MIC."""

import os
import sys
from collections import Counter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

import unreal                                        # noqa: E402
from report import Reporter                          # noqa: E402

PLACEHOLDERS = ("T_White_srgb", "T_White_Linear", "DefaultTexture", "")


report = Reporter("probe_mic_texture_audit")
try:
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level("/Game/Maps/Garner"):
        raise RuntimeError("не загрузилась /Game/Maps/Garner")

    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    terrain = [a for a in actors if a.get_actor_label().startswith("Terrain_")]
    mats = Counter()
    for actor in terrain:
        material = actor.static_mesh_component.get_material(0)
        mats[str(material.get_path_name()) if material else "None"] += 1
    report.line(f"MAP AUDIT: terrain actors={len(terrain)}")
    for path, count in sorted(mats.items()):
        report.line(f"  {count}x {path}")

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(["/Game/GarnerCity"], force_rescan=True)
    assets = registry.get_assets_by_path(
        unreal.Name("/Game/GarnerCity"), recursive=True)
    mics = [
        asset for asset in assets
        if str(asset.asset_class_path.asset_name) == "MaterialInstanceConstant"
    ]
    stats = Counter()
    offenders = []
    library = unreal.MaterialEditingLibrary
    for asset in mics:
        mic = unreal.load_asset(f"{asset.package_name}.{asset.asset_name}")
        value = library.get_material_instance_texture_parameter_value(
            mic, unreal.Name("BaseColorTexture"))
        name = value.get_name() if value is not None else ""
        if value is None or name in PLACEHOLDERS:
            stats["placeholder"] += 1
            offenders.append(f"{asset.package_name}.{asset.asset_name}")
        else:
            stats["bound"] += 1
    report.line(f"MIC AUDIT: total={len(mics)} {dict(stats)}")
    for entry in offenders[:40]:
        report.line(f"  PLACEHOLDER {entry}")
finally:
    report.close()
