"""Переводит все импортированные scene MIC на owned legacy-Unlit parents.

Запуск из корня репозитория:

    UnrealEditor-Cmd CorsairsUE/CorsairsUE.uproject -unattended -nop4 \
        -NullRHI -NoSound -run=pythonscript \
        -script="CorsairsUE/Scripts/apply_scene_material_modes.py \
        <каталог-gltf> <content-root>"

Interchange создаёт Lit parents и оба специальных ``alphaMode=BLEND`` режима
сводит к translucent. Поэтому authoritative mode читается прямо из glTF, а
все 355 MIC перепривязываются к пяти Unlit parents с единым CPD[0..32]
lighting graph. До первого изменения проверяются corpus, mapping и исходные
``BaseColorTexture``.
"""

from collections import Counter
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import sys

try:
    import unreal
except ImportError:  # Pure-Python тесты не запускаются внутри UE.
    unreal = None


SCRIPT_DIR = Path(__file__).resolve().parent

BASE_COLOR_PARAM = "BaseColorTexture"
SOURCE_OPACITY_PARAM = "SourceOpacity"
ALPHA_CUTOFF_PARAM = "AlphaCutoff"
USE_VERTEX_COLOR_PARAM = "UseVertexColor"
MASTER_ROOT = "/Game/SceneParityMaterials"
MASTER_PATHS = {
    mode: f"{MASTER_ROOT}/M_Legacy_{mode.title()}"
    for mode in ("opaque", "masked", "alpha", "additive", "subtractive")
}
EXPECTED_MODE_COUNTS = {
    "opaque": 124,
    "masked": 215,
    "additive": 13,
    "subtractive": 3,
}

# Имена материалов в полном корпусе сцены содержат больше одного сегмента
# ('ocean_h.01.bmp0'); Interchange заменяет каждую точку подчёркиванием.
_SOURCE_NAME = re.compile(r"[A-Za-z0-9_-]+(\.[A-Za-z0-9_-]+)+")
_EXPECTED_ALPHA_MODE = {
    "opaque": "OPAQUE",
    "masked": "MASK",
    "alpha": "BLEND",
    "additive": "BLEND",
    "subtractive": "BLEND",
}
_EXPECTED_SPECIAL_STATE = {
    "additive": {
        "rawTranspType": 1,
        "effectiveTranspType": 1,
        "alphaBlendEnabled": True,
        "srcBlend": 2,
        "destBlend": 2,
    },
    "subtractive": {
        "rawTranspType": 2,
        "effectiveTranspType": 5,
        "alphaBlendEnabled": True,
        "srcBlend": 1,
        "destBlend": 4,
    },
}


class SourceMaterialError(RuntimeError):
    """Исходный glTF corpus не соответствует material contract."""


class MaterialMappingError(RuntimeError):
    """Один или несколько glTF-материалов нельзя однозначно связать с MIC."""

    def __init__(self, issues):
        self.issues = tuple(issues)
        self.problems = frozenset(problem for problem, _detail in self.issues)
        detail = "; ".join(text for _problem, text in self.issues)
        super().__init__(f"невалидный glTF->MIC mapping: {detail}")


@dataclass(frozen=True)
class SourceMaterial:
    model_name: str
    material_name: str
    mode: str
    gltf_path: str
    source_opacity: float = 1.0
    alpha_cutoff: float | None = None
    use_vertex_color: bool = False


@dataclass(frozen=True)
class ImportedMaterial:
    package_name: str
    asset_name: str


@dataclass(frozen=True)
class MaterialApplication:
    parent_path: str
    blend: str
    unlit: bool
    source_opacity: float
    alpha_cutoff: float | None
    use_vertex_color: float
    invert_lit_rgb: bool


@dataclass(frozen=True)
class MaterialGraphPlan:
    mode: str
    blend: str
    expected_expression_count: int
    effective_vertex_rgb_inputs: tuple[str, str, str]
    effective_vertex_alpha_inputs: tuple[str, str, str]
    raw_rgb_inputs: tuple[str, str]
    lit_rgb_inputs: tuple[str, str]
    selected_rgb_inputs: tuple[str, str, str]
    output_alpha_inputs: tuple[str, str, str]
    invert_selected_rgb: bool


_BLEND_BY_MODE = {
    "opaque": "opaque",
    "masked": "masked",
    "alpha": "translucent",
    "additive": "additive",
    "subtractive": "modulate",
}


_EXPRESSION_COUNT_BY_MODE = {
    "opaque": 110,
    "masked": 113,
    "alpha": 110,
    "additive": 110,
    "subtractive": 112,
}


def plan_material_graph(mode):
    """Описывает общую VertexColor-aware цепочку пяти legacy parents."""
    if mode not in MASTER_PATHS:
        raise ValueError(f"неподдерживаемый legacy material mode: {mode!r}")
    return MaterialGraphPlan(
        mode=mode,
        blend=_BLEND_BY_MODE[mode],
        expected_expression_count=_EXPRESSION_COUNT_BY_MODE[mode],
        effective_vertex_rgb_inputs=(
            "one", "vertex_color_rgb", "use_vertex_color"),
        effective_vertex_alpha_inputs=(
            "one", "vertex_color_alpha", "use_vertex_color"),
        raw_rgb_inputs=("texture_rgb", "effective_vertex_rgb"),
        lit_rgb_inputs=("texture_vertex_rgb", "legacy_lighting"),
        selected_rgb_inputs=(
            "texture_vertex_rgb", "lit_rgb", "legacy_lit_flag"),
        output_alpha_inputs=(
            "texture_alpha", "effective_vertex_alpha", "source_opacity"),
        invert_selected_rgb=mode == "subtractive",
    )


