"""Осматривает модель тела персонажа.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_body.py [путь-к-мешу]"

Отчёт: `Scripts/reports/check_body.txt`.

«Видно только голову» может значить и что тело не загрузилось, и что скелет
не совпал с анимацией и меш схлопнулся, и что модель на порядок мельче
капсулы. Отличить их можно только по числам: границы меша, число костей,
совпадение скелета с анимацией.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

DEFAULT_MESH = "/Game/All/0000000000/SkeletalMeshes/0000000000"
DEFAULT_ANIM = "/Game/Animations/0000/SkeletalMeshes/0000_Anim"


def main(report):
    mesh_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MESH
    mesh = unreal.load_asset(mesh_path)
    if not isinstance(mesh, unreal.SkeletalMesh):
        report.error(f"ПРОВАЛ: {mesh_path} — не скелетный меш")
        return

    bounds = mesh.get_bounds()
    extent = bounds.box_extent
    report.line(f"меш {mesh_path}")
    report.line(f"  габариты (полуразмер): "
                f"{extent.x:.1f} x {extent.y:.1f} x {extent.z:.1f} см")

    skeleton = mesh.skeleton
    if skeleton is None:
        report.error("ПРОВАЛ: у меша нет скелета")
        return
    bones = skeleton.get_editor_property("bone_tree")
    report.line(f"  скелет {skeleton.get_name()}, костей {len(bones)}")

    materials = mesh.get_editor_property("materials")
    report.line(f"  разделов материала: {len(materials)}")
    for index, slot in enumerate(materials[:5]):
        interface = slot.material_interface
        report.line(f"    {index}: {interface.get_name() if interface else 'нет'}")

    anim = unreal.load_asset(DEFAULT_ANIM)
    if isinstance(anim, unreal.AnimSequence):
        anim_skeleton = anim.get_editor_property("skeleton")
        same = anim_skeleton == skeleton
        report.line(f"  анимация {DEFAULT_ANIM}: скелет "
                    f"{'тот же' if same else 'ДРУГОЙ — ' + anim_skeleton.get_name()}")
        if not same:
            report.error("ПРОВАЛ: скелет анимации не совпадает со скелетом меша")

    # Рост персонажа задаёт капсула высотой 176 см. Модель заметно меньше
    # означает, что в кадре она выглядит игрушкой рядом с домами.
    height = extent.z * 2.0
    report.line(f"  высота модели: {height:.0f} см (капсула — 176)")
    if height < 100.0:
        report.error(f"ПРОВАЛ: модель втрое ниже капсулы — {height:.0f} см")
    elif height > 400.0:
        report.error(f"ПРОВАЛ: модель непомерно велика — {height:.0f} см")


report = Reporter("check_body")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
