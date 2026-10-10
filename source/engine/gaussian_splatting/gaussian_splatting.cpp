module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
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