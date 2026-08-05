"""Импорт конвертированных ассетов в UE и проверка результата.

Запускается редактором в headless-режиме:

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/import_assets.py <каталог-с-gltf> <путь-в-Content>"

Скрипт импортирует каждый .gltf через Interchange и печатает, что реально
получилось: тип созданного ассета, число вершин, костей, материалов. Это
проверка того, что конвертер отдаёт файлы, пригодные именно для UE, —
корректность самого glTF уже подтверждена импортом в Blender.
"""

import os
import sys

import unreal


def gather_gltf(source_dir):
    found = []
    for root, _dirs, files in os.walk(source_dir):
        for name in files:
            if name.lower().endswith(".gltf"):
                found.append(os.path.join(root, name))
    return sorted(found)


def build_task(path, destination):
    task = unreal.AssetImportTask()
    task.filename = path
    task.destination_path = destination
    task.automated = True
    task.replace_existing = True
    task.save = True
    return task


def describe(asset):
    """Краткое описание импортированного ассета для отчёта."""
    if isinstance(asset, unreal.StaticMesh):
        lod = asset.get_num_vertices(0) if asset.get_num_lods() > 0 else 0
        return f"StaticMesh вершин={lod} материалов={len(asset.static_materials)}"

    if isinstance(asset, unreal.SkeletalMesh):
        skeleton = asset.skeleton
        bones = len(skeleton.get_editor_property("bone_tree")) if skeleton else 0
        return f"SkeletalMesh материалов={len(asset.materials)} костей={bones}"

    if isinstance(asset, unreal.Skeleton):
        return f"Skeleton костей={len(asset.get_editor_property('bone_tree'))}"

    if isinstance(asset, unreal.AnimSequence):
        return (f"AnimSequence кадров={asset.get_editor_property('number_of_sampled_frames')} "
                f"длительность={asset.get_play_length():.3f}с")

    if isinstance(asset, unreal.Texture2D):
        return f"Texture2D {asset.blueprint_get_size_x()}x{asset.blueprint_get_size_y()}"

    return type(asset).__name__


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        unreal.log_error("нужны аргументы: <каталог-с-gltf> <путь-в-Content>")
        return

    source_dir, destination = args[0], args[1]
    files = gather_gltf(source_dir)
    if not files:
        unreal.log_error(f"в {source_dir} нет .gltf")
        return

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    imported_total = 0
    failed = []

    for path in files:
        task = build_task(path, destination)
        tools.import_asset_tasks([task])

        created = list(task.get_editor_property("imported_object_paths"))
        if not created:
            failed.append(path)
            unreal.log_warning(f"ИМПОРТ_ПУСТ {path}")
            continue

        imported_total += len(created)
        for object_path in created:
            asset = unreal.load_asset(object_path)
            if asset is None:
                unreal.log_warning(f"НЕ_ЗАГРУЗИЛСЯ {object_path}")
                continue
            unreal.log(f"АССЕТ {os.path.basename(path)} -> {object_path} :: {describe(asset)}")

    unreal.log(f"ИТОГО файлов={len(files)} ассетов={imported_total} без_результата={len(failed)}")
    for path in failed:
        unreal.log(f"  БЕЗ_РЕЗУЛЬТАТА {path}")


main()
