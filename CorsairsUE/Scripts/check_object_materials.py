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

from report import Reporter                     # noqa: E402
import material_usage_editor as editor          # noqa: E402


def main(report):
    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    if not editor.load_level(map_name):
        report.error(f"ПРОВАЛ: уровень /Game/Maps/{map_name} не загрузился")
        return 1

    bindings = [
        binding for binding in editor.material_bindings()
        if binding.component_kind == "hism"
    ]
    components = {binding.component_path for binding in bindings}
    materials = {binding.material_path for binding in bindings}
    missing_texture = {
        binding.material_path
        for binding in bindings
        if editor.missing_base_color_texture(binding.material)
    }
    missing_instancing = {
        binding.material_path
        for binding in bindings
        if "instanced_static_meshes" in binding.required
        and not editor.has_usage(
            binding.material, "instanced_static_meshes")
    }
    missing_nanite = {
        binding.material_path
        for binding in bindings
        if "nanite" in binding.required
        and not editor.has_usage(binding.material, "nanite")
    }
    translucent_nanite = {
        binding.component_path
        for binding in bindings
        if "disallow_nanite" in binding.required
    }

    report.line(f"компонентов с инстансами: {len(components)}")
    report.line(f"уникальных HISM материалов: {len(materials)}")
    report.line(
        "HISM материалов без BaseColorTexture/с заглушкой: "
        f"{len(missing_texture)}")
    for path in sorted(missing_texture):
        report.line("  " + path)

    report.line(
        f"usage без Instanced Static Mesh: {len(missing_instancing)}")
    for path in sorted(missing_instancing):
        report.line("  " + path)
    report.line(f"usage без Nanite: {len(missing_nanite)}")
    for path in sorted(missing_nanite):
        report.line("  " + path)
    report.line(
        f"translucent Nanite компонентов: {len(translucent_nanite)}")
    for path in sorted(translucent_nanite):
        report.line("  " + path)

    failures = (
        len(missing_instancing)
        + len(missing_nanite)
        + len(translucent_nanite))
    if not components:
        report.error("ПРОВАЛ: на уровне нет размещённых объектов")
        failures += 1
    elif len(missing_texture) > len(materials) // 2:
        report.error(
            f"ПРОВАЛ: заглушка у {len(missing_texture)} из "
            f"{len(materials)} HISM материалов — "
                     "постройки останутся серыми")
        failures += 1
    if missing_instancing:
        report.error(
            "ПРОВАЛ: есть HISM материалы без Instanced Static Mesh usage")
    if missing_nanite:
        report.error("ПРОВАЛ: есть HISM материалы без Nanite usage")
    if translucent_nanite:
        report.error(
            "ПРОВАЛ: translucent HISM компоненты используют Nanite")
    return failures


def _run():
    report = Reporter("check_object_materials")
    try:
        failures = main(report)
    except Exception as exc:                    # noqa: BLE001
        report.exception(exc)
        raise
    finally:
        report.close()
    if failures:
        raise RuntimeError(
            f"object material checker found {failures} failures")


if __name__ == "__main__":
    _run()