def plan_material_application(
        mode, source_opacity, alpha_cutoff, use_vertex_color):
    """Возвращает свойства owned Unlit parent и сохраняемые параметры MIC."""
    graph_plan = plan_material_graph(mode)
    if (isinstance(source_opacity, bool)
            or not isinstance(source_opacity, (int, float))
            or not math.isfinite(source_opacity)
            or not 0.0 <= source_opacity <= 1.0):
        raise ValueError(f"невалидный source opacity: {source_opacity!r}")
    if mode == "masked":
        if (isinstance(alpha_cutoff, bool)
                or not isinstance(alpha_cutoff, (int, float))
                or not math.isfinite(alpha_cutoff)
                or not 0.0 <= alpha_cutoff <= 1.0):
            raise ValueError(f"невалидный alpha cutoff: {alpha_cutoff!r}")
        cutoff = float(alpha_cutoff)
    else:
        if alpha_cutoff is not None:
            raise ValueError(
                f"alpha cutoff допустим только для masked, получен {mode}")
        cutoff = None
    if not isinstance(use_vertex_color, bool):
        raise ValueError(
            f"use vertex color должен быть bool: {use_vertex_color!r}")
    return MaterialApplication(
        parent_path=MASTER_PATHS[mode],
        blend=graph_plan.blend,
        unlit=True,
        source_opacity=float(source_opacity),
        alpha_cutoff=cutoff,
        use_vertex_color=1.0 if use_vertex_color else 0.0,
        invert_lit_rgb=graph_plan.invert_selected_rgb,
    )


def validate_use_vertex_color_readback(expected, actual, object_path):
    """Строго проверяет сохранённый бинарный MIC scalar."""
    if expected not in (0.0, 1.0):
        raise ValueError(
            f"ожидался бинарный {USE_VERTEX_COLOR_PARAM}: {expected!r}")
    if (isinstance(actual, bool)
            or not isinstance(actual, (int, float))
            or float(actual) not in (0.0, 1.0)
            or float(actual) != expected):
        raise RuntimeError(
            f"readback изменил {USE_VERTEX_COLOR_PARAM}: {object_path}: "
            f"expected={expected} actual={actual!r}")


def _vector_length(value):
    return math.sqrt(sum(component * component for component in value))


def _normalize(value):
    length = _vector_length(value)
    if length <= 1.0e-8:
        return (0.0, 0.0, 0.0)
    return tuple(component / length for component in value)


def _dot(left, right):
    return sum(a * b for a, b in zip(left, right))


def _point_contribution(payload, base, normal, actor_position, world_position):
    offset = payload[base:base + 3]
    color = payload[base + 3:base + 6]
    light_range = payload[base + 6]
    attenuation = payload[base + 7:base + 10]
    delta = tuple(
        actor + light_offset - world
        for actor, light_offset, world in zip(
            actor_position, offset, world_position)
    )
    distance = _vector_length(delta)
    if distance > light_range:
        return (0.0, 0.0, 0.0)
    lambert = max(_dot(normal, _normalize(delta)), 0.0)
    denominator = max(
        attenuation[0]
        + attenuation[1] * distance
        + attenuation[2] * distance * distance,
        1.0e-4,
    )
    scale = payload[2] * lambert / denominator
    return tuple(component * scale for component in color)


def evaluate_legacy_lighting(
        texture_rgb,
        payload,
        *,
        normal,
        actor_position,
        world_position):
    """CPU-эталон ровно той же 33-float формулы, что собирается в UE graph."""
    if len(payload) != 33:
        raise ValueError(f"lighting payload должен иметь 33 float: {len(payload)}")
    values = tuple(float(value) for value in payload)
    vectors = (texture_rgb, normal, actor_position, world_position)
    if (any(len(value) != 3 for value in vectors)
            or any(not math.isfinite(component)
                   for value in vectors for component in value)
            or any(not math.isfinite(value) for value in values)):
        raise ValueError("lighting equation принимает только finite float3/33")
    texture_rgb = tuple(float(value) for value in texture_rgb)

    # Нулевой mode и три нулевых source-флага — literal legacy-unlit pass.
    if max(values[0:4]) <= 0.0:
        return texture_rgb

    normal = _normalize(tuple(float(value) for value in normal))
    ambient = values[4:7]
    light_direction = values[7:10]
    direction_color = values[10:13]
    directional_scale = values[1] * max(
        _dot(normal, tuple(-value for value in light_direction)), 0.0)
    directional = tuple(
        component * directional_scale for component in direction_color)
    point1 = _point_contribution(
        values, 13, normal, actor_position, world_position)
    point2 = _point_contribution(
        values, 23, normal, actor_position, world_position)
    light = tuple(
        min(max(ambient[index] + directional[index]
                + point1[index] + point2[index], 0.0), 1.0)
        for index in range(3)
    )
    return tuple(texture_rgb[index] * light[index] for index in range(3))


def imported_material_name(source_name):
    """Возвращает детерминированное имя MIC, создаваемое Interchange."""
    if _SOURCE_NAME.fullmatch(source_name) is None:
        raise SourceMaterialError(
            f"имя glTF-материала не поддерживается: {source_name!r}")
    return source_name.replace(".", "_")


def map_imported_materials(source_materials, imported_materials, content_root):
    """Строит полный mapping по model root и точному sanitized имени MIC."""
    content_root = content_root.rstrip("/")
    imported_materials = tuple(imported_materials)
    mapped = {}
    issues = []

    for source in source_materials:
        asset_name = imported_material_name(source.material_name)
        model_root = f"{content_root}/{source.model_name}/"
        candidates = [
            entry for entry in imported_materials
            if entry.asset_name == asset_name
            and entry.package_name.startswith(model_root)
        ]
        source_label = (
            f"{source.gltf_path}:{source.material_name} -> "
            f"{model_root}*{asset_name}"
        )
        if not candidates:
            issues.append(("missing", f"не найден {source_label}"))
            continue
        if len(candidates) != 1:
            paths = sorted(entry.package_name for entry in candidates)
            issues.append(
                ("ambiguous", f"неоднозначен {source_label}: {paths}"))
            continue
        mapped[source] = candidates[0]

    if issues:
        raise MaterialMappingError(issues)
    return mapped


def _gltf_paths(source_dir):
    paths = []
    for root, dirs, files in os.walk(source_dir):
        dirs.sort()
        for name in sorted(files):
            if name.lower().endswith(".gltf"):
                paths.append(Path(root) / name)
    return paths


