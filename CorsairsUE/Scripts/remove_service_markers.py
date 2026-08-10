"""Убирает с карты сервисные маркеры, отрендеренные как здания.

В `scene_objects` поле `type` — это `SCENEOBJ_TYPE` оригинала (Scene.h:55):
0 — обычный объект, 1 — место сидения/прислонения (POSE), 2 — маркер
проходимости, 3 — точечный источник света, 6 — источник звука. Оригинальный
клиент раскладывает их по отдельным спискам и как геометрию не рисует —
их меши (yyyy0**.lmo) — цветные квады для редактора карт.

Наша расстановка ставила всё подряд, и 815 маркеров garner стали красными,
синими и зелёными плашками на площади: 266 «Sit», 41 «Lean», 269 маркеров
проходимости, 236 ламп. Ровно они были «красными плашками» и на сверках
эталонного квартала.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/remove_service_markers.py"

Отчёт: `Scripts/reports/remove_service_markers.txt`.
"""

import json
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
GAMEDATA = os.path.join(REPO, "databases", "gamedata.sqlite")
MODEL_MAP = os.path.join(REPO, "artifacts", "garner_model_map.json")
LEVEL_PATH = "/Game/Maps/Garner"


def service_asset_paths(report):
    """Пути ассетов всех сервисных моделей (type != 0)."""
    db = sqlite3.connect(GAMEDATA)
    ids = [str(row[0]) for row in
           db.execute("SELECT id FROM scene_objects WHERE type != 0")]
    db.close()

    with open(MODEL_MAP, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    models = data.get("models", data)

    paths = set()
    for model_id in ids:
        for part in models.get(model_id, []):
            if isinstance(part, dict):
                part = part.get("asset") or part.get("path") or ""
            if part:
                paths.add(str(part).split(".")[0])
    report.line(f"сервисных моделей: {len(ids)}, ассетов: {len(paths)}")
    return paths


def component_mesh_path(component):
    mesh = None
    if isinstance(component, unreal.StaticMeshComponent):
        mesh = component.static_mesh
    elif isinstance(component, unreal.SkeletalMeshComponent):
        mesh = component.get_skeletal_mesh_asset()
    if mesh is None:
        return ""
    return mesh.get_path_name().split(".")[0]


def main(report):
    if RefuseIfEditorOpen(report):
        return

    targets = service_asset_paths(report)
    if not targets:
        report.error("список сервисных ассетов пуст")
        return

    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(LEVEL_PATH):
        report.error(f"уровень не открылся: {LEVEL_PATH}")
        return

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    removed_actors = 0
    removed_instances = 0
    for actor in actor_subsystem.get_all_level_actors():
        hit = False
        instances = 0
        for component in actor.get_components_by_class(
                unreal.StaticMeshComponent):
            if component_mesh_path(component) in targets:
                hit = True
                if isinstance(component,
                              unreal.InstancedStaticMeshComponent):
                    instances += component.get_instance_count()
        if not hit:
            for component in actor.get_components_by_class(
                    unreal.SkeletalMeshComponent):
                if component_mesh_path(component) in targets:
                    hit = True
        if hit:
            actor_subsystem.destroy_actor(actor)
            removed_actors += 1
            removed_instances += instances

    subsystem.save_current_level()
    report.line(f"УБРАНО актёров={removed_actors} "
                f"инстансов={removed_instances}")
    if removed_actors == 0:
        report.warn("ничего не убрано — либо маркеры уже сняты, либо "
                    "пути ассетов не совпали со сценой")
    else:
        report.line(f"УСПЕХ: {LEVEL_PATH}")


report = Reporter("remove_service_markers")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
