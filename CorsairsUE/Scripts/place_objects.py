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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402
from scene_placement import (                    # noqa: E402
    effect_summary, load_heights, object_location, split_effects)


def load_manifest(path):
    """Читает манифест через C++-биндинг.

    Путь приводится к абсолютному: рабочий каталог редактора — это
    Engine/Binaries/Mac, а не каталог проекта, и относительный путь
    указывает в пустоту.
    """
    path = os.path.abspath(path)
    if not os.path.exists(path):
        return None, f"файла нет: {path}"

    library = unreal.CorsairsSceneManifestLibrary
    result = library.load_scene_manifest(path)

    # У функции, возвращающей bool при выходных параметрах, UE отдаёт None,
    # когда bool равен false, и кортеж выходных значений, когда true.
    if result is None:
        return None, f"биндинг вернул отказ для {path}"
    if isinstance(result, tuple) and len(result) == 3:
        ok, manifest, error = result
        if not ok:
            return None, error
        return manifest, ""
    if isinstance(result, tuple) and len(result) == 2:
        manifest, error = result
        if error:
            return None, error
        return manifest, ""
    return None, f"неожиданный ответ биндинга: {type(result)}"


def load_model_map(script_dir):
    """Читает таблицу modelId -> список путей ассетов."""
    path = os.path.join(script_dir, "model_map.json")
    if not os.path.exists(path):
        return None, f"нет таблицы моделей: {path} (сначала build_model_map.py)"
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    return data.get("models", {}), ""


def group_by_mesh(objects, models, heights=None):
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

        location = object_location(library, obj, heights)
        rotation = library.get_object_rotation(obj)
        transform = unreal.Transform(location, rotation, unreal.Vector(1.0, 1.0, 1.0))

        for asset in assets:
            groups.setdefault(asset, []).append(transform)

    return groups, unresolved


def add_hism_component(actor):
    """Добавляет актёру компонент инстансированных мешей.

    У `unreal.Actor` нет метода добавления компонента: в UE5 это делается
    через SubobjectDataSubsystem — ту же подсистему, которой пользуется
    панель компонентов редактора. Возвращает компонент либо None.
    """
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
    if isinstance(component, unreal.HierarchicalInstancedStaticMeshComponent):
        return component
    return None


