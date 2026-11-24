using System;
using System.Collections.Generic;
using System.Numerics;
using deren;

namespace deren.editor;

internal sealed class scene_collision {
    /// A surface counts as standing ground when its normal is within about 45 degrees of straight up, so
    /// a ramp counts as a floor and a wall or a ceiling does not. The kernel uses the same default.
    internal const float walkable_floor_z = collision_world.default_walkable_floor_normal_y;
    /// A floor this close under the capsule is snapped to, so stairs and slopes do not make the walk
    /// bounce. The band is the Unreal one scaled to metres (MIN_FLOOR_DIST / MAX_FLOOR_DIST = 1.9 / 2.4 cm).
    private const float min_floor_distance = 0.019f;
    private const float max_floor_distance = 0.024f;
    /// A caller that asks for no radius still gets a point, not a line: the kernel needs a positive one.
    private const float point_radius = 0.001f;
    /// A candidate step must beat the plain slide by more than this to be accepted.
    private const float epsilon = 1e-4f;
    /// Longest slice of game time one sweep covers (Unreal's MaxSimulationTimeStep), so a frame that
    /// hiccups is walked in several chunks instead of one long tunnel-prone jump.
    private const float max_time_step = 0.05f;
    /// Most slices a single call may use (Unreal's MaxSimulationIterations).
    private const int max_substeps = 8;

    private readonly scene_tree tree;
    private readonly Dictionary<node_id, node_id> parents = [];
    private readonly Dictionary<node_id, document_node> sources = [];
    private readonly List<node_id> solids = [];
    private readonly Dictionary<node_id, uint> ids = [];
    private readonly HashSet<node_id> moving = [];
    private bool world_built;

    internal scene_collision(scene_document document, scene_tree tree) {
        this.tree = tree;
        collect(document.roots, default);
        // Static boxes keep the transform the document had when the scene started; only a moving solid is
        // re-sampled per sweep. Building here pins that transform even when the first sweep is much later.
        world_built = build_world();
    }

    private void collect(IReadOnlyList<document_node> nodes, node_id parent) {
        foreach (var node in nodes) {
            if (!node.runtime.is_valid) { continue; }
            parents[node.runtime] = parent;
            sources[node.runtime] = node;
            if (node.collision_box) {
                solids.Add(node.runtime);
                ids[node.runtime] = (uint)solids.Count;
                if (node.moving_solid) { moving.Add(node.runtime); }
            }
            collect(node.children, node.runtime);
        }
    }

    internal bool try_move(node_id node, vector_3 displacement, float radius, float half_height, float step_height,
                           float delta_time, out collision_move result) {
        result = default;
        var delta = vector(displacement);
        if (!finite(delta) || !finite(radius) || !finite(half_height) || !finite(step_height) ||
            radius < 0 || half_height < 0 || step_height < 0 ||
            !parents.ContainsKey(node) ||
            !try_world_position(node, out var start)) { return false; }

        var pulled = MathF.Max(radius, point_radius);
        if (!finite(start) || !finite(start + delta)) { return false; }
        if (!build_world()) { return false; }

        var applied = move_substeps(start, delta, pulled, half_height, delta_time);
        if (applied.blocked && step_height > 0.0f && half_height > 0.0f) {
            var stepped = try_step(start, delta, applied.displacement, pulled, half_height, step_height);
            if (stepped.accepted) {
                applied = stepped.move;
            }
        }
        var final_position = start + applied.displacement;
        if (!finite(final_position) || !try_local(node, final_position, out var local)) { return false; }
        if (final_position != start && !tree.try_set_transform(node, local)) { return false; }
        result = new collision_move(values(final_position - start), applied.blocked, values(applied.normal),
                                    applied.grounded);
        return true;
    }

    private readonly record struct substep_result(Vector3 displacement, bool blocked, bool grounded, Vector3 normal);

