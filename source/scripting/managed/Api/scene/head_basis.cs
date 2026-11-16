using System;

namespace deren.scripting {
    /// <summary>
    /// The game-toon shaders shade a face against a FRAME (where the face looks, its right and up, and
    /// where the head is) rather than against the mesh's own axes. This module derives that frame from
    /// the face frame the glTF loader MEASURED for an asset and publishes it for one character slot.
    ///
    /// The measurement is not a bone transform: the loader fits it to the face primitive's UV side bands
    /// and averaged normal, which is why it survives assets whose skeleton was baked away. A RIGGED asset
    /// also measures the head joint the face hangs from, and only then is there a head position to publish:
    /// without one the shader's sphere term stays off, because publishing the world origin as a centre
    /// would turn `normalize(posWS - centre)` into a radial vector from the origin instead of a head
    /// normal (see deren::engine::head_ubo::center).
    /// </summary>
    public static unsafe class head_basis {
        /// <summary>Characters one frame can shade with their own head frame (mirrors the engine).</summary>
        public const uint max_characters = 16;

        /// <summary>
        /// Derives the head frame of <paramref name="character_index"/> from the frame measured for
        /// <paramref name="asset"/> and publishes it. Returns false (having logged why) when the asset
        /// has no usable face measurement, when the frame is degenerate, or when the host refuses it.
        /// </summary>
        public static bool try_publish(asset_handle asset, uint character_index = 0) {
            return try_publish(asset, character_index, out _);
        }

        public static bool try_publish(asset_handle asset, uint character_index, out script_status status) {
            if (character_index >= max_characters) {
                status = script_status.invalid_argument;
                logger.write($"Game: head basis: character slot {character_index} is above the {max_characters} the renderer keeps");
                return false;
            }
            scene_api.face_frame_desc measured;
            status = scene_api.query_face_frame(asset.value, &measured);
            if (status != script_status.success) {
                logger.write($"Game: head basis: no face frame for asset {asset.value} ({status})");
                return false;
            }
            if (measured.valid == 0) {
                status = script_status.asset_not_found;
                logger.write($"Game: head basis: asset {asset.value} has no usable face frame");
                return false;
            }
            // A rigged asset reports that its frame was measured against the head joint, and then the
            // load-pose head position is part of the measurement too. Managed code never names the joint:
            // the loader already resolved it, so the world frame is the published one either way.
            var front = normalize(measured.world_forward, measured.forward);
            var right = normalize(measured.world_right, measured.right);
            if (!is_usable(front) || !is_usable(right)) {
                status = script_status.asset_invalid;
                logger.write($"Game: head basis: asset {asset.value} reported a degenerate face frame");
                return false;
            }
            // front/right come from the measurement; up is the third axis of a right-handed frame, so the
            // three cannot disagree with each other the way three independent measurements could.
            var up = cross(right, front);
            if (!is_usable(up)) {
                status = script_status.asset_invalid;
                logger.write($"Game: head basis: asset {asset.value} reported parallel face axes");
                return false;
            }
            var has_center = measured.head_relative != 0;
            var record = new scene_api.head_basis_desc {
                captured = 1, // the axes ARE the captured face frame, so the shader reads right as Q80
                has_center = has_center ? (byte)1 : (byte)0
            };
            write(record.front, front);
            write(record.right, right);
            write(record.up, up);
            if (has_center) {
                write(record.center, read(measured.world_center));
            }
            status = scene_api.assign_head_basis(character_index, &record);
            if (status != script_status.success) {
                logger.write($"Game: head basis: character slot {character_index} rejected ({status})");
                return false;
            }
            logger.write($"Game: head basis: slot {character_index} front ({front.x:0.###}, {front.y:0.###}, {front.z:0.###}) " +
                $"right ({right.x:0.###}, {right.y:0.###}, {right.z:0.###})");
            return true;
        }

        private static vector_3 normalize(float* primary, float* fallback) {
            var source = is_usable(primary) ? read(primary) : read(fallback);
            var length = MathF.Sqrt(source.x * source.x + source.y * source.y + source.z * source.z);
            if (!(length > 1.0e-6f) || !float.IsFinite(length)) {
                return default;
            }
            return new vector_3 { x = source.x / length, y = source.y / length, z = source.z / length };
        }

        private static vector_3 read(float* values) {
            return new vector_3 { x = values[0], y = values[1], z = values[2] };
        }

        private static void write(float* target, vector_3 value) {
            target[0] = value.x;
            target[1] = value.y;
            target[2] = value.z;
        }

        private static bool is_usable(vector_3 value) {
            return float.IsFinite(value.x) && float.IsFinite(value.y) && float.IsFinite(value.z) &&
                   value.x * value.x + value.y * value.y + value.z * value.z > 1.0e-12f;
        }

        private static bool is_usable(float* values) {
            return is_usable(read(values));
        }

        private static vector_3 cross(vector_3 left, vector_3 right) {
            return new vector_3 {
                x = left.y * right.z - left.z * right.y,
                y = left.z * right.x - left.x * right.z,
                z = left.x * right.y - left.y * right.x
            };
        }

    }
}
