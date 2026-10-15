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
import deren.utility.shared;
import deren.utility;
namespace deren::engine::gaussian_splatting {
    namespace {
        auto failure(error_code code, deren::utility::vessel::string message) {
            return std::unexpected(foundation_error{code, std::move(message)});
        }
        bool finite_lane(deren::utility::vessel::array<float, 4> const& lane) {
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
        std::expected<deren::utility::vessel::array<float, 6>, foundation_error> validate_packed(packed_asset const& asset) {
            if (asset.sh_degree > 3 || asset.geometry.size() != asset.sh.size()) {
                return failure(error_code::invalid_argument, "invalid packed count or SH degree");
            }
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(asset.bounds_min[axis]) || !std::isfinite(asset.bounds_max[axis]) || asset.bounds_min[axis] > asset.bounds_max[axis]) {
                    return failure(error_code::invalid_argument, "invalid packed bounds");
                }
            }
            deren::utility::vessel::array<float, 6> bounds{};
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
                deren::utility::vessel::array<double, 3> const variances = {g.covariance_x[0], g.covariance_yz[0], g.covariance_yz[2]};
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
        // Validate before making the legacy interface's owning copy. Production transfers packed storage.
        auto size = packed_byte_size(asset.geometry.size(), limits);
        if (!size)
            return std::unexpected(size.error());
        auto bounds = validate_packed(asset);
        if (!bounds)
            return std::unexpected(bounds.error());
        return upload_asset(core, std::make_shared<packed_asset const>(asset), limits, {});
    }
    std::expected<std::shared_ptr<gpu_asset const>, foundation_error> upload_asset(deren::promise::rhi::api_core& core, std::shared_ptr<packed_asset const> source, foundation_limits const& limits, std::shared_ptr<deren::promise::rhi::api_core> device_lease) {
        if (!source || (device_lease && device_lease.get() != &core))
            return failure(error_code::invalid_argument, "Invalid Gaussian source or device lease");
        auto const& asset = *source;
        namespace rhi = deren::promise::rhi;
        deren::utility::log("gaussian upload: enter ({} geometry, {} SH)", asset.geometry.size(), asset.sh.size());
        auto size = packed_byte_size(asset.geometry.size(), limits);
        if (!size) {
            return std::unexpected(size.error());
        }
        deren::utility::log("gaussian upload: byte size ok ({})", *size);
        auto bounds = validate_packed(asset);
        if (!bounds) {
            return std::unexpected(bounds.error());
        }
        deren::utility::log("gaussian upload: packed data validated");
        std::shared_ptr<gpu_asset> result(new gpu_asset{});
        result->device_lease = device_lease;
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
        deren::utility::log("gaussian upload: result fields written; asking whether device_address is announced");
        // ASK THE BIT BEFORE THE OBJECT, which is what the contract's own words prescribe ("a bit is announced only
        // if the ability object exists AND can serve the objects this backend produces"; for every set bit
        // query_extension() must answer). The Vulkan backend does announce and serve it today (core.api_core.cpp's
        // abilities() sets the bit and query_extension() answers with the address view), so this guard is the
        // contract's rule obeyed rather than a workaround: a backend that does NOT announce it gets one named
        // refusal instead of an upload that cannot project.
        if ((core.abilities() & rhi::to_bits(rhi::extension_kind::device_address)) == 0) {
            return failure(error_code::unsupported, "backend does not announce device_address (the Gaussian raster path projects through buffer addresses)");
        }
        auto* address = rhi::query_extension<rhi::device_address>(core);
        if (!address) {
            return failure(error_code::unsupported, "backend lacks device_address");
        }
        deren::utility::log("gaussian upload: {} geometry record(s), {} SH record(s), {} byte(s) total, device_address available",
                            asset.geometry.size(), asset.sh.size(), *size);
        namespace managed = render_resource::instances;
        deren::utility::vessel::array<managed::request, 2> requests;
        deren::utility::vessel::array ids{render_resource::resource_id::gaussian_geometry, render_resource::resource_id::gaussian_sh};
        deren::utility::vessel::array payloads{std::as_bytes(std::span{asset.geometry}), std::as_bytes(std::span{asset.sh})};
        // THE INTENT IS NAMED FOR THE ONLY UPLOAD PATH THAT EXISTS, NOT FOR WHAT THIS BUFFER IS. storage_gpu_only
        // is allocate-only: its Vk usage carries no TRANSFER_DST and create_buffer refuses initial contents
        // (measured in vma.cppm), so an immutable, GPU-only table has to be created through uniform_gpu_only -
        // whose staging copy IS the upload mechanism - and the storage flag below is what makes it readable as an
        // SSBO instead. This splat table is never bound as a uniform buffer and is not one. A dedicated
        // upload-capable storage intent is the honest home for this and is left as a follow-up rather than added
        // here: this batch's constraint is no ABI change.
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            request.id = ids[i];
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::scene;
            request.scope = render_resource::resource_scope::device_wide;
            request.buffer.usage = rhi::buffer_usage::uniform_gpu_only;
            request.buffer.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
            request.buffer.size = payloads[i].size();
            request.initial_data = std::shared_ptr<std::byte const>{source, payloads[i].data()};
            request.initial_data_size = payloads[i].size();
            request.initialization_key = std::make_shared<std::uint8_t const>(0);
        }
        auto& provider = result->storage_chain.emplace<deren::engine::pass::buffer_storage_pass>();
        provider.configure(requests);
        deren::utility::vessel::array<rhi::buffer*, 2> buffers{};
        deren::utility::vessel::array<std::uint64_t, 2> addresses{};
        error_code admission_code = error_code::allocation_failure;
        char const* admission_message = "Gaussian static buffer creation failed";
        auto const admit = [&](managed::resource_snapshot const& snapshot) {
            for (std::size_t i = 0; i < requests.size(); ++i) {
                auto entry = snapshot.find(ids[i]);
                buffers[i] = entry && entry->objects.size() == 1 ? entry->objects[0].buffer.get() : nullptr;
                if (!buffers[i] || buffers[i]->size() < requests[i].buffer.size)
                    return rhi::error::unsupported;
                addresses[i] = address->buffer_address(*buffers[i], 0);
                auto const alignment = i == 0 ? alignof(geometry_record) : alignof(sh_record);
                if (!addresses[i] || addresses[i] % alignment) {
                    admission_code = error_code::invalid_address;
                    admission_message = "Invalid Gaussian static buffer address";
                    return rhi::error::unsupported;
                }
            }
            return rhi::error::ok;
        };
        auto root = device_lease ? device_lease : std::shared_ptr<rhi::api_core>{&core, [](rhi::api_core*) {}};
        if (result->storage_chain.prepare_resources(root, {}, admit) != rhi::error::ok)
            return failure(admission_code, admission_message);
        result->storage_view = result->storage_chain.resources().snapshot();
        result->geometry_buffer = {buffers[0]};
        result->sh_buffer = {buffers[1]};
        result->geometry_gpu_address = addresses[0];
        result->sh_gpu_address = addresses[1];
        for (auto& request : requests)
            request.initial_data.reset();
        provider.configure(requests); // Keep declarations/content identities, release packed upload storage.
        return std::shared_ptr<gpu_asset const>{std::move(result)};
    }
} // namespace deren::engine::gaussian_splatting
