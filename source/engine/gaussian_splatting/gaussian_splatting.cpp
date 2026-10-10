module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <limits>
#include <span>
#include <string>
#include <vector>
module deren.engine.gaussian_splatting;
namespace deren::engine::gaussian_splatting {
    namespace {
        auto invalid(std::string message) {
            return std::unexpected(foundation_error{error_code::invalid_argument, std::move(message)});
        }
        auto limited(std::string message) {
            return std::unexpected(foundation_error{error_code::limit_exceeded, std::move(message)});
        }
        bool float_finite(double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        }
        std::expected<std::array<double, 6>, foundation_error> covariance_of(deren::gaussian::particle const& p) {
            for (float c : p.center) {
                if (!std::isfinite(c)) {
                    return invalid("non-finite center");
                }
            }
            for (float s : p.scale) {
                if (!std::isfinite(s) || s <= 0) {
                    return invalid("scale must be finite and positive");
                }
            }
            if (!std::isfinite(p.opacity) || p.opacity < 0 || p.opacity > 1) {
                return invalid("opacity must lie in [0,1]");
            }
            for (float c : p.sh) {
                if (!std::isfinite(c)) {
                    return invalid("non-finite SH coefficient");
                }
            }
            double norm = 0;
            for (float q : p.rotation) {
                if (!std::isfinite(q)) {
                    return invalid("non-finite quaternion");
                }
                norm += static_cast<double>(q) * q;
            }
            double const length = std::sqrt(norm);
            if (std::abs(length - 1) > 1e-4) {
                return invalid("quaternion must be normalized within 1e-4");
            }
            double const w = p.rotation[0] / length, x = p.rotation[1] / length, y = p.rotation[2] / length, z = p.rotation[3] / length;
            std::array<std::array<double, 3>, 3> const rotation = {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)}, {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)}, {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
            auto entry = [&](unsigned i, unsigned j) { double result=0;for(unsigned k=0;k<3;++k) { double const s=p.scale[k]; result+=rotation[i][k]*rotation[j][k]*s*s; } return result; };
            std::array<double, 6> const covariance = {entry(0, 0), entry(0, 1), entry(0, 2), entry(1, 1), entry(1, 2), entry(2, 2)};
            for (double c : covariance) {
                if (!float_finite(c)) {
                    return invalid("covariance exceeds float32 range");
                }
            }
            return covariance;
        }
        std::expected<std::array<float, 6>, foundation_error> support_bounds(deren::gaussian::particle const& p, std::array<double, 6> const& cov) {
            std::array<float, 6> result{};
            constexpr std::array<unsigned, 3> diagonals = {0, 3, 5};
            for (unsigned i = 0; i < 3; ++i) {
                double const radius = 3 * std::sqrt(cov[diagonals[i]]);
                double const low = static_cast<double>(p.center[i]) - radius, high = static_cast<double>(p.center[i]) + radius;
                if (!float_finite(low) || !float_finite(high)) {
                    return invalid("support bounds exceed float32 range");
                }
                float lo = static_cast<float>(low), hi = static_cast<float>(high);
                if (static_cast<double>(lo) > low) {
                    lo = std::nextafter(lo, -std::numeric_limits<float>::infinity());
                }
                if (static_cast<double>(hi) < high) {
                    hi = std::nextafter(hi, std::numeric_limits<float>::infinity());
                }
                if (!std::isfinite(lo) || !std::isfinite(hi)) {
                    return invalid("outward bounds exceed float32 range");
                }
                result[i] = lo;
                result[i + 3] = hi;
            }
            return result;
        }
    } // namespace

    std::expected<glm::mat4, foundation_error> validate_model(glm::mat4 const& model) {
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned j = 0; j < 4; ++j) {
                if (!std::isfinite(model[i][j])) {
                    return invalid("non-finite model matrix");
                }
            }
        }
        if (model[0][3] != 0 || model[1][3] != 0 || model[2][3] != 0 || model[3][3] != 1) {
            return invalid("model matrix must be affine");
        }
        glm::dmat4 const m{model};
        std::array<glm::dvec3, 3> axes = {glm::dvec3{m[0]}, glm::dvec3{m[1]}, glm::dvec3{m[2]}};
        std::array<double, 3> lengths{};
        for (unsigned i = 0; i < 3; ++i) {
            lengths[i] = glm::length(axes[i]);
            if (!(lengths[i] > 0)) {
                return invalid("singular model matrix");
            }
            axes[i] /= lengths[i];
        }
        for (unsigned i = 1; i < 3; ++i) {
            if (std::abs(lengths[i] - lengths[0]) > 1e-5 * std::max(lengths[i], lengths[0])) {
                return invalid("non-uniform scale is unsupported");
            }
        }
        for (unsigned i = 0; i < 3; ++i) {
            for (unsigned j = i + 1; j < 3; ++j) {
                if (std::abs(glm::dot(axes[i], axes[j])) > 1e-5) {
                    return invalid("shear is unsupported");
                }
            }
        }
        if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) <= 0) {
            return invalid("reflection is unsupported");
        }
        glm::dmat4 const inverse = glm::inverse(m);
        glm::mat4 result{0};
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned j = 0; j < 4; ++j) {
                if (!float_finite(inverse[i][j])) {
                    return invalid("inverse model exceeds float32 range");
                }
                result[i][j] = static_cast<float>(inverse[i][j]);
            }
        }
        return result;
    }
    std::expected<std::vector<draw_reference>, foundation_error> sort_draw_references(std::span<sort_instance const> instances, sort_camera const& camera, foundation_limits const& limits) {
        for (unsigned i = 0; i < 3; ++i) {
            if (!std::isfinite(camera.position[i]) || !std::isfinite(camera.forward[i])) {
                return invalid("non-finite camera");
            }
        }
        glm::dvec3 const forward{camera.forward};
        if (std::abs(glm::length(forward) - 1) > 1e-5) {
            return invalid("camera forward must be unit length within 1e-5");
        }
        if (instances.size() > std::numeric_limits<std::uint32_t>::max()) {
            return limited("too many instances");
        }
        std::uint64_t total = 0;
        // Count before allocating either keys or references.
        for (auto const& instance : instances) {
            if (!instance.asset) {
                return invalid("null CPU asset");
            }
            if (instance.asset->sh_degree > 3) {
                return invalid("SH degree must be 0..3");
            }
            auto inverse = validate_model(instance.model);
            if (!inverse) {
                return std::unexpected(inverse.error());
            }
            auto const count = instance.asset->particles.size();
            if (count > std::numeric_limits<std::uint32_t>::max() || count > std::numeric_limits<std::uint32_t>::max() - total) {
                return limited("draw count exceeds uint32");
            }
            total += count;
            if (total > limits.max_references || total > limits.max_packed_bytes / sizeof(draw_reference) || total > std::numeric_limits<std::size_t>::max() / sizeof(draw_reference)) {
                return limited("draw references exceed budget");
            }
        }
        std::vector<std::uint64_t> ids;
        ids.reserve(instances.size());
        for (auto const& i : instances) {
            ids.push_back(i.stable_id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            return invalid("duplicate stable instance ID");
        }
        struct depth_reference {
            double depth;
            draw_reference reference;
        };
        std::vector<depth_reference> keyed;
        if (total > keyed.max_size()) {
            return limited("sort scratch exceeds addressable size");
        }
        keyed.reserve(static_cast<std::size_t>(total));
        for (std::size_t i = 0; i < instances.size(); ++i) {
            auto const& instance = instances[i];
            glm::dmat4 const model{instance.model};
            for (std::size_t j = 0; j < instance.asset->particles.size(); ++j) {
                auto const& p = instance.asset->particles[j];
                auto cov = covariance_of(p);
                if (!cov) {
                    return std::unexpected(cov.error());
                }
                auto bounds = support_bounds(p, *cov);
                if (!bounds) {
                    return std::unexpected(bounds.error());
                }
                glm::dvec4 const world = model * glm::dvec4{p.center[0], p.center[1], p.center[2], 1};
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (!float_finite(world[axis])) {
                        return invalid("transformed center exceeds float32 range");
                    }
                }
                double const depth = glm::dot(glm::dvec3{world} - glm::dvec3{camera.position}, forward);
                if (!std::isfinite(depth)) {
                    return invalid("non-finite sort depth");
                }
                keyed.push_back({depth, {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)}});
            }
        }
        std::sort(keyed.begin(), keyed.end(), [&](depth_reference const& a, depth_reference const& b) {
            if (a.depth != b.depth) {
                return a.depth > b.depth;
            }
            auto const aid = instances[a.reference.instance_index].stable_id, bid = instances[b.reference.instance_index].stable_id;
            if (aid != bid) {
                return aid < bid;
            }
            return a.reference.local_gaussian_index < b.reference.local_gaussian_index;
        });
        std::vector<draw_reference> result;
        result.reserve(static_cast<std::size_t>(total));
        for (auto const& k : keyed) {
            result.push_back(k.reference);
        }
        return result;
    }
    std::expected<std::uint64_t, foundation_error> packed_byte_size(std::uint64_t count, foundation_limits const& limits) {
        constexpr std::uint64_t stride = sizeof(geometry_record) + sizeof(sh_record);
        if (count > std::numeric_limits<std::uint32_t>::max() || count > limits.max_references || count > std::numeric_limits<std::uint64_t>::max() / stride || count > std::numeric_limits<std::size_t>::max() / stride) {
            return limited("particle count or byte arithmetic exceeds limit");
        }
        std::uint64_t const bytes = count * stride;
        if (bytes > limits.max_packed_bytes) {
            return limited("packed asset exceeds byte budget");
        }
        return bytes;
    }
    std::expected<packed_asset, foundation_error> pack_asset(deren::gaussian::asset const& input, foundation_limits const& limits) {
        if (input.sh_degree > 3) {
            return invalid("SH degree must be 0..3");
        }
        auto bytes = packed_byte_size(input.particles.size(), limits);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        packed_asset result{};
        result.sh_degree = input.sh_degree;
        // Validate all inputs before allocating output arrays; no partial asset is published.
        for (auto const& p : input.particles) {
            auto cov = covariance_of(p);
            if (!cov) {
                return std::unexpected(cov.error());
            }
            auto bounds = support_bounds(p, *cov);
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
        }
        result.geometry.reserve(input.particles.size());
        result.sh.reserve(input.particles.size());
        bool first = true;
        for (auto const& p : input.particles) {
            auto const cov = *covariance_of(p);
            auto const bounds = *support_bounds(p, cov);
            geometry_record record{};
            record.center_opacity = {p.center[0], p.center[1], p.center[2], p.opacity};
            record.covariance_x = {static_cast<float>(cov[0]), static_cast<float>(cov[1]), static_cast<float>(cov[2]), 0};
            record.covariance_yz = {static_cast<float>(cov[3]), static_cast<float>(cov[4]), static_cast<float>(cov[5]), 0};
            sh_record sh{};
            unsigned const used = 3 * (input.sh_degree + 1) * (input.sh_degree + 1);
            std::copy_n(p.sh.begin(), used, sh.coefficients.begin());
            result.geometry.push_back(record);
            result.sh.push_back(sh);
            for (unsigned i = 0; i < 3; ++i) {
                result.bounds_min[i] = first ? bounds[i] : std::min(result.bounds_min[i], bounds[i]);
                result.bounds_max[i] = first ? bounds[i + 3] : std::max(result.bounds_max[i], bounds[i + 3]);
            }
            first = false;
        }
        return result;
    }
} // namespace deren::engine::gaussian_splatting