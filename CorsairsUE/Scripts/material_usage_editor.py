"""Read-only helpers for auditing effective material usage in loaded levels.

Importing this module does not load a level, run a check, or save anything.
"""

from dataclasses import dataclass

import unreal

from material_usage_rules import required_usages, resolve_blend_mode


TERRAIN_TAG = "CorsairsTerrain"
PLACEHOLDERS = ("WorldGrid", "DefaultMaterial", "T_White")

USAGE_ENUMS = {
    "instanced_static_meshes":
        unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
    "nanite": unreal.MaterialUsage.MATUSAGE_NANITE,
}


@dataclass
class MaterialBinding:
    actor: object
    component: object
    component_kind: str
    mesh: object
    material: object
    material_index: int
    blend_mode: str
    nanite_enabled: bool
    required: set

    @property
    def material_path(self):
        return self.material.get_path_name()

    @property
    def component_path(self):
        return f"{self.actor.get_path_name()}:{self.component.get_name()}"


def load_level(map_name):
    return unreal.get_editor_subsystem(
        unreal.LevelEditorSubsystem).load_level(f"/Game/Maps/{map_name}")


def _mesh_has_nanite(mesh):
    settings = mesh.get_editor_property("nanite_settings")
    return bool(settings.enabled)


def _component_disallows_nanite(component):
    return bool(component.get_editor_property("disallow_nanite"))


def material_blend_mode(material):
    base_material = material.get_base_material()
    base_value = base_material.get_editor_property("blend_mode")
    base_blend_mode = (
        "translucent"
        if base_value == unreal.BlendMode.BLEND_TRANSLUCENT
        else "opaque")
    effective_value = material.get_blend_mode()
    effective_blend_mode = (
        "translucent"
        if effective_value == unreal.BlendMode.BLEND_TRANSLUCENT
        else "opaque")
    return resolve_blend_mode(base_blend_mode, effective_blend_mode)


def _actor_has_terrain_tag(actor):
    return any(str(tag) == TERRAIN_TAG
               for tag in actor.get_editor_property("tags"))


def _components(actors):
    for actor in actors:
        for component in actor.get_components_by_class(
                unreal.HierarchicalInstancedStaticMeshComponent):
            mesh = component.get_editor_property("static_mesh")
            if mesh is None or component.get_instance_count() == 0:
                continue
            yield actor, component, "hism", mesh

        if not _actor_has_terrain_tag(actor):
            continue
        for component in actor.get_components_by_class(
                unreal.StaticMeshComponent):
            if isinstance(
                    component,
                    unreal.HierarchicalInstancedStaticMeshComponent):
                continue
            mesh = component.get_editor_property("static_mesh")
            if mesh is None:
                continue
            yield actor, component, "terrain", mesh


def material_bindings():
    actors = unreal.get_editor_subsystem(
        unreal.EditorActorSubsystem).get_all_level_actors()
    for actor, component, component_kind, mesh in _components(actors):
        nanite_enabled = (
            _mesh_has_nanite(mesh)
            and not _component_disallows_nanite(component))
        for material_index, material in enumerate(component.get_materials()):
            if material is None:
                continue
            blend_mode = material_blend_mode(material)
            yield MaterialBinding(
                actor=actor,
                component=component,
                component_kind=component_kind,
                mesh=mesh,
                material=material,
                material_index=material_index,
                blend_mode=blend_mode,
                nanite_enabled=nanite_enabled,
                required=required_usages(
                    component_kind, nanite_enabled, blend_mode))


def has_usage(material, usage_name):
    return unreal.MaterialEditingLibrary.has_material_usage(
        material, USAGE_ENUMS[usage_name])


def has_usage_override(material, usage_name):
    if not isinstance(material, unreal.MaterialInstanceConstant):
        return False
    return unreal.MaterialEditingLibrary.has_material_usage_override(
        material, USAGE_ENUMS[usage_name])


def base_color_texture(material):
    if not isinstance(material, unreal.MaterialInstanceConstant):
        return None
    return unreal.MaterialEditingLibrary \
        .get_material_instance_texture_parameter_value(
            material, unreal.Name("BaseColorTexture"))


def missing_base_color_texture(material):
    texture = base_color_texture(material)
    if texture is None:
        return True
    return any(marker in texture.get_name() for marker in PLACEHOLDERS)


def material_parent_path(material):
    if not isinstance(material, unreal.MaterialInstanceConstant):
        return ""
    parent = material.get_editor_property("parent")
    return parent.get_path_name() if parent else ""