    /// Walks a move in slices of at most max_time_step of game time (half the radius when the caller
    /// passes no time). Each slice probes the floor first, like Unreal's PhysWalking: standing ground is
    /// a floor answer, never a side contact, and a floor within the snap band is followed instead of
    /// letting the capsule float above a slope.
    private substep_result move_substeps(Vector3 start, Vector3 delta, float radius, float half_height,
                                         float delta_time) {
        int steps = substep_count(delta, radius, delta_time);
        var step = delta / steps;
        var position = start;
        var applied = Vector3.Zero;
        var normal = Vector3.Zero;
        bool blocked = false;
        for (int index = 0; index < steps; ++index) {
            if (!probe_floor(position, radius, half_height, out var floor)) {
                // No floor under the capsule: not standing, and nothing to follow downwards.
            } else if (floor.distance >= min_floor_distance) {
                var snap = new Vector3(0, floor.distance, 0);
                position -= snap;
                applied -= snap;
            }
            var capsule = capsule_at(position, radius, half_height);
            if (!collision_world.move(in capsule, values(step), out var moved)) { break; }
            var travelled = vector(moved.displacement);
            position += travelled;
            applied += travelled;
            if (moved.blocked) {
                blocked = true;
                normal = vector(moved.normal);
            }
        }
        var grounded = probe_floor(position, radius, half_height, out var end);
        if (grounded) {
            normal = vector(end.normal);
        }
        return new substep_result(applied, blocked, grounded, normal);
    }

    /// How many slices this call may spend: game time first (0.05 s each, at most eight), and the
    /// radius as a fallback for callers that move without a clock, so a fast move still cannot tunnel.
    private int substep_count(Vector3 delta, float radius, float delta_time) {
        if (delta_time > 0.0f) {
            return Math.Clamp((int)MathF.Ceiling(delta_time / max_time_step), 1, max_substeps);
        }
        var stride = radius * 0.5f;
        var length = delta.Length();
        return length <= stride || stride <= epsilon
            ? 1
            : Math.Clamp((int)MathF.Ceiling(length / stride), 1, max_substeps);
    }

    private readonly record struct stepped_move(substep_result move, bool accepted);

    /// The step-up a character performs on a kerb: rise out of the obstruction, walk the remainder, and
    /// drop back onto the surface. It is accepted only when it ends on ground and covers more horizontal
    /// distance than the plain slide, so it never makes a wall climbable.
    private stepped_move try_step(Vector3 start, Vector3 delta, Vector3 plain, float radius, float half_height,
                                  float step_height) {
        var plain_horizontal = new Vector3(plain.X, 0, plain.Z);
        var capsule = capsule_at(start, radius, half_height);
        if (!collision_world.move(in capsule, values(new Vector3(0, step_height, 0)), out var lift) || lift.blocked) {
            return new stepped_move(default, false);
        }
        var raised = start + vector(lift.displacement);
        var forward_capsule = capsule_at(raised, radius, half_height);
        if (!collision_world.move(in forward_capsule, values(delta), out var forward)) {
            return new stepped_move(default, false);
        }
        var walked = raised + vector(forward.displacement);
        var walked_horizontal = new Vector3(forward.displacement.x, 0, forward.displacement.z);
        if (walked_horizontal.Length() <= plain_horizontal.Length() + epsilon) {
            return new stepped_move(default, false);
        }
        // Dropping back down is where the step is proven: a probe that finds no ground is a fall, not a step.
        var drop = capsule_at(walked, radius, half_height);
        var reach = step_height + max_floor_distance;
        if (!collision_world.find_floor(in drop, reach, out var floor) || !floor.hit || !floor.walkable) {
            return new stepped_move(default, false);
        }
        var total = vector(lift.displacement) + vector(forward.displacement) +
                    new Vector3(0, -MathF.Max(0.0f, floor.distance), 0);
        return new stepped_move(new substep_result(total, forward.blocked, true, vector(forward.normal)), true);
    }

    /// True when the capsule stands on a walkable floor within the snap band. The distance is measured
    /// from the capsule's lowest point and already excludes the radius.
    private bool probe_floor(Vector3 position, float radius, float half_height, out capsule_floor floor) {
        var capsule = capsule_at(position, radius, half_height);
        if (!collision_world.find_floor(in capsule, max_floor_distance, out floor) ||
            !floor.hit || !floor.walkable || floor.distance < 0.0f || floor.distance > max_floor_distance) {
            return false;
        }
        return true;
    }

    private static collision_capsule capsule_at(Vector3 position, float radius, float half_height) {
        return new collision_capsule(
            new vector_3 { x = position.X, y = position.Y - half_height, z = position.Z },
            new vector_3 { x = position.X, y = position.Y + half_height, z = position.Z },
            radius);
    }