def _material_vertex_color_usage(document, materials, path):
    """Возвращает согласованный COLOR_0 state каждого glTF material."""
    meshes = document.get("meshes")
    if not isinstance(meshes, list) or not meshes:
        raise SourceMaterialError(f"нет meshes[]: {path}")

    states = [set() for _material in materials]
    for mesh_index, mesh in enumerate(meshes):
        if not isinstance(mesh, dict):
            raise SourceMaterialError(
                f"meshes[{mesh_index}] не object: {path}")
        primitives = mesh.get("primitives")
        if not isinstance(primitives, list) or not primitives:
            raise SourceMaterialError(
                f"нет meshes[{mesh_index}].primitives[]: {path}")
        for primitive_index, primitive in enumerate(primitives):
            label = f"meshes[{mesh_index}].primitives[{primitive_index}]"
            if not isinstance(primitive, dict):
                raise SourceMaterialError(f"{label} не object: {path}")
            if "material" not in primitive:
                continue
            material_index = primitive["material"]
            if (isinstance(material_index, bool)
                    or not isinstance(material_index, int)
                    or not 0 <= material_index < len(materials)):
                raise SourceMaterialError(
                    f"неверный {label}.material={material_index!r}: {path}")
            attributes = primitive.get("attributes")
            if not isinstance(attributes, dict):
                raise SourceMaterialError(f"нет {label}.attributes: {path}")
            uses_vertex_color = "COLOR_0" in attributes
            if uses_vertex_color:
                accessor_index = attributes["COLOR_0"]
                if (isinstance(accessor_index, bool)
                        or not isinstance(accessor_index, int)
                        or accessor_index < 0):
                    raise SourceMaterialError(
                        f"неверный {label}.attributes.COLOR_0="
                        f"{accessor_index!r}: {path}")
            states[material_index].add(uses_vertex_color)

    usage = []
    for material_index, material_states in enumerate(states):
        material_name = materials[material_index]["name"]
        if not material_states:
            raise SourceMaterialError(
                f"материал не используется ни одним primitive: "
                f"{path}:{material_name}")
        if len(material_states) != 1:
            raise SourceMaterialError(
                f"COLOR_0 расходится между primitive references: "
                f"{path}:{material_name}")
        usage.append(next(iter(material_states)))
    return tuple(usage)


def _parse_source_materials(document, path):
    """Проверяет один glTF и возвращает его authoritative materials."""
    path = Path(path)
    if not isinstance(document, dict):
        raise SourceMaterialError(f"корень glTF не object: {path}")
    materials = document.get("materials")
    if not isinstance(materials, list) or not materials:
        raise SourceMaterialError(f"нет materials[]: {path}")

    imported_names = set()
    for index, material in enumerate(materials):
        if not isinstance(material, dict):
            raise SourceMaterialError(
                f"materials[{index}] не object: {path}")
        material_name = material.get("name")
        if not isinstance(material_name, str):
            raise SourceMaterialError(
                f"нет строкового materials[{index}].name: {path}")
        imported_name = imported_material_name(material_name)
        if imported_name in imported_names:
            raise SourceMaterialError(
                f"повторный MIC key {path.stem}/{imported_name}: {path}")
        imported_names.add(imported_name)

    vertex_color_usage = _material_vertex_color_usage(
        document, materials, path)
    parsed = []
    for index, material in enumerate(materials):
        material_name = material["name"]
        extras = material.get("extras")
        metadata = (
            extras.get("corsairsLegacyMaterial")
            if isinstance(extras, dict) else None
        )
        if not isinstance(metadata, dict):
            raise SourceMaterialError(
                f"нет extras.corsairsLegacyMaterial: {path}:{material_name}")
        if metadata.get("schemaVersion") != 1:
            raise SourceMaterialError(
                f"неверный schemaVersion: {path}:{material_name}")

        mode = metadata.get("mode")
        if mode not in _EXPECTED_ALPHA_MODE:
            raise SourceMaterialError(
                f"неподдерживаемый mode {mode!r}: {path}:{material_name}")
        if material.get("alphaMode") != _EXPECTED_ALPHA_MODE[mode]:
            raise SourceMaterialError(
                f"mode/alphaMode расходятся: {path}:{material_name}")

        expected_state = _EXPECTED_SPECIAL_STATE.get(mode, {})
        mismatched = [
            field for field, expected in expected_state.items()
            if metadata.get(field) != expected
        ]
        if mismatched:
            raise SourceMaterialError(
                f"неверный legacy blend state {mismatched}: "
                f"{path}:{material_name}")

        application = plan_material_application(
            mode,
            metadata.get("opacity"),
            material.get("alphaCutoff"),
            vertex_color_usage[index],
        )
        parsed.append(SourceMaterial(
            model_name=path.stem,
            material_name=material_name,
            mode=mode,
            gltf_path=str(path),
            source_opacity=application.source_opacity,
            alpha_cutoff=application.alpha_cutoff,
            use_vertex_color=vertex_color_usage[index],
        ))
    return parsed


def read_source_materials(source_dir):
    """Читает nested extras и проверяет точный production census."""
    source_dir = Path(source_dir).resolve()
    if not source_dir.is_dir():
        raise SourceMaterialError(f"нет каталога glTF: {source_dir}")

    paths = _gltf_paths(source_dir)
    if not paths:
        raise SourceMaterialError(f"в {source_dir} нет .gltf")

    source_materials = []
    seen_models = set()
    for path in paths:
        model_name = path.stem
        if model_name in seen_models:
            raise SourceMaterialError(
                f"повторяется model stem {model_name!r}: {path}")
        seen_models.add(model_name)

        with path.open("r", encoding="utf-8") as handle:
            document = json.load(handle)
        source_materials.extend(_parse_source_materials(document, path))

    counts = Counter(source.mode for source in source_materials)
    if dict(counts) != EXPECTED_MODE_COUNTS:
        raise SourceMaterialError(
            f"неверный material census: {dict(counts)}, "
            f"ожидался {EXPECTED_MODE_COUNTS}")
    return source_materials


def _require_unreal():
    if unreal is None:
        raise RuntimeError("UE API недоступен: запускайте скрипт через UnrealEditor-Cmd")


def _collect_imported_materials(content_root):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(
        unreal.Name(content_root), recursive=True)
    return [
        ImportedMaterial(
            package_name=str(asset.package_name),
            asset_name=str(asset.asset_name),
        )
        for asset in assets
        if str(asset.asset_class_path.asset_name) == "MaterialInstanceConstant"
    ]


def _object_path(entry):
    return f"{entry.package_name}.{entry.asset_name}"


