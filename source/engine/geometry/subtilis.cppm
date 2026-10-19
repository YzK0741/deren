module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
export module deren.engine.subtilis;
import deren.engine.meshlet;
import deren.utility.shared;

export namespace deren::engine::subtilis {
    enum class run_source : std::uint32_t { shared = 0,
                                            visible = 1 };
    enum class fallback_reason { none,
                                 capacity,
                                 source_mismatch,
                                 overlap,
                                 publication_failed };
    struct reservation {
        std::uint32_t base = 0, count = 0;
    };
    struct visible_run {
        std::uint32_t base = 0, count = 0;
        run_source source = run_source::shared;
        fallback_reason reason = fallback_reason::none;
    };
    [[nodiscard]] constexpr visible_run shared_run(reservation space, fallback_reason reason = fallback_reason::none) noexcept {
        return {space.base, space.count, run_source::shared, reason};
    }
    /// Object-space planes from a column-major clip_from_object matrix. Testing the original
    /// sphere against each plane handles nonuniform scale and shear without a guessed radius scale.
    [[nodiscard]] inline bool sphere_visible(meshlet const& sphere, std::array<float, 16> const& clip) noexcept {
        if (!std::isfinite(sphere.center_x) || !std::isfinite(sphere.center_y) || !std::isfinite(sphere.center_z) ||
            !std::isfinite(sphere.radius) || sphere.radius < 0)
            return true;
        for (float value : clip)
            if (!std::isfinite(value))
                return true;
        for (unsigned plane = 0; plane < 6; ++plane) {
            unsigned axis = plane / 2;
            double sign = (plane & 1u) ? -1.0 : 1.0;
            std::array<double, 4> equation{};
            for (unsigned column = 0; column < 4; ++column)
                // Vulkan's near plane is z >= 0; other planes are +/-axis <= w.
                equation[column] = plane == 4 ? double(clip[column * 4 + 2])
                                              : double(clip[column * 4 + 3]) + sign * double(clip[column * 4 + axis]);
            double distance = equation[0] * sphere.center_x + equation[1] * sphere.center_y + equation[2] * sphere.center_z + equation[3];
            double radius = sphere.radius * std::sqrt(equation[0] * equation[0] + equation[1] * equation[1] + equation[2] * equation[2]);
            double slack = 1e-5 * std::max({1.0, std::abs(distance), radius});
            if (distance < -radius - slack)
                return false;
        }
        return true;
    }
    /// Publish complete records first; only a successful writer enables the visible-table metadata.
    /// `published` protects earlier items sharing this frame's table. Empty success needs no bytes.
    template <class Publish>
    [[nodiscard]] visible_run prepare_lod0(std::span<meshlet const> records, reservation space,
                                           std::array<float, 16> const& clip_from_object,
                                           deren::utility::vessel::vector<meshlet>& scratch,
                                           std::span<reservation const> published, Publish&& publish) {
        scratch.clear();
        if (space.base > meshlet_capacity || space.count > meshlet_capacity - space.base || space.count > (1u << 14))
            return shared_run(space, fallback_reason::capacity);
        if (records.size() != space.count)
            return shared_run(space, fallback_reason::source_mismatch);
        for (auto other : published)
            if (space.count && other.count && std::uint64_t(space.base) < std::uint64_t(other.base) + other.count &&
                std::uint64_t(other.base) < std::uint64_t(space.base) + space.count)
                return shared_run(space, fallback_reason::overlap);
        scratch.reserve(records.size());
        for (auto const& record : records)
            if (sphere_visible(record, clip_from_object))
                scratch.push_back(record);
        if (!scratch.empty() && !publish(space.base, std::span<meshlet const>{scratch}))
            return shared_run(space, fallback_reason::publication_failed);
        return {space.base, static_cast<std::uint32_t>(scratch.size()), run_source::visible, fallback_reason::none};
    }
} // namespace deren::engine::subtilis
