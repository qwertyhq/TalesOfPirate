"""Расстановка объектов карты на уровне UE по манифесту.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/place_objects.py <карта>.objects.json <путь-уровня> [лимит]"

Пример: `... /tmp/maps-out/garner.objects.json /Game/Maps/Garner 2000`

Что делает и чего НЕ делает. Позиции и повороты берутся из манифеста через
C++-модуль CorsairsImport — тот же путь, которым пойдёт финальная расстановка.
Но связь `modelId` -> файл модели живёт в таблице `scene_objects` игровых
данных, которых в репозитории нет, поэтому вместо моделей ставятся помеченные
пустышки: имя актёра содержит modelId, и когда таблица появится, замена
пустышек на меши будет механической.

Смысл шага сейчас — проверить, что математика размещения верна и карта
получается нужной формы и масштаба.
"""

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


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        unreal.log_error("нужны аргументы: <манифест> <путь-уровня> [лимит]")
        return

    manifest_path, level_path = args[0], args[1]
    limit = int(args[2]) if len(args) > 2 else 0

    manifest, error = load_manifest(manifest_path)
    if manifest is None:
        unreal.log_error(f"МАНИФЕСТ_ОШИБКА {error}")
        return

    objects = list(manifest.objects)
    if limit > 0:
        objects = objects[:limit]

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level(level_path)

    library = unreal.CorsairsSceneManifestLibrary
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    placed = 0
    for index, obj in enumerate(objects):
        location = library.get_object_location(obj, 100.0)
        rotation = library.get_object_rotation(obj)

        actor = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor, location, rotation)
        if actor is None:
            continue

        # modelId в имени: по нему потом подставляется настоящая модель.
        actor.set_actor_label(f"Obj_{obj.model_id}_{index}")
        actor.tags = [unreal.Name(f"modelId={obj.model_id}"),
                      unreal.Name(f"type={obj.type}")]
        placed += 1

    subsystem.save_current_level()

    unreal.log(f"РАССТАВЛЕНО актёров={placed} из объектов={len(objects)} "
               f"уровень={level_path}")


main()