def _preflight_instances(mapping):
    """Загружает и проверяет все MIC, не меняя ни одного asset."""
    library = unreal.MaterialEditingLibrary
    loaded = {}
    textures = {}
    problems = []
    allowed_blends = {
        "opaque": (unreal.BlendMode.BLEND_OPAQUE,),
        "masked": (unreal.BlendMode.BLEND_MASKED,),
        "alpha": (unreal.BlendMode.BLEND_TRANSLUCENT,),
        # Повторный запуск уже видит наши owned parent blend modes.
        "additive": (
            unreal.BlendMode.BLEND_TRANSLUCENT,
            unreal.BlendMode.BLEND_ADDITIVE,
        ),
        "subtractive": (
            unreal.BlendMode.BLEND_TRANSLUCENT,
            unreal.BlendMode.BLEND_MODULATE,
        ),
    }

    for source, entry in mapping.items():
        instance = unreal.load_asset(_object_path(entry))
        if not isinstance(instance, unreal.MaterialInstanceConstant):
            problems.append(f"это не MaterialInstanceConstant: {_object_path(entry)}")
            continue
        loaded[source] = instance

        blend_mode = instance.get_blend_mode()
        if blend_mode not in allowed_blends[source.mode]:
            problems.append(
                f"неверный preflight blend {blend_mode}: {_object_path(entry)}")

        texture = library.get_material_instance_texture_parameter_value(
            instance, unreal.Name(BASE_COLOR_PARAM))
        if texture is None:
            problems.append(
                f"нет {BASE_COLOR_PARAM}: {_object_path(entry)}")
        else:
            textures[source] = texture

    if problems:
        raise RuntimeError("preflight MIC провален:\n" + "\n".join(problems))
    return loaded, textures


def _preflight_masters():
    """Проверяет занятые master paths до создания/изменения assets."""
    existing = {}
    problems = []
    for mode, path in MASTER_PATHS.items():
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            existing[mode] = None
            continue
        material = unreal.load_asset(path)
        if not isinstance(material, unreal.Material):
            problems.append(f"owned master path занят не Material: {path}")
        else:
            existing[mode] = material
    if problems:
        raise RuntimeError("preflight master провален:\n" + "\n".join(problems))
    return existing


_PAYLOAD_NAMES = (
    "Mode",
    "EnvEnabled",
    "PointEnabled",
    "ShadeEnabled",
    "AmbientR",
    "AmbientG",
    "AmbientB",
    "DirectionX",
    "DirectionY",
    "DirectionZ",
    "DirectionColorR",
    "DirectionColorG",
    "DirectionColorB",
    "Point1OffsetX",
    "Point1OffsetY",
    "Point1OffsetZ",
    "Point1ColorR",
    "Point1ColorG",
    "Point1ColorB",
    "Point1Range",
    "Point1Attenuation0",
    "Point1Attenuation1",
    "Point1Attenuation2",
    "Point2OffsetX",
    "Point2OffsetY",
    "Point2OffsetZ",
    "Point2ColorR",
    "Point2ColorG",
    "Point2ColorB",
    "Point2Range",
    "Point2Attenuation0",
    "Point2Attenuation1",
    "Point2Attenuation2",
)


class _MaterialGraph:
    """Маленький fail-fast builder поверх MaterialEditingLibrary."""

    def __init__(self, material, path):
        self.material = material
        self.path = path
        self.library = unreal.MaterialEditingLibrary
        self.count = 0

    def expression(self, expression_type):
        column = self.count % 12
        row = self.count // 12
        expression = self.library.create_material_expression(
            self.material,
            expression_type,
            -2600 + column * 240,
            -1400 + row * 180,
        )
        if expression is None:
            raise RuntimeError(
                f"не создан {expression_type.__name__}: {self.path}")
        self.count += 1
        return expression

    def connect(self, source, target, input_name):
        if isinstance(source, tuple):
            expression, output_name = source
        else:
            expression, output_name = source, ""
        if not self.library.connect_material_expressions(
                expression, output_name, target, input_name):
            raise RuntimeError(
                f"не подключён {expression.get_name()}:{output_name} -> "
                f"{target.get_name()}:{input_name}: {self.path}")

    def scalar_parameter(self, name, default_value, cpd_index=None):
        expression = self.expression(unreal.MaterialExpressionScalarParameter)
        expression.set_editor_property("parameter_name", unreal.Name(name))
        expression.set_editor_property("default_value", float(default_value))
        if cpd_index is not None:
            expression.set_editor_property("use_custom_primitive_data", True)
            expression.set_editor_property("primitive_data_index", cpd_index)
        return expression

    def binary(self, expression_type, left, right):
        expression = self.expression(expression_type)
        self.connect(left, expression, "A")
        self.connect(right, expression, "B")
        return expression

    def with_const_b(self, expression_type, value, source):
        expression = self.expression(expression_type)
        expression.set_editor_property("const_b", float(value))
        self.connect(source, expression, "A")
        return expression

    def unary(self, expression_type, input_name, source):
        expression = self.expression(expression_type)
        # MaterialGraphNode сокращает literal `Input` до NAME_None; API
        # принимает для такого первого pin только пустую строку.
        if input_name == "Input":
            input_name = ""
        self.connect(source, expression, input_name)
        return expression

    def vector3(self, x_value, y_value, z_value):
        xy = self.binary(
            unreal.MaterialExpressionAppendVector, x_value, y_value)
        return self.binary(
            unreal.MaterialExpressionAppendVector, xy, z_value)


