using System;
using System.Globalization;
using System.Numerics;

namespace deren.editor {
    internal sealed class viewport_gizmo {
        internal enum gizmo_mode { select, move, rotate, scale }

        private const float radians = MathF.PI / 180;
        private const float handle_length = 58.0f;
        private static readonly string[] ids = ["gizmo_x", "gizmo_y", "gizmo_z"];
        private static readonly Vector3[] axes = [Vector3.UnitX, Vector3.UnitY, Vector3.UnitZ];
        private readonly axis_handle[] handles = new axis_handle[3];
        private readonly axis_handle[] displayed = new axis_handle[3];
        private Vector2 pivot;
        private Vector2 displayed_pivot;
        private document_node? target;
        private gizmo_mode tool;
        private int active_axis = -1;
        private Vector2 press;
        private axis_handle constraint;

        internal transform_value original { get; private set; }
        internal gizmo_mode active_tool { get; private set; }
        internal document_node? dragging => active_axis >= 0 ? target : null;

        private readonly record struct axis_handle(Vector2 direction, Vector3 parent_axis, float pixels_per_unit);

        internal void layout(rml_document ui, rml_rect viewport, document_node? node, Matrix4x4 parent, gizmo_mode next_tool,
                             bool local, uint width, uint height) {
            target = node;
            tool = next_tool;
            Array.Clear(handles);
            Vector3 world_pivot = node == null ? default : Vector3.Transform(vector(node.local.position), parent);
            if (node != null && tool != gizmo_mode.select && width > 0 && height > 0 &&
                Matrix4x4.Invert(parent, out var inverse) &&
                project(world_pivot, viewport, width, height, out pivot, out var depth)) {
                var basis = rotation(node.local.rotation_degrees);
                bool local_axes = local || tool == gizmo_mode.scale;
                float sample = MathF.Max(depth * 0.01f, 0.001f);
                for (int index = 0; index < handles.Length; ++index) {
                    Vector3 world_axis = axes[index];
                    if (local_axes) {
                        world_axis = Vector3.TransformNormal(Vector3.TransformNormal(world_axis, basis), parent);
                    }
                    float length = world_axis.Length();
                    if (length <= 0 || !float.IsFinite(length)) {
                        continue;
                    }
                    world_axis /= length;
                    if (!project(world_pivot + world_axis * sample, viewport, width, height, out var tip, out _)) {
                        continue;
                    }
                    Vector2 delta = tip - pivot;
                    float screen_length = delta.Length();
                    if (screen_length < 0.15f) {
                        // An axis pointing at the camera still has a useful rotation handle.
                        if (tool != gizmo_mode.rotate) {
                            continue;
                        }
                        delta = new Vector2(-1, 1);
                        screen_length = delta.Length();
                    }
                    handles[index] = new axis_handle(delta / screen_length, Vector3.TransformNormal(world_axis, inverse),
                                                screen_length / sample);
                }
            }
            for (int index = 0; index < handles.Length; ++index) {
                var axis = handles[index];
                if (axis == displayed[index] && (axis.direction == Vector2.Zero || displayed_pivot == pivot)) {
                    continue;
                }
                var id = ids[index];
                ui.try_set_element_style(id, "display", axis.direction == Vector2.Zero ? "none" : "block");
                if (axis.direction != Vector2.Zero) {
                    ui.try_set_element_style(id, "left", dp(pivot.X));
                    ui.try_set_element_style(id, "top", dp(pivot.Y - 1));
                    float angle = MathF.Atan2(axis.direction.Y, axis.direction.X) / radians;
                    ui.try_set_element_style(id, "transform", string.Create(CultureInfo.InvariantCulture, $"rotate({angle:0.##}deg)"));
                }
                displayed[index] = axis;
            }
            displayed_pivot = pivot;
        }

