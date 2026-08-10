"""Досклеивает BaseColorTexture у MIC /Game/GarnerCity, оставшихся с заглушкой.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/fix_garner_city_placeholder_textures.py"

Отчёт: `Scripts/reports/fix_garner_city_placeholder_textures.txt`.

Привязка по имени: MIC `t0002_01_dds0` относится к текстуре `t0002_01`
из Textures/ той же модели. Почему эти 169 MIC остались с плейсхолдером
после общего прогона — отдельный вопрос; здесь важно закрыть дыру и
проверить, что пустых больше нет.
"""

import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

CONTENT_ROOT = "/Game/GarnerCity"
BASE_COLOR_PARAM = "BaseColorTexture"
PLACEHOLDERS = {"T_White_srgb", "T_White_Linear", "DefaultTexture"}

_TEXTURE_SUFFIX = re.compile(r"_(bmp|dds|tga|png|jpg|jpeg)\d*$",
                             flags=re.IGNORECASE)


def texture_name_for(material_name):
    wanted = _TEXTURE_SUFFIX.sub("", material_name)
    return wanted if wanted != material_name else None


def main():
    report = Reporter("fix_garner_city_placeholder_textures")
    try:
        if RefuseIfEditorOpen(report):
            raise RuntimeError("графический UnrealEditor открыт")
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        registry.scan_paths_synchronous([CONTENT_ROOT], force_rescan=True)
        assets = registry.get_assets_by_path(
            unreal.Name(CONTENT_ROOT), recursive=True)
        mics = [
            asset for asset in assets
            if str(asset.asset_class_path.asset_name)
            == "MaterialInstanceConstant"
        ]

        library = unreal.MaterialEditingLibrary
        fixed = 0
        skipped = []
        for asset in mics:
            package = str(asset.package_name)
            mic = unreal.load_asset(f"{package}.{asset.asset_name}")
            current = library.get_material_instance_texture_parameter_value(
                mic, unreal.Name(BASE_COLOR_PARAM))
            current_name = current.get_name() if current is not None else None
            if current is not None and current_name not in PLACEHOLDERS:
                continue

            wanted = texture_name_for(str(asset.asset_name))
            if wanted is None:
                skipped.append(package)
                continue
            candidate = package.rsplit("/Materials", 1)[0]
            texture = unreal.load_asset(
                f"{candidate}/Textures/{wanted}")
            if not isinstance(texture, unreal.Texture2D):
                skipped.append(
                    f"{package}.{asset.asset_name}: нет Texture2D "
                    f"{candidate}/Textures/{wanted}")
                continue
            library.set_material_instance_texture_parameter_value(
                mic, unreal.Name(BASE_COLOR_PARAM), texture)
            if not unreal.EditorAssetLibrary.save_loaded_asset(
                    mic, only_if_is_dirty=False):
                raise RuntimeError(f"не сохранился {package}")

            readback = library.get_material_instance_texture_parameter_value(
                mic, unreal.Name(BASE_COLOR_PARAM))
            if readback is None or readback.get_name() != wanted:
                raise RuntimeError(
                    f"readback изменил {BASE_COLOR_PARAM}: {package}")
            fixed += 1

        report.line(f"FIXED {fixed}; SKIPPED {len(skipped)}")
        for entry in skipped[:40]:
            report.line(f"  SKIPPED {entry}")

        # fail-closed: после фикса в списке не должно остаться ни одного
        # плейсхолдера
        remain = 0
        for asset in mics:
            package = str(asset.package_name)
            mic = unreal.load_asset(f"{package}.{asset.asset_name}")
            value = library.get_material_instance_texture_parameter_value(
                mic, unreal.Name(BASE_COLOR_PARAM))
            name = value.get_name() if value is not None else ""
            if value is None or name in PLACEHOLDERS:
                remain += 1
        if remain:
            raise RuntimeError(f"осталось placeholders после фикса: {remain}")
        report.line("ИТОГО PASS: placeholders=0")
    except Exception as exc:                         # noqa: BLE001
        report.exception(exc)
        raise
    finally:
        report.close()


main()
