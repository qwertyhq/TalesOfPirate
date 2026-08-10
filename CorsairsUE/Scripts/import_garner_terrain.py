"""Импортирует 64 rigid-Q страницы рельефа garner в /Game/Terrain/GarnerCityQ.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/import_garner_terrain.py <каталог-с-gltf> /Game/Terrain/GarnerCityQ"

Пример:
    ... Scripts/import_garner_terrain.py ../artifacts/terrain/garner /Game/Terrain/GarnerCityQ

Отчёт: `Scripts/reports/import_garner_terrain.txt`.

Каждая страница — отдельный StaticMesh с мировыми координатами в вершинах,
поэтому у актёров достаточно нулевого transform. Коллизия выставляется
CTF_USE_COMPLEX_AS_SIMPLE: страницы — это земля, по ней должны ходить
капсулой, пока gameplay-земля ходит по half-meter grid.
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

EXPECTED_PAGES = 64
EXPECTED_MAP_STEM = "garner"


def imported_mesh_name(stem):
    """Имя ассета после sanitize Interchange: точки становятся подчёрками."""
    return stem.replace(".", "_")


def import_one_page(gltf_path, destination):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = gltf_path
    task.destination_path = destination
    task.automated = True
    task.replace_existing = True
    task.save = True
    tools.import_asset_tasks([task])
    # В UE 5.8 get_objects() дожидается завершения AssetImportTask.
    import_result_objects = list(task.get_objects())
    if not import_result_objects:
        raise RuntimeError(f"импорт {gltf_path} не вернул ни одного объекта")

    mesh_name = imported_mesh_name(os.path.splitext(os.path.basename(gltf_path))[0])
    candidates = []
    for object_path in task.get_editor_property("imported_object_paths"):
        asset = unreal.load_asset(object_path)
        if isinstance(asset, unreal.StaticMesh) and asset.get_name() == mesh_name:
            candidates.append(asset)
    if len(candidates) != 1:
        raise RuntimeError(
            f"импорт {gltf_path}: StaticMesh {mesh_name} не найден или "
            f"дублируется ({len(candidates)})")
    return candidates[0]


def require_complex_collision(mesh):
    body_setup = mesh.get_editor_property("body_setup")
    if body_setup is None:
        raise RuntimeError(f"у меша {mesh.get_path_name()} нет body_setup")
    body_setup.set_editor_property(
        "collision_trace_flag",
        unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    if not unreal.EditorAssetLibrary.save_loaded_asset(
            mesh, only_if_is_dirty=False):
        raise RuntimeError(f"меш {mesh.get_path_name()} не сохранился")


def main(report):
    if RefuseIfEditorOpen(report):
        raise RuntimeError("графический UnrealEditor открыт")

    if len(sys.argv) != 3:
        raise RuntimeError(
            "нужны аргументы: <каталог-с-gltf> <путь-content>, "
            f"получено: {sys.argv[1:]}")
    source_dir = os.path.abspath(sys.argv[1])
    content_root = sys.argv[2].rstrip("/")
    if not content_root.startswith("/Game/"):
        raise RuntimeError(f"content root должен начинаться с /Game/: {content_root}")

    gltf_paths = sorted(glob.glob(os.path.join(source_dir, "*.gltf")))
    map_gltfs = [
        path for path in gltf_paths
        if os.path.basename(path).startswith(f"{EXPECTED_MAP_STEM}.terrain_")
    ]
    if len(map_gltfs) != EXPECTED_PAGES:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: страниц {len(map_gltfs)}, "
            f"ожидалось {EXPECTED_PAGES} в {source_dir}")

    imported = 0
    for gltf_path in map_gltfs:
        mesh = import_one_page(gltf_path, content_root)
        require_complex_collision(mesh)
        imported += 1
        report.line(
            f"PAGE {os.path.basename(gltf_path)} -> {mesh.get_path_name()} "
            "collision=ComplexAsSimple")

    if imported != EXPECTED_PAGES:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: импортировано {imported}, "
            f"ожидалось {EXPECTED_PAGES}")
    report.line(
        f"ИТОГО PASS: pages={imported}/{EXPECTED_PAGES} root={content_root}")


report = Reporter("import_garner_terrain")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
    raise
finally:
    report.close()
