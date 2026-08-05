"""Импортирует текстуры рельефа в Content.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/import_terrain_textures.py <каталог-текстур>"

Пример:
    ... Scripts/import_terrain_textures.py ../Client/texture/terrain

Отчёт: `Scripts/reports/import_terrain_textures.txt`.

Текстуры рельефа не приходят вместе с моделями: слои задаются номерами в
данных карты, а не ссылками в файле модели, поэтому конвертер их не трогает.
Импортируются как есть, плоско по имени — так же на них ссылается таблица,
которую готовит `build_terrain_map.py`.

Таблица в игровых данных называет файлы с расширением `.bmp`, а на диске они
лежат в PNG. Поэтому берётся всё, что похоже на изображение, и сопоставление
идёт по имени без расширения.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

DESTINATION = "/Game/Terrain/Textures"
IMAGE_EXTENSIONS = {".png", ".bmp", ".tga", ".jpg", ".jpeg"}


def main(report):
    if RefuseIfEditorOpen(report):
        return

    source_dir = sys.argv[1] if len(sys.argv) > 1 else "../Client/texture/terrain"
    source_dir = os.path.abspath(source_dir)

    if not os.path.isdir(source_dir):
        report.error(f"каталога нет: {source_dir}")
        return

    files = []
    for name in sorted(os.listdir(source_dir)):
        path = os.path.join(source_dir, name)
        if os.path.isfile(path) and os.path.splitext(name)[1].lower() in IMAGE_EXTENSIONS:
            files.append(path)

    report.line(f"изображений найдено: {len(files)}")
    if not files:
        report.error("ПРОВАЛ: импортировать нечего")
        return

    tasks = []
    for path in files:
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = DESTINATION
        task.automated = True
        task.replace_existing = True
        task.save = True
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    # Считаем то, что подтвердил редактор, а не число заданий: молчаливый
    # отказ импорта иначе выглядел бы как успех.
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    imported = [a for a in registry.get_assets_by_path(unreal.Name(DESTINATION),
                                                       recursive=True)
                if str(a.asset_class_path.asset_name) == "Texture2D"]

    report.line(f"ТЕКСТУР В CONTENT: {len(imported)}")
    if len(imported) < len(files):
        report.warn(f"импортировано меньше, чем найдено: {len(imported)} из {len(files)}")
    else:
        report.line("УСПЕХ: все изображения импортированы")


report = Reporter("import_terrain_textures")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
