"""Проверяет, доехали ли текстуры до объектов на уровне.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_object_materials.py [карта]"

Отчёт: `Scripts/reports/check_object_materials.txt`.

Материал, записанный в ассет меша, и материал, которым его рисуют на уровне, —
разные вещи: компонент держит собственный список и перекрывает ассет. С
рельефом так и вышло. Здесь проверяется то же самое для построек.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

PLACEHOLDERS = ("WorldGrid", "DefaultMaterial", "T_White")


def main(report):
    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(
        f"/Game/Maps/{map_name}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

    total = 0
    placeholder = 0
    samples = []

    for actor in actors:
        for component in actor.get_components_by_class(
                unreal.HierarchicalInstancedStaticMeshComponent):
            if component.get_instance_count() == 0:
                continue
            total += 1
            materials = component.get_materials()
            names = [m.get_name() if m else "нет" for m in materials]

            # Материал может быть свой, а текстуры в нём — заглушечные: тогда
            # постройка серая, хотя формально всё назначено.
            white = False
            for material in materials:
                if not isinstance(material, unreal.MaterialInstanceConstant):
                    continue
                texture = unreal.MaterialEditingLibrary \
                    .get_material_instance_texture_parameter_value(
                        material, unreal.Name("BaseColorTexture"))
                if texture is None or any(p in texture.get_name() for p in PLACEHOLDERS):
                    white = True

            if white or any(any(p in n for p in PLACEHOLDERS) for n in names):
                placeholder += 1
                if len(samples) < 6:
                    mesh = component.static_mesh
                    samples.append(
                        f"{mesh.get_name() if mesh else '?'}: {names}")

    report.line(f"компонентов с инстансами: {total}")
    report.line(f"из них с заглушкой: {placeholder}")
    for line in samples:
        report.line("  " + line)

    if total == 0:
        report.error("ПРОВАЛ: на уровне нет размещённых объектов")
    elif placeholder > total // 2:
        report.error(f"ПРОВАЛ: заглушка у {placeholder} из {total} — "
                     "постройки останутся серыми")


report = Reporter("check_object_materials")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