        internal bool begin(Vector2 pointer) {
            if (target == null || dragging != null) {
                return false;
            }
            // Pick the closest projected segment. Leave a small gap around the common pivot.
            float best = 8 * 8;
            for (int index = 0; index < handles.Length; ++index) {
                var axis = handles[index];
                if (axis.direction == Vector2.Zero) {
                    continue;
                }
                Vector2 offset = pointer - pivot;
                float along = Vector2.Dot(offset, axis.direction);
                if (along < 10 || along > handle_length + 8) {
                    continue;
                }
                float distance = (offset - axis.direction * along).LengthSquared();
                if (distance < best) {
                    best = distance;
                    active_axis = index;
                }
            }
            if (active_axis < 0) {
                return false;
            }
            press = pointer;
            original = target.local;
            active_tool = tool;
            constraint = handles[active_axis];
            return true;
        }

        internal transform_value update(Vector2 pointer) {
            var value = original;
            Vector2 delta = pointer - press;
            float along = Vector2.Dot(delta, constraint.direction);
            if (delta.LengthSquared() < 1) {
                return value;
            }
            switch (active_tool) {
                case gizmo_mode.move:
                    value.position = vector(vector(value.position) + constraint.parent_axis * (along / constraint.pixels_per_unit));
                    break;
                case gizmo_mode.rotate:
                    Vector2 tangent = new(-constraint.direction.Y, constraint.direction.X);
                    float angle = Vector2.Dot(delta, tangent) / handle_length;
                    if (MathF.Abs(angle) > 0.00001f) {
                        var basis = rotation(original.rotation_degrees);
                        basis *= Matrix4x4.CreateFromAxisAngle(Vector3.Normalize(constraint.parent_axis), angle);
                        value.rotation_degrees = euler(basis);
                    }
                    break;
                case gizmo_mode.scale:
                    float factor = MathF.Exp(Math.Clamp(along / handle_length, -8, 8));
                    if (active_axis == 0) { value.scale.x *= factor; }
                    if (active_axis == 1) { value.scale.y *= factor; }
                    if (active_axis == 2) { value.scale.z *= factor; }
                    break;
            }
            return value;
        }

        internal void end() => active_axis = -1;

        internal bool try_handle(int index, out Vector2 start, out Vector2 direction) {
            direction = handles[index].direction;
            start = pivot + direction * (handle_length - 6);
            return direction != Vector2.Zero;
        }

        internal static Matrix4x4 local_matrix(transform_value value) =>
            Matrix4x4.CreateScale(vector(value.scale)) * rotation(value.rotation_degrees) *
            Matrix4x4.CreateTranslation(vector(value.position));

        private static Matrix4x4 rotation(vector_3 value) =>
            Matrix4x4.CreateRotationZ(value.z * radians) * Matrix4x4.CreateRotationX(value.x * radians) *
            Matrix4x4.CreateRotationY(value.y * radians);

        internal static vector_3 euler(Matrix4x4 basis) {
            float x = MathF.Asin(Math.Clamp(-basis.M32, -1, 1));
            bool singular = MathF.Abs(MathF.Cos(x)) < 0.0001f;
            float y = singular ? MathF.Atan2(-basis.M13, basis.M11) : MathF.Atan2(basis.M31, basis.M33);
            float z = singular ? 0 : MathF.Atan2(basis.M12, basis.M22);
            return new vector_3 { x = x / radians, y = y / radians, z = z / radians };
        }

        internal static bool project(Vector3 world, rml_rect viewport, uint width, uint height,
                                    out Vector2 screen, out float depth) {
            bool visible = camera_projection.try_project_point(world.X, world.Y, world.Z, out var x, out var y, out depth);
            screen = new Vector2(x * viewport.width / width, y * viewport.height / height);
            return visible && depth > 0 && float.IsFinite(screen.X) && float.IsFinite(screen.Y);
        }

        private static string dp(float value) => string.Create(CultureInfo.InvariantCulture, $"{value:0.##}dp");
        private static Vector3 vector(vector_3 value) => new(value.x, value.y, value.z);
        private static vector_3 vector(Vector3 value) => new() { x = value.X, y = value.Y, z = value.Z };
    }
}
