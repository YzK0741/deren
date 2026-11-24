using System;
using deren.scripting;

namespace deren {
    /// <summary>Where a swept capsule ended up: the first surface it touched, if any.</summary>
    /// <remarks>
    /// <see cref="time"/> is the fraction of the requested displacement that was clear (1 when the
    /// whole path was clear, 0 when the start already overlaps something) and <see cref="depth"/> is
    /// how far the start was inside that box.
    /// </remarks>
    public readonly record struct capsule_hit(bool hit, float time, vector_3 point, vector_3 normal, float depth,
                                              uint id);

    /// <summary>What a slide actually travelled, and whether it ran into anything on the way.</summary>
    /// <remarks>
    /// <see cref="grounded"/> is set when the last contact's surface was walkable - the normal points
    /// up within the walkable threshold - so a behaviour can tell a wall from a floor without looking
    /// at the normal itself.
    /// </remarks>
    public readonly record struct capsule_move(vector_3 displacement, vector_3 normal, uint id, int contacts,
                                               bool blocked, bool grounded);

    /// <summary>One downward floor probe. <see cref="distance"/> is measured along the probe.</summary>
    public readonly record struct capsule_floor(bool hit, bool walkable, float distance, vector_3 point,
                                                vector_3 normal, uint id);

    /// <summary>The swept shape: the segment from <paramref name="lower"/> to <paramref name="upper"/> dilated by the radius.</summary>
    public readonly record struct collision_capsule(vector_3 lower, vector_3 upper, float radius);

    /// <summary>One box of the collision world: a centre, the extent along each of three unit axes, and a caller-chosen id.</summary>
    public readonly record struct collision_box(vector_3 center, vector_3 half, vector_3 axis_x, vector_3 axis_y,
                                                vector_3 axis_z, uint id);

    /// <summary>
    /// The collision world the host's kernel answers, written from managed code.
    /// </summary>
    /// <remarks>
    /// Two halves. The set of boxes is state: the host keeps it for the active scene and
    /// <see cref="reset"/> empties it, so a scene change is followed by a rebuild. The queries are
    /// stateless: <see cref="sweep"/>, <see cref="move"/> and <see cref="find_floor"/> answer for one
    /// capsule against the whole set.
    ///
    /// The POLICY is on this side on purpose - which boxes a document instance produces, the capsule's
    /// size, substepping over large frame times, step-up, floor snapping and what counts as walkable -
    /// because all of it is project tuning, not engine law. The kernel only sweeps a capsule and slides
    /// it along what it hits.
    ///
    /// Boxes are world space: a rotated or non-uniformly scaled document node would otherwise turn the
    /// capsule into an ellipsoid in the node's own frame.
    /// </remarks>
    public static unsafe class collision_world {
        /// <summary>How vertical a surface must be for the kernel to call it a floor (about 44.8 degrees).</summary>
        public const float default_walkable_floor_normal_y = 0.71f;

        /// <summary>Drops every box, for a freshly loaded scene. Returns false when the host has no kernel.</summary>
        public static bool reset() {
            return reset(out _);
        }

