// ============================================================================
// module: deren.engine.collision
// module version: 1.0.0  (independent of the app version in CMakeLists project(VERSION))
//
// Character collision against oriented boxes, in world space. Pure CPU, no GPU and
// no Vulkan dependency - the editor and the managed host call it directly.
//
// WHY THIS EXISTS: the previous implementation swept the character as a point with a
// radius that dilated the BOX in box space (bound = 0.5 + radius * extent). That is
// only correct for a sphere when the box is cubic and unrotated, and it is wrong for
// every capsule: a capsule transformed into a non-uniformly scaled box space is an
// ellipsoid-capsule, so the clearance it measures is not the clearance the renderer
// shows. A character standing on a floor of scale (6, 0.2, 6) got 0.2/6 of the intended
// vertical clearance. This module therefore works in WORLD space: the box is an OBB
// (center, three unit axes, world half extents), the character is a capsule (two
// sphere centers + radius), and the query is a real distance between the two.
//
// THE THREE QUERIES, and what each is for:
//   * sweep_capsule   - one linear move: the first time of impact, the contact normal
//                       and the contact id. Conservative advancement over a
//                       signed-distance function; see the .cpp for why it cannot miss
//                       a box and why a grazing pass is reported slightly early.
//   * move_capsule    - one move that already resolves collisions the way the
//                       character controller needs it: advance to the first impact,
//                       project the remainder onto the contact plane, and let the
//                       cross product of two consecutive normals pick the corner
//                       direction when the projection is trapped between two walls.
//   * find_floor      - the downward probe behind "am I standing on something": the
//                       distance to the floor, its normal and whether the normal is
//                       walkable at the caller's threshold.
// Policy (substepping, step-up, floor snapping, gravity, walk speed) stays in the
// managed character behaviour: this module only answers "where can this capsule go".
//
// NO OWNERSHIP: every query takes a caller-owned array of boxes. The caller decides
// which boxes exist (the scene), which of them move (a moving solid re-sends its OBB
// before each sweep) and which to skip (never include the capsule's own volume).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

export module deren.engine.collision;

/**
 * @file collision_world.cppm
 * @defgroup engine_collision Engine Collision (CPU)
 * @brief Capsule-vs-OBB distance, sweeps, slide moves and floor probes in world space
 * @note
 *      - world space only: a box is an OBB, the character is a capsule
 *      - no ownership: the caller passes the box array on every query
 *      - the default walkable threshold matches Unreal's WalkableFloorZ (0.71, ~45 degrees)
 */

namespace deren::engine::collision {

    /// Unreal's default WalkableFloorZ: a normal at least this close to straight up is
    /// walkable (cos(45 degrees) = 0.7071, rounded up to 0.71 as Unreal does).
    export inline constexpr float default_walkable_floor_normal_y = 0.71f;

    /**
     * @ingroup engine_collision
     * @brief one oriented box in world space
     * @note `axis_x/axis_y/axis_z` must be unit length and mutually perpendicular; `half`
     *       is the extent along each of them in world units. A box with a non-uniform
     *       document scale is represented exactly - that is the point of keeping the axes
     *       and the extents apart.
     */
    export struct obb {
        glm::vec3 center{0.0f};
        glm::vec3 axis_x{1.0f, 0.0f, 0.0f};
        glm::vec3 axis_y{0.0f, 1.0f, 0.0f};
        glm::vec3 axis_z{0.0f, 0.0f, 1.0f};
        glm::vec3 half{0.5f};
        /// Stable caller-side identity, reported back in every hit (a scene node id).
        std::uint32_t id = 0;
        /// A disabled box takes part in no query. The caller uses this for hidden solids.
        bool enabled = true;
    };

    /**
     * @ingroup engine_collision
     * @brief the character volume: all points within `radius` of the segment lower..upper
     * @note A standing character has `lower.y < upper.y` and `upper - lower` = height minus
     *       two radii; a sphere is `lower == upper`.
     */
    export struct capsule {
        glm::vec3 lower{0.0f};
        glm::vec3 upper{0.0f};
        float radius = 0.3f;
    };

    /// Result of one linear sweep. `time` is in units of the requested displacement
    /// (0 = already touching at the start, 1 = no impact inside the move).
    export struct sweep_hit {
        bool hit = false;
        float time = 1.0f;
        /// Contact point on the box surface, at the time of impact.
        glm::vec3 point{0.0f};
        /// Contact normal, pointing from the box toward the capsule.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        /// How deep the capsule already overlapped the box at `time == 0`; 0 otherwise.
        float depth = 0.0f;
        std::uint32_t id = 0;
    };

