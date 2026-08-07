"""Проверяет effective usage flags материалов уровня без записи ассетов.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \
        -script="<путь>/check_material_usage.py [карта]"

Отчёт: `Scripts/reports/check_material_usage.txt`.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from report import Reporter  # noqa: E402
import material_usage_editor as editor  # noqa: E402


def _write_paths(report, heading, paths):
    report.line(f"{heading}={len(paths)}")
    for path in sorted(paths):
        report.line(f"  {path}")


def main(report):
    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    if not editor.load_level(map_name):
        report.error(f"ПРОВАЛ: уровень /Game/Maps/{map_name} не загрузился")
        return 1

    bindings = list(editor.material_bindings())
    material_paths = {binding.material_path for binding in bindings}
    component_paths = {binding.component_path for binding in bindings}

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
    missing_base_color = {
        binding.material_path
        for binding in bindings
        if editor.missing_base_color_texture(binding.material)
    }

    report.line(f"КАРТА={map_name}")
    report.line(f"КОМПОНЕНТОВ={len(component_paths)}")
    _write_paths(report, "УНИКАЛЬНЫХ_МАТЕРИАЛОВ", material_paths)
    _write_paths(
        report, "НЕТ_INSTANCED_STATIC_MESHES", missing_instancing)
    _write_paths(report, "НЕТ_NANITE", missing_nanite)
    _write_paths(
        report, "TRANSLUCENT_NANITE_КОМПОНЕНТОВ", translucent_nanite)
    _write_paths(report, "НЕТ_BASE_COLOR_TEXTURE", missing_base_color)

    failures = (
        len(missing_instancing)
        + len(missing_nanite)
        + len(translucent_nanite))
    if missing_instancing:
        report.error(
            "ПРОВАЛ: есть материалы без Instanced Static Mesh usage")
    if missing_nanite:
        report.error("ПРОВАЛ: есть материалы без Nanite usage")
    if translucent_nanite:
        report.error(
            "ПРОВАЛ: translucent-компоненты пытаются использовать Nanite")
    return failures


def _run():
    report = Reporter("check_material_usage")
    try:
        failures = main(report)
    except Exception as exc:  # noqa: BLE001
        report.exception(exc)
        raise
    finally:
        report.close()
    if failures:
        raise RuntimeError(
            f"material usage checker found {failures} failures")


if __name__ == "__main__":
    _run()
