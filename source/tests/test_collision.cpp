// Headless unit tests: deren.engine.collision (capsule vs OBB, world space) ==============
//
// The cases are chosen so that each one fails for a SPECIFIC wrong implementation, not just
// "it returned something": the rotated box catches box-space dilation (which would report a
// contact 0.2 units early), the thin box catches a sweep that steps by the whole displacement,
// the corner case catches a slide that does not use the cross product of two normals, and the
// tilted floor catches a walkable test that uses a smaller threshold than Unreal's 0.71.
#include "vk_test.h"

#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>

import deren.engine.collision;

namespace {

    using deren::engine::collision::capsule;
    using deren::engine::collision::closest_point_on_obb;
    using deren::engine::collision::closest_point_on_segment;
    using deren::engine::collision::find_floor;
    using deren::engine::collision::move_capsule;
    using deren::engine::collision::obb;
    using deren::engine::collision::sweep_capsule;

    obb make_box(glm::vec3 center, glm::vec3 half, std::uint32_t id = 1) {
        obb box;
        box.center = center;
        box.half = half;
        box.id = id;
        return box;
    }

    capsule make_sphere(glm::vec3 center, float radius) {
        capsule shape;
        shape.lower = center;
        shape.upper = center;
        shape.radius = radius;
        return shape;
    }

    bool near(float actual, float expected, float tolerance = 1e-3f) {
        return std::abs(actual - expected) <= tolerance;
    }

