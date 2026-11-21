using System;
using System.Numerics;

namespace deren.editor {
    internal sealed class viewport_camera {
        private const float radians = MathF.PI / 180.0f;
        private static readonly float half_fov_tan = MathF.Tan(22.5f * radians);
        private int drag_button = -1;
        private Vector2 last_pointer;

        internal void cancel_drag() => drag_button = -1;

        internal void handle_pointer(rml_event input, float viewport_height) {
            bool over_viewport = input.id is "viewport" or "viewport_image";
            switch (input.type) {
                case rml_event_type.pointer_down:
                    if (over_viewport && drag_button < 0 && input.button is 1 or 2) {
                        drag_button = input.button;
                        last_pointer = new Vector2(input.x, input.y);
                    }
                    break;
                case rml_event_type.pointer_up:
                    if (input.button == drag_button) {
                        cancel_drag();
                    }
                    break;
                case rml_event_type.pointer_cancel:
                    cancel_drag();
                    break;
                case rml_event_type.pointer_move:
                    if (drag_button >= 0) {
                        var pointer = new Vector2(input.x, input.y);
                        drag(pointer - last_pointer, viewport_height);
                        last_pointer = pointer;
                    }
                    break;
                case rml_event_type.pointer_wheel:
                    if (over_viewport && orbit_camera.try_get(out var pose)) {
                        pose.distance = Math.Clamp(pose.distance * MathF.Pow(0.9f, input.wheel), 0.25f, 5000.0f);
                        pose.try_apply();
                    }
                    break;
            }
        }

        private void drag(Vector2 delta, float viewport_height) {
            if (viewport_height <= 0 || delta == Vector2.Zero || !orbit_camera.try_get(out var pose)) {
                return;
            }
            if (drag_button == 1) {
                pose.yaw_degrees = MathF.IEEERemainder(pose.yaw_degrees + delta.X * 0.28f, 360);
                pose.pitch_degrees = Math.Clamp(pose.pitch_degrees - delta.Y * 0.28f, -85.0f, 85.0f);
            } else {
                float yaw = pose.yaw_degrees * radians, pitch = pose.pitch_degrees * radians;
                var right = new Vector3(MathF.Cos(yaw), 0, -MathF.Sin(yaw));
                var up = new Vector3(-MathF.Sin(yaw) * MathF.Sin(pitch), MathF.Cos(pitch),
                                     -MathF.Cos(yaw) * MathF.Sin(pitch));
                float world_per_dp = 2 * pose.distance * half_fov_tan / viewport_height;
                var offset = (-delta.X * right + delta.Y * up) * world_per_dp;
                pose.target.x += offset.X;
                pose.target.y += offset.Y;
                pose.target.z += offset.Z;
            }
            pose.try_apply();
        }

        internal bool focus(scene_tree tree, node_id node, float aspect) {
            if (!tree.try_get_bounds(node, out var center, out var radius) || !orbit_camera.try_get(out var pose)) {
                return false;
            }
            if (!float.IsFinite(aspect) || aspect <= 0 || !float.IsFinite(radius)) {
                return false;
            }
            float half_angle = MathF.Atan(half_fov_tan * MathF.Min(aspect, 1.0f));
            pose.target = center;
            // The limiting horizontal or vertical frustum plane must clear the bounding sphere.
            pose.distance = radius > 0 ? MathF.Max(radius * 1.1f / MathF.Sin(half_angle), radius + 0.15f) : 3.0f;
            cancel_drag();
            return pose.try_apply();
        }
    }
}