    /// Result of one slide move (see move_capsule).
    export struct move_result {
        /// The displacement actually applied. Its length is never larger than the request.
        glm::vec3 displacement{0.0f};
        /// Normal of the last contact; straight up when nothing was touched.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        std::uint32_t id = 0;
        /// Number of contacts the move resolved (0 = the whole displacement was free).
        int contacts = 0;
        /// True when anything was touched, including a start overlap.
        bool blocked = false;
        /// True when the last contact's normal is at or above the walkable threshold.
        bool grounded = false;
    };

    /// Result of a downward floor probe.
    export struct floor_result {
        bool hit = false;
        /// Distance from the capsule's lowest point to the floor along the probe.
        float distance = 0.0f;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        std::uint32_t id = 0;
        /// True when the hit normal is at or above the caller's walkable threshold.
        bool walkable = false;
    };

    /**
     * @ingroup engine_collision
     * @brief the point of `box` closest to `point`, in world space
     * @note For a point inside the box this returns the point itself; overlapping geometry
     *       is reported by capsule_obb_signed_distance, not by this helper.
     */
    export glm::vec3 closest_point_on_obb(obb const& box, glm::vec3 point) noexcept;

    /**
     * @ingroup engine_collision
     * @brief the point of segment a..b closest to `point`; equals `a` for a degenerate segment
     */
    export glm::vec3 closest_point_on_segment(glm::vec3 a, glm::vec3 b, glm::vec3 point) noexcept;

    /**
     * @ingroup engine_collision
     * @brief signed distance from the capsule's surface to the box (negative = overlapping)
     * @param normal when the two overlap or touch, receives the direction that separates them
     *        (from the box toward the capsule); untouched when they are apart
     * @note Exact for a sphere and for an axis-aligned capsule, and iterated (alternating
     *       projection between the segment and the box) to the true minimum otherwise - the
     *       distance it returns is never SMALLER than the truth, which is what makes the
     *       conservative advancement in sweep_capsule safe.
     */
    export float capsule_obb_signed_distance(capsule const& shape, obb const& box, glm::vec3* normal = nullptr) noexcept;

    /**
     * @ingroup engine_collision
     * @brief the first impact of a capsule moving along `displacement`
     * @param boxes caller-owned boxes; disabled entries are skipped
     * @param count number of entries in `boxes`
     * @note Uses conservative advancement over capsule_obb_signed_distance: every step
     *       advances by the current clearance, so the capsule can never jump past a box
     *       (no tunnelling at any speed), and a step that lands inside is bisected back to
     *       the surface. A grazing pass is therefore reported up to one clearance estimate
     *       early rather than late - the safe direction for a character controller.
     */
    export sweep_hit sweep_capsule(obb const* boxes, std::size_t count, capsule const& shape, glm::vec3 displacement) noexcept;

    /**
     * @ingroup engine_collision
     * @brief move a capsule, sliding along whatever it hits
     * @param walkable_normal_y normals at or above this height are "walkable" (see
     *        default_walkable_floor_normal_y)
     * @param max_contacts how many impacts one move may resolve before the rest of the
     *        displacement is dropped (Unreal's SlideAlongSurface budget is analogous)
     * @note The loop is the classic one: advance to the first impact, project the remaining
     *       displacement onto the contact plane, and when a second distinct normal appears
     *       use the cross product of the two normals to pick the direction along the corner
     *       line (Unreal's TwoWallAdjust). A move that starts overlapping depenetrates along
     *       the contact normal first and then continues, so a character that was pushed into
     *       a wall by a moving solid recovers instead of staying stuck.
     */
    export move_result move_capsule(obb const* boxes, std::size_t count, capsule const& shape, glm::vec3 displacement,
                                    float walkable_normal_y = default_walkable_floor_normal_y, int max_contacts = 4) noexcept;

    /**
     * @ingroup engine_collision
     * @brief probe downward for the floor under a capsule
     * @param max_distance how far below the capsule's lowest point to look, in world units
     * @note `distance` is measured from the capsule's lowest point, i.e. it is already minus
     *       the radius: a capsule resting on the floor reports ~0, not ~radius. The caller
     *       decides what to do with the distance (Unreal snaps only outside a tolerance
     *       band); this function only reports it.
     */
    export floor_result find_floor(obb const* boxes, std::size_t count, capsule const& shape, float max_distance,
                                   float walkable_normal_y = default_walkable_floor_normal_y) noexcept;

} // namespace deren::engine::collision
