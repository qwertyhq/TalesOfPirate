def resolve_blend_mode(base_blend_mode, effective_blend_mode):
    return effective_blend_mode or base_blend_mode


def required_usages(component_kind, nanite_enabled, blend_mode):
    required = set()
    if component_kind == "hism":
        required.add("instanced_static_meshes")
    if not nanite_enabled:
        return required
    if blend_mode not in ("opaque", "masked"):
        required.add("disallow_nanite")
    else:
        required.add("nanite")
    return required
