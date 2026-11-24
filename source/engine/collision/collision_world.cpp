// Implementation of deren.engine.collision. See the module interface for why the kernel
// works in world space and what each query is for.
//
// The one idea that carries the whole file: every query is built out of ONE signed distance
// function, `capsule_obb_signed_distance`, and a sweep is conservative advancement over that
// distance. Advancing by exactly the current clearance cannot skip a box (the capsule is
// touching the box after the step in the worst case), and a step that overshoots into the box
// is bisected back to the surface. It is the same argument Unreal's sweeps get from the
// analytic TOI, without needing a closed form per (shape, box, angle) pair.

module;

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

module deren.engine.collision;

namespace deren::engine::collision {

    namespace {

        /// Below this the segment is treated as a point (a sphere capsule).
        constexpr float degenerate_segment_squared = 1e-12f;
        /// Below this the closest-point pair is treated as coincident and the box's shallowest
        /// face supplies the separating direction.
        constexpr float coincident_distance = 1e-5f;
        /// Surfaces closer than this are already touching: a sweep stops HERE instead of
        /// reporting a second contact for the same face (closes grazing asymptotes).
        constexpr float touching_clearance = 1e-6f;
        /// Extra separation applied when the move starts overlapping, so the follow-up sweep
        /// does not immediately re-report the same overlap.
        constexpr float depenetration_slop = 1e-3f;

        /// Penetration depth of a point inside `box`, with the axis of least penetration as the
        /// separating direction. Returns 0 (and a face normal) for a point on the surface.
        float overlap_depth(obb const& box, glm::vec3 point, glm::vec3& normal) noexcept {
            glm::vec3 const local = point - box.center;
            glm::vec3 const axes[3] = {box.axis_x, box.axis_y, box.axis_z};
            float const coordinates[3] = {glm::dot(local, box.axis_x), glm::dot(local, box.axis_y), glm::dot(local, box.axis_z)};
            float const halves[3] = {box.half.x, box.half.y, box.half.z};
            float least = 1e30f;
            glm::vec3 least_normal = box.axis_y;
            for (int axis = 0; axis < 3; ++axis) {
                float const sign = coordinates[axis] >= 0.0f ? 1.0f : -1.0f;
                float const depth = halves[axis] - sign * coordinates[axis];
                if (depth < least) {
                    least = depth;
                    least_normal = axes[axis] * sign;
                }
            }
            normal = least_normal;
            return least > 0.0f ? least : 0.0f;
        }

    } // namespace

    glm::vec3 closest_point_on_obb(obb const& box, glm::vec3 point) noexcept {
        glm::vec3 const local = point - box.center;
        float const x = glm::clamp(glm::dot(local, box.axis_x), -box.half.x, box.half.x);
        float const y = glm::clamp(glm::dot(local, box.axis_y), -box.half.y, box.half.y);
        float const z = glm::clamp(glm::dot(local, box.axis_z), -box.half.z, box.half.z);
        return box.center + box.axis_x * x + box.axis_y * y + box.axis_z * z;
    }

    glm::vec3 closest_point_on_segment(glm::vec3 a, glm::vec3 b, glm::vec3 point) noexcept {
        glm::vec3 const span = b - a;
        float const length_squared = glm::dot(span, span);
        if (length_squared <= degenerate_segment_squared) {
            return a;
        }
        float const t = glm::clamp(glm::dot(point - a, span) / length_squared, 0.0f, 1.0f);
        return a + span * t;
    }