def _build_point_light(graph, cpd, base, actor_position, world_position, normal):
    offset = graph.vector3(cpd[base], cpd[base + 1], cpd[base + 2])
    color = graph.vector3(cpd[base + 3], cpd[base + 4], cpd[base + 5])
    light_range = cpd[base + 6]
    attenuation0 = cpd[base + 7]
    attenuation1 = cpd[base + 8]
    attenuation2 = cpd[base + 9]

    light_position = graph.binary(
        unreal.MaterialExpressionAdd, actor_position, offset)
    delta = graph.binary(
        unreal.MaterialExpressionSubtract, light_position, world_position)
    distance = graph.unary(
        unreal.MaterialExpressionLength, "Input", delta)
    light_vector = graph.unary(
        unreal.MaterialExpressionNormalize, "VectorInput", delta)
    lambert = graph.binary(
        unreal.MaterialExpressionDotProduct, normal, light_vector)
    lambert = graph.with_const_b(
        unreal.MaterialExpressionMax, 0.0, lambert)

    in_range = graph.expression(unreal.MaterialExpressionStep)
    graph.connect(distance, in_range, "Y")
    graph.connect(light_range, in_range, "X")

    attenuation1_distance = graph.binary(
        unreal.MaterialExpressionMultiply, attenuation1, distance)
    distance_squared = graph.binary(
        unreal.MaterialExpressionMultiply, distance, distance)
    attenuation2_distance = graph.binary(
        unreal.MaterialExpressionMultiply, attenuation2, distance_squared)
    denominator = graph.binary(
        unreal.MaterialExpressionAdd, attenuation0, attenuation1_distance)
    denominator = graph.binary(
        unreal.MaterialExpressionAdd, denominator, attenuation2_distance)
    denominator = graph.with_const_b(
        unreal.MaterialExpressionMax, 1.0e-4, denominator)

    strength = graph.binary(
        unreal.MaterialExpressionMultiply, cpd[2], in_range)
    strength = graph.binary(
        unreal.MaterialExpressionMultiply, strength, lambert)
    strength = graph.binary(
        unreal.MaterialExpressionDivide, strength, denominator)
    return graph.binary(unreal.MaterialExpressionMultiply, color, strength)


def _build_legacy_lighting_graph(graph, mode):
    plan = plan_material_graph(mode)
    sampler = graph.expression(
        unreal.MaterialExpressionTextureSampleParameter2D)
    sampler.set_editor_property(
        "parameter_name", unreal.Name(BASE_COLOR_PARAM))
    vertex_color = graph.expression(unreal.MaterialExpressionVertexColor)
    use_vertex_color = graph.scalar_parameter(USE_VERTEX_COLOR_PARAM, 0.0)
    effective_vertex_rgb = graph.expression(
        unreal.MaterialExpressionLinearInterpolate)
    effective_vertex_rgb.set_editor_property("const_a", 1.0)
    graph.connect(vertex_color, effective_vertex_rgb, "B")
    graph.connect(use_vertex_color, effective_vertex_rgb, "Alpha")
    # UE 5.8 оставляет имя первого VertexColor output пустым; передача самого
    # expression выбирает этот RGB output, тогда как канал alpha называется A.
    texture_vertex_rgb = graph.binary(
        unreal.MaterialExpressionMultiply,
        (sampler, "RGB"),
        effective_vertex_rgb,
    )
    source_opacity = graph.scalar_parameter(SOURCE_OPACITY_PARAM, 1.0)
    cpd = [
        graph.scalar_parameter(f"LegacyCpd{index:02d}_{name}", 0.0, index)
        for index, name in enumerate(_PAYLOAD_NAMES)
    ]

    normal = graph.expression(unreal.MaterialExpressionPixelNormalWS)
    actor_position = graph.expression(unreal.MaterialExpressionActorPositionWS)
    world_position = graph.expression(unreal.MaterialExpressionWorldPosition)
    ambient = graph.vector3(cpd[4], cpd[5], cpd[6])
    light_direction = graph.vector3(cpd[7], cpd[8], cpd[9])
    direction_color = graph.vector3(cpd[10], cpd[11], cpd[12])
    negative_direction = graph.with_const_b(
        unreal.MaterialExpressionMultiply, -1.0, light_direction)
    directional_lambert = graph.binary(
        unreal.MaterialExpressionDotProduct, normal, negative_direction)
    directional_lambert = graph.with_const_b(
        unreal.MaterialExpressionMax, 0.0, directional_lambert)
    directional_scale = graph.binary(
        unreal.MaterialExpressionMultiply, cpd[1], directional_lambert)
    directional = graph.binary(
        unreal.MaterialExpressionMultiply, direction_color, directional_scale)

    point1 = _build_point_light(
        graph, cpd, 13, actor_position, world_position, normal)
    point2 = _build_point_light(
        graph, cpd, 23, actor_position, world_position, normal)
    lighting = graph.binary(
        unreal.MaterialExpressionAdd, ambient, directional)
    lighting = graph.binary(unreal.MaterialExpressionAdd, lighting, point1)
    lighting = graph.binary(unreal.MaterialExpressionAdd, lighting, point2)
    lighting = graph.unary(
        unreal.MaterialExpressionSaturate, "Input", lighting)
    lit_rgb = graph.binary(
        unreal.MaterialExpressionMultiply, texture_vertex_rgb, lighting)

    mode_or_env = graph.binary(
        unreal.MaterialExpressionMax, cpd[0], cpd[1])
    point_or_shade = graph.binary(
        unreal.MaterialExpressionMax, cpd[2], cpd[3])
    lit_flag = graph.binary(
        unreal.MaterialExpressionMax, mode_or_env, point_or_shade)
    # Параметр остаётся доступен MIC и для opaque/subtractive, но не меняет RGB.
    opacity_metadata = graph.with_const_b(
        unreal.MaterialExpressionMultiply, 0.0, source_opacity)
    lit_flag = graph.binary(
        unreal.MaterialExpressionAdd, lit_flag, opacity_metadata)
    lit_flag = graph.unary(
        unreal.MaterialExpressionSaturate, "Input", lit_flag)
    selected_rgb = graph.expression(
        unreal.MaterialExpressionLinearInterpolate)
    graph.connect(texture_vertex_rgb, selected_rgb, "A")
    graph.connect(lit_rgb, selected_rgb, "B")
    graph.connect(lit_flag, selected_rgb, "Alpha")

    effective_vertex_alpha = graph.expression(
        unreal.MaterialExpressionLinearInterpolate)
    effective_vertex_alpha.set_editor_property("const_a", 1.0)
    graph.connect((vertex_color, "A"), effective_vertex_alpha, "B")
    graph.connect(use_vertex_color, effective_vertex_alpha, "Alpha")
    texture_vertex_alpha = graph.binary(
        unreal.MaterialExpressionMultiply,
        (sampler, "A"),
        effective_vertex_alpha,
    )
    output_alpha = graph.binary(
        unreal.MaterialExpressionMultiply,
        texture_vertex_alpha,
        source_opacity,
    )
    alpha_cutoff = None
    if mode == "masked":
        alpha_cutoff = graph.scalar_parameter(
            ALPHA_CUTOFF_PARAM, 1.0 / 255.0)
    return plan, sampler, selected_rgb, output_alpha, alpha_cutoff