def spawn_instanced(asset_path, transforms):
    """Ставит все вхождения одного меша одним актёром с инстансами.

    Возвращает (число_инстансов, способ). Способ — "инстансы" либо
    "актёры": если компонент добавить не удалось, объекты ставятся
    обычными StaticMeshActor. Это медленнее и тяжелее, но мир виден —
    молча ставить ноль объектов хуже.
    """
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    mesh = unreal.load_asset(asset_path)

    # 79 моделей сцены несут скиннинг — это анимированные объекты вроде
    # мельниц, и Interchange импортирует их скелетными мешами. Инстансирование
    # к ним неприменимо, ставятся обычными актёрами.
    if mesh is None:
        skeletal_path = asset_path.replace("/StaticMeshes/", "/SkeletalMeshes/")
        mesh = unreal.load_asset(skeletal_path)

    if isinstance(mesh, unreal.SkeletalMesh):
        placed = 0
        for transform in transforms:
            actor = actor_subsystem.spawn_actor_from_class(
                unreal.SkeletalMeshActor, transform.translation,
                transform.rotation.rotator())
            if actor is None:
                continue
            actor.skeletal_mesh_component.set_skeletal_mesh(mesh)
            # Метка нужна, чтобы следующий прогон убрал этих актёров.
            actor.set_actor_label(f"Obj_{os.path.basename(asset_path)}")
            placed += 1
        return placed, "скелетные актёры"

    if not isinstance(mesh, unreal.StaticMesh):
        return 0, "нет меша"
    actor = actor_subsystem.spawn_actor_from_class(
        unreal.Actor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if actor is None:
        return 0, "актёр не создан"

    actor.set_actor_label(f"Inst_{os.path.basename(asset_path)}")

    component = add_hism_component(actor)
    if component is not None:
        component.set_static_mesh(mesh)
        for transform in transforms:
            component.add_instance(transform, True)

        # Считаем то, что подтвердил компонент, а не длину списка на входе:
        # молчаливый отказ add_instance иначе выглядел бы как полный успех.
        actual = component.get_instance_count()
        if actual != len(transforms):
            return actual, f"инстансы (принято {actual} из {len(transforms)})"
        return actual, "инстансы"

    actor_subsystem.destroy_actor(actor)

    placed = 0
    for transform in transforms:
        single = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor, transform.translation,
            transform.rotation.rotator())
        if single is None:
            continue
        single.static_mesh_component.set_static_mesh(mesh)
        single.set_actor_label(f"Obj_{os.path.basename(asset_path)}")
        placed += 1
    return placed, "актёры"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    args = sys.argv[1:]
    if len(args) < 2:
        report.error("нужны аргументы: <манифест> <путь-уровня> [лимит]")
        return

    manifest_path, level_path = args[0], args[1]
    limit = int(args[2]) if len(args) > 2 else 0

    script_dir = os.path.dirname(os.path.abspath(__file__))
    models, error = load_model_map(script_dir)
    if models is None:
        report.error(f"ТАБЛИЦА_ОШИБКА {error}")
        return

    manifest, error = load_manifest(manifest_path)
    if manifest is None:
        report.error(f"МАНИФЕСТ_ОШИБКА {error}")
        return

    objects = list(manifest.objects)
    total_in_manifest = len(objects)

    objects, effects = split_effects(objects)

    if limit > 0:
        objects = objects[:limit]

    # Прежняя расстановка убирается: скрипт ставит объекты заново, и без
    # очистки каждый прогон удваивает город. Узнаются наши актёры по метке
    # `Inst_` и по скелетным, поставленным этим же скриптом.
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    removed = 0
    for actor in actor_subsystem.get_all_level_actors():
        label = actor.get_actor_label()
        if label.startswith("Inst_") or label.startswith("Obj_"):
            actor_subsystem.destroy_actor(actor)
            removed += 1
    if removed:
        report.line(f"убрано прежних объектов: {removed}")

    # Карта высот берётся по имени манифеста: garner.objects.json -> garner.
    map_name = os.path.basename(manifest_path).split(".")[0]
    heights, heights_path = load_heights(script_dir, map_name)
    if heights.side:
        report.line(f"карта высот {map_name}: сетка {heights.side}x{heights.side}")
    else:
        report.warn(f"карты высот нет: {heights_path} — "
                    "постройки встанут на голое смещение и пойдут ступеньками")

    groups, unresolved = group_by_mesh(objects, models, heights)

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level(level_path)

    placed = 0
    failed_assets = []
    methods = {}
    for asset_path, transforms in sorted(groups.items()):
        count, method = spawn_instanced(asset_path, transforms)
        methods[method] = methods.get(method, 0) + 1
        if count == 0:
            failed_assets.append(f"{asset_path} ({method})")
        placed += count
    report.line(f"СПОСОБ размещения по мешам: {methods}")

    subsystem.save_current_level()

    skipped = sum(unresolved.values())
    report.line(f"РАССТАВЛЕНО инстансов={placed} мешей={len(groups)} "
                f"уровень={level_path}")
    report.line(f"ОБЪЕКТОВ обработано={len(objects)} "
                f"в_манифесте={total_in_manifest}")

    # Число отложенных эффектов печатается всегда, даже когда оно ожидаемо:
    # это единственный признак того, что часть карты сознательно не поставлена,
    # а не потерялась по дороге.
    if effects:
        count, kinds, top = effect_summary(effects)
        report.line(f"ЭФФЕКТОВ отложено={count} видов={kinds}; "
                    f"частые id: {top}")

    # Пропуски печатаются всегда: молчаливое сокращение выглядит как полный
    # охват, хотя часть мира на уровень не попала.
    if skipped:
        top = sorted(unresolved.items(), key=lambda kv: -kv[1])[:5]
        report.warn(f"ПРОПУЩЕНО объектов={skipped} без записи в scene_objects; "
                    f"частые id: {top}")
    if failed_assets:
        report.warn(f"НЕ_ЗАГРУЗИЛОСЬ мешей={len(failed_assets)}: "
                    f"{failed_assets[:5]}")


report = Reporter("place_objects")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
