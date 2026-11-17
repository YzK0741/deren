using System;
using deren.scripting;

namespace deren {
    /// <summary>
    /// One punctual light, as the game authors it: degrees and plain colours, not cosines and lanes.
    ///
    /// A point light ignores <see cref="direction"/> and both cone angles. A spot light uses the
    /// direction as its axis (the host normalizes it) and the cone angles as half angles in degrees,
    /// with <see cref="inner_degrees"/> defaulting to 0 so an author who does not care gets a
    /// hard-edged cone rather than an accidental penumbra.
    ///
    /// <see cref="npr_mode"/> and <see cref="npr_data"/> are carried verbatim into the light UBO for
    /// the toon stages: 0 is their wrap mode, 2 a specular lobe, 3 a rim light, 4 the fog light.
    /// Stylised rigs set them; ordinary lighting leaves them at zero.
    /// </summary>
    public struct point_light {
        public vector_3 position;
        public vector_3 direction;
        public vector_3 colour;
        public float intensity;
        public float range;
        public bool spot;
        public float outer_degrees;
        public float inner_degrees;
        public float npr_mode;
        public float npr_data_x;
        public float npr_data_y;
        public float npr_data_z;
        public float npr_data_w;

        /// <summary>
        /// Writes this light into the flat ABI record. The half-angle cosine conversion lives here so
        /// the interop struct stays a dumb mirror of the C++ one.
        /// </summary>
        internal readonly unsafe void write_to(point_light_desc* target) {
            target->position[0] = position.x;
            target->position[1] = position.y;
            target->position[2] = position.z;
            target->direction[0] = direction.x;
            target->direction[1] = direction.y;
            target->direction[2] = direction.z;
            target->colour[0] = colour.x;
            target->colour[1] = colour.y;
            target->colour[2] = colour.z;
            target->intensity = intensity;
            target->range = range;
            target->outer_cos = half_angle_cosine(outer_degrees);
            target->inner_cos = half_angle_cosine(inner_degrees);
            target->has_inner_cos = inner_degrees > 0.0f ? (byte)1 : (byte)0;
            target->npr_mode = npr_mode;
            target->npr_data[0] = npr_data_x;
            target->npr_data[1] = npr_data_y;
            target->npr_data[2] = npr_data_z;
            target->npr_data[3] = npr_data_w;
            target->spot = spot ? (byte)1 : (byte)0;
        }

        private static float half_angle_cosine(float degrees) {
            return MathF.Cos(degrees * (MathF.PI / 180.0f) * 0.5f);
        }
    }
}
