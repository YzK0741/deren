// -*- C++ -*-
// module: deren.promise.rhi:core
module;

#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:core;
export import deren.vstd;
export import deren.utility.shared;

import :contract;
import :pipeline;
import :resource;

export namespace deren::promise::rhi {

    struct resource_memory_usage {
        std::uint64_t current_bytes = 0, peak_bytes = 0;
        std::uint64_t current_device_local_bytes = 0, peak_device_local_bytes = 0;
        std::uint64_t current_allocations = 0, peak_allocations = 0;
    };
    /// Actual managed allocations, including backend buffers. Aliases/views do not count twice.
    /// Device-local denotes the memory type, not WDDM residency. Peaks cover allocator lifetime.
    struct resource_memory_statistics {
        std::uint32_t struct_size = sizeof(resource_memory_statistics);
        bool supported = false;
        resource_memory_usage buffers, images, total;
        /// Separately sampled VMA totals; reserved blocks include cached/unallocated capacity.
        std::uint64_t allocator_allocation_bytes = 0, allocator_reserved_bytes = 0, allocator_device_local_reserved_bytes = 0;
    };
    static_assert(sizeof(resource_memory_usage) == 48);
    static_assert(sizeof(resource_memory_statistics) == 176);

    struct resource_factory {
        resource_factory() noexcept = default;
        virtual ~resource_factory() noexcept = default;
        resource_factory(resource_factory const&) = delete;
        resource_factory& operator=(resource_factory const&) = delete;
        [[nodiscard]] virtual resource_memory_statistics memory_statistics() const noexcept {
            return {};
        }
        [[nodiscard]] virtual std::expected<unique_object<lazy_buffer>, error> create_lazy_buffer(lazy_buffer_desc const& /*desc*/) {
            return std::unexpected(error::unsupported);
        }
        [[nodiscard]] virtual std::expected<unique_object<lazy_image>, error> create_lazy_image(lazy_image_desc const& /*desc*/) {
            return std::unexpected(error::unsupported);
        }
        [[nodiscard]] virtual unique_object<buffer> create_buffer(buffer_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<image> create_image(image_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<acceleration_structure> create_acceleration_structure(acceleration_structure_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<micromap> create_micromap(micromap_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<sampler> create_sampler(sampler_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<shader> create_shader(shader_desc const& desc) = 0;
        [[nodiscard]] virtual unique_object<pipeline> create_pipeline(pipeline_desc const& desc) = 0;
        [[nodiscard]] virtual std::expected<resource_group_ref, error> create_resource_group(resource_group_desc const& /*unused*/) {
            return std::unexpected(error::unsupported);
        }
        [[nodiscard]] virtual texture_group_ref assign_texture_group(texture_group_info const& /*info*/, error* result = nullptr) {
            if (result != nullptr) {
                *result = error::unsupported;
            }
            return {};
        }
    };

    /// Borrowed independent command allocation/submission interface; rejects acquired-frame commands.
    /// Submit validates provenance and recording state before queuing. Caller-owned buffers/images
    /// must outlive GPU use; existing primary/secondary/texture-group retention remains in force.
    /// Batches execute in span order, validate atomically, and add no implicit visibility barriers.

} // namespace deren::promise::rhi