        public static bool reset(out script_status status) {
            if (!collision_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            try {
                status = collision_api.table.world_reset();
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: collision reset failed: {error.GetBaseException().Message}");
            }
            return report(status, "reset the collision world");
        }

        /// <summary>Adds a box, replacing one with the same <see cref="collision_box.id"/> in place.</summary>
        public static bool add_box(in collision_box box) {
            return add_box(in box, out _);
        }

        public static bool add_box(in collision_box box, out script_status status) {
            if (box.half.x <= 0.0f || box.half.y <= 0.0f || box.half.z <= 0.0f ||
                !finite(box.center.x) || !finite(box.center.y) || !finite(box.center.z)) {
                status = script_status.invalid_argument;
                host_logger.write($"Game: collision box {box.id} has a degenerate shape; it was not added");
                return false;
            }
            if (!collision_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            var value = new deren.scripting.collision_box_desc {
                center_x = box.center.x,
                center_y = box.center.y,
                center_z = box.center.z,
                half_x = box.half.x,
                half_y = box.half.y,
                half_z = box.half.z,
                axis_0 = box.axis_x.x,
                axis_1 = box.axis_x.y,
                axis_2 = box.axis_x.z,
                axis_3 = box.axis_y.x,
                axis_4 = box.axis_y.y,
                axis_5 = box.axis_y.z,
                axis_6 = box.axis_z.x,
                axis_7 = box.axis_z.y,
                axis_8 = box.axis_z.z,
                id = box.id,
                enabled = 1,
            };
            try {
                status = collision_api.table.add_box(&value);
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: collision box {box.id} request failed: {error.GetBaseException().Message}");
                return false;
            }
            return report(status, $"add collision box {box.id}");
        }

        /// <summary>Removes the box with that id; removing an id that is not there succeeds.</summary>
        public static bool remove_box(uint id) {
            return remove_box(id, out _);
        }

        public static bool remove_box(uint id, out script_status status) {
            if (!collision_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            try {
                status = collision_api.table.remove_box(id);
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: collision box {id} removal failed: {error.GetBaseException().Message}");
                return false;
            }
            return report(status, $"remove collision box {id}");
        }

        /// <summary>The first surface a capsule meets on the way to <paramref name="displacement"/>.</summary>
        public static bool sweep(in collision_capsule capsule, vector_3 displacement, out capsule_hit hit) {
            return sweep(in capsule, displacement, out hit, out _);
        }

        public static bool sweep(in collision_capsule capsule, vector_3 displacement, out capsule_hit hit,
                                 out script_status status) {
            hit = default;
            if (!valid(in capsule, out status)) {
                return false;
            }
            var shape = to_desc(in capsule);
            var delta = stackalloc float[3] { displacement.x, displacement.y, displacement.z };
            var answer = default(collision_hit_desc);
            try {
                status = collision_api.table.sweep_capsule(&shape, delta, &answer);
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: capsule sweep failed: {error.GetBaseException().Message}");
                return false;
            }
            if (!report(status, "sweep the capsule")) {
                return false;
            }
            hit = new capsule_hit(answer.hit != 0, answer.time,
                                  new vector_3 { x = answer.point_x, y = answer.point_y, z = answer.point_z },
                                  new vector_3 { x = answer.normal_x, y = answer.normal_y, z = answer.normal_z },
                                  answer.depth, answer.id);
            return true;
        }

        /// <summary>
        /// Slides the capsule along everything it touches on the way to <paramref name="displacement"/>,
        /// the way a character walks: hit a face, keep the part of the motion along it, try again.
        /// </summary>
        public static bool move(in collision_capsule capsule, vector_3 displacement, out capsule_move result,
                                float walkable_normal_y = default_walkable_floor_normal_y) {
            return move(in capsule, displacement, out result, out _, walkable_normal_y);
        }

        public static bool move(in collision_capsule capsule, vector_3 displacement, out capsule_move result,
                                out script_status status,
                                float walkable_normal_y = default_walkable_floor_normal_y) {
            result = default;
            if (!valid(in capsule, out status)) {
                return false;
            }
            var shape = to_desc(in capsule);
            var delta = stackalloc float[3] { displacement.x, displacement.y, displacement.z };
            var answer = default(collision_move_desc);
            try {
                status = collision_api.table.move_capsule(&shape, delta, walkable_normal_y, &answer);
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: capsule move failed: {error.GetBaseException().Message}");
                return false;
            }
            if (!report(status, "move the capsule")) {
                return false;
            }
            result = new capsule_move(
                new vector_3 { x = answer.displacement_x, y = answer.displacement_y, z = answer.displacement_z },
                new vector_3 { x = answer.normal_x, y = answer.normal_y, z = answer.normal_z },
                answer.id, answer.contacts, answer.blocked != 0, answer.grounded != 0);
            return true;
        }

        /// <summary>Probes straight down for a surface within <paramref name="max_distance"/>.</summary>
        public static bool find_floor(in collision_capsule capsule, float max_distance, out capsule_floor floor,
                                      float walkable_normal_y = default_walkable_floor_normal_y) {
            return find_floor(in capsule, max_distance, out floor, out _, walkable_normal_y);
        }

        public static bool find_floor(in collision_capsule capsule, float max_distance, out capsule_floor floor,
                                      out script_status status,
                                      float walkable_normal_y = default_walkable_floor_normal_y) {
            floor = default;
            if (!valid(in capsule, out status)) {
                return false;
            }
            if (!finite(max_distance) || max_distance <= 0.0f) {
                status = script_status.invalid_argument;
                host_logger.write($"Game: floor probe distance {max_distance} must be positive");
                return false;
            }
            var shape = to_desc(in capsule);
            var answer = default(collision_floor_desc);
            try {
                status = collision_api.table.find_floor(&shape, max_distance, walkable_normal_y, &answer);
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write($"Game: floor probe failed: {error.GetBaseException().Message}");
                return false;
            }
            if (!report(status, "probe for a floor")) {
                return false;
            }
            floor = new capsule_floor(answer.hit != 0, answer.walkable != 0, answer.distance,
                                      new vector_3 { x = answer.point_x, y = answer.point_y, z = answer.point_z },
                                      new vector_3 { x = answer.normal_x, y = answer.normal_y, z = answer.normal_z },
                                      answer.id);
            return true;
        }

        private static collision_capsule_desc to_desc(in collision_capsule capsule) {
            return new collision_capsule_desc {
                lower_x = capsule.lower.x,
                lower_y = capsule.lower.y,
                lower_z = capsule.lower.z,
                upper_x = capsule.upper.x,
                upper_y = capsule.upper.y,
                upper_z = capsule.upper.z,
                radius = capsule.radius,
            };
        }

        private static bool valid(in collision_capsule capsule, out script_status status) {
            if (!collision_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            if (!finite(capsule.radius) || capsule.radius <= 0.0f ||
                !finite(capsule.lower.x) || !finite(capsule.lower.y) || !finite(capsule.lower.z) ||
                !finite(capsule.upper.x) || !finite(capsule.upper.y) || !finite(capsule.upper.z)) {
                status = script_status.invalid_argument;
                host_logger.write("Game: the capsule must have a positive radius and finite ends");
                return false;
            }
            status = script_status.success;
            return true;
        }

        private static bool finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);

        private static bool report(script_status status, string what) {
            if (status == script_status.success) {
                return true;
            }
            host_logger.write($"Game: cannot {what} ({status})");
            return false;
        }
    }
}