    /// Builds the host's world from the document once, and refreshes the boxes whose node moves.
    private bool build_world() {
        if (!world_built) {
            if (!collision_world.reset()) { return false; }
            foreach (var node in solids) {
                if (!shown(node) || !try_box(node, out var box)) { continue; }
                if (!collision_world.add_box(in box)) { return false; }
            }
            world_built = true;
            return true;
        }
        // A moving solid is sampled every sweep: a door driven by a behaviour blocks the opening it
        // currently fills, and a hidden door stops blocking entirely.
        foreach (var node in moving) {
            if (!shown(node) || !try_box(node, out var box)) {
                _ = collision_world.remove_box(ids[node]);
                continue;
            }
            if (!collision_world.add_box(in box)) { return false; }
        }
        return true;
    }

    /// True when this node and every ancestor is visible in the document, so a hidden door does not block.
    private bool shown(node_id node) {
        for (var current = node; current.is_valid; current = parents[current]) {
            if (sources.TryGetValue(current, out var source) && !source.visible) { return false; }
        }
        return true;
    }

    /// The box a document node stands for. The document node is a unit cube shaped by its own transform,
    /// so the box is that cube in world space: the world matrix's rotation gives the three axes and its
    /// absolute scale gives the half extents. Parent scale is part of the world matrix, so a box inside a
    /// scaled subtree keeps the size it has on screen.
    private bool try_box(node_id node, out collision_box box) {
        box = default;
        if (!try_world_matrix(node, out var world) ||
            !Matrix4x4.Decompose(world, out var scale, out var rotation, out var _)) { return false; }
        var axes = Matrix4x4.CreateFromQuaternion(rotation);
        var half = new Vector3(MathF.Abs(scale.X), MathF.Abs(scale.Y), MathF.Abs(scale.Z)) * 0.5f;
        if (!finite(half) || half.X <= 0 || half.Y <= 0 || half.Z <= 0) { return false; }
        box = new collision_box(values(world.Translation),
                                values(half),
                                values(new Vector3(axes.M11, axes.M12, axes.M13)),
                                values(new Vector3(axes.M21, axes.M22, axes.M23)),
                                values(new Vector3(axes.M31, axes.M32, axes.M33)),
                                ids[node]);
        return true;
    }

    private bool try_world_position(node_id node, out Vector3 position) {
        position = default;
        if (!try_world_matrix(node, out var world)) { return false; }
        position = world.Translation;
        return finite(position);
    }

    private bool try_local(node_id node, Vector3 position, out transform_value local) {
        local = default;
        if (!try_parent_matrix(node, out var parent) ||
            !Matrix4x4.Invert(parent, out var inverse) || !finite(inverse)) { return false; }
        if (!tree.try_get_transform(node, out local)) { return false; }
        local.position = values(Vector3.Transform(position, inverse));
        return true;
    }

    /// The world matrix of a node: its own local transform composed with every ancestor's.
    private bool try_world_matrix(node_id node, out Matrix4x4 world) {
        if (!try_parent_matrix(node, out var parent) || !tree.try_get_transform(node, out var pose)) {
            world = default;
            return false;
        }
        world = viewport_gizmo.local_matrix(pose) * parent;
        return finite(world);
    }

    private bool try_parent_matrix(node_id node, out Matrix4x4 parent) {
        parent = Matrix4x4.Identity;
        for (var current = parents[node]; current.is_valid; current = parents[current]) {
            if (!tree.try_get_transform(current, out var pose)) { return false; }
            parent *= viewport_gizmo.local_matrix(pose);
        }
        return finite(parent);
    }

    private static bool finite(Vector3 value) => float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);

    private static bool finite(Matrix4x4 value) =>
        finite(new Vector3(value.M11, value.M12, value.M13)) &&
        finite(new Vector3(value.M21, value.M22, value.M23)) &&
        finite(new Vector3(value.M31, value.M32, value.M33)) && finite(value.Translation);

    private static bool finite(float value) => float.IsFinite(value);

    private static Vector3 vector(vector_3 value) => new(value.x, value.y, value.z);
    private static vector_3 values(Vector3 value) => new() { x = value.X, y = value.Y, z = value.Z };
}
