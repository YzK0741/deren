module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
module deren.engine.gaussian_splatting;
namespace deren::engine::gaussian_splatting {
    namespace {
        auto failure(error_code code, std::string message) {
            return std::unexpected(foundation_error{code, std::move(message)});
        }
        bool finite_lane(std::array<float, 4> const& lane) {
            return std::all_of(lane.begin(), lane.end(), [](float v) { return std::isfinite(v); });
        }
        bool positive_semidefinite(geometry_record const& g) {
            if (g.covariance_x[0] < 0 || g.covariance_yz[0] < 0 || g.covariance_yz[2] < 0) {
                return false;
            }
            double const scale = std::max({std::abs(static_cast<double>(g.covariance_x[0])), std::abs(static_cast<double>(g.covariance_x[1])), std::abs(static_cast<double>(g.covariance_x[2])), std::abs(static_cast<double>(g.covariance_yz[0])), std::abs(static_cast<double>(g.covariance_yz[1])), std::abs(static_cast<double>(g.covariance_yz[2]))});
            if (scale == 0) {
                return true;
            }
            double const a = g.covariance_x[0] / scale;
            double const d = g.covariance_x[1] / scale;
            double const e = g.covariance_x[2] / scale;
            double const b = g.covariance_yz[0] / scale;
            double const f = g.covariance_yz[1] / scale;
            double const c = g.covariance_yz[2] / scale;
            // Bound negative eigenvalues directly: C/scale + tolerance*I must be PSD.
            // A tolerance on determinants/minors instead would hide much larger negative variances.
            constexpr double tolerance = 1e-6;
            double const shifted_a = a + tolerance;
            double const shifted_b = b + tolerance;
            double const shifted_c = c + tolerance;
            return shifted_a * shifted_b - d * d >= 0 && shifted_a * shifted_c - e * e >= 0 && shifted_b * shifted_c - f * f >= 0 && shifted_a * shifted_b * shifted_c + 2 * d * e * f - shifted_a * f * f - shifted_b * e * e - shifted_c * d * d >= 0;
        }
        std::expected<std::array<float, 6>, foundation_error> validate_packed(packed_asset const& asset) {
            if (asset.sh_degree > 3 || asset.geometry.size() != asset.sh.size()) {
                return failure(error_code::invalid_argument, "invalid packed count or SH degree");
            }
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(asset.bounds_min[axis]) || !std::isfinite(asset.bounds_max[axis]) || asset.bounds_min[axis] > asset.bounds_max[axis]) {
                    return failure(error_code::invalid_argument, "invalid packed bounds");
                }
            }
            std::array<float, 6> bounds{};
            bool first = true;
            unsigned const used = 3 * (asset.sh_degree + 1) * (asset.sh_degree + 1);
            for (std::size_t i = 0; i < asset.geometry.size(); ++i) {
                auto const& g = asset.geometry[i];
                auto const& sh = asset.sh[i];
                if (!finite_lane(g.center_opacity) || !finite_lane(g.covariance_x) || !finite_lane(g.covariance_yz) || g.center_opacity[3] < 0 || g.center_opacity[3] > 1 || g.covariance_x[3] != 0 || g.covariance_yz[3] != 0 || !positive_semidefinite(g)) {
                    return failure(error_code::invalid_argument, "invalid packed geometry");
                }
                for (unsigned k = 0; k < 48; ++k) {
                    if (!std::isfinite(sh.coefficients[k]) || (k >= used && sh.coefficients[k] != 0)) {
                        return failure(error_code::invalid_argument, "invalid packed SH");
                    }
                }
                std::array<double, 3> const variances = {g.covariance_x[0], g.covariance_yz[0], g.covariance_yz[2]};
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (g.center_opacity[axis] < asset.bounds_min[axis] || g.center_opacity[axis] > asset.bounds_max[axis]) {
                        return failure(error_code::invalid_argument, "packed bounds exclude center");
                    }
                    double const radius = 3 * std::sqrt(variances[axis]);
                    double const low = static_cast<double>(g.center_opacity[axis]) - radius;
                    double const high = static_cast<double>(g.center_opacity[axis]) + radius;
                    if (!std::isfinite(low) || !std::isfinite(high) || std::abs(low) > std::numeric_limits<float>::max() || std::abs(high) > std::numeric_limits<float>::max()) {
                        return failure(error_code::invalid_argument, "GPU support exceeds float32 range");
                    }
                    float lo = static_cast<float>(low);
                    float hi = static_cast<float>(high);
                    if (static_cast<double>(lo) > low || (radius > 0 && low == g.center_opacity[axis])) {
                        lo = std::nextafter(lo, -std::numeric_limits<float>::infinity());
                    }
                    if (static_cast<double>(hi) < high || (radius > 0 && high == g.center_opacity[axis])) {
                        hi = std::nextafter(hi, std::numeric_limits<float>::infinity());
                    }
                    if (!std::isfinite(lo) || !std::isfinite(hi)) {
                        return failure(error_code::invalid_argument, "GPU support rounding exceeds float32 range");
                    }
                    bounds[axis] = first ? lo : std::min(bounds[axis], lo);
                    bounds[axis + 3] = first ? hi : std::max(bounds[axis + 3], hi);
                }
                first = false;
            }
            return bounds;
        }
    } // namespace
    std::expected<std::shared_ptr<gpu_asset const>, foundation_error> upload_asset(deren::promise::rhi::api_core& core, packed_asset const& asset, foundation_limits const& limits) {
        namespace rhi = deren::promise::rhi;
        auto size = packed_byte_size(asset.geometry.size(), limits);
        if (!size) {
            return std::unexpected(size.error());
        }
        auto bounds = validate_packed(asset);
        if (!bounds) {
            return std::unexpected(bounds.error());
        }
        std::shared_ptr<gpu_asset> result(new gpu_asset{});
        result->count = static_cast<std::uint32_t>(asset.geometry.size());
        result->degree = asset.sh_degree;
        result->bytes = *size;
        for (unsigned i = 0; i < 3; ++i) {
            result->minimum[i] = (*bounds)[i];
            result->maximum[i] = (*bounds)[i + 3];
        }
        if (result->count == 0) {
            return std::shared_ptr<gpu_asset const>{std::move(result)};
        }
        auto* address = rhi::query_extension<rhi::device_address>(core);
        if (!address) {
            return failure(error_code::unsupported, "backend lacks device_address");
        }
        rhi::buffer_desc desc{};
        desc.usage = rhi::buffer_usage::storage_gpu_only;
        desc.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
        desc.initial_bytes = std::as_bytes(std::span{asset.geometry});
        desc.size = desc.initial_bytes.size();
        result->geometry_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
        if (!result->geometry_buffer) {
            return failure(error_code::allocation_failure, "geometry buffer creation failed");
        }
        if (result->geometry_buffer->size() < desc.size) {
            return failure(error_code::allocation_failure, "geometry buffer is undersized");
        }
        result->geometry_gpu_address = address->buffer_address(*result->geometry_buffer, 0);
        if (result->geometry_gpu_address == 0 || result->geometry_gpu_address % alignof(geometry_record) != 0) {
            return failure(error_code::invalid_address, "invalid geometry address");
        }
        desc.initial_bytes = std::as_bytes(std::span{asset.sh});
        desc.size = desc.initial_bytes.size();
        result->sh_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
        if (!result->sh_buffer) {
            return failure(error_code::allocation_failure, "SH buffer creation failed");
        }
        if (result->sh_buffer->size() < desc.size) {
            return failure(error_code::allocation_failure, "SH buffer is undersized");
        }
        result->sh_gpu_address = address->buffer_address(*result->sh_buffer, 0);
        if (result->sh_gpu_address == 0 || result->sh_gpu_address % alignof(sh_record) != 0) {
            return failure(error_code::invalid_address, "invalid SH address");
        }
        return std::shared_ptr<gpu_asset const>{std::move(result)};
    }
} // namespace deren::engine::gaussian_splatting