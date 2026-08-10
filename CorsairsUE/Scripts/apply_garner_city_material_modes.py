"""Переводит MIC полного корпуса GarnerCity на legacy-Unlit parents.

Обёртка над apply_scene_material_modes: переиспользует весь контракт
(пять родителей, CPD[0..32], texture binding, cold readback), но заменяет
вшитый census эталонного квартала на census полного корпуса garner и
ослабляет additive-семейство: в нём легальны rawTranspType 1 (ADDITIVE) и 0
(FILTER с парой ONE/ONE), а alpha test сочетается со смешиванием — так
рендерил оригинальный движок (sources/Engine/Resource/ResourceMgr.cpp).

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/apply_garner_city_material_modes.py <gltf> <root>"

Отчёт: `Scripts/reports/apply_garner_city_material_modes.txt`.
"""

import json
import os
import sys
from collections import Counter
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from report import Reporter  # noqa: E402

import apply_scene_material_modes as modes  # noqa: E402
SourceMaterialError = modes.SourceMaterialError

GARNER_COUNT_KEYS = ("opaque", "masked", "alpha", "additive", "subtractive")


def compute_garner_census(source_dir):
    """Считает mode-census полного корпуса по тому же парсеру, что и боевой."""
    source_dir = Path(source_dir).resolve()
    relaxed = {"additive": {"alphaBlendEnabled": True,
                            "srcBlend": 2,
                            "destBlend": 2}}
    originals = modes._EXPECTED_SPECIAL_STATE
    modes._EXPECTED_SPECIAL_STATE = relaxed
    try:
        materials = []
        for path in modes._gltf_paths(source_dir):
            with path.open("r", encoding="utf-8") as handle:
                document = json.load(handle)
            materials.extend(modes._parse_source_materials(document, path))
    finally:
        modes._EXPECTED_SPECIAL_STATE = originals

    census = Counter(material.mode for material in materials)
    if any(count <= 0 for mode, count in census.items()):
        raise SourceMaterialError(f"пустой mode в census: {dict(census)}")
    return {mode: census.get(mode, 0) for mode in GARNER_COUNT_KEYS}


def main(argv):
    if len(argv) != 2:
        raise RuntimeError(
            "аргументы: <каталог-gltf> <content-root>, получено: %r" % (argv,))
    source_dir = Path(argv[0]).resolve()
    content_root = argv[1]
    if content_root != "/Game/GarnerCity":
        raise RuntimeError(
            "обёртка владеет только /Game/GarnerCity, получено: "
            f"{content_root}")

    census = compute_garner_census(source_dir)
    modes.EXPECTED_MODE_COUNTS = census
    modes._EXPECTED_SPECIAL_STATE = {
        "additive": {"alphaBlendEnabled": True, "srcBlend": 2,
                     "destBlend": 2},
        "subtractive": {"rawTranspType": 2, "effectiveTranspType": 5,
                        "alphaBlendEnabled": True, "srcBlend": 1,
                        "destBlend": 4},
    }

    from report import RefuseIfEditorOpen  # noqa: E402
    import unreal  # noqa: E402

    report = Reporter("apply_garner_city_material_modes")
    try:
        if RefuseIfEditorOpen(report):
            raise RuntimeError("графический UnrealEditor открыт")
        # Ловушка плана №2: реестр ассетов в headless пуст, пока его не
        # попросили явно. Сканируем свой root и родителей заранее.
        unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(
            [content_root, modes.MASTER_ROOT], force_rescan=True)
        report.line(f"GARNER CITY CENSUS: {census}")
        modes.apply_scene_material_modes(source_dir, content_root, report)
        report.line("ИТОГО PASS GarnerCity material modes")
    except Exception as exc:
        report.exception(exc)
        raise
    finally:
        report.close()


main(sys.argv[1:])
