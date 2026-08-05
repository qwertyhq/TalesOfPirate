"""Проверка чтения манифеста сцены модулем CorsairsImport.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_manifest.py <карта>.objects.json"

Читает манифест C++-кодом модуля (а не питоном), чтобы проверять именно тот
путь, которым потом пойдёт расстановка актёров, и печатает разброс мировых
координат — если координаты остались относительными внутри секции, всё
сожмётся в пределы одной секции и это будет видно.
"""

import sys

import unreal


def main():
    args = sys.argv[1:]
    if not args:
        unreal.log_error("нужен аргумент: <карта>.objects.json")
        return

    path = args[0]
    library = unreal.CorsairsSceneManifestLibrary

    # UE отдаёт out-параметры кортежем. Состав зависит от того, как биндинг
    # трактует bool-возврат, поэтому разбираем длину явно, а не жёстким
    # распаковыванием.
    result = library.load_scene_manifest(path)
    if isinstance(result, tuple) and len(result) == 3:
        ok, manifest, error = result
    elif isinstance(result, tuple) and len(result) == 2:
        manifest, error = result
        ok = bool(error) is False
    else:
        unreal.log_error(f"МАНИФЕСТ_НЕОЖИДАННЫЙ_ОТВЕТ {type(result)} {result}")
        return

    if not ok:
        unreal.log_error(f"МАНИФЕСТ_ОШИБКА {error}")
        return

    objects = manifest.objects
    unreal.log(f"МАНИФЕСТ секций={manifest.section_cnt_x}x{manifest.section_cnt_y} "
               f"размер_секции={manifest.section_width}x{manifest.section_height} "
               f"объектов={len(objects)}")

    if not objects:
        unreal.log_error("МАНИФЕСТ_ПУСТ объектов нет")
        return

    xs, ys, zs = [], [], []
    model_ids = set()
    for obj in objects:
        location = library.get_object_location(obj, 100.0)
        xs.append(location.x)
        ys.append(location.y)
        zs.append(location.z)
        model_ids.add(obj.model_id)

    unreal.log(f"КООРДИНАТЫ X=[{min(xs):.0f}, {max(xs):.0f}] "
               f"Y=[{min(ys):.0f}, {max(ys):.0f}] Z=[{min(zs):.0f}, {max(zs):.0f}]")
    unreal.log(f"МОДЕЛИ уникальных_id={len(model_ids)}")

    # Размер карты в единицах UE: секции * тайлов в секции * 100.
    span = manifest.section_cnt_x * manifest.section_width * 100
    spread_x = max(xs) - min(xs)
    if spread_x < span * 0.1:
        unreal.log_error(
            f"КООРДИНАТЫ_ПОДОЗРИТЕЛЬНЫ разброс по X {spread_x:.0f} мал "
            f"относительно карты {span} — похоже, координаты остались "
            f"относительными внутри секции")
    else:
        unreal.log(f"РАЗБРОС_ОК по X {spread_x:.0f} при размере карты {span}")

    first = objects[0]
    location = library.get_object_location(first, 100.0)
    rotation = library.get_object_rotation(first)
    unreal.log(f"ПРИМЕР modelId={first.model_id} позиция=({location.x:.0f}, "
               f"{location.y:.0f}, {location.z:.0f}) поворот_yaw={rotation.yaw:.1f}")


main()
