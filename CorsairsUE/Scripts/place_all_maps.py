"""Собирает уровни всех карт: объекты, рельеф, свет и точка появления.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/place_all_maps.py <каталог-манифестов> [карта ...]"

Пример:
    ... Scripts/place_all_maps.py ../artifacts/maps

Отчёт: `Scripts/reports/place_all_maps.txt`.

Делает за один запуск то же, что place_objects, place_terrain и
setup_level_view поодиночке. Порознь на 47 карт ушло бы 141 запуск редактора,
а старт редактора занимает больше, чем сама расстановка небольшой карты.

Уровни создаются заново: повторный запуск обязан давать тот же результат, а не
накладывать вторую копию мира поверх первой.
"""

import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402
from scene_placement import (                         # noqa: E402
    effect_summary, load_heights, object_location, split_effects)

TERRAIN_ROOT = "/Game/Terrain"
LEVEL_ROOT = "/Game/Maps"

# Высота камеры над центром мира и наклон вниз.
CAMERA_HEIGHT = 60000.0
CAMERA_PITCH = -35.0

# На сколько поднять точку появления над рельефом.
SPAWN_MARGIN = 2000.0


def load_model_map(script_dir):
    path = os.path.join(script_dir, "model_map.json")
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle).get("models", {})


def load_manifest(path):
    path = os.path.abspath(path)
    library = unreal.CorsairsSceneManifestLibrary
    result = library.load_scene_manifest(path)
    if result is None:
        return None, f"биндинг вернул отказ для {path}"
    if isinstance(result, tuple) and len(result) == 3:
        ok, manifest, error = result
        return (manifest, "") if ok else (None, error)
    if isinstance(result, tuple) and len(result) == 2:
        manifest, error = result
        return (None, error) if error else (manifest, "")
    return None, f"неожиданный ответ биндинга: {type(result)}"


def add_hism_component(actor):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subsystem.k2_gather_subobject_data_for_instance(actor)
    if not handles:
        return None
    params = unreal.AddNewSubobjectParams(
        parent_handle=handles[0],
        new_class=unreal.HierarchicalInstancedStaticMeshComponent,
        blueprint_context=None)
    handle, fail_reason = subsystem.add_new_subobject(params)
    if not fail_reason.is_empty():
        return None
    data = subsystem.k2_find_subobject_data_from_handle(handle)
    component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    return component if isinstance(
        component, unreal.HierarchicalInstancedStaticMeshComponent) else None


def place_objects(report, manifest, models, heights=None):
    """Ставит объекты карты инстансами. Возвращает (инстансов, центр)."""
    library = unreal.CorsairsSceneManifestLibrary
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    scene_objects, effects = split_effects(list(manifest.objects))
    if effects:
        count, kinds, top = effect_summary(effects)
        report.line(f"  эффектов отложено={count} видов={kinds}; частые id: {top}")

    groups = {}
    unresolved = 0
    for obj in scene_objects:
        assets = models.get(str(obj.model_id))
        if not assets:
            unresolved += 1
            continue
        transform = unreal.Transform(
            object_location(library, obj, heights),
            library.get_object_rotation(obj),
            unreal.Vector(1.0, 1.0, 1.0))
        for asset in assets:
            groups.setdefault(asset, []).append(transform)

    placed = 0
    bounds = [[float("inf")] * 3, [float("-inf")] * 3]

    for asset_path, transforms in sorted(groups.items()):
        mesh = unreal.load_asset(asset_path)
        if mesh is None:
            mesh = unreal.load_asset(
                asset_path.replace("/StaticMeshes/", "/SkeletalMeshes/"))

        if isinstance(mesh, unreal.SkeletalMesh):
            # Скиннутые объекты сцены — анимированные вроде мельниц;
            # инстансирование к ним неприменимо.
            for transform in transforms:
                actor = actor_subsystem.spawn_actor_from_class(
                    unreal.SkeletalMeshActor, transform.translation,
                    transform.rotation.rotator())
                if actor is not None:
                    actor.skeletal_mesh_component.set_skeletal_mesh(mesh)
                    placed += 1
            continue

        if not isinstance(mesh, unreal.StaticMesh):
            continue

        actor = actor_subsystem.spawn_actor_from_class(
            unreal.Actor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
        if actor is None:
            continue
        actor.set_actor_label(f"Inst_{os.path.basename(asset_path)}")

        component = add_hism_component(actor)
        if component is None:
            actor_subsystem.destroy_actor(actor)
            continue

        component.set_static_mesh(mesh)
        for transform in transforms:
            component.add_instance(transform, True)
        placed += component.get_instance_count()

        for transform in transforms[:50]:
            location = transform.translation
            for axis, value in enumerate((location.x, location.y, location.z)):
                bounds[0][axis] = min(bounds[0][axis], value)
                bounds[1][axis] = max(bounds[1][axis], value)

    if bounds[0][0] == float("inf"):
        centre = unreal.Vector(0.0, 0.0, 0.0)
    else:
        centre = unreal.Vector(*[(bounds[0][i] + bounds[1][i]) / 2.0 for i in range(3)])

    return placed, centre, unresolved


def place_terrain(map_name):
    """Ставит плитки рельефа карты. Возвращает их число."""
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(unreal.Name(TERRAIN_ROOT), recursive=True)

    prefix = f"{map_name}_terrain_"
    placed = 0
    for asset in assets:
        name = str(asset.asset_name)
        if not name.startswith(prefix):
            continue
        mesh = unreal.load_asset(f"{asset.package_name}.{name}")
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        actor = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0))
        if actor is None:
            continue
        actor.static_mesh_component.set_static_mesh(mesh)
        actor.set_actor_label(f"Terrain_{name}")
        placed += 1
    return placed


