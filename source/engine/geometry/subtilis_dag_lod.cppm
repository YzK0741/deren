module;
#include "subtilis_dag_cut_closure.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
export module deren.engine.subtilis.dag_lod;
export import deren.engine.subtilis.dag_asset;
export import deren.engine.subtilis.lod;
export namespace deren::engine::subtilis {
    namespace dag_lod_detail {
        inline int projection_type(std::array<float, 16> const& p) {
            if (!lod_detail::finite(p) || p[0] <= 0 || p[5] == 0 || p[1] != 0 || p[2] != 0 || p[3] != 0 || p[4] != 0 || p[6] != 0 || p[7] != 0 || p[14] >= 0)
                return 0;
            if (p[11] == -1 && p[15] == 0 && p[10] <= -1 && p[12] == 0 && p[13] == 0)
                return 1;
            if (p[11] == 0 && p[15] == 1 && p[10] < 0 && p[8] == 0 && p[9] == 0)
                return 2;
            return 0;
        }
        inline double scale_bound(lod_detail::matrix const& m) {
            double one = 0, inf = 0;
            for (unsigned i = 0; i < 3; ++i) {
                double col = 0, row = 0;
                for (unsigned j = 0; j < 3; ++j) {
                    col += std::abs(m[i * 4 + j]);
                    row += std::abs(m[j * 4 + i]);
                }
                one = std::max(one, col);
                inf = std::max(inf, row);
            }
            return std::sqrt(one * inf);
        }
    } // namespace dag_lod_detail
    /// Whole-object DAG cut, in stable cluster ID order. Object/frustum and HZB
    /// culling stay in the caller's existing pipeline. Capacity never truncates.
    inline std::expected<cut_statistics, cut_error> select_cut(dag_asset const& geometry, view_parameters const& view, uint32_t capacity, utility::vessel::vector<meshlet>& out) {
        out.clear();
        auto kind = dag_lod_detail::projection_type(view.projection);
        bool force = view.force_finest || !kind || !lod_detail::affine(view.model) || !lod_detail::affine(view.view) || !view.width || !view.height || !std::isfinite(view.near_plane) || view.near_plane <= 0 || !std::isfinite(view.target_error_px) || view.target_error_px < 0;
        force |= kind != (view.kind == projection_kind::perspective ? 1 : view.kind == projection_kind::orthographic ? 2
                                                                                                                     : 0);
        lod_detail::matrix camera{};
        double scale = 1, pixels = 1;
        if (!force) {
            camera = lod_detail::multiply(lod_detail::widen(view.view), lod_detail::widen(view.model));
            scale = dag_lod_detail::scale_bound(camera);
            pixels = std::max(std::abs(double(view.projection[0])) * view.width, std::abs(double(view.projection[5])) * view.height) * .5;
            force = !std::isfinite(scale) || !std::isfinite(pixels);
        }
        cut_statistics stats{};
        stats.forced_finest = force;
        utility::vessel::vector<bool> coarsen(geometry.groups().size(), false);
        auto clusters = geometry.clusters();
        for (uint32_t id = 0; id < geometry.groups().size(); ++id) {
            ++stats.groups_visited;
            if (force)
                continue;
            auto const& g = geometry.groups()[id];
            double depth = -(camera[2] * g.lod.x + camera[6] * g.lod.y + camera[10] * g.lod.z + camera[14]);
            double closest = depth - double(g.lod.radius) * scale;
            if (closest <= double(view.near_plane)) {
                stats.forced_finest = true;
                continue;
            }
            double metric = g.error;
            metric *= scale * pixels;
            if (kind == 1)
                metric /= closest;
            if (!std::isfinite(metric)) {
                stats.forced_finest = true;
                continue;
            }
            coarsen[id] = metric <= double(view.target_error_px);
        }
        // Close every dependency decision before inspecting output clusters.
        dag_lod_detail::close_coarsening(geometry, coarsen);
        stats.groups_selected = static_cast<uint32_t>(std::count(coarsen.begin(), coarsen.end(), true));
        out.reserve(std::min(capacity, geometry.maximum_selected_bound()));
        for (auto const& c : clusters) {
            if ((c.producer == dag_none || coarsen[c.producer]) && (c.consumer == dag_none || !coarsen[c.consumer])) {
                if (out.size() >= capacity) {
                    out.clear();
                    return std::unexpected(cut_error::capacity);
                }
                out.push_back(c.record);
                stats.triangles += c.record.index_count / 3;
            }
        }
        return stats;
    }
} // namespace deren::engine::subtilis
