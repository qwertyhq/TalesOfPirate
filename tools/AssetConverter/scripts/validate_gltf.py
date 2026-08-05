"""Валидация конвертированных glTF импортом в Blender.

Запускается headless, без GUI и без аддонов:

    blender --background --factory-startup --python validate_gltf.py -- <файл.gltf> [...]
    blender --background --factory-startup --python validate_gltf.py -- список.txt

Во втором виде единственный аргумент — текстовый файл со списком путей, по
одному в строке; так удобно прогонять десятки файлов.

Смысл проверки: тесты конвертера подтверждают, что JSON синтаксически верен и
счётчики сходятся, но не что файл примет сторонний импортёр. Здесь его
принимает Blender — независимая реализация спецификации glTF 2.0.

`--factory-startup` обязателен: без него подхватываются пользовательские
настройки и аддоны, которые могут менять поведение импорта.
"""

import sys
import bpy


def clear_scene():
    # read_factory_settings(use_empty=True) в Blender 5.2 оставляет объекты
    # стартового файла, поэтому чистим явно — иначе дефолтная Icosphere
    # (42 вершины, 80 треугольников) подмешивается в статистику.
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # Удаляем из bpy.data, а не из scene.objects: в фоновом режиме объект
    # стартового файла может быть не привязан к активной сцене, но всё равно
    # подхватывается импортёром в её коллекцию.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for mesh in list(bpy.data.meshes):
        bpy.data.meshes.remove(mesh)
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)
    for material in list(bpy.data.materials):
        bpy.data.materials.remove(material)
    for image in list(bpy.data.images):
        bpy.data.images.remove(image)


def count_keyframes(action):
    """Число ключей в действии.

    Начиная с Blender 4.4 действия «слотовые»: кривые лежат в
    layers[].strips[].channelbag(slot).fcurves, а прямого action.fcurves больше
    нет. Поддерживаем оба варианта.
    """
    total = 0

    legacy = getattr(action, "fcurves", None)
    if legacy is not None:
        for fcurve in legacy:
            total += len(fcurve.keyframe_points)
        return total

    for layer in action.layers:
        for strip in layer.strips:
            for slot in action.slots:
                bag = strip.channelbag(slot)
                if bag is None:
                    continue
                for fcurve in bag.fcurves:
                    total += len(fcurve.keyframe_points)
    return total


def report(path):
    clear_scene()

    try:
        bpy.ops.import_scene.gltf(filepath=path)
    except Exception as exc:  # импортёр Blender бросает при невалидном файле
        print(f"РЕЗУЛЬТАТ {path} ОШИБКА_ИМПОРТА {exc}")
        return False

    scene_objects = list(bpy.context.scene.objects)
    # Импортёр Blender подставляет объект-заглушку (икосферу на 42 вершины) для
    # skin, на который не ссылается ни один меш. В файлах скелетов такой skin
    # присутствует намеренно — он несёт обратные bind-матрицы. Заглушку из
    # статистики исключаем, иначе она искажает счётчики.
    meshes = [o for o in scene_objects
              if o.type == "MESH" and not o.name.startswith("Icosphere")]
    armatures = [o for o in scene_objects if o.type == "ARMATURE"]
    empties = [o for o in scene_objects if o.type == "EMPTY"]

    for o in meshes:
        o.data.calc_loop_triangles()

    verts = sum(len(o.data.vertices) for o in meshes)
    tris = sum(len(o.data.loop_triangles) for o in meshes)
    materials = len(bpy.data.materials)
    images = len(bpy.data.images)
    bones = sum(len(a.data.bones) for a in armatures)

    keyframes = 0
    frame_end = 0.0
    for action in bpy.data.actions:
        keyframes += count_keyframes(action)
        if action.frame_range[1] > frame_end:
            frame_end = action.frame_range[1]

    skinned = 0
    for o in meshes:
        for v in o.data.vertices:
            if len(v.groups) > 0:
                skinned += 1

    print(
        f"РЕЗУЛЬТАТ {path} OK "
        f"мешей={len(meshes)} вершин={verts} треугольников={tris} "
        f"материалов={materials} изображений={images} "
        f"арматур={len(armatures)} костей={bones} пустышек={len(empties)} "
        f"действий={len(bpy.data.actions)} ключей={keyframes} "
        f"последний_кадр={frame_end:.0f} вершин_со_скиннингом={skinned}"
    )
    return True


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    # Один аргумент, указывающий на .txt — список путей, по одному в строке.
    # Так удобнее передавать десятки файлов, не упираясь в разбор argv.
    if len(args) == 1 and args[0].endswith(".txt"):
        with open(args[0], encoding="utf-8") as handle:
            args = [line.strip() for line in handle if line.strip()]
    if not args:
        print("нет входных файлов")
        return

    failed = 0
    for path in args:
        if not report(path):
            failed += 1

    print(f"ИТОГО файлов={len(args)} ошибок={failed}")


main()
