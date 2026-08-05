"""Связывает модели персонажей со скелетом их анимаций.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/link_character_skeletons.py"

Отчёт: `Scripts/reports/link_character_skeletons.txt`.

Interchange заводит отдельный ассет `Skeleton` на каждый импортируемый файл,
поэтому у тела получается `0000000000_Skeleton`, а у дорожки —
`0000_Skeleton`. Деревья костей после правки конвертера совпадают полностью,
но UE решает о применимости дорожки по ассету, а не по структуре: пока это
разные объекты, анимация к телу не привязывается.

Скелет модели переназначить нельзя — свойство доступно только для чтения, а
задавать его при импорте пришлось бы отдельным ассетом конвейера на каждую из
2858 моделей. Вместо этого скелеты объявляются совместимыми: UE проигрывает
дорожку с совместимого скелета, если деревья костей сходятся, — а они теперь
сходятся по построению.

Соответствие по имени: `0000000000` относится к `0000`, первые четыре цифры.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

MODEL_ROOT = "/Game/All"
ANIMATION_ROOT = "/Game/Animations"


def skeleton_for(asset_name):
    """Скелет по имени модели: первые четыре цифры — номер файла .lab."""
    if len(asset_name) < 4 or not asset_name[:4].isdigit():
        return None
    bone = asset_name[:4]
    return f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}_Skeleton"


def main(report):
    if RefuseIfEditorOpen(report):
        return

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = [a for a in registry.get_assets_by_path(unreal.Name(MODEL_ROOT), recursive=True)
              if str(a.asset_class_path.asset_name) == "SkeletalMesh"]

    report.line(f"скелетных мешей: {len(assets)}")

    linked = 0
    already = 0
    no_skeleton = 0
    failed = []

    for asset in assets:
        name = str(asset.asset_name)
        path = skeleton_for(name)
        if path is None:
            no_skeleton += 1
            continue

        skeleton = unreal.load_asset(path)
        if not isinstance(skeleton, unreal.Skeleton):
            no_skeleton += 1
            continue

        mesh = unreal.load_asset(f"{asset.package_name}.{name}")
        if not isinstance(mesh, unreal.SkeletalMesh):
            continue

        mesh_skeleton = mesh.skeleton
        if mesh_skeleton is None or mesh_skeleton == skeleton:
            already += 1
            continue

        try:
            # Совместимость объявляется в обе стороны: дорожку могут спросить
            # и у скелета модели, и у скелета анимации.
            skeleton.add_compatible_skeleton(mesh_skeleton)
            mesh_skeleton.add_compatible_skeleton(skeleton)
            unreal.EditorAssetLibrary.save_loaded_asset(skeleton, only_if_is_dirty=False)
            unreal.EditorAssetLibrary.save_loaded_asset(mesh_skeleton, only_if_is_dirty=False)
            linked += 1
        except Exception as exc:                      # noqa: BLE001
            if len(failed) < 5:
                failed.append(f"{name}: {exc}")

    report.line(f"ОБЪЯВЛЕНО СОВМЕСТИМЫМИ: {linked}")
    report.line(f"  уже совместимы:  {already}")
    report.line(f"  скелета нет:     {no_skeleton}")
    if failed:
        report.warn(f"не удалось: {len(failed)} — {failed}")
    if linked == 0 and already == 0:
        report.error("ПРОВАЛ: ни одна модель не связана со скелетом анимации")


report = Reporter("link_character_skeletons")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