    float capsule_obb_signed_distance(capsule const& shape, obb const& box, glm::vec3* normal) noexcept {
        // Alternating projection between the segment and the (convex) box: each step projects
        // the last pair's counterpart back onto the other set. Both iterates stay ON their
        // sets, so the distance between them is an upper bound on the true minimum - never an
        // underestimate, which is what makes the sweep's advancement safe.
        glm::vec3 on_segment = closest_point_on_segment(shape.lower, shape.upper, (shape.lower + shape.upper) * 0.5f);
        glm::vec3 on_box = closest_point_on_obb(box, on_segment);
        for (int iteration = 0; iteration < 24; ++iteration) {
            glm::vec3 const next_segment = closest_point_on_segment(shape.lower, shape.upper, on_box);
            if (glm::dot(next_segment - on_segment, next_segment - on_segment) <= degenerate_segment_squared) {
                on_segment = next_segment;
                break;
            }
            on_segment = next_segment;
            on_box = closest_point_on_obb(box, on_segment);
        }
        on_box = closest_point_on_obb(box, on_segment);

        glm::vec3 const separation = on_segment - on_box;
        float const separation_length = glm::length(separation);
        if (separation_length > coincident_distance) {
            if (normal != nullptr) {
                *normal = separation / separation_length;
            }
            return separation_length - shape.radius;
        }
        // The segment passes through (or exactly touches) the box: the shallowest face is the
        // direction that costs the least to walk out of.
        glm::vec3 push{0.0f, 1.0f, 0.0f};
        float const depth = overlap_depth(box, on_segment, push);
        if (normal != nullptr) {
            *normal = push;
        }
        return -depth - shape.radius;
    }

    sweep_hit sweep_capsule(obb const* boxes, std::size_t count, capsule const& shape, glm::vec3 displacement) noexcept {
        sweep_hit best;
        float const speed = glm::length(displacement);
        for (std::size_t index = 0; index < count; ++index) {
            obb const& box = boxes[index];
            if (!box.enabled) {
                continue;
            }
            glm::vec3 start_normal{0.0f, 1.0f, 0.0f};
            float const start_clearance = capsule_obb_signed_distance(shape, box, &start_normal);
            if (start_clearance < -depenetration_slop) {
                // Already overlapping: time 0 is the earliest possible impact, so nothing else
                // can win. Report it verbatim and let the caller depenetrate. A surface the shape is
                // merely touching is NOT an overlap: reporting it as one would make the caller push
                // itself out and retry forever instead of sliding, which is how a character parked
                // against a closed door ends up unable to walk along it.
                sweep_hit overlapping;
                overlapping.hit = true;
                overlapping.time = 0.0f;
                overlapping.point = closest_point_on_obb(box, shape.lower);
                overlapping.normal = start_normal;
                overlapping.depth = -start_clearance;
                overlapping.id = box.id;
                return overlapping;
            }
            if (speed <= touching_clearance) {
                continue;
            }

            float time = 0.0f;
            float clearance = glm::max(start_clearance, 0.0f);
            glm::vec3 contact_normal = start_normal;
            bool found = false;
            for (int iteration = 0; iteration < 32; ++iteration) {
                float const step = clearance / speed;
                float const next_time = time + step;
                if (step <= touching_clearance) {
                    // The shape is already on this surface. Only a move that presses INTO the plane is a
                    // contact; a tangential or separating move grazes past it. Without this a character
                    // standing on the floor (or parked against a closed door) reports a time-zero hit on
                    // every sweep and can never walk, because the slide would re-hit the same plane.
                    if (glm::dot(displacement, contact_normal) < 0.0f) {
                        found = true;
                    }
                    break;
                }
                if (next_time > 1.0f) {
                    // The requested move ends before touching this box.
                    break;
                }
                capsule moved = shape;
                moved.lower += displacement * next_time;
                moved.upper += displacement * next_time;
                glm::vec3 next_normal{0.0f, 1.0f, 0.0f};
                float const next_clearance = capsule_obb_signed_distance(moved, box, &next_normal);
                if (next_clearance > 0.0f) {
                    time = next_time;
                    clearance = next_clearance;
                    contact_normal = next_normal;
                    continue;
                }
                // The step landed inside: bisect [time, next_time] down to the surface.
                float low = time;
                float high = next_time;
                for (int refine = 0; refine < 32; ++refine) {
                    float const middle = 0.5f * (low + high);
                    capsule middle_shape = shape;
                    middle_shape.lower += displacement * middle;
                    middle_shape.upper += displacement * middle;
                    glm::vec3 middle_normal{0.0f, 1.0f, 0.0f};
                    if (capsule_obb_signed_distance(middle_shape, box, &middle_normal) <= 0.0f) {
                        high = middle;
                        next_normal = middle_normal;
                    } else {
                        low = middle;
                    }
                }
                time = high;
                contact_normal = next_normal;
                found = true;
                break;
            }
            if (!found) {
                continue;
            }
            if (best.hit && time >= best.time) {
                continue;
            }
            capsule at_hit = shape;
            at_hit.lower += displacement * time;
            at_hit.upper += displacement * time;
            best.hit = true;
            best.time = time;
            best.point = closest_point_on_obb(box, (at_hit.lower + at_hit.upper) * 0.5f);
            best.normal = contact_normal;
            best.depth = 0.0f;
            best.id = box.id;
        }
        return best;
    }

