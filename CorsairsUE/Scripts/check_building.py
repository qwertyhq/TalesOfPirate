"""Прослеживает путь текстуры до отрисовки одного здания.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="<путь>/check_building.py [имя-меша]"

Отчёт: `Scripts/reports/check_building.txt`.

Текстура может быть на диске, быть импортированной, быть привязанной к
материалу — и всё равно не попасть на постройку, если рисуют её другим
материалом. Здесь проверяется вся цепочка на одном здании: меш, его
материалы, параметры каждого и то, чем он нарисован на уровне.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

DEFAULT = "by-bd001_2"
PARAMS = ("BaseColorTexture", "BaseColor", "Diffuse", "DiffuseTexture")


def main(report):
    name = sys.argv[1] if len(sys.argv) > 1 else DEFAULT

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    found = [a for a in registry.get_assets_by_path(unreal.Name("/Game/All"), recursive=True)
             if str(a.asset_name) == name
             and str(a.asset_class_path.asset_name) == "StaticMesh"]
    if not found:
        report.error(f"ПРОВАЛ: меш {name} не найден")
        return

    mesh = unreal.load_asset(f"{found[0].package_name}.{name}")
    materials = mesh.get_editor_property("static_materials")
    report.line(f"меш {name}: разделов материала {len(materials)}")

    for index, slot in enumerate(materials):
        interface = slot.material_interface
        if interface is None:
            report.warn(f"  {index}: материала нет")
            continue

        report.line(f"  {index}: {interface.get_name()} "
                    f"({interface.get_class().get_name()})")

        if not isinstance(interface, unreal.MaterialInstanceConstant):
            report.warn("     не экземпляр материала — параметры не читаются")
            continue

        parent = interface.get_editor_property("parent")
        report.line(f"     родитель: {parent.get_name() if parent else 'нет'}")

        # Ищем, в каком параметре лежит текстура: имя зависит от того,
        # какой мастер-материал подставил Interchange.
        shown = False
        for param in PARAMS:
            texture = unreal.MaterialEditingLibrary \
                .get_material_instance_texture_parameter_value(
                    interface, unreal.Name(param))
            if texture is not None:
                report.line(f"     {param} = {texture.get_name()}")
                shown = True
        if not shown:
            report.error("     ПРОВАЛ: ни в одном известном параметре нет текстуры")

        # Переключатели: у материалов, которые заводит Interchange для glTF,
        # текстура включается отдельным флагом. Назначить её мало — если
        # переключатель выключен, рисуется плоский цвет.
        if parent is not None:
            switches = unreal.MaterialEditingLibrary \
                .get_static_switch_parameter_names(parent)
            report.line(f"     переключатели: {[str(n) for n in switches][:10]}")
            for switch in switches:
                value = unreal.MaterialEditingLibrary \
                    .get_material_instance_static_switch_parameter_value(
                        interface, switch)
                if "BaseColor" in str(switch) or "Color" in str(switch):
                    report.line(f"       {switch} = {value}")


report = Reporter("check_building")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
