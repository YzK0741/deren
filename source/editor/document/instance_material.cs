using System;
using deren;

namespace deren.editor;

internal readonly record struct instance_material(vector_3 tint, float metallic, float roughness, float emissive) {
    internal static instance_material imported => new(material_override.imported.tint, 1, 1, 1);

    internal bool valid => tint.x is >= 0 and <= 1 && tint.y is >= 0 and <= 1 && tint.z is >= 0 and <= 1 &&
                           float.IsFinite(metallic) && metallic >= 0 && float.IsFinite(roughness) && roughness >= 0 &&
                           float.IsFinite(emissive) && emissive >= 0;

    internal material_override to_override(int environment) => new() {
        tint = tint, metallic_scale = metallic, roughness_scale = roughness, emissive_scale = emissive,
        environment_slot = (uint)environment,
    };
}
