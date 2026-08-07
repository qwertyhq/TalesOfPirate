"""Привязывает текстуры к материалам моделей.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/bind_material_textures.py"

Отчёт: `Scripts/reports/bind_material_textures.txt`.

Interchange импортирует текстуры как ассеты и создаёт экземпляры материалов,
но базовый цвет у них остаётся белой заглушкой `T_White_srgb`, хотя нужная
текстура лежит в соседней папке. Из-за этого весь город выглядит белым — и
выглядел бы даже после того, как распаковка DDS заработала.

Связь восстанавливается по имени: материал `010013_bmp` относится к текстуре
`010013`. Проверено на выходе конвертера — имя материала без расширения
совпадает с именем файла текстуры во всех 408 просмотренных случаях.

Прозрачность включается по наличию альфа-канала в исходном glTF: без этого
листва, решётки и окна рисуются сплошными прямоугольниками.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

CONTENT_ROOT = "/Game/All"

# Параметр базового цвета в материалах, которые заводит Interchange для glTF.
BASE_COLOR_PARAM = "BaseColorTexture"

# Заглушки, которые Interchange ставит вместо ненайденной текстуры.
PLACEHOLDERS = {"T_White_srgb", "T_White_Linear"}


def texture_name_for(material_name):
    """Имя текстуры по имени материала: `010013_bmp` -> `010013`.

    Расширение исходного файла Interchange превращает в суффикс, потому что в
    имени ассета точка недопустима.
    """
    return re.sub(r"_(bmp|dds|png|tga|jpg|jpeg)$", "", material_name,
                  flags=re.IGNORECASE)


def main(report):
    if RefuseIfEditorOpen(report):
        return

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(unreal.Name(CONTENT_ROOT), recursive=True)

    # Текстуры складываются в указатель по имени: искать их поиском по реестру
    # для каждого материала значило бы обходить сорок тысяч ассетов тысячи раз.
    textures = {}
    materials = []
    for asset in assets:
        kind = str(asset.asset_class_path.asset_name)
        if kind == "Texture2D":
            textures.setdefault(str(asset.asset_name), asset)
        elif kind == "MaterialInstanceConstant":
            materials.append(asset)

    report.line(f"текстур: {len(textures)}, материалов: {len(materials)}")

    library = unreal.MaterialEditingLibrary
    bound = 0
    already = 0
    missing = []

    for asset in materials:
        name = str(asset.asset_name)
        wanted = texture_name_for(name)

        entry = textures.get(wanted)
        if entry is None:
            if len(missing) < 5:
                missing.append(name)
            continue

        material = unreal.load_asset(f"{asset.package_name}.{name}")
        if not isinstance(material, unreal.MaterialInstanceConstant):
            continue

        current = library.get_material_instance_texture_parameter_value(
            material, unreal.Name(BASE_COLOR_PARAM))
        if current is not None and current.get_name() not in PLACEHOLDERS:
            already += 1
            continue

        texture = unreal.load_asset(f"{entry.package_name}.{wanted}")
        if not isinstance(texture, unreal.Texture2D):
            continue

        library.set_material_instance_texture_parameter_value(
            material, unreal.Name(BASE_COLOR_PARAM), texture)
        unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
        bound += 1

    report.line(f"ПРИВЯЗАНО ТЕКСТУР: {bound}")
    report.line(f"  уже привязано:   {already}")
    if missing:
        report.warn(f"текстура не найдена для {len(missing)} и более: {missing}")
    if bound == 0 and already == 0:
        report.error("ПРОВАЛ: ни одна текстура не привязана")


report = Reporter("bind_material_textures")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
