using deren.scripting;

namespace deren {
    /// <summary>
    /// Per-object material factors for a whole node subtree, as the game authors them.
    ///
    /// An object's shading is not the imported material's: the statue in the shot is the same mesh
    /// with its own tint, its own roughness, or its own shading path (an unlit screen, a character
    /// whose forward pass is the toon one). The engine keeps one material row per imported slot, so
    /// the host clones that row once per leaf and rebuilds the clone from these factors every time
    /// they change -- the values are replacements, not accumulations.
    ///
    /// <see cref="environment_slot"/> selects one of the four environment lanes the material was
    /// built against (0 is the material's own). <see cref="unlit"/> forces the material down the
    /// unlit path regardless of its pipeline, which is how a UI or emissive panel is expressed.
    /// </summary>
    public struct material_override {
        public vector_3 tint;
        public float metallic_scale;
        public float roughness_scale;
        public float emissive_scale;
        public uint environment_slot;
        public bool unlit;

        /// <summary>The factors every material starts from: no tint shift, no scale, lit.</summary>
        public static material_override imported => new() {
            tint = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f },
            metallic_scale = 1.0f,
            roughness_scale = 1.0f,
            emissive_scale = 1.0f,
            environment_slot = 0,
            unlit = false
        };

        /// <summary>
        /// Writes this override into the flat ABI record. <paramref name="enabled"/> is separate from
        /// the factors so a caller can restore the imported material without losing what it authored.
        /// </summary>
        internal readonly unsafe void write_to(material_override_desc* target, bool enabled) {
            target->tint[0] = tint.x;
            target->tint[1] = tint.y;
            target->tint[2] = tint.z;
            target->metallic_scale = metallic_scale;
            target->roughness_scale = roughness_scale;
            target->emissive_scale = emissive_scale;
            target->environment_slot = environment_slot;
            target->unlit = unlit ? (byte)1 : (byte)0;
            target->enabled = enabled ? (byte)1 : (byte)0;
        }
    }
}