def add_lighting_and_start(centre):
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    sun = actor_subsystem.spawn_actor_from_class(
        unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 100000.0),
        unreal.Rotator(0.0, 0.0, 0.0))
    if sun is not None:
        # Наклон вбок: отвесный свет делает вертикальные стены одинаково
        # плоскими, и форма зданий не читается.
        sun.set_actor_rotation(unreal.Rotator(0.0, -45.0, 30.0), False)
        sun.set_actor_label("Sun")

    for actor_class, label in ((unreal.SkyAtmosphere, "SkyAtmosphere"),
                               (unreal.SkyLight, "SkyLight")):
        actor = actor_subsystem.spawn_actor_from_class(
            actor_class, unreal.Vector(0.0, 0.0, 100000.0), unreal.Rotator(0.0, 0.0, 0.0))
        if actor is not None:
            actor.set_actor_label(label)

    start = actor_subsystem.spawn_actor_from_class(
        unreal.PlayerStart,
        unreal.Vector(centre.x, centre.y, centre.z + SPAWN_MARGIN),
        unreal.Rotator(0.0, 0.0, 0.0))
    if start is not None:
        start.set_actor_label("PlayerStart")


def level_name(map_name):
    """Имя уровня: первая буква заглавная, остальное как есть."""
    return map_name[:1].upper() + map_name[1:]


def build_map(report, manifest_path, map_name, models, game_mode, script_dir):
    manifest, error = load_manifest(manifest_path)
    if manifest is None:
        report.warn(f"{map_name}: манифест не прочитан — {error}")
        return False

    level_path = f"{LEVEL_ROOT}/{level_name(map_name)}"
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level(level_path)

    # Без карты высот постройки встают на голое смещение и город идёт
    # ступеньками. Отсутствие файла не останавливает сборку, но должно быть
    # видно в отчёте: иначе кривая карта выглядит как удачная.
    heights, heights_path = load_heights(script_dir, map_name)
    if not heights.side:
        report.warn(f"{map_name}: карты высот нет ({heights_path}) — "
                    "постройки встанут на голое смещение")

    placed, centre, unresolved = place_objects(report, manifest, models, heights)
    tiles = place_terrain(map_name)
    add_lighting_and_start(centre)

    world_settings = unreal.EditorLevelLibrary.get_editor_world().get_world_settings()
    if game_mode is not None:
        world_settings.set_editor_property("default_game_mode", game_mode)

    unreal.EditorLevelLibrary.set_level_viewport_camera_info(
        unreal.Vector(centre.x, centre.y, centre.z + CAMERA_HEIGHT),
        unreal.Rotator(0.0, CAMERA_PITCH, 0.0))

    subsystem.save_current_level()

    note = f", не разрешено {unresolved}" if unresolved else ""
    report.line(f"{map_name}: инстансов {placed}, плиток {tiles}{note}")
    return True


def main(report):
    if RefuseIfEditorOpen(report):
        return

    args = sys.argv[1:]
    manifest_dir = args[0] if args else "../artifacts/maps"
    only = set(args[1:])

    script_dir = os.path.dirname(os.path.abspath(__file__))
    models = load_model_map(script_dir)
    game_mode = unreal.load_class(None, "/Script/CorsairsGame.CorsairsGameMode")
    if game_mode is None:
        report.warn("класс режима игры не найден — уровни будут без него")

    manifests = sorted(glob.glob(os.path.join(os.path.abspath(manifest_dir),
                                              "*.objects.json")))
    report.line(f"манифестов найдено: {len(manifests)}")

    built = 0
    for path in manifests:
        map_name = re.sub(r"\.objects\.json$", "", os.path.basename(path))
        if only and map_name not in only:
            continue
        if build_map(report, path, map_name, models, game_mode, script_dir):
            built += 1

    report.line(f"УРОВНЕЙ СОБРАНО: {built}")
    if built == 0:
        report.error("ПРОВАЛ: ни один уровень не собран")


report = Reporter("place_all_maps")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