    void test_closest_point_on_obb() {
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f});
        CHECK(near(glm::length(closest_point_on_obb(box, {2.0f, 0.0f, 0.0f}) - glm::vec3(1.0f, 0.0f, 0.0f)), 0.0f));
        CHECK(near(glm::length(closest_point_on_obb(box, {0.5f, 5.0f, 0.0f}) - glm::vec3(0.5f, 2.0f, 0.0f)), 0.0f));
        // A point inside clamps to itself, so the caller has to use the signed distance for overlap.
        CHECK(near(glm::length(closest_point_on_obb(box, {0.0f, 0.0f, 0.0f}) - glm::vec3(0.0f)), 0.0f));
    }

    void test_degenerate_segment() {
        CHECK(near(glm::length(closest_point_on_segment({1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f}) - glm::vec3(1.0f, 2.0f, 3.0f)), 0.0f));
        // Closest point of a slanted segment to a point beside its middle.
        glm::vec3 const on_segment = closest_point_on_segment({0.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, {1.0f, 1.0f, 0.0f});
        CHECK(near(glm::length(on_segment - glm::vec3(0.0f, 1.0f, 0.0f)), 0.0f));
    }

    void test_signed_distance() {
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        glm::vec3 normal{0.0f};
        // Sphere 2.0 above the origin: box top is 0.5, so the surface gap is 2.0 - 0.5 - 0.25.
        float const apart = deren::engine::collision::capsule_obb_signed_distance(make_sphere({0.0f, 2.0f, 0.0f}, 0.25f), box, &normal);
        CHECK(near(apart, 1.25f, 1e-4f));
        CHECK(apart > 0.0f);

        // A vertical capsule measures from its NEAREST end, not from its middle.
        capsule vertical = make_sphere({0.0f, 2.0f, 0.0f}, 0.2f);
        vertical.upper = {0.0f, 3.0f, 0.0f};
        float const vertical_gap = deren::engine::collision::capsule_obb_signed_distance(vertical, box, &normal);
        CHECK(near(vertical_gap, 1.3f, 1e-4f));

        // Overlapping: the normal points from the box toward the capsule (out along +x here).
        float const overlap = deren::engine::collision::capsule_obb_signed_distance(make_sphere({0.2f, 0.0f, 0.0f}, 0.3f), box, &normal);
        CHECK(overlap < 0.0f);
        // Centre 0.2 inside a half extent of 0.5: 0.3 to the face, plus the radius, is 0.6.
        CHECK(near(overlap, -0.6f, 1e-4f));
        CHECK(normal.x > 0.99f);
    }

    void test_axial_sweep() {
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 42u);
        capsule const shape = make_sphere({0.0f, 2.0f, 0.0f}, 0.5f);
        auto const hit = sweep_capsule(&box, 1, shape, {0.0f, -1.5f, 0.0f});
        CHECK(hit.hit);
        // Contact when the sphere centre reaches y = 0.5 + 0.5 = 1.0: 1.0 of the 1.5 requested.
        CHECK(near(hit.time, 2.0f / 3.0f, 1e-3f));
        CHECK(hit.normal.y > 0.99f);
        CHECK(hit.id == 42u);
        // A move that stops short must not report an impact.
        auto const short_move = sweep_capsule(&box, 1, shape, {0.0f, -0.5f, 0.0f});
        CHECK(!short_move.hit);
        // A disabled box is invisible to every query.
        obb hidden = box;
        hidden.enabled = false;
        CHECK(!sweep_capsule(&hidden, 1, shape, {0.0f, -3.0f, 0.0f}).hit);
    }

    void test_rotated_box_corner() {
        // A unit box rotated 45 degrees about z: its world x extent is 0.5 * (cos45 + sin45) =
        // 0.7071, NOT 0.5. Box-space dilation with a radius would report the contact at
        // x = 0.5 + 0.5 = 1.0; the truth is x = 0.7071 + 0.5 = 1.2071.
        obb rotated;
        float const cosine = 0.70710678f;
        float const sine = 0.70710678f;
        rotated.center = {0.0f, 0.0f, 0.0f};
        rotated.axis_x = {cosine, sine, 0.0f};
        rotated.axis_y = {-sine, cosine, 0.0f};
        rotated.axis_z = {0.0f, 0.0f, 1.0f};
        rotated.half = {0.5f, 0.5f, 0.5f};
        capsule const shape = make_sphere({3.0f, 0.0f, 0.0f}, 0.5f);
        auto const hit = sweep_capsule(&rotated, 1, shape, {-3.0f, 0.0f, 0.0f});
        CHECK(hit.hit);
        float const travelled = 3.0f - 1.20710678f;
        CHECK(near(hit.time, travelled / 3.0f, 2e-3f));
        CHECK(hit.normal.x > 0.99f);
    }

    void test_start_penetration() {
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        capsule const shape = make_sphere({0.2f, 0.0f, 0.0f}, 0.3f);
        auto const hit = sweep_capsule(&box, 1, shape, {1.0f, 0.0f, 0.0f});
        CHECK(hit.hit);
        CHECK(hit.time == 0.0f);
        CHECK(near(hit.depth, 1.1f, 1e-3f));
        CHECK(hit.normal.x > 0.99f);
    }

    void test_thin_box_does_not_tunnel() {
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {0.02f, 1.0f, 1.0f});
        capsule const shape = make_sphere({-3.0f, 0.0f, 0.0f}, 0.2f);
        auto const hit = sweep_capsule(&box, 1, shape, {6.0f, 0.0f, 0.0f});
        CHECK(hit.hit);
        float const travelled = 3.0f - (0.02f + 0.2f);
        CHECK(near(hit.time, travelled / 6.0f, 1e-3f));
    }

    void test_no_false_positive_at_corner_gap() {
        // A sphere sitting diagonally off a box corner: centre distance is 0.212 from the
        // corner, radius 0.2, so it does NOT touch. Box-space dilation (|x| < 0.7 and |y| < 0.7)
        // would claim it does.
        obb const box = make_box({0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        capsule const shape = make_sphere({0.65f, 0.65f, 0.0f}, 0.2f);
        glm::vec3 normal{0.0f};
        float const gap = deren::engine::collision::capsule_obb_signed_distance(shape, box, &normal);
        CHECK(gap > 0.0f);
        CHECK(near(gap, 0.21213203f - 0.2f, 1e-3f));
        CHECK(!sweep_capsule(&box, 1, shape, {0.0f, 0.0f, 1.0f}).hit);
    }

    void test_slide_along_wall() {
        // Wall face at x = 1.0 (box centred at 1.5 with half 0.5), sphere radius 0.25.
        obb const wall = make_box({1.5f, 0.0f, 0.0f}, {0.5f, 5.0f, 5.0f}, 9u);
        capsule const shape = make_sphere({0.0f, 0.0f, 0.0f}, 0.25f);
        auto const moved = move_capsule(&wall, 1, shape, {1.5f, 0.0f, 1.0f});
        CHECK(moved.blocked);
        CHECK(!moved.grounded);
        CHECK(moved.contacts >= 1);
        // x stops at the wall (0.75 of the 1.5), z keeps its whole 1.0.
        CHECK(near(moved.displacement.x, 0.75f, 1e-2f));
        CHECK(near(moved.displacement.z, 1.0f, 1e-2f));
        CHECK(moved.normal.x < -0.99f);
    }

    void test_corner_stops_at_two_walls() {
        // Faces at x = 0.75 and z = 0.75 for a sphere of radius 0.25 whose centre starts at the
        // origin: the cross product of the two normals must stop the diagonal move, not let it
        // squeeze along the newer wall for the whole remainder.
        obb const walls[2] = {
            make_box({1.0f, 0.0f, 0.0f}, {0.25f, 5.0f, 5.0f}),
            make_box({0.0f, 0.0f, 1.0f}, {5.0f, 5.0f, 0.25f}),
        };
        capsule const shape = make_sphere({0.0f, 0.0f, 0.0f}, 0.25f);
        auto const moved = move_capsule(walls, 2, shape, {1.0f, 0.0f, 1.0f});
        CHECK(moved.blocked);
        CHECK(near(moved.displacement.x, 0.5f, 2e-2f));
        CHECK(near(moved.displacement.z, 0.5f, 2e-2f));
    }

    void test_grounded_and_find_floor() {
        obb const floor = make_box({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f}, 3u);
        auto const landed = move_capsule(&floor, 1, make_sphere({0.0f, 1.0f, 0.0f}, 0.25f), {0.0f, -2.0f, 0.0f});
        CHECK(landed.blocked);
        CHECK(landed.grounded);
        CHECK(near(landed.displacement.y, -0.75f, 1e-2f));

        auto const probed = find_floor(&floor, 1, make_sphere({0.0f, 1.0f, 0.0f}, 0.25f), 2.0f);
        CHECK(probed.hit);
        CHECK(probed.walkable);
        CHECK(near(probed.distance, 0.75f, 1e-2f));
        CHECK(probed.id == 3u);

        // Nothing below the capsule: no hit.
        CHECK(!find_floor(&floor, 1, make_sphere({0.0f, 6.0f, 0.0f}, 0.25f), 2.0f).hit);
    }

    void test_walkable_threshold() {
        // A 50 degree ramp: its normal is (sin50, cos50, 0) = (0.766, 0.643, 0), below Unreal's
        // default WalkableFloorZ of 0.71, so the same probe must report it as NOT walkable.
        obb ramp;
        float const radians = 50.0f * 3.14159265f / 180.0f;
        ramp.center = {0.0f, -0.5f, 0.0f};
        ramp.axis_x = {std::cos(radians), std::sin(radians), 0.0f};
        ramp.axis_y = {-std::sin(radians), std::cos(radians), 0.0f};
        ramp.axis_z = {0.0f, 0.0f, 1.0f};
        ramp.half = {5.0f, 0.5f, 5.0f};
        auto const probed = find_floor(&ramp, 1, make_sphere({0.0f, 1.0f, 0.0f}, 0.25f), 3.0f);
        CHECK(probed.hit);
        CHECK(!probed.walkable);
        CHECK(near(probed.normal.y, std::cos(radians), 2e-2f));

        // The same geometry probed with a looser threshold IS walkable.
        auto const loose = find_floor(&ramp, 1, make_sphere({0.0f, 1.0f, 0.0f}, 0.25f), 3.0f, 0.6f);
        CHECK(loose.hit);
        CHECK(loose.walkable);
    }

} // namespace

int main() {
    test_closest_point_on_obb();
    test_degenerate_segment();
    test_signed_distance();
    test_axial_sweep();
    test_rotated_box_corner();
    test_start_penetration();
    test_thin_box_does_not_tunnel();
    test_no_false_positive_at_corner_gap();
    test_slide_along_wall();
    test_corner_stops_at_two_walls();
    test_grounded_and_find_floor();
    test_walkable_threshold();
    return deren::vk_test::finish("test_collision");
}