def _delete_material_expressions(library, material, path):
    """Удаляет snapshot графа: UE 5.8 bulk API мутирует live collection."""
    expressions = list(library.get_material_expressions(material) or [])
    for expression in expressions:
        library.delete_material_expression(material, expression)
    remaining = library.get_num_material_expressions(material)
    if remaining != 0:
        raise RuntimeError(
            f"material graph не очищен: {path}: remaining={remaining}")


def _configure_master(mode, material):
    """Пересобирает owned Unlit parent с общей 33-float lighting формулой."""
    plan = plan_material_graph(mode)
    path = MASTER_PATHS[mode]
    if material is None:
        name = path.rsplit("/", 1)[1]
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name,
            MASTER_ROOT,
            unreal.Material,
            unreal.MaterialFactoryNew(),
        )
        if not isinstance(material, unreal.Material):
            raise RuntimeError(f"не создан master material: {path}")

    library = unreal.MaterialEditingLibrary
    blend_mode = {
        "opaque": unreal.BlendMode.BLEND_OPAQUE,
        "masked": unreal.BlendMode.BLEND_MASKED,
        "translucent": unreal.BlendMode.BLEND_TRANSLUCENT,
        "additive": unreal.BlendMode.BLEND_ADDITIVE,
        "modulate": unreal.BlendMode.BLEND_MODULATE,
    }[plan.blend]
    material.set_editor_property("blend_mode", blend_mode)
    material.set_editor_property("shading_model",
                                 unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("opacity_mask_clip_value", 0.5)
    _delete_material_expressions(library, material, path)

    graph = _MaterialGraph(material, path)
    graph_plan, _sampler, output, output_alpha, alpha_cutoff = (
        _build_legacy_lighting_graph(graph, mode))
    if graph_plan != plan:
        raise RuntimeError(f"material graph plan расходится: {path}")
    if plan.invert_selected_rgb:
        saturated = graph.unary(
            unreal.MaterialExpressionSaturate, "Input", output)
        inverse = graph.expression(unreal.MaterialExpressionOneMinus)
        # UE 5.8 сокращает pin с именем `Input` в NAME_None. Пустое имя —
        # документированный способ выбрать первый input выражения.
        graph.connect(saturated, inverse, "")
        output = inverse

    if not library.connect_material_property(
            output, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError(f"не подключён Emissive Color: {path}")
    if mode in ("alpha", "additive"):
        if not library.connect_material_property(
                output_alpha, "", unreal.MaterialProperty.MP_OPACITY):
            raise RuntimeError(f"не подключён Opacity: {path}")
    if mode == "masked":
        opacity_minus_cutoff = graph.binary(
            unreal.MaterialExpressionSubtract, output_alpha, alpha_cutoff)
        thresholded_mask = graph.with_const_b(
            unreal.MaterialExpressionAdd, 0.5, opacity_minus_cutoff)
        if not library.connect_material_property(
                thresholded_mask,
                "",
                unreal.MaterialProperty.MP_OPACITY_MASK):
            raise RuntimeError(f"не подключён Opacity Mask: {path}")
    if graph.count != plan.expected_expression_count:
        raise RuntimeError(
            f"material graph builder count расходится: {path}: "
            f"expected={plan.expected_expression_count} actual={graph.count}")
    library.layout_material_expressions(material)

    compile_errors = list(library.recompile_material(material) or [])
    if compile_errors:
        raise RuntimeError(
            f"master material не скомпилирован {path}: {compile_errors}")
    actual_expression_count = library.get_num_material_expressions(material)
    if actual_expression_count != plan.expected_expression_count:
        raise RuntimeError(
            f"неверный graph expression count: {path}: "
            f"expected={plan.expected_expression_count} "
            f"actual={actual_expression_count}")
    if material.get_editor_property("blend_mode") != blend_mode:
        raise RuntimeError(f"не сохранился blend mode: {path}")
    if (material.get_editor_property("shading_model")
            != unreal.MaterialShadingModel.MSM_UNLIT):
        raise RuntimeError(f"master не Unlit: {path}")
    if not material.get_editor_property("two_sided"):
        raise RuntimeError(f"master не Two Sided: {path}")
    if not unreal.EditorAssetLibrary.save_loaded_asset(
            material, only_if_is_dirty=False):
        raise RuntimeError(f"master material не сохранён: {path}")
    return material


def _reparent_instances(mapping, loaded, textures, masters):
    library = unreal.MaterialEditingLibrary
    applied = Counter()
    for source in sorted(
            mapping, key=lambda item: (item.model_name, item.material_name)):
        instance = loaded[source]
        texture = textures[source]
        application = plan_material_application(
            source.mode,
            source.source_opacity,
            source.alpha_cutoff,
            source.use_vertex_color,
        )
        library.set_material_instance_parent(instance, masters[source.mode])
        # В UE 5.8 этот setter всегда возвращает False из-за bResult, который
        # native implementation не обновляет. Проверяем фактическое значение.
        library.set_material_instance_texture_parameter_value(
            instance, unreal.Name(BASE_COLOR_PARAM), texture)
        current = library.get_material_instance_texture_parameter_value(
            instance, unreal.Name(BASE_COLOR_PARAM))
        if (current is None
                or current.get_path_name() != texture.get_path_name()):
            raise RuntimeError(
                f"не восстановлен {BASE_COLOR_PARAM}: "
                f"{_object_path(mapping[source])}")
        library.set_material_instance_scalar_parameter_value(
            instance,
            unreal.Name(SOURCE_OPACITY_PARAM),
            application.source_opacity,
        )
        current_opacity = (
            library.get_material_instance_scalar_parameter_value(
                instance, unreal.Name(SOURCE_OPACITY_PARAM)))
        if not math.isclose(
                current_opacity,
                application.source_opacity,
                rel_tol=0.0,
                abs_tol=1.0e-7):
            raise RuntimeError(
                f"не восстановлен {SOURCE_OPACITY_PARAM}: "
                f"{_object_path(mapping[source])}")
        library.set_material_instance_scalar_parameter_value(
            instance,
            unreal.Name(USE_VERTEX_COLOR_PARAM),
            application.use_vertex_color,
        )
        current_use_vertex_color = (
            library.get_material_instance_scalar_parameter_value(
                instance, unreal.Name(USE_VERTEX_COLOR_PARAM)))
        validate_use_vertex_color_readback(
            application.use_vertex_color,
            current_use_vertex_color,
            _object_path(mapping[source]),
        )
        if source.mode == "masked":
            library.set_material_instance_scalar_parameter_value(
                instance,
                unreal.Name(ALPHA_CUTOFF_PARAM),
                application.alpha_cutoff,
            )
            current_cutoff = (
                library.get_material_instance_scalar_parameter_value(
                    instance, unreal.Name(ALPHA_CUTOFF_PARAM)))
            if not math.isclose(
                    current_cutoff,
                    application.alpha_cutoff,
                    rel_tol=0.0,
                    abs_tol=1.0e-7):
                raise RuntimeError(
                    f"не восстановлен {ALPHA_CUTOFF_PARAM}: "
                    f"{_object_path(mapping[source])}")
        if not unreal.EditorAssetLibrary.save_loaded_asset(
                instance, only_if_is_dirty=False):
            raise RuntimeError(
                f"MIC не сохранён: {_object_path(mapping[source])}")
        applied[source.mode] += 1

    expected = EXPECTED_MODE_COUNTS
    if dict(applied) != expected:
        raise RuntimeError(
            f"перепривязано неверное число MIC: {dict(applied)}, "
            f"ожидалось {expected}")
    return applied


def _readback(mapping, loaded, textures, masters):
    """Читает сохранённые MIC обратно и проверяет mode/parent/texture census."""
    library = unreal.MaterialEditingLibrary
    final_blends = {
        "opaque": unreal.BlendMode.BLEND_OPAQUE,
        "masked": unreal.BlendMode.BLEND_MASKED,
        "alpha": unreal.BlendMode.BLEND_TRANSLUCENT,
        "additive": unreal.BlendMode.BLEND_ADDITIVE,
        "subtractive": unreal.BlendMode.BLEND_MODULATE,
    }
    counts = Counter()
    parent_counts = Counter()

    for source, entry in mapping.items():
        instance = unreal.load_asset(_object_path(entry))
        if not isinstance(instance, unreal.MaterialInstanceConstant):
            raise RuntimeError(f"readback не загрузил MIC: {_object_path(entry)}")
        if instance.get_blend_mode() != final_blends[source.mode]:
            raise RuntimeError(
                f"readback blend не совпал: {_object_path(entry)}")
        counts[source.mode] += 1

        parent = instance.get_editor_property("parent")
        expected_parent = masters[source.mode]
        if parent is None or parent.get_path_name() != expected_parent.get_path_name():
            raise RuntimeError(
                f"readback parent не совпал: {_object_path(entry)}")
        parent_counts[source.mode] += 1

        texture = library.get_material_instance_texture_parameter_value(
            instance, unreal.Name(BASE_COLOR_PARAM))
        if (texture is None
                or texture.get_path_name() != textures[source].get_path_name()):
            raise RuntimeError(
                f"readback изменил {BASE_COLOR_PARAM}: {_object_path(entry)}")

        application = plan_material_application(
            source.mode,
            source.source_opacity,
            source.alpha_cutoff,
            source.use_vertex_color,
        )
        source_opacity = library.get_material_instance_scalar_parameter_value(
            instance, unreal.Name(SOURCE_OPACITY_PARAM))
        if not math.isclose(
                source_opacity,
                application.source_opacity,
                rel_tol=0.0,
                abs_tol=1.0e-7):
            raise RuntimeError(
                f"readback изменил {SOURCE_OPACITY_PARAM}: "
                f"{_object_path(entry)}")
        use_vertex_color = (
            library.get_material_instance_scalar_parameter_value(
                instance, unreal.Name(USE_VERTEX_COLOR_PARAM)))
        validate_use_vertex_color_readback(
            application.use_vertex_color,
            use_vertex_color,
            _object_path(entry),
        )
        if source.mode == "masked":
            alpha_cutoff = (
                library.get_material_instance_scalar_parameter_value(
                    instance, unreal.Name(ALPHA_CUTOFF_PARAM)))
            if not math.isclose(
                    alpha_cutoff,
                    application.alpha_cutoff,
                    rel_tol=0.0,
                    abs_tol=1.0e-7):
                raise RuntimeError(
                    f"readback изменил {ALPHA_CUTOFF_PARAM}: "
                    f"{_object_path(entry)}")

    if dict(counts) != EXPECTED_MODE_COUNTS:
        raise RuntimeError(
            f"readback census неверен: {dict(counts)}, "
            f"ожидался {EXPECTED_MODE_COUNTS}")
    expected_parents = EXPECTED_MODE_COUNTS
    if dict(parent_counts) != expected_parents:
        raise RuntimeError(
            f"readback parent census неверен: {dict(parent_counts)}")
    return counts, parent_counts


def _verify_master_readback(masters):
    """Проверяет cold-loaded parent properties и полный CPD index set."""
    library = unreal.MaterialEditingLibrary
    expected_blends = {
        "opaque": unreal.BlendMode.BLEND_OPAQUE,
        "masked": unreal.BlendMode.BLEND_MASKED,
        "alpha": unreal.BlendMode.BLEND_TRANSLUCENT,
        "additive": unreal.BlendMode.BLEND_ADDITIVE,
        "subtractive": unreal.BlendMode.BLEND_MODULATE,
    }
    details = {}
    for mode, material in masters.items():
        path = MASTER_PATHS[mode]
        if material is None:
            raise RuntimeError(f"cold readback не нашёл parent: {path}")
        if material.get_editor_property("blend_mode") != expected_blends[mode]:
            raise RuntimeError(f"cold readback blend неверен: {path}")
        if (material.get_editor_property("shading_model")
                != unreal.MaterialShadingModel.MSM_UNLIT):
            raise RuntimeError(f"cold readback parent не Unlit: {path}")
        if not material.get_editor_property("two_sided"):
            raise RuntimeError(f"cold readback parent не Two Sided: {path}")

        expressions = list(library.get_material_expressions(material))
        expected_expression_count = (
            plan_material_graph(mode).expected_expression_count)
        if len(expressions) != expected_expression_count:
            raise RuntimeError(
                f"cold readback expression count неверен: {path}: "
                f"expected={expected_expression_count} "
                f"actual={len(expressions)}")
        parameter_names = [
            str(expression.get_editor_property("parameter_name"))
            for expression in expressions
            if isinstance(expression, unreal.MaterialExpressionScalarParameter)
        ]
        if parameter_names.count(USE_VERTEX_COLOR_PARAM) != 1:
            raise RuntimeError(
                f"cold readback {USE_VERTEX_COLOR_PARAM} неверен: {path}")
        cpd_indices = sorted(
            expression.get_editor_property("primitive_data_index")
            for expression in expressions
            if isinstance(expression, unreal.MaterialExpressionScalarParameter)
            and expression.get_editor_property("use_custom_primitive_data")
        )
        if cpd_indices != list(range(33)):
            raise RuntimeError(
                f"cold readback CPD indexes неверны {cpd_indices}: {path}")
        one_minus_count = sum(
            isinstance(expression, unreal.MaterialExpressionOneMinus)
            for expression in expressions
        )
        expected_one_minus = 1 if mode == "subtractive" else 0
        if one_minus_count != expected_one_minus:
            raise RuntimeError(
                f"cold readback subtractive fingerprint неверен: {path}")
        details[mode] = {
            "path": material.get_path_name(),
            "expressions": len(expressions),
            "cpd": len(cpd_indices),
        }
    return details


def verify_scene_material_modes(source_dir, content_root, report):
    """Выполняет только cold/reload readback без save или mutation API."""
    _require_unreal()
    content_root = content_root.rstrip("/")
    source_materials = read_source_materials(source_dir)
    imported_materials = _collect_imported_materials(content_root)
    mapping = map_imported_materials(
        source_materials, imported_materials, content_root)
    mapped_packages = {entry.package_name for entry in mapping.values()}
    imported_packages = {entry.package_name for entry in imported_materials}
    if (len(imported_materials) != len(source_materials)
            or mapped_packages != imported_packages):
        raise RuntimeError(
            "cold readback MIC corpus не совпал: "
            f"source={len(source_materials)} imported={len(imported_materials)}")

    loaded, textures = _preflight_instances(mapping)
    masters = _preflight_masters()
    master_details = _verify_master_readback(masters)
    counts, parent_counts = _readback(
        mapping, loaded, textures, masters)

    representatives = {}
    for mode in ("opaque", "masked", "additive", "subtractive"):
        source = min(
            (item for item in mapping if item.mode == mode),
            key=lambda item: (item.model_name, item.material_name),
        )
        representatives[mode] = _object_path(mapping[source])
    report.line(f"COLD PARENTS {master_details}")
    report.line(
        f"COLD MIC modes={dict(counts)} parents={dict(parent_counts)}")
    report.line(f"COLD REPRESENTATIVE {representatives}")
    report.line("ИТОГО PASS cold material readback без mutation")


def apply_scene_material_modes(source_dir, content_root, report):
    """Выполняет полный preflight, mutation и readback."""
    _require_unreal()
    content_root = content_root.rstrip("/")
    if not content_root.startswith("/Game/"):
        raise RuntimeError(f"content root должен начинаться с /Game/: {content_root}")

    source_materials = read_source_materials(source_dir)
    report.line(
        f"ИСТОЧНИК glTF={len({item.gltf_path for item in source_materials})} "
        f"материалов={len(source_materials)} census={EXPECTED_MODE_COUNTS}")

    imported_materials = _collect_imported_materials(content_root)
    mapping = map_imported_materials(
        source_materials, imported_materials, content_root)
    mapped_packages = {entry.package_name for entry in mapping.values()}
    imported_packages = {entry.package_name for entry in imported_materials}
    if (len(imported_materials) != len(source_materials)
            or mapped_packages != imported_packages):
        extras = sorted(imported_packages - mapped_packages)
        raise RuntimeError(
            f"импортированный MIC corpus не точный: source={len(source_materials)} "
            f"imported={len(imported_materials)} extras={extras[:10]}")

    # Последние read-only проверки: после этой точки можно создавать assets.
    loaded, textures = _preflight_instances(mapping)
    existing_masters = _preflight_masters()
    report.line(
        f"PREFLIGHT PASS mapping={len(mapping)} "
        f"BaseColorTexture={len(textures)}")

    masters = {
        mode: _configure_master(mode, existing_masters[mode])
        for mode in ("opaque", "masked", "alpha", "additive", "subtractive")
    }
    applied = _reparent_instances(
        mapping, loaded, textures, masters)
    counts, parent_counts = _readback(
        mapping, loaded, textures, masters)

    report.line(f"ПЕРЕПРИВЯЗАНО {dict(applied)}")
    report.line(f"READBACK modes={dict(counts)} parents={dict(parent_counts)}")
    report.line("ИТОГО PASS material post-pass завершён")


def _script_arguments(argv):
    argv = list(argv)
    verify_only = bool(argv and argv[0] == "--verify-only")
    if verify_only:
        argv = argv[1:]
    if len(argv) != 2:
        raise RuntimeError(
            "аргументы: [--verify-only] <каталог-gltf> <content-root>; "
            "оба production-пути обязательны; "
            f"получено: {argv}")
    source_dir = Path(argv[0]).resolve()
    content_root = argv[1]
    return source_dir, content_root, verify_only


def _run_from_ue():
    sys.path.insert(0, str(SCRIPT_DIR))
    from report import Reporter, RefuseIfEditorOpen  # noqa: E402

    report_name = (
        "verify_scene_material_modes"
        if "--verify-only" in sys.argv[1:]
        else "apply_scene_material_modes"
    )
    report = Reporter(report_name)
    try:
        if RefuseIfEditorOpen(report):
            raise RuntimeError("графический UnrealEditor открыт")
        source_dir, content_root, verify_only = _script_arguments(sys.argv[1:])
        if verify_only:
            verify_scene_material_modes(source_dir, content_root, report)
        else:
            apply_scene_material_modes(source_dir, content_root, report)
    except Exception as exc:  # noqa: BLE001 — отчёт должен сохранить traceback.
        report.exception(exc)
        raise
    finally:
        report.close()


if __name__ == "__main__":
    _run_from_ue()
