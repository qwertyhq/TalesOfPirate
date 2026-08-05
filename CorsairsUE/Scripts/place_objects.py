"""Расстановка объектов карты на уровне UE по манифесту.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/place_objects.py <карта>.objects.json <путь-уровня> [лимит]"

Пример:
    ... Scripts/place_objects.py ../artifacts/maps/garner.objects.json /Game/Maps/Garner

Позиции и повороты берутся из манифеста через C++-модуль CorsairsImport.
Связь `modelId` -> ассеты меша читается из `Scripts/model_map.json`, который
готовит `build_model_map.py` по таблице `scene_objects` игровых данных.

Объекты ставятся инстансами, а не отдельными актёрами: на `garner` их 50 017,
и по актёру на каждый редактор не выдержит — а инстансы одного меша к тому же
рисуются одним вызовом. На каждый уникальный меш заводится один актёр с
компонентом HierarchicalInstancedStaticMesh, в него добавляются все вхождения.

Одному `modelId` может отвечать несколько мешей: `.lmo` — дерево, и конвертер
разворачивает его в отдельные glTF. Все части ставятся в одну точку; смещение
части внутри модели уже запечено в её меш при импорте.
"""

import json
import os
import sys

import unreal


def load_manifest(path):
    library = unreal.CorsairsSceneManifestLibrary
    result = library.load_scene_manifest(path)
    if isinstance(result, tuple) and len(result) == 3:
        ok, manifest, error = result
    elif isinstance(result, tuple) and len(result) == 2:
        manifest, error = result
        ok = not error
    else:
        return None, f"неожиданный ответ биндинга: {type(result)}"

    if not ok:
        return None, error
    return manifest, ""


def load_model_map(script_dir):
    """Читает таблицу modelId -> список путей ассетов."""
    path = os.path.join(script_dir, "model_map.json")
    if not os.path.exists(path):
        return None, f"нет таблицы моделей: {path} (сначала build_model_map.py)"
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    return data.get("models", {}), ""


def group_by_mesh(objects, models):
    """Раскладывает объекты по ассетам мешей.

    Возвращает (группы, не_разрешено), где группы — словарь
    путь_ассета -> список трансформаций.
    """
    library = unreal.CorsairsSceneManifestLibrary
    groups = {}
    unresolved = {}

    for obj in objects:
        assets = models.get(str(obj.model_id))
        if not assets:
            unresolved[obj.model_id] = unresolved.get(obj.model_id, 0) + 1
            continue

        location = library.get_object_location(obj, 100.0)
        rotation = library.get_object_rotation(obj)
        transform = unreal.Transform(location, rotation, unreal.Vector(1.0, 1.0, 1.0))

        for asset in assets:
            groups.setdefault(asset, []).append(transform)

    return groups, unresolved


def spawn_instanced(asset_path, transforms):
    """Ставит все вхождения одного меша одним актёром с инстансами.

    Возвращает число размещённых инстансов; 0 — если меш не загрузился.
    """
    mesh = unreal.load_asset(asset_path)
    if not isinstance(mesh, unreal.StaticMesh):
        return 0

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = actor_subsystem.spawn_actor_from_class(
        unreal.Actor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        return 0

    actor.set_actor_label(f"Inst_{os.path.basename(asset_path)}")

    component = actor.add_component_by_class(
        unreal.HierarchicalInstancedStaticMeshComponent,
        False, unreal.Transform(), False)
    if component is None:
        actor_subsystem.destroy_actor(actor)
        return 0

    component.set_static_mesh(mesh)
    for transform in transforms:
        component.add_instance(transform, True)

    return len(transforms)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        unreal.log_error("нужны аргументы: <манифест> <путь-уровня> [лимит]")
        return

    manifest_path, level_path = args[0], args[1]
    limit = int(args[2]) if len(args) > 2 else 0

    script_dir = os.path.dirname(os.path.abspath(__file__))
    models, error = load_model_map(script_dir)
    if models is None:
        unreal.log_error(f"ТАБЛИЦА_ОШИБКА {error}")
        return

    manifest, error = load_manifest(manifest_path)
    if manifest is None:
        unreal.log_error(f"МАНИФЕСТ_ОШИБКА {error}")
        return

    objects = list(manifest.objects)
    total_in_manifest = len(objects)
    if limit > 0:
        objects = objects[:limit]

    groups, unresolved = group_by_mesh(objects, models)

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level(level_path)

    placed = 0
    failed_assets = []
    for asset_path, transforms in sorted(groups.items()):
        count = spawn_instanced(asset_path, transforms)
        if count == 0:
            failed_assets.append(asset_path)
        placed += count

    subsystem.save_current_level()

    skipped = sum(unresolved.values())
    unreal.log(f"РАССТАВЛЕНО инстансов={placed} мешей={len(groups)} "
               f"уровень={level_path}")
    unreal.log(f"ОБЪЕКТОВ обработано={len(objects)} в_манифесте={total_in_manifest}")

    # Пропуски печатаются всегда: молчаливое сокращение выглядит как полный
    # охват, хотя часть мира на уровень не попала.
    if skipped:
        top = sorted(unresolved.items(), key=lambda kv: -kv[1])[:5]
        unreal.log_warning(
            f"ПРОПУЩЕНО объектов={skipped} без записи в scene_objects; "
            f"частые id: {top}")
    if failed_assets:
        unreal.log_warning(
            f"НЕ_ЗАГРУЗИЛОСЬ мешей={len(failed_assets)}: {failed_assets[:5]}")


main()
