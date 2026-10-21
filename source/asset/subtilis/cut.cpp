module;
#include "cut_closure.hpp"

#include <cstddef>
#include <cstdint>
module deren.asset.subtilis.dag;
namespace deren::asset::subtilis {
    namespace {
        bool affine(array<float, 16> const& m) {
            return std::all_of(m.begin(), m.end(), [](float f) { return std::isfinite(f); }) && m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1;
        }
        array<double, 16> multiply(array<float, 16> const& a, array<float, 16> const& b) {
            array<double, 16> out{};
            for (size_t col = 0; col < 4; ++col)
                for (size_t row = 0; row < 4; ++row)
                    for (size_t k = 0; k < 4; ++k)
                        out[col * 4 + row] += double(a[k * 4 + row]) * b[col * 4 + k];
            return out;
        }
        double scale_bound(array<double, 16> const& m) {
            double one = 0, inf = 0;
            for (size_t i = 0; i < 3; ++i) {
                double col = 0, row = 0;
                for (size_t j = 0; j < 3; ++j) {
                    col += std::abs(m[i * 4 + j]);
                    row += std::abs(m[j * 4 + i]);
                }
                one = std::max(one, col);
                inf = std::max(inf, row);
            }
            // Spectral norm <= sqrt(||M||1 * ||M||infinity), including shear/nonuniform scale.
            return std::sqrt(one * inf);
        }
        int projection_kind(array<float, 16> const& p) {
            if (!std::all_of(p.begin(), p.end(), [](float x) { return std::isfinite(x); }) || p[0] <= 0 || p[5] == 0 || p[1] != 0 || p[2] != 0 || p[3] != 0 || p[4] != 0 || p[6] != 0 || p[7] != 0 || p[14] >= 0)
                return 0;
            if (p[11] == -1 && p[15] == 0 && p[10] <= -1 && p[12] == 0 && p[13] == 0)
                return 1;
            if (p[11] == 0 && p[15] == 1 && p[10] < 0 && p[8] == 0 && p[9] == 0)
                return 2;
            return 0;
        }
    } // namespace
    result<vector<uint32_t>> select_cut(validated_asset const& asset, cut_parameters const& parameters) {
        auto const& p = asset.data();
        auto const& v = parameters;
        auto fallback = [&]() -> result<vector<uint32_t>> {
            if (p.leaves.size() > v.max_clusters)
                return std::unexpected(error{error_code::capacity, "complete LOD0 exceeds cut capacity", {}, {}});
            auto leaves = p.leaves;
            std::sort(leaves.begin(), leaves.end());
            return leaves;
        };
        if (!std::isfinite(v.threshold) || v.threshold < 0)
            return std::unexpected(error{error_code::invalid_options, "invalid cut threshold", {}, {}});
        int kind = 0;
        double scale = 1, pixels = 1;
        array<double, 16> camera{};
        if (v.mode == cut_mode::screen_error) {
            kind = projection_kind(v.projection);
            if (!kind || !affine(v.model) || !affine(v.view) || !v.extent[0] || !v.extent[1] || !std::isfinite(v.near_plane) || v.near_plane <= 0)
                return fallback();
            camera = multiply(v.view, v.model);
            scale = scale_bound(camera);
            pixels = std::max(std::abs(double(v.projection[0])) * v.extent[0], std::abs(double(v.projection[5])) * v.extent[1]) * .5;
            if (!std::isfinite(scale) || !std::isfinite(pixels))
                return fallback();
        } else if (v.mode != cut_mode::model_error)
            return fallback();
        vector<bool> coarsen(p.groups.size(), false);
        for (uint32_t id = 0; id < p.groups.size(); ++id) {
            auto const& g = p.groups[id];
            double metric = g.error;
            if (kind) {
                double depth = -(camera[2] * g.lod.x + camera[6] * g.lod.y + camera[10] * g.lod.z + camera[14]);
                double closest = depth - double(g.lod.radius) * scale;
                if (closest <= double(v.near_plane))
                    continue;
                metric *= scale * pixels;
                if (kind == 1)
                    metric /= closest;
            }
            coarsen[id] = std::isfinite(metric) && metric <= double(v.threshold);
        }
        detail::close_coarsening(p, coarsen);
        vector<uint32_t> selected;
        for (uint32_t id = 0; id < p.clusters.size(); ++id) {
            auto const& c = p.clusters[id];
            if ((c.producer == none || coarsen[c.producer]) && (c.consumer == none || !coarsen[c.consumer]))
                selected.push_back(id);
        }
        if (selected.size() > v.max_clusters)
            return fallback();
        return selected;
    }
} // namespace deren::asset::subtilis
