using System;
using System.Collections.Generic;
using System.Numerics;
using deren;

namespace deren.editor;

internal sealed class scene_collision {
    private const float separation = 0.0005f;
    private const int max_contacts = 6;

    internal readonly record struct static_box(Matrix4x4 world_to_box) {
        internal Vector3 face_gradient(int axis) => axis switch {
            0 => new Vector3(world_to_box.M11, world_to_box.M21, world_to_box.M31),
            1 => new Vector3(world_to_box.M12, world_to_box.M22, world_to_box.M32),
            _ => new Vector3(world_to_box.M13, world_to_box.M23, world_to_box.M33),
        };
    }
    private readonly record struct scene_box(node_id node, static_box shape);

    private readonly scene_tree tree;
    private readonly Dictionary<node_id, node_id> parents = [];
    private readonly List<scene_box> boxes = [];
    private readonly List<static_box> candidates = [];

    internal scene_collision(scene_document document, scene_tree tree) {
        this.tree = tree;
        collect(document.roots, default, Matrix4x4.Identity, true);
    }

    private void collect(IReadOnlyList<document_node> nodes, node_id parent, Matrix4x4 parent_world, bool visible) {
        foreach (var node in nodes) {
            if (!node.runtime.is_valid) { continue; }
            parents.Add(node.runtime, parent);
            var world = viewport_gizmo.local_matrix(node.local) * parent_world;
            bool shown = visible && node.visible;
            if (shown && node.collision_box && Matrix4x4.Invert(world, out var inverse) && finite(inverse)) {
                boxes.Add(new scene_box(node.runtime, new static_box(inverse)));
            }
            collect(node.children, node.runtime, world, shown);
        }
    }

    internal bool try_move(node_id node, vector_3 displacement, float radius, out collision_move result) {
        result = default;
        var delta = vector(displacement);
        if (!finite(delta) || !float.IsFinite(radius) || radius < 0 ||
            !parents.TryGetValue(node, out var parent) ||
            !tree.try_get_transform(node, out var local)) { return false; }

        var parent_world = Matrix4x4.Identity;
        while (parent.is_valid) {
            if (!tree.try_get_transform(parent, out var pose)) { return false; }
            parent_world *= viewport_gizmo.local_matrix(pose);
            parent = parents[parent];
        }
        if (!Matrix4x4.Invert(parent_world, out var inverse) || !finite(inverse)) { return false; }
        var start = Vector3.Transform(vector(local.position), parent_world);
        if (!finite(start) || !finite(start + delta)) { return false; }

        candidates.Clear();
        foreach (var box in boxes) {
            if (box.node == node || descends_from(box.node, node) || !tree.try_get_transform(box.node, out _)) { continue; }
            candidates.Add(box.shape);
        }
        var applied = slide(start, delta, radius, candidates, out bool blocked);
        var position = vector(local.position) + Vector3.TransformNormal(applied, inverse);
        if (!finite(position)) { return false; }
        local.position = vector(position);
        if (applied != Vector3.Zero && !tree.try_set_transform(node, local)) { return false; }
        result = new collision_move(vector(applied), blocked);
        return true;
    }

    private bool descends_from(node_id node, node_id root) {
        for (var parent = parents[node]; parent.is_valid; parent = parents[parent]) {
            if (parent == root) { return true; }
        }
        return false;
    }

    internal static Vector3 slide(Vector3 start, Vector3 delta, float radius, IReadOnlyList<static_box> boxes,
                                  out bool blocked) {
        blocked = false;
        var applied = Vector3.Zero;
        for (int iteration = 0; iteration < max_contacts && delta != Vector3.Zero; ++iteration) {
            float first = 1;
            var normal = Vector3.Zero;
            foreach (var box in boxes) {
                if (sweep(box, start + applied, delta, radius, out float time, out var hit_normal) && time <= first) {
                    first = time;
                    normal = hit_normal;
                }
            }
            if (normal == Vector3.Zero) { return applied + delta; }
            blocked = true;
            float closing = -Vector3.Dot(delta, normal);
            float travel = MathF.Max(0, first - separation / closing);
            applied += delta * travel;
            delta *= 1 - travel;
            delta -= normal * MathF.Min(0, Vector3.Dot(delta, normal));
        }
        return applied;
    }

    private static bool sweep(static_box box, Vector3 start, Vector3 displacement, float radius,
                              out float time, out Vector3 normal) {
        time = 0;
        normal = Vector3.Zero;
        var origin = Vector3.Transform(start, box.world_to_box);
        var delta = Vector3.TransformNormal(displacement, box.world_to_box);
        if (!finite(origin) || !finite(delta)) { return false; }
        float enter = float.NegativeInfinity;
        float exit = float.PositiveInfinity;
        float closest_face = float.PositiveInfinity;
        var inside_normal = Vector3.Zero;
        bool inside = true;
        for (int axis = 0; axis < 3; ++axis) {
            var gradient = box.face_gradient(axis);
            float length = gradient.Length();
            if (!float.IsFinite(length) || length == 0) { return false; }
            float position = origin[axis];
            float step = delta[axis];
            float bound = 0.5f + radius * length;
            float depth = (bound - MathF.Abs(position)) / length;
            if (!float.IsFinite(depth)) { return false; }
            inside &= depth > 0;
            var outward = gradient / length;
            if (depth < closest_face) {
                closest_face = depth;
                inside_normal = (position < 0 || (position == 0 && step < 0)) ? -outward : outward;
            }
            if (step == 0) {
                if (depth <= 0) { return false; }
                continue;
            }
            float near = (-bound - position) / step;
            float far = (bound - position) / step;
            if (near > far) { (near, far) = (far, near); }
            if (near > enter) {
                enter = near;
                normal = step > 0 ? -outward : outward;
            }
            exit = MathF.Min(exit, far);
            if (enter >= exit) { return false; }
        }
        if (inside) {
            normal = inside_normal;
        } else {
            if (!(enter >= 0 && enter <= 1 && exit > 0)) { return false; }
            time = enter;
        }
        return Vector3.Dot(displacement, normal) < 0;
    }

    private static bool finite(Vector3 value) => float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);

    private static bool finite(Matrix4x4 value) =>
        finite(new Vector3(value.M11, value.M12, value.M13)) &&
        finite(new Vector3(value.M21, value.M22, value.M23)) &&
        finite(new Vector3(value.M31, value.M32, value.M33)) && finite(value.Translation);

    private static Vector3 vector(vector_3 value) => new(value.x, value.y, value.z);
    private static vector_3 vector(Vector3 value) => new() { x = value.X, y = value.Y, z = value.Z };
}