    move_result move_capsule(obb const* boxes, std::size_t count, capsule const& shape, glm::vec3 displacement,
                             float walkable_normal_y, int max_contacts) noexcept {
        move_result result;
        capsule current = shape;
        glm::vec3 remaining = displacement;
        glm::vec3 previous_normal{0.0f, 1.0f, 0.0f};
        bool have_previous = false;
        int const limit = max_contacts > 0 ? max_contacts + 2 : 2;

        for (int step_index = 0; step_index < limit; ++step_index) {
            if (glm::dot(remaining, remaining) <= degenerate_segment_squared) {
                break;
            }
            sweep_hit const impact = sweep_capsule(boxes, count, current, remaining);
            if (!impact.hit) {
                result.displacement += remaining;
                break;
            }
            glm::vec3 const advance = remaining * impact.time;
            result.displacement += advance;
            current.lower += advance;
            current.upper += advance;
            result.blocked = true;
            result.normal = impact.normal;
            result.id = impact.id;
            ++result.contacts;
            if (impact.normal.y >= walkable_normal_y) {
                result.grounded = true;
            }

            if (impact.time <= 0.0f) {
                // Started inside: walk out along the separating normal and retry the SAME
                // request from there. This is Unreal's bStartPenetrating path.
                glm::vec3 const push = impact.normal * (impact.depth + depenetration_slop);
                result.displacement += push;
                current.lower += push;
                current.upper += push;
                previous_normal = impact.normal;
                have_previous = true;
                continue;
            }

            glm::vec3 const rest = remaining * (1.0f - impact.time);
            glm::vec3 slide = rest - impact.normal * glm::dot(rest, impact.normal);
            if (have_previous && glm::dot(previous_normal, impact.normal) < 0.99f) {
                // Two distinct planes: the free direction is their intersection line, not the
                // projection onto the newer plane (Unreal's TwoWallAdjust).
                glm::vec3 const corner = glm::cross(previous_normal, impact.normal);
                float const corner_length = glm::length(corner);
                if (corner_length > 1e-4f) {
                    glm::vec3 const direction = corner / corner_length;
                    slide = direction * glm::dot(rest, direction);
                }
            }
            previous_normal = impact.normal;
            have_previous = true;
            if (glm::dot(slide, slide) <= degenerate_segment_squared) {
                break;
            }
            remaining = slide;
        }
        return result;
    }

    floor_result find_floor(obb const* boxes, std::size_t count, capsule const& shape, float max_distance,
                            float walkable_normal_y) noexcept {
        floor_result result;
        if (max_distance <= 0.0f) {
            return result;
        }
        glm::vec3 const probe{0.0f, -max_distance, 0.0f};
        sweep_hit const impact = sweep_capsule(boxes, count, shape, probe);
        if (!impact.hit) {
            return result;
        }
        result.hit = true;
        result.distance = impact.time * max_distance;
        result.point = impact.point;
        result.normal = impact.normal;
        result.id = impact.id;
        result.walkable = impact.normal.y >= walkable_normal_y;
        return result;
    }

} // namespace deren::engine::collision
