"""Делает usage flags материалов уровня явными и идемпотентными.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \
        -script="<путь>/fix_material_usage.py [карта]"

Отчёт: `Scripts/reports/fix_material_usage.txt`.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal  # noqa: E402
from report import Reporter, RefuseIfEditorOpen  # noqa: E402
import material_usage_editor as editor  # noqa: E402


def _texture_path(material):
    texture = editor.base_color_texture(material)
    return texture.get_path_name() if texture else ""


def _material_invariants(material):
    return (
        editor.material_parent_path(material),
        editor.material_blend_mode(material),
        _texture_path(material),
    )


def main(report):
    if RefuseIfEditorOpen(report):
        return 1

    map_name = sys.argv[1] if len(sys.argv) > 1 else "Garner"
    if not editor.load_level(map_name):
        report.error(f"ПРОВАЛ: уровень /Game/Maps/{map_name} не загрузился")
        return 1

    bindings = list(editor.material_bindings())
    changed_materials = {}
    changed_components = {}
    invariants = {}
    failures = 0

    for binding in bindings:
        for usage_name in ("instanced_static_meshes", "nanite"):
            if usage_name not in binding.required:
                continue

            material = binding.material
            effective = editor.has_usage(material, usage_name)
            if not isinstance(material, unreal.MaterialInstanceConstant):
                if not effective:
                    report.error(
                        f"ПРОВАЛ: {binding.material_path} не MIC и не имеет "
                        f"usage {usage_name}")
                    failures += 1
                continue

            overridden = editor.has_usage_override(material, usage_name)
            if effective and overridden:
                continue

            if binding.material_path not in invariants:
                invariants[binding.material_path] = _material_invariants(
                    material)

            unreal.MaterialEditingLibrary.set_material_usage_override(
                material,
                editor.USAGE_ENUMS[usage_name],
                True,
                True)
            if not editor.has_usage(material, usage_name):
                report.error(
                    f"ПРОВАЛ: usage {usage_name} не включился у "
                    f"{binding.material_path}")
                failures += 1
                continue
            if not editor.has_usage_override(material, usage_name):
                report.error(
                    f"ПРОВАЛ: override {usage_name} не включился у "
                    f"{binding.material_path}")
                failures += 1
                continue
            changed_materials[binding.material_path] = material

        if "disallow_nanite" not in binding.required:
            continue
        component = binding.component
        if component.get_editor_property("disallow_nanite"):
            continue
        component.set_editor_property("disallow_nanite", True)
        if not component.get_editor_property("disallow_nanite"):
            report.error(
                f"ПРОВАЛ: bDisallowNanite не установился у "
                f"{binding.component_path}")
            failures += 1
            continue
        changed_components[binding.component_path] = component

    for binding in bindings:
        expected = invariants.get(binding.material_path)
        if expected is None:
            continue
        actual = _material_invariants(binding.material)
        if actual != expected:
            report.error(
                f"ПРОВАЛ: parent/blend/texture изменились у "
                f"{binding.material_path}: {expected!r} -> {actual!r}")
            failures += 1
            changed_materials.pop(binding.material_path, None)

    saved_materials = 0
    for material_path, material in sorted(changed_materials.items()):
        if unreal.EditorAssetLibrary.save_loaded_asset(
                material, only_if_is_dirty=True):
            saved_materials += 1
        else:
            report.error(f"ПРОВАЛ: MIC не сохранился: {material_path}")
            failures += 1

    if changed_components:
        saved = unreal.get_editor_subsystem(
            unreal.LevelEditorSubsystem).save_current_level()
        if not saved:
            report.error("ПРОВАЛ: уровень с bDisallowNanite не сохранился")
            failures += 1

    report.line(f"КАРТА={map_name}")
    report.line(f"ИСПРАВЛЕНО_МАТЕРИАЛОВ={saved_materials}")
    report.line(f"ИСПРАВЛЕНО_КОМПОНЕНТОВ={len(changed_components)}")
    return failures


def _run():
    report = Reporter("fix_material_usage")
    try:
        failures = main(report)
    except Exception as exc:  # noqa: BLE001
        report.exception(exc)
        raise
    finally:
        report.close()
    if failures:
        raise RuntimeError(f"material usage fixer had {failures} failures")


if __name__ == "__main__":
    _run()
