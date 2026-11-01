module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <span>
export module deren.engine.subtilis.lod;
import deren.engine.subtilis;
import deren.engine.meshlet;
import deren.engine.subtilis.asset;
import deren.utility.shared;

export namespace deren::engine::subtilis {
    [[nodiscard]] constexpr std::array<float, 16> identity_matrix() noexcept {
        return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }
    enum class projection_kind { perspective,
                                 orthographic,
    };
    struct view_parameters {
        std::array<float, 16> model = identity_matrix(), view = identity_matrix(), projection = identity_matrix();
        std::uint32_t width = 1, height = 1;
        float near_plane = .1f, target_error_px = 1;
        projection_kind kind = projection_kind::orthographic;
        bool force_finest = false;
    };
    enum class cut_error { capacity };
    struct cut_statistics {
        std::uint32_t groups_visited = 0, groups_selected = 0, triangles = 0;
        bool forced_finest = false;
    };
    namespace lod_detail {
        using matrix = std::array<double, 16>;
        inline matrix widen(std::array<float, 16> const& in) noexcept {
            matrix out{};
            for (uint32_t i = 0; i < 16; ++i) {
                out[i] = in[i];
            }
            return out;
        }
        inline matrix multiply(matrix const& a, matrix const& b) noexcept {
            matrix out{};
            for (uint32_t c = 0; c < 4; ++c) {
                for (uint32_t r = 0; r < 4; ++r) {
                    for (uint32_t k = 0; k < 4; ++k) {
                        out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
                    }
                }
            }
            return out;
        }
        inline bool finite(std::array<float, 16> const& in) noexcept {
            return std::ranges::all_of(in, [](float value) { return std::isfinite(value); });
        }
        inline bool affine(std::array<float, 16> const& in) noexcept {
            return finite(in) && in[3] == 0 && in[7] == 0 && in[11] == 0 && in[15] == 1;
        }
        inline bool valid(view_parameters const& p) noexcept {
            if (!affine(p.model) || !affine(p.view) || !finite(p.projection) || !p.width || !p.height ||
                !std::isfinite(p.near_plane) || p.near_plane <= 0 || !std::isfinite(p.target_error_px) || p.target_error_px <= 0) {
                return false;
            }
            auto const& m = p.projection;
            if (m[3] != 0 || m[7] != 0) {
                return false;
            }
            if (p.kind == projection_kind::perspective) {
                return m[11] == -1 && m[15] == 0;
            }
            if (p.kind == projection_kind::orthographic) {
                return m[11] == 0 && m[15] == 1;
            }
            return false;
        }
    } // namespace lod_detail
    /// Deterministic, stateless reference. The screen error is a selection metric, not a final-pixel bound.
    [[nodiscard]] inline std::expected<cut_statistics, cut_error> select_cut(asset const& geometry, view_parameters const& view,
                                                                             std::uint32_t capacity,
                                                                             deren::utility::vessel::vector<meshlet>& selected) {
        selected.clear();
        bool const valid_view = lod_detail::valid(view);
        bool const force_all = view.force_finest || !valid_view;
        cut_statistics stats{};
        stats.forced_finest = force_all;
        auto view_model = lod_detail::multiply(lod_detail::widen(view.view), lod_detail::widen(view.model));
        auto clip = lod_detail::multiply(lod_detail::widen(view.projection), view_model);
        double norm_squared = 0;
        for (uint32_t c = 0; c < 3; ++c) {
            for (uint32_t r = 0; r < 3; ++r) {
                double v = view_model[c * 4 + r];
                norm_squared += v * v;
            }
        }
        double stretch = std::sqrt(norm_squared);
        auto const& p = view.projection;
        double projection_scale = std::max(std::hypot(double(p[0]), double(p[4]), double(p[8])) * view.width / 2,
                                           std::hypot(double(p[1]), double(p[5]), double(p[9])) * view.height / 2);
        struct pending_group {
            std::uint32_t index;
            bool force_finest;
        };
        deren::utility::vessel::vector<pending_group> stack;
        stack.reserve(geometry.groups().size());
        auto roots = geometry.roots();
        for (std::size_t i = roots.size(); i > 0; --i) {
            stack.push_back({roots[i - 1], force_all});
        }
        selected.reserve(std::min(capacity, geometry.maximum_cut()));
        while (!stack.empty()) {
            auto pending = stack.back();
            stack.pop_back();
            ++stats.groups_visited;
            auto const& node = geometry.groups()[pending.index];
            if (valid_view && !sphere_visible(node.bounds.center, node.bounds.radius, clip)) {
                continue;
            }
            double z = view_model[2] * node.bounds.center[0] + view_model[6] * node.bounds.center[1] + view_model[10] * node.bounds.center[2] + view_model[14];
            double nearest = -z - node.bounds.radius * stretch;
            bool near = nearest <= view.near_plane;
            double error = node.record.error * stretch * projection_scale;
            if (view.kind == projection_kind::perspective && !near) {
                error /= nearest;
            }
            bool force_finest = pending.force_finest || near || !std::isfinite(error);
            bool refine = force_finest || error > view.target_error_px;
            if (near || !std::isfinite(error)) {
                stats.forced_finest = true;
            }
            if (node.record.children.count && refine) {
                auto children = node.record.children;
                for (std::uint32_t i = children.count; i > 0; --i) {
                    stack.push_back({children.first + i - 1, force_finest});
                }
                continue;
            }
            auto range = node.record.coarse;
            if (selected.size() > capacity || range.count > capacity - selected.size()) {
                selected.clear();
                return std::unexpected(cut_error::capacity);
            }
            ++stats.groups_selected;
            for (std::uint32_t i = 0; i < range.count; ++i) {
                auto const& record = geometry.clusters()[range.first + i];
                selected.push_back(record);
                stats.triangles += record.index_count / 3;
            }
        }
        return stats;
    }
    template <class Publish>
    [[nodiscard]] visible_run prepare_hierarchy(asset const& geometry, reservation space, view_parameters const& view,
                                                deren::utility::vessel::vector<meshlet>& scratch,
                                                std::span<reservation const> published, Publish&& publish) {
        scratch.clear();
        if (auto reason = check_reservation(space, geometry.lod0_count(), published); reason != fallback_reason::none) {
            return shared_run(space, reason);
        }
        auto cut = select_cut(geometry, view, space.count, scratch);
        if (!cut) {
            return shared_run(space, fallback_reason::capacity);
        }
        if (!scratch.empty() && !publish(space.base, std::span<meshlet const>{scratch})) {
            return shared_run(space, fallback_reason::publication_failed);
        }
        return {space.base, static_cast<std::uint32_t>(scratch.size()), run_source::visible, fallback_reason::none};
    }
} // namespace deren::engine::subtilis
