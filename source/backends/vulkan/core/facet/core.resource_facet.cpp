// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/facet/core.resource_facet.cpp
//
// The resource face owns the descriptor heaps, samplers, texture groups,
// provenance sets and tier-2 resource views. Its virtual entry points implement
// the creation logic here; the root keeps thin forwarders for its own callers.
// ============================================================================
module;
#include "../vma/texture_upload_layout.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.promise.rhi;
import deren.vulkan.core.pipeline;
import deren.vulkan.constant_init;
import :declarations;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    namespace {

        /// the caller's buffer descriptor with every field outside the bytes it declared left at THIS
        /// build's default
        deren::promise::rhi::buffer_desc sanitize_buffer_desc(deren::promise::rhi::buffer_desc const& desc) {
            using rhi_buffer_desc = deren::promise::rhi::buffer_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_buffer_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI buffer_desc is {} B here and {} B in the caller -> the caller's structure is treated as TOO SHORT: "
                                    "a field whose whole extent is not inside those {} B keeps this build's default",
                                    known, declared, declared);
            }

            rhi_buffer_desc options = {};
            if (covered_by(declared, offsetof(rhi_buffer_desc, size), sizeof(rhi_buffer_desc::size))) {
                options.size = desc.size;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, usage), sizeof(rhi_buffer_desc::usage))) {
                options.usage = desc.usage;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, flags), sizeof(rhi_buffer_desc::flags))) {
                options.flags = desc.flags;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, initial_bytes), sizeof(rhi_buffer_desc::initial_bytes))) {
                options.initial_bytes = desc.initial_bytes;
            }
            return options;
        }

        /// the contract's format name in this backend's spelling; UNDEFINED means "refuse" (unknown),
        /// and the `depth` ROLE resolves to the device's own depth attachment format - which concrete
        /// depth format a device serves is the backend's capability question, not the caller's (§17).
        [[nodiscard]] VkFormat native_image_format(deren::promise::rhi::image_format const format, VkFormat const depth_format) noexcept {
            using rhi_image_format = deren::promise::rhi::image_format;
            switch (format) {
            case rhi_image_format::rgba8_unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case rhi_image_format::rgba8_srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case rhi_image_format::bgra8_unorm:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case rhi_image_format::bgra8_srgb:
                return VK_FORMAT_B8G8R8A8_SRGB;
            case rhi_image_format::r16g16_sfloat:
                return VK_FORMAT_R16G16_SFLOAT;
            case rhi_image_format::r16g16b16a16_sfloat:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case rhi_image_format::r32g32b32_sfloat:
                return VK_FORMAT_R32G32B32_SFLOAT;
            // APPENDED in A1.2 with the contract value of the same name: the ray-traced visibility image,
            // which the engine creates now. The mapping is spelled out here like every other one because a
            // new contract value must not silently become whatever the integer happens to mean.
            case rhi_image_format::r16_sfloat:
                return VK_FORMAT_R16_SFLOAT;
            // APPENDED for the visibility buffer (2026-10-17): one 32-bit unsigned channel holding the
            // packed visibility id. Spelled out like every other mapping for the reason above.
            case rhi_image_format::r32_uint:
                return VK_FORMAT_R32_UINT;
            case rhi_image_format::depth:
                return depth_format;
            case rhi_image_format::unknown:
                return VK_FORMAT_UNDEFINED;
            }
            return VK_FORMAT_UNDEFINED;
        }

        /// the contract's capability flags as the usage bits they LET THE CALLER DO.
        [[nodiscard]] VkImageUsageFlags native_image_usage(rhi::image_flags const flags) noexcept {
            namespace rf = rhi;
            VkImageUsageFlags usage = 0;
            if (rf::has_flag(flags, rf::image_flag::sampled)) {
                usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::storage)) {
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::color_attachment)) {
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::depth_attachment)) {
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::transfer_source)) {
                usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::transfer_destination)) {
                usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::host_transfer)) {
                usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
            }
            if (rf::has_flag(flags, rf::image_flag::cube_compatible)) {
                // no usage bit: the flag's other half is the ALLOCATOR's type (see create_image), which
                // is what makes the six layers cube-creatable at all
            }
            return usage;
        }

        /// the same ABI guard `sanitize_buffer_desc` runs, field by field for `image_desc`.
        [[nodiscard]] deren::promise::rhi::image_desc sanitize_image_desc(deren::promise::rhi::image_desc const& desc) {
            using rhi_image_desc = deren::promise::rhi::image_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_image_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI image_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_image_desc options = {};
            if (covered_by(declared, offsetof(rhi_image_desc, extent), sizeof(rhi_image_desc::extent))) {
                options.extent = desc.extent;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, mip_levels), sizeof(rhi_image_desc::mip_levels))) {
                options.mip_levels = desc.mip_levels;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, array_layers), sizeof(rhi_image_desc::array_layers))) {
                options.array_layers = desc.array_layers;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, format), sizeof(rhi_image_desc::format))) {
                options.format = desc.format;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, flags), sizeof(rhi_image_desc::flags))) {
                options.flags = desc.flags;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, initial_bytes), sizeof(rhi_image_desc::initial_bytes))) {
                options.initial_bytes = desc.initial_bytes;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, debug_name), sizeof(rhi_image_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            return options;
        }

        /// the same ABI guard for `shader_desc`.
        [[nodiscard]] deren::promise::rhi::shader_desc sanitize_shader_desc(deren::promise::rhi::shader_desc const& desc) {
            using rhi_shader_desc = deren::promise::rhi::shader_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_shader_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI shader_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_shader_desc options = {};
            if (covered_by(declared, offsetof(rhi_shader_desc, stage), sizeof(rhi_shader_desc::stage))) {
                options.stage = desc.stage;
            }
            if (covered_by(declared, offsetof(rhi_shader_desc, code), sizeof(rhi_shader_desc::code))) {
                options.code = desc.code;
            }
            if (covered_by(declared, offsetof(rhi_shader_desc, debug_name), sizeof(rhi_shader_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            return options;
        }

        /// the same ABI guard for `pipeline_desc`.
        [[nodiscard]] deren::promise::rhi::pipeline_desc sanitize_pipeline_desc(deren::promise::rhi::pipeline_desc const& desc) {
            using rhi_pipeline_desc = deren::promise::rhi::pipeline_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_pipeline_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI pipeline_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_pipeline_desc options = {};
            if (covered_by(declared, offsetof(rhi_pipeline_desc, color_formats), sizeof(rhi_pipeline_desc::color_formats))) {
                options.color_formats = desc.color_formats;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_format), sizeof(rhi_pipeline_desc::depth_format))) {
                options.depth_format = desc.depth_format;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, vertex_code), sizeof(rhi_pipeline_desc::vertex_code))) {
                options.vertex_code = desc.vertex_code;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, fragment_code), sizeof(rhi_pipeline_desc::fragment_code))) {
                options.fragment_code = desc.fragment_code;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, first_stage), sizeof(rhi_pipeline_desc::first_stage))) {
                options.first_stage = desc.first_stage;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, sample_count), sizeof(rhi_pipeline_desc::sample_count))) {
                options.sample_count = desc.sample_count;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_test), sizeof(rhi_pipeline_desc::depth_test))) {
                options.depth_test = desc.depth_test;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_constant_factor), sizeof(rhi_pipeline_desc::depth_bias_constant_factor))) {
                options.depth_bias_constant_factor = desc.depth_bias_constant_factor;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_slope_factor), sizeof(rhi_pipeline_desc::depth_bias_slope_factor))) {
                options.depth_bias_slope_factor = desc.depth_bias_slope_factor;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_clamp), sizeof(rhi_pipeline_desc::depth_bias_clamp))) {
                options.depth_bias_clamp = desc.depth_bias_clamp;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, blend_modes), sizeof(rhi_pipeline_desc::blend_modes))) {
                options.blend_modes = desc.blend_modes;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, compare), sizeof(rhi_pipeline_desc::compare))) {
                options.compare = desc.compare;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, debug_name), sizeof(rhi_pipeline_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            // THE COMPUTE SPELLING (appended): a caller that declares only the graphics prefix keeps an EMPTY
            // `compute_code`, which the compute branch below refuses by name - an older caller cannot ask for a
            // compute pipeline by accident, and a newer one is read exactly as far as it declared.
            if (covered_by(declared, offsetof(rhi_pipeline_desc, compute_code), sizeof(rhi_pipeline_desc::compute_code))) {
                options.compute_code = desc.compute_code;
            }
            // THE RAY-TRACING SPELLING (appended with it): same rule - an older caller's prefix keeps the three
            // fields empty, which is "not a ray-tracing pipeline" rather than a mis-read.
            if (covered_by(declared, offsetof(rhi_pipeline_desc, ray_tracing_stages), sizeof(rhi_pipeline_desc::ray_tracing_stages))) {
                options.ray_tracing_stages = desc.ray_tracing_stages;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, ray_tracing_groups), sizeof(rhi_pipeline_desc::ray_tracing_groups))) {
                options.ray_tracing_groups = desc.ray_tracing_groups;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, max_ray_recursion), sizeof(rhi_pipeline_desc::max_ray_recursion))) {
                options.max_ray_recursion = desc.max_ray_recursion;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, acceleration_structure_bindings), sizeof(rhi_pipeline_desc::acceleration_structure_bindings))) {
                options.acceleration_structure_bindings = desc.acceleration_structure_bindings;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, texture_group), sizeof(rhi_pipeline_desc::texture_group))) {
                options.texture_group = desc.texture_group;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, resource_group), sizeof(rhi_pipeline_desc::resource_group))) {
                options.resource_group = desc.resource_group;
            }
            return options;
        }

        /// the same ABI guard for `sampler_desc`.
        [[nodiscard]] deren::promise::rhi::sampler_desc sanitize_sampler_desc(deren::promise::rhi::sampler_desc const& desc) {
            using rhi_sampler_desc = deren::promise::rhi::sampler_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_sampler_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI sampler_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_sampler_desc options = {};
            if (covered_by(declared, offsetof(rhi_sampler_desc, address_mode), sizeof(rhi_sampler_desc::address_mode))) {
                options.address_mode = desc.address_mode;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, max_lod), sizeof(rhi_sampler_desc::max_lod))) {
                options.max_lod = desc.max_lod;
            }
            // abi 16's APPENDED fields, read the same way: the default-constructed `options` carries today's
            // defaults (linear / linear / linear, no comparison), so a caller built against the previous
            // revision keeps exactly the behaviour it compiled against.
            if (covered_by(declared, offsetof(rhi_sampler_desc, mag_filter), sizeof(rhi_sampler_desc::mag_filter))) {
                options.mag_filter = desc.mag_filter;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, min_filter), sizeof(rhi_sampler_desc::min_filter))) {
                options.min_filter = desc.min_filter;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, mipmap_mode), sizeof(rhi_sampler_desc::mipmap_mode))) {
                options.mipmap_mode = desc.mipmap_mode;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, compare_enable), sizeof(rhi_sampler_desc::compare_enable))) {
                options.compare_enable = desc.compare_enable;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, compare_op), sizeof(rhi_sampler_desc::compare_op))) {
                options.compare_op = desc.compare_op;
            }
            return options;
        }

    } // namespace
    // ==== core.api_core.cpp:1149-1304 ====

    rhi::unique_object<rhi::buffer> core::resource_facet::create_buffer(rhi::buffer_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN THE DESCRIPTOR ------------------------------------------------
        // Same rule the context's descriptor follows (core.constructor.cppm): the caller declares how
        // many bytes of the structure IT compiled, and a field whose whole extent is not inside them
        // keeps this build's default. A shorter structure is an older caller; a longer one is a newer
        // caller whose appended tail this build has never heard of - either way only the prefix is read.
        rhi::buffer_desc const desc = sanitize_buffer_desc(declared_desc);

        if (rhi::validate_buffer_initialization(desc.size, desc.initial_bytes.size()) != rhi::error::ok) {
            deren::utility::log("rhi: create_buffer refused: size {} requires either no initial data or exactly {} bytes (got {})",
                                desc.size, desc.size, desc.initial_bytes.size());
            return nullptr;
        }
        if (!desc.initial_bytes.empty() &&
            (desc.usage == rhi::buffer_usage::storage_gpu_only || desc.usage == rhi::buffer_usage::acceleration_structure_storage ||
             desc.usage == rhi::buffer_usage::acceleration_structure_scratch)) {
            // These native allocation paths have no upload mechanism. Refuse caller input before
            // allocating, rather than reaching the allocator's fatal upload-failure path.
            deren::utility::log("rhi: create_buffer refused: initial data upload is unsupported for usage {}", static_cast<std::uint32_t>(desc.usage));
            return nullptr;
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S -----------------------------------------
        // `buffer_usage` names what the CALLER does with the buffer; the allocator's `buffer_type` is
        // the same intent in this backend's spelling, so the mapping is one to one and deliberately
        // written out rather than cast: a new contract value must not silently become whatever the
        // integer happens to mean here.
        buffer_type type = buffer_type::storage_gpu_only;
        switch (desc.usage) {
        case rhi::buffer_usage::vertex:
            type = buffer_type::vertex;
            break;
        case rhi::buffer_usage::index:
            type = buffer_type::index;
            break;
        case rhi::buffer_usage::uniform_gpu_only:
            type = buffer_type::uniform_gpu_only;
            break;
        case rhi::buffer_usage::uniform_coherent:
            type = buffer_type::uniform_coherent;
            break;
        case rhi::buffer_usage::uniform_cached:
            type = buffer_type::uniform_cached;
            break;
        case rhi::buffer_usage::storage_coherent:
            type = buffer_type::storage_coherent;
            break;
        case rhi::buffer_usage::readback_coherent:
            type = buffer_type::readback_coherent;
            break;
        case rhi::buffer_usage::acceleration_structure_storage:
            type = buffer_type::acceleration_structure_storage;
            break;
        case rhi::buffer_usage::acceleration_structure_scratch:
            type = buffer_type::acceleration_structure_scratch;
            break;
        case rhi::buffer_usage::storage_gpu_only:
            type = buffer_type::storage_gpu_only;
            break;
        // APPENDED for the frame-ordered upload: the staging side of a `copy_buffer` a frame records. One to one,
        // like every other case here - the contract's own note says a new value must not silently become whatever
        // the integer happens to mean in this backend.
        case rhi::buffer_usage::staging_source:
            type = buffer_type::staging_source;
            break;
        }

        // THE CAPABILITY FLAGS ARE THE ONLY VULKAN USAGE BITS THE CONTRACT NAMES, and they are named
        // for what they LET THE CALLER DO rather than for the bit: the renderer's twelve creation
        // sites ask for exactly these three (a device address, an acceleration-structure build input,
        // micromap storage) and for nothing else.
        // Slang heap ConstantBuffer lowers through a StorageBuffer pointer. Keep the logical
        // uniform role while making its native allocation usable by the general binding path.
        uint32_t extra_usage = (desc.usage == rhi::buffer_usage::uniform_gpu_only ||
                                desc.usage == rhi::buffer_usage::uniform_coherent ||
                                desc.usage == rhi::buffer_usage::uniform_cached)
                                   ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                   : 0u;
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::device_address)) {
            extra_usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::acceleration_structure_input)) {
            extra_usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::micromap_storage)) {
            extra_usage |= VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT;
        }
        // THE THREE THAT CAME FROM THE ENGINE'S OWN CENSUS RATHER THAN FROM A GUESS (the writers found
        // them, and each one is a case where a missing bit is a SILENT wrong-data bug rather than a
        // validation error):
        //   - `storage`: the renderer writes its per-slot uniform blocks as STORAGE descriptors and reads
        //     them through a Slang StorageBuffer pointer; without the bit the mismatch "reads as zeros
        //     with NO validation finding" (source/engine/runtime/runtime.constructor.cppm's own note).
        //   - `indirect`: a buffer that carries dispatch/draw commands.
        //   - `shader_binding_table`: a ray-tracing SBT.
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::storage)) {
            extra_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::transfer_destination)) {
            extra_usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::transfer_source)) {
            extra_usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::indirect)) {
            extra_usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::shader_binding_table)) {
            extra_usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::micromap_build_input)) {
            extra_usage |= VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT;
        }

        // ---- THE ALLOCATION, AND THE ONE REFERENCE IT HANDS OVER --------------------------------
        // `initial_bytes` empty means ALLOCATE ONLY - which is what the GPU-only targets, the read-back
        // slot and an acceleration structure's storage ask for, and what a content-keyed allocator must
        // never match against anything.
        auto* const answer = new core::buffer{};
        // `initial_bytes` is the contract's `std::byte` view and the allocator takes `uint8_t const*`:
        // the cast is the boundary between the contract's byte type and VMA's, spelled here so neither
        // side has to know the other's.
        answer->allocation = this->full_core.device.vma.create_buffer(
            desc.initial_bytes.empty() ? nullptr : reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.size, type, extra_usage);
        if (!answer->allocation.valid()) {
            deren::utility::log("rhi: create_buffer could not allocate {} B (usage {}, flags {:#x})", desc.size,
                                static_cast<std::uint32_t>(desc.usage), desc.flags);
            delete answer;
            return nullptr;
        }
        answer->size_bytes = desc.size;
        answer->addressable = rhi::has_flag(desc.flags, rhi::buffer_flag::device_address);
        answer->readback = desc.usage == rhi::buffer_usage::readback_coherent;
        answer->declared_usage = desc.usage;
        answer->declared_flags = desc.flags;
        answer->parent = &this->full_core;

        // THE DETAIL IS READ, NEVER KEPT: `get_buffer_detail` answers with a pointer into the
        // allocator's own map and has already released its lock by the time it returns, so what is
        // copied out here are the two VALUES the contract needs - the Vulkan handle and the mapped
        // address - not the pointer to the record (see the type's note).
        auto const* const detail = this->full_core.device.vma.get_buffer_detail(answer->allocation.handle());
        if (detail != nullptr) {
            answer->native = detail->buffer;
            answer->mapped_bytes = detail->allocation_info.pMappedData;
        }
        deren::utility::log("rhi: create_buffer {} B usage {} flags {:#x} -> extra {:#x}, native {:#x}, mapped {}",
                            desc.size, static_cast<std::uint32_t>(desc.usage), desc.flags, extra_usage,
                            reinterpret_cast<std::uintptr_t>(answer->native), answer->mapped_bytes != nullptr);
        {
            std::lock_guard const lock(this->contract_buffers_mutex);
            answer->identity = this->resource_identity.fetch_add(1, std::memory_order_relaxed);
            this->contract_buffers.insert(answer);
        }
        return rhi::unique_object<rhi::buffer>{answer};
    }

    // ==== core.api_core.cpp:1335-1337 ====

    rhi::resource_memory_statistics core::resource_facet::memory_statistics() const noexcept {
        return this->full_core.device.vma.memory_statistics();
    }

    // ==== core.api_core.cpp:1339-1421 ====

    rhi::unique_object<rhi::image> core::resource_facet::create_image(rhi::image_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN THE DESCRIPTOR ------------------------------------------------
        // Same rule create_buffer runs: only the caller's declared prefix is read.
        rhi::image_desc const desc = sanitize_image_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed image";

        // ---- THE REFUSALS, EACH NAMED -----------------------------------------------------------
        // The contract's answer to "this descriptor cannot be honoured" is nullptr plus a log line
        // (§4.2: no throwing path) - a caller that needs the reason looks at the log the name marks.
        if (desc.extent.width == 0 || desc.extent.height == 0) {
            deren::utility::log("rhi: create_image {} refused: zero extent ({}x{})", what, desc.extent.width, desc.extent.height);
            return nullptr;
        }
        if (desc.format == rhi::image_format::unknown) {
            deren::utility::log("rhi: create_image {} refused: image_format::unknown names no format", what);
            return nullptr;
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S ------------------------------------------
        // The format: named formats map one to one; the `depth` ROLE resolves to the device's own depth
        // attachment format, which is the capability question §17 moved out of the caller's hands.
        VkFormat const native_format = native_image_format(desc.format, this->full_core.frame.depth_attachment_format);
        // The allocator's type: the shape flags pick it, because the type is what carries the memory
        // intent and the cube-creatability (vma.cppm's image_type note).
        image_type type = image_type::texture_2d;
        if (desc.format == rhi::image_format::depth || rhi::has_flag(desc.flags, rhi::image_flag::depth_attachment)) {
            type = image_type::texture_2d_depth;
        } else if (rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible)) {
            if (desc.array_layers != 6) {
                deren::utility::log("rhi: create_image {} refused: cube_compatible needs 6 layers, asked for {}", what, desc.array_layers);
                return nullptr;
            }
            type = image_type::texture_cubemap;
        }
        image_create_info const create_info = {
            .width = desc.extent.width,
            .height = desc.extent.height,
            .mip_levels = desc.mip_levels == 0
                              ? static_cast<uint32_t>(std::bit_width(std::max(desc.extent.width, desc.extent.height)))
                              : desc.mip_levels, // 0 means "the full chain", the contract's spelling
            .array_layers = desc.array_layers,
            .format = native_format,
            .extra_usage = native_image_usage(desc.flags),
        };

        // ---- THE ALLOCATION (the allocator's own dedup sees the content at creation) --------------
        // create_image uploads the staging copy itself for data-carrying types and answers an owning
        // RAII handle (vma.cppm's type table); empty bytes mean allocate-only.
        vk_image const allocation = this->full_core.device.vma.create_image(
            reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.initial_bytes.size(), create_info, type);
        if (allocation.handle() == 0) {
            deren::utility::log("rhi: create_image {} refused: the allocator could not serve {}x{}, {} layer(s), {} mip(s)",
                                what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels);
            return nullptr;
        }

        // The detail lookup is an UNLOCKED BORROW: the VkImage is copied out NOW (the same rule
        // core::buffer's comment states), never the pointer into the allocator's map.
        image_detail const* const detail = this->full_core.device.vma.get_image_detail(allocation.handle());
        if (detail == nullptr) {
            deren::utility::log("rhi: create_image {} refused: the allocator holds no detail for the allocation", what);
            return nullptr;
        }
        auto* const answer = new core::image();
        answer->parent = &this->full_core;
        answer->allocation = allocation;
        answer->native_handle = detail->image;
        answer->resolved_format = native_format;
        answer->width = create_info.width;
        answer->height = create_info.height;
        answer->mip_levels = create_info.mip_levels;
        answer->array_layers = create_info.array_layers;
        answer->cube_compatible = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible);
        answer->declared_format = desc.format;
        answer->declared_flags = desc.flags;
        {
            std::lock_guard const lock(this->contract_images_mutex);
            this->contract_images.insert(answer);
        }
        deren::utility::log("rhi: create_image {} {}x{} layers {} mips {} -> handle {:#x}",
                            what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels, allocation.handle());
        return rhi::unique_object<rhi::image>{answer};
    }

    // ==== core.api_core.cpp:1423-1470 ====

    rhi::unique_object<rhi::sampler> core::resource_facet::create_sampler(rhi::sampler_desc const& declared_desc) {
        rhi::sampler_desc const desc = sanitize_sampler_desc(declared_desc);

        // The contract's four modes are exactly VkSamplerAddressMode's common four, written out per
        // the same rule as every mapping above.
        VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        switch (desc.address_mode) {
        case rhi::sampler_address_mode::repeat:
            mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        case rhi::sampler_address_mode::mirrored_repeat:
            mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case rhi::sampler_address_mode::clamp_to_edge:
            mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case rhi::sampler_address_mode::clamp_to_border:
            mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            break;
        }

        // abi 16's fields ride on the SAME builder the backend's own six samplers use
        // (`make_texture_sampler_info`), so a description that spells out those six is value-for-value the
        // sampler the backend used to create on the engine's behalf - that equality is what keeps the
        // pictures identical, and it is why only the four knobs are overridden here.
        VkSamplerCreateInfo info = make_texture_sampler_info(mode, desc.max_lod);
        info.magFilter = desc.mag_filter == rhi::sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter = desc.min_filter == rhi::sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.mipmapMode = desc.mipmap_mode == rhi::sampler_mipmap_mode::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
        info.compareOp = desc.compare_enable ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
        VkSampler handle = VK_NULL_HANDLE;
        if (vkCreateSampler(this->full_core.device.logical_device, &info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_sampler refused: vkCreateSampler failed (mode {}, max lod {})",
                                static_cast<int>(desc.address_mode), desc.max_lod);
            return nullptr;
        }
        auto* const answer = new core::sampler();
        answer->parent = &this->full_core;
        answer->info = info;
        {
            std::lock_guard lock(this->group_members_mutex);
            this->group_members.insert(answer);
        }
        answer->native_sampler_handle = handle;
        answer->device = this->full_core.device.logical_device;
        return rhi::unique_object<rhi::sampler>{answer};
    }

    // ==== core.api_core.cpp:1614-1663 ====

    rhi::unique_object<rhi::shader> core::resource_facet::create_shader(rhi::shader_desc const& declared_desc) {
        rhi::shader_desc const desc = sanitize_shader_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed shader";

        VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
        switch (desc.stage) {
        case rhi::shader_stage::vertex:
            stage = VK_SHADER_STAGE_VERTEX_BIT;
            break;
        case rhi::shader_stage::mesh:
            stage = VK_SHADER_STAGE_MESH_BIT_EXT;
            break;
        case rhi::shader_stage::fragment:
            stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case rhi::shader_stage::compute:
            stage = VK_SHADER_STAGE_COMPUTE_BIT;
            break;
        // THE RAY-TRACING STAGES (appended with the ray-tracing pipeline spelling, abi 21): the module is
        // stage-less, so this mapping is only what a caller's stage value MEANS - but a switch that silently
        // fell through to the vertex default would be a wrong answer at the one place the contract states it.
        case rhi::shader_stage::ray_generation:
            stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            break;
        case rhi::shader_stage::miss:
            stage = VK_SHADER_STAGE_MISS_BIT_KHR;
            break;
        case rhi::shader_stage::closest_hit:
            stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            break;
        case rhi::shader_stage::any_hit:
            stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            break;
        case rhi::shader_stage::intersection:
            stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            break;
        }
        (void)stage; // the module itself is stage-less; the stage rides the pipeline's stage info

        std::optional<vk_shader_module> module = ::deren::vulkan::make_shader_module(
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.code.data()), desc.code.size()),
            this->full_core.device.logical_device);
        if (!module.has_value()) {
            deren::utility::log("rhi: create_shader {} refused: vkCreateShaderModule failed", what);
            return nullptr;
        }
        auto* const answer = new core::shader();
        answer->module.emplace(std::move(module.value()));
        return rhi::unique_object<rhi::shader>{answer};
    }

    // ==== core.api_core.cpp:1665-1783 ====

    rhi::unique_object<rhi::pipeline> core::resource_facet::create_pipeline(rhi::pipeline_desc const& declared_desc) {
        rhi::pipeline_desc const desc = sanitize_pipeline_desc(declared_desc);
        if (this->full_core.validate_resource_group_pipeline(desc) != rhi::error::ok) {
            deren::utility::log("rhi: incompatible resource-group pipeline layout");
            return nullptr;
        }
        if (desc.texture_group.enabled && this->full_core.validate_texture_group_pipeline(desc) != rhi::error::ok) {
            deren::utility::log("rhi: incompatible texture-group pipeline binding");
            return nullptr;
        }
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed pipeline";

        // ---- THE COMPUTE SPELLING IS ITS OWN PATH (abi 21) ---------------------------------------
        // ONE stage, no attachment state: `first_stage == compute` is the whole difference the caller
        // states, and everything the graphics path below derives from formats and blends is meaningless
        // here. The branch is FIRST so the graphics field reads below cannot be reached with a compute
        // descriptor (a compute pipeline whose `color_formats` names `unknown` would be refused for the
        // wrong reason).
        if (!desc.acceleration_structure_bindings.empty() &&
            (desc.first_stage == rhi::shader_stage::compute || desc.ray_tracing_stages.empty())) {
            deren::utility::log("rhi: create_pipeline {} refused: acceleration-structure bindings require a ray-tracing pipeline", what);
            return nullptr;
        }
        if (desc.first_stage == rhi::shader_stage::compute) {
            return rhi::unique_object<rhi::pipeline>{this->full_core.create_compute_pipeline(desc, what)};
        }
        // ---- AND THE RAY-TRACING SPELLING (abi 21) ------------------------------------------------
        // The switch is the STAGES, not a first stage: a ray-tracing pipeline is the only kind built from
        // several named entry points and a group table (see the descriptor's own note), so their presence is
        // what says which path this is.
        if (!desc.ray_tracing_stages.empty()) {
            return rhi::unique_object<rhi::pipeline>{this->full_core.create_ray_tracing_pipeline(desc, what)};
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S ------------------------------------------
        // Color formats one to one; the `depth` ROLE resolves to the device's own depth attachment
        // format; `unknown` as the depth format means NO depth attachment (make_pipeline's
        // VK_FORMAT_UNDEFINED spelling).
        deren::utility::vessel::vector<VkFormat> color_formats(desc.color_formats.size());
        for (std::size_t index = 0; index < desc.color_formats.size(); ++index) {
            color_formats[index] = native_image_format(desc.color_formats[index], this->full_core.frame.depth_attachment_format);
            if (color_formats[index] == VK_FORMAT_UNDEFINED) {
                deren::utility::log("rhi: create_pipeline {} refused: color attachment {} is image_format::unknown", what, index);
                return nullptr;
            }
        }
        VkFormat const depth_format = desc.depth_format == rhi::image_format::unknown
                                          ? VK_FORMAT_UNDEFINED
                                          : native_image_format(desc.depth_format, this->full_core.frame.depth_attachment_format);

        // The blend modes are the FOUR RECIPES the survey found; empty means every target is
        // overwritten (opaque), which is the default make_pipeline itself spells.
        deren::utility::vessel::vector<VkPipelineColorBlendAttachmentState> blends;
        blends.reserve(desc.blend_modes.size());
        for (rhi::blend_mode const mode : desc.blend_modes) {
            switch (mode) {
            case rhi::blend_mode::opaque:
                blends.push_back(make_color_blend_attachment_opaque());
                break;
            case rhi::blend_mode::alpha:
                blends.push_back(make_color_blend_attachment());
                break;
            case rhi::blend_mode::additive:
                blends.push_back(make_color_blend_attachment_additive());
                break;
            case rhi::blend_mode::multiply:
                blends.push_back(make_color_blend_attachment_multiply());
                break;
            }
        }

        VkSampleCountFlagBits const samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
        VkCompareOp const compare = [&desc] {
            switch (desc.compare) {
            case rhi::depth_compare::less_or_equal:
                return VK_COMPARE_OP_LESS_OR_EQUAL;
            case rhi::depth_compare::equal:
                return VK_COMPARE_OP_EQUAL;
            case rhi::depth_compare::greater:
                return VK_COMPARE_OP_GREATER;
            }
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        }();
        if (desc.first_stage == rhi::shader_stage::mesh) {
            // the full overload takes the compare op; the simple one the mesh spelling routes through
            // carries the same default. Routed here because the mesh stage REPLACES the vertex stage.
        }
        std::expected<vk_pipeline, std::string_view> pipeline = make_pipeline(
            this->full_core.device.logical_device,
            std::span<VkFormat const>(color_formats.data(), color_formats.size()),
            depth_format,
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.vertex_code.data()), desc.vertex_code.size()),
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.fragment_code.data()), desc.fragment_code.size()),
            samples,
            desc.depth_test,
            desc.depth_bias_constant_factor,
            desc.depth_bias_slope_factor,
            desc.depth_bias_clamp,
            std::span<VkPipelineColorBlendAttachmentState const>(blends.data(), blends.size()),
            desc.first_stage == rhi::shader_stage::mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
            compare);
        if (!pipeline.has_value()) {
            deren::utility::log("rhi: create_pipeline {} refused: {}", what, pipeline.error());
            return nullptr;
        }
        auto* const answer = new core::pipeline();
        answer->parent = &this->full_core;
        {
            std::lock_guard const lock(this->contract_pipelines_mutex);
            answer->identity = this->resource_identity.fetch_add(1, std::memory_order_relaxed);
            this->contract_pipelines.insert(answer);
        }
        answer->native_object.emplace(std::move(pipeline.value()));
        answer->group_binding = desc.texture_group;
        answer->resource_layout.assign(desc.resource_group.begin(), desc.resource_group.end());
        std::sort(answer->resource_layout.begin(), answer->resource_layout.end(), [](auto const& a, auto const& b) { return a.binding < b.binding; });
        answer->native_handle = answer->native_object->get_pipeline();
        return rhi::unique_object<rhi::pipeline>{answer};
    }

    // ==== core.api_core.cpp:3716-3881 ====

    rhi::unique_object<rhi::micromap> core::resource_facet::create_micromap(rhi::micromap_desc const& declared_desc) {
        // ---- THE CONTRACT'S LAYOUTS ARE THE API'S, AND THIS IS WHERE THAT IS PROVEN --------------------
        static_assert(static_cast<std::uint32_t>(rhi::micromap_format::four_state) == VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                      "the contract's four-state format IS the API's");
        static_assert(static_cast<std::uint32_t>(rhi::micromap_format::two_state) == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT,
                      "the contract's two-state format IS the API's");
        static_assert(sizeof(rhi::micromap_triangle) == sizeof(VkMicromapTriangleEXT),
                      "a caller fills an array of the contract's records and the build reads the API's");
        if (this->full_core.device.micromap_create == nullptr || this->full_core.device.micromap_build_sizes == nullptr) {
            if (!this->full_core.device.micromap_refusal_logged) {
                this->full_core.device.micromap_refusal_logged = true;
                deren::utility::log("rhi: create_micromap refused: this device has no VK_EXT_opacity_micromap "
                                    "(its four entry points did not resolve at startup)");
            }
            return nullptr;
        }

        // THE ABI GUARD, then the description's fields.
        std::uint32_t const declared = declared_desc.struct_size;
        std::uint32_t triangle_count = 0;
        std::span<std::byte const> data = {};
        std::uint32_t data_stride = 4u;
        std::span<rhi::micromap_triangle const> triangles = {};
        std::span<std::uint32_t const> indices = {};
        rhi::micromap_format format = rhi::micromap_format::four_state;
        if (covered_by(declared, offsetof(rhi::micromap_desc, triangle_count), sizeof(rhi::micromap_desc::triangle_count))) {
            triangle_count = declared_desc.triangle_count;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, data), sizeof(rhi::micromap_desc::data))) {
            data = declared_desc.data;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, data_stride), sizeof(rhi::micromap_desc::data_stride))) {
            data_stride = declared_desc.data_stride;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, triangles), sizeof(rhi::micromap_desc::triangles))) {
            triangles = declared_desc.triangles;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, indices), sizeof(rhi::micromap_desc::indices))) {
            indices = declared_desc.indices;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, format), sizeof(rhi::micromap_desc::format))) {
            format = declared_desc.format;
        }
        if (triangle_count == 0 || data.empty() || triangles.size() < triangle_count || indices.size() < triangle_count) {
            deren::utility::log("rhi: create_micromap refused: a micromap needs {}+ attributes, {}+ records and {}+ indices for {} triangles",
                                data.empty() ? 1u : data.size(), triangle_count, triangle_count, triangle_count);
            return nullptr;
        }
        // THE STRIDE IS WHAT SAYS THE ATTRIBUTES ARE ACTUALLY THERE: a span shorter than one record per
        // micro-triangle would build a micromap whose later lookups read past the data (the format decides how
        // many bytes a record is, and `data_stride` is how the caller laid them out).
        if (data_stride == 0u || data.size() < static_cast<std::size_t>(triangle_count) * data_stride) {
            deren::utility::log("rhi: create_micromap refused: {} bytes of attributes do not cover {} micro-triangles at a stride of {}",
                                data.size(), triangle_count, data_stride);
            return nullptr;
        }

        auto* const out = new core::micromap{};
        out->parent = &this->full_core;
        out->triangle_count = triangle_count;
        out->triangle_array_stride = sizeof(VkMicromapTriangleEXT);
        out->index_stride = static_cast<std::uint32_t>(sizeof(std::uint32_t));
        out->usage = VkMicromapUsageEXT{.count = triangle_count, .subdivisionLevel = 0u, .format = static_cast<std::uint32_t>(format)};

        // ---- THE SETUP BUFFERS, AND THE ALIGNMENT THE API REQUIRES OF THEIR ADDRESSES --------------------
        // 256 BYTES ON THE DEVICE ADDRESS, not on the buffer: the allocator gives no such promise and the address
        // does not exist before the allocation, so each buffer carries one alignment worth of slack and the
        // payload is written at the first aligned address inside it. THIS IS THE ENGINE'S OLD JOB, and it is the
        // backend's now - which is the whole reason `micromap` is an object rather than a caller-managed pair.
        constexpr VkDeviceSize address_alignment = 256u;
        constexpr rhi::buffer_flags setup_flags =
            rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::micromap_build_input);
        auto const setup = [this](std::span<std::byte const> const bytes, std::uintptr_t& address) -> rhi::buffer* {
            rhi::buffer* const buffer = this->full_core.create_buffer_raw(rhi::buffer_desc{
                .size = static_cast<std::uint64_t>(bytes.size()) + address_alignment,
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = setup_flags});
            if (buffer == nullptr) {
                return nullptr;
            }
            std::span<std::byte> const mapped = buffer->mapped();
            std::uint64_t const base = this->address_view.buffer_address(*buffer, 0);
            if (mapped.data() == nullptr || base == 0) {
                buffer->release();
                return nullptr;
            }
            std::uint64_t const offset = (address_alignment - (base % address_alignment)) % address_alignment;
            std::memcpy(mapped.data() + offset, bytes.data(), bytes.size());
            address = base + offset;
            return buffer;
        };
        out->data = setup(data, out->data_address);
        std::span<std::byte const> const triangle_bytes{reinterpret_cast<std::byte const*>(triangles.data()),
                                                        static_cast<std::size_t>(triangle_count) * sizeof(rhi::micromap_triangle)};
        out->triangles = setup(triangle_bytes, out->triangles_address);
        std::span<std::byte const> const index_bytes{reinterpret_cast<std::byte const*>(indices.data()),
                                                     static_cast<std::size_t>(triangle_count) * sizeof(std::uint32_t)};
        out->indices = setup(index_bytes, out->indices_address);
        if (out->data == nullptr || out->triangles == nullptr || out->indices == nullptr) {
            deren::utility::log("rhi: create_micromap refused: a setup buffer could not be allocated or has no device address");
            delete out;
            return nullptr;
        }

        VkMicromapBuildInfoEXT const info = make_micromap_build_info(
            VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT, VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT,
            VK_BUILD_MICROMAP_MODE_BUILD_EXT, VK_NULL_HANDLE, 1, &out->usage,
            out->data_address, out->triangles_address, out->triangle_array_stride);

        VkMicromapBuildSizesInfoEXT sizes = make_micromap_build_sizes_info();
        this->full_core.device.micromap_build_sizes(this->full_core.device.logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &sizes);
        if (sizes.micromapSize == 0) {
            deren::utility::log("rhi: create_micromap refused: the build-size query answered zero bytes");
            delete out;
            return nullptr;
        }
        out->storage = this->full_core.create_buffer_raw(rhi::buffer_desc{
            .size = sizes.micromapSize,
            .usage = rhi::buffer_usage::storage_gpu_only,
            .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::micromap_storage)});
        if (out->storage == nullptr) {
            deren::utility::log("rhi: create_micromap refused: the micromap storage could not be allocated");
            delete out;
            return nullptr;
        }
        if (sizes.buildScratchSize != 0) {
            // only its ADDRESS is read (by the build), so the device-address flag is the whole requirement
            out->scratch = this->full_core.create_buffer_raw(rhi::buffer_desc{
                .size = sizes.buildScratchSize,
                .usage = rhi::buffer_usage::storage_gpu_only,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address)});
            if (out->scratch == nullptr) {
                deren::utility::log("rhi: create_micromap refused: the micromap scratch could not be allocated");
                delete out;
                return nullptr;
            }
            out->scratch_address = this->address_view.buffer_address(*out->scratch, 0);
        }

        auto const* const storage_buffer = rhi::do_concrete<core::buffer>(*out->storage, concrete::buffer);
        VkMicromapCreateInfoEXT create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
        create_info.pNext = nullptr;
        create_info.createFlags = 0;
        create_info.buffer = storage_buffer->native;
        create_info.offset = 0;
        create_info.size = sizes.micromapSize;
        create_info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        create_info.deviceAddress = 0;
        if (this->full_core.device.micromap_create(this->full_core.device.logical_device, &create_info, nullptr, &out->native) != VK_SUCCESS || out->native == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_micromap refused: vkCreateMicromapEXT failed");
            delete out;
            return nullptr;
        }
        deren::utility::log("rhi: create_micromap: {} micro-triangles, {} bytes of storage, {} bytes of scratch",
                            out->triangle_count, sizes.micromapSize, sizes.buildScratchSize);
        return rhi::unique_object<rhi::micromap>{out};
    }

    // ==== core.api_core.cpp:3883-4133 ====

    rhi::unique_object<rhi::acceleration_structure> core::resource_facet::create_acceleration_structure(rhi::acceleration_structure_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN WHAT A CREATE ACTUALLY IS (tier-1 since abi 26) ------------------------
        //
        //   1. THE GEOMETRY, in the driver's words, built ONCE: the caller's records are PODs of the CONTRACT's
        //      layout, and this is the ONE place the two spellings meet - the backend owns the conversion, the
        //      same rule `mesh_task_command` follows.
        //   2. THE SIZES (`vkGetAccelerationStructureBuildSizesKHR`) and the memory: the storage the structure
        //      lives in, allocated through this backend's own factory. The caller never sees the number.
        //   3. THE HANDLE (`vkCreateAccelerationStructureKHR`) and its ADDRESS
        //      (`vkGetAccelerationStructureDeviceAddressKHR`), which is what a shader and an instance record use.
        //
        // THE SCRATCH IS ALLOCATED HERE TOO, not at a later first build: both sizes are known at creation (a
        // bottom level's geometry is fixed; a top level is sized for its capacity), so recording a build never
        // allocates - which is what keeps the recording verbs free of a failure mode in the middle of a frame.
        if (this->full_core.device.acceleration_structure_create == nullptr || this->full_core.device.acceleration_structure_build_sizes == nullptr ||
            this->full_core.device.acceleration_structure_address == nullptr || this->full_core.device.acceleration_structure_build == nullptr) {
            if (!this->full_core.device.acceleration_structure_refusal_logged) {
                this->full_core.device.acceleration_structure_refusal_logged = true;
                deren::utility::log("rhi: create_acceleration_structure refused: this device has no "
                                    "VK_KHR_acceleration_structure (its five entry points did not resolve at startup)");
            }
            return nullptr;
        }

        // THE ABI GUARD: only the prefix the caller declared is read (the rule every descriptor here follows).
        std::uint32_t const declared = declared_desc.struct_size;
        rhi::acceleration_structure_type type = rhi::acceleration_structure_type::bottom_level;
        rhi::acceleration_structure_flags flags = rhi::no_acceleration_structure_flags;
        rhi::acceleration_structure_geometry const* geometries = nullptr;
        std::uint32_t geometry_count = 0;
        std::uint32_t instance_capacity = 0;
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, type), sizeof(rhi::acceleration_structure_desc::type))) {
            type = declared_desc.type;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, flags), sizeof(rhi::acceleration_structure_desc::flags))) {
            flags = declared_desc.flags;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, geometries), sizeof(rhi::acceleration_structure_desc::geometries))) {
            geometries = declared_desc.geometries;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, geometry_count), sizeof(rhi::acceleration_structure_desc::geometry_count))) {
            geometry_count = declared_desc.geometry_count;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, instance_capacity), sizeof(rhi::acceleration_structure_desc::instance_capacity))) {
            instance_capacity = declared_desc.instance_capacity;
        }

        bool const top_level = type == rhi::acceleration_structure_type::top_level;
        if (!top_level && (geometries == nullptr || geometry_count == 0)) {
            deren::utility::log("rhi: create_acceleration_structure refused: a bottom-level structure needs at least one geometry");
            return nullptr; // a structure with nothing to trace is a caller bug, refused by name
        }
        if (top_level && instance_capacity == 0) {
            deren::utility::log("rhi: create_acceleration_structure refused: a top-level structure needs an instance capacity");
            return nullptr;
        }

        auto* const structure = new core::acceleration_structure{};
        structure->parent = &this->full_core;
        structure->top_level = top_level;
        structure->refittable = rhi::has_flag(flags, rhi::acceleration_structure_flag::allow_update);
        structure->instance_capacity = instance_capacity;

        // ---- 1. THE GEOMETRY ---------------------------------------------------------------------------
        if (top_level) {
            rhi::buffer* const records = this->full_core.create_buffer_raw(rhi::buffer_desc{
                .size = static_cast<std::uint64_t>(instance_capacity) * sizeof(rhi::acceleration_structure_instance),
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input)});
            if (records == nullptr || records->mapped().data() == nullptr) {
                deren::utility::log("rhi: create_acceleration_structure refused: the instance buffer could not be allocated");
                delete structure;
                return nullptr;
            }
            structure->instances = records;
            structure->instances_mapped = records->mapped().data();
            std::uint64_t const records_address = this->address_view.buffer_address(*records, 0);
            if (records_address == 0) {
                deren::utility::log("rhi: create_acceleration_structure refused: the instance buffer has no device address");
                delete structure;
                return nullptr;
            }
            VkAccelerationStructureGeometryInstancesDataKHR instances_data{};
            instances_data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            instances_data.pNext = nullptr;
            instances_data.arrayOfPointers = VK_FALSE;
            instances_data.data.deviceAddress = records_address;
            structure->geometries.push_back(VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .pNext = nullptr,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .geometry = {.instances = instances_data},
                .flags = 0,
            });
            structure->ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{.primitiveCount = 0u, .primitiveOffset = 0u, .firstVertex = 0u, .transformOffset = 0u});
        } else {
            structure->geometries.reserve(geometry_count);
            structure->ranges.reserve(geometry_count);
            structure->micromap_attachments.reserve(geometry_count);
            for (std::uint32_t index = 0; index < geometry_count; ++index) {
                rhi::acceleration_structure_geometry const& source = geometries[index];
                VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
                triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.pNext = nullptr;
                triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // the contract's geometry is positions
                triangles.vertexData.deviceAddress = source.vertex_address;
                triangles.vertexStride = source.vertex_stride;
                triangles.maxVertex = source.vertex_count == 0 ? 0u : source.vertex_count - 1u;
                // Zero index address denotes triangle soup (for example the MASK bake's output).
                triangles.indexType = source.index_address == 0 ? VK_INDEX_TYPE_NONE_KHR : static_cast<VkIndexType>(source.index_format);
                triangles.indexData.deviceAddress = source.index_address;
                triangles.transformData.deviceAddress = 0;
                // THE OPACITY MICROMAP, CHAINED INTO THE TRIANGLES DATA (not into the geometry: the geometry's own
                // pNext accepts only the micromap-DATA struct, and validation named exactly that when the engine
                // first attached it in the wrong place). THE INDEX ARRAY IS THIS BACKEND'S - it built the micromap
                // and owns the buffer - which is what makes the contract's attachment one handle and a usage record.
                if (source.opacity_micromap != nullptr) {
                    if (!rhi::is_concrete(*source.opacity_micromap, concrete::micromap)) {
                        deren::utility::log("rhi: create_acceleration_structure refused: a geometry's opacity micromap is not a handle this backend handed out");
                        delete structure;
                        return nullptr;
                    }
                    auto& micromap = static_cast<core::micromap&>(*source.opacity_micromap);
                    if (micromap.parent != &this->full_core) {
                        deren::utility::log("rhi: create_acceleration_structure refused: a geometry's opacity micromap belongs to another device");
                        delete structure;
                        return nullptr;
                    }
                    structure->micromap_attachments.push_back(VkAccelerationStructureTrianglesOpacityMicromapEXT{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,
                        .pNext = nullptr,
                        .indexType = VK_INDEX_TYPE_UINT32,
                        .indexBuffer = {.deviceAddress = micromap.indices_address},
                        .indexStride = micromap.index_stride,
                        .baseTriangle = 0u,
                        .usageCountsCount = 1u,
                        .pUsageCounts = nullptr,  // filled below: it has to point INSIDE this vector
                        .ppUsageCounts = nullptr, // ... and this backend uses the single-record form
                        .micromap = micromap.native,
                    });
                    // `pUsageCounts` points at the MICROMAP's own usage record, which lives as long as the caller
                    // holds the micromap (the engine's structure set keeps every micromap it built alive for the
                    // whole session, which is why that is a safe anchor rather than a local).
                    structure->micromap_attachments.back().pUsageCounts = &micromap.usage;
                    triangles.pNext = &structure->micromap_attachments.back();
                }
                structure->geometries.push_back(VkAccelerationStructureGeometryKHR{
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .pNext = nullptr,
                    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                    .geometry = {.triangles = triangles},
                    // Preserve the engine's any-hit alpha test, including geometries without a micromap.
                    .flags = 0,
                });
                structure->ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{
                    .primitiveCount = (source.index_address == 0 ? source.vertex_count : source.index_count) / 3u,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u,
                });
            }
        }

        // ---- 2. THE SIZES, THEN THE MEMORY --------------------------------------------------------------
        VkAccelerationStructureBuildGeometryInfoKHR const size_info = make_acceleration_structure_build_info(
            top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            (top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                (structure->refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u),
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, VK_NULL_HANDLE, VK_NULL_HANDLE,
            static_cast<std::uint32_t>(structure->geometries.size()), structure->geometries.data(), 0);
        deren::utility::vessel::vector<std::uint32_t> primitive_counts;
        primitive_counts.reserve(structure->ranges.size());
        for (auto const& range : structure->ranges) {
            primitive_counts.push_back(top_level ? instance_capacity : range.primitiveCount);
        }
        VkAccelerationStructureBuildSizesInfoKHR sizes = make_acceleration_structure_build_sizes_info();
        this->full_core.device.acceleration_structure_build_sizes(this->full_core.device.logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &size_info, primitive_counts.data(), &sizes);
        if (sizes.accelerationStructureSize == 0) {
            deren::utility::log("rhi: create_acceleration_structure refused: the build-size query answered zero bytes");
            delete structure;
            return nullptr;
        }

        structure->storage = this->full_core.create_buffer_raw(
            rhi::buffer_desc{.size = sizes.accelerationStructureSize, .usage = rhi::buffer_usage::acceleration_structure_storage});
        if (structure->storage == nullptr) {
            deren::utility::log("rhi: create_acceleration_structure refused: the structure's storage could not be allocated ({} bytes)", sizes.accelerationStructureSize);
            delete structure;
            return nullptr;
        }
        structure->structure_size = sizes.accelerationStructureSize;

        std::uint64_t const scratch_size = std::max(sizes.buildScratchSize, structure->refittable ? sizes.updateScratchSize : 0u);
        if (scratch_size != 0) {
            std::uint64_t const alignment = std::max<std::uint64_t>(this->full_core.device.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment, 1u);
            structure->scratch = this->full_core.create_buffer_raw(rhi::buffer_desc{.size = scratch_size + alignment - 1u,
                                                                                    .usage = rhi::buffer_usage::acceleration_structure_scratch,
                                                                                    .flags = rhi::to_bits(rhi::buffer_flag::device_address)});
            if (structure->scratch == nullptr) {
                deren::utility::log("rhi: create_acceleration_structure refused: the build scratch could not be allocated ({} bytes)", scratch_size);
                delete structure;
                return nullptr;
            }
            structure->scratch_size = scratch_size;
            std::uint64_t const base = this->address_view.buffer_address(*structure->scratch, 0);
            if (base == 0) {
                delete structure;
                return nullptr;
            }
            // The requirement applies to the device address, rather than merely an offset in the buffer.
            structure->scratch_address = base + (alignment - base % alignment) % alignment;
        }

        // ---- 3. THE HANDLE AND ITS ADDRESS --------------------------------------------------------------
        auto const* const storage_buffer = rhi::do_concrete<core::buffer>(*structure->storage, concrete::buffer);
        VkAccelerationStructureCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.pNext = nullptr;
        create_info.createFlags = 0;
        create_info.buffer = storage_buffer->native;
        create_info.offset = 0;
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = size_info.type;
        create_info.deviceAddress = 0;
        if (this->full_core.device.acceleration_structure_create(this->full_core.device.logical_device, &create_info, nullptr, &structure->native) != VK_SUCCESS) {
            deren::utility::log("rhi: create_acceleration_structure refused: vkCreateAccelerationStructureKHR failed");
            delete structure;
            return nullptr;
        }
        VkAccelerationStructureDeviceAddressInfoKHR address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address_info.pNext = nullptr;
        address_info.accelerationStructure = structure->native;
        structure->address = this->full_core.device.acceleration_structure_address(this->full_core.device.logical_device, &address_info);
        deren::utility::log("rhi: create_acceleration_structure: {} level, {} geometries, {} bytes of storage, {} bytes of scratch, address {:#x}",
                            top_level ? "top" : "bottom", structure->geometries.size(), structure->structure_size,
                            structure->scratch_size, static_cast<std::uint64_t>(structure->address));
        {
            std::lock_guard lock(this->group_members_mutex);
            this->group_members.insert(structure);
        }
        {
            std::lock_guard lock(this->contract_structures_mutex);
            this->contract_structures.insert(structure);
        }
        return rhi::unique_object<rhi::acceleration_structure>{structure};
    }

    // ==== core.lazy_resources.cpp:16-42 ====

    std::expected<rhi::unique_object<rhi::lazy_buffer>, rhi::error>
    core::resource_facet::create_lazy_buffer(rhi::lazy_buffer_desc const& request) {
        auto checked = rhi::validate_lazy_buffer(request);
        if (checked != rhi::error::ok) {
            return std::unexpected(checked);
        }
        auto const& desc = request.resource;
        constexpr auto allowed = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage);
        if (desc.flags & ~allowed) {
            return std::unexpected(rhi::error::unsupported);
        }
        auto value = std::unique_ptr<core::buffer>(new (std::nothrow) core::buffer);
        if (!value) {
            return std::unexpected(rhi::error::out_of_host_memory);
        }
        value->parent = &this->full_core;
        value->source = request.data;
        value->description = desc;

        value->size_bytes = desc.size;
        value->declared_usage = desc.usage;
        value->declared_flags = desc.flags;
        value->addressable = rhi::has_flag(desc.flags, rhi::buffer_flag::device_address);
        value->loading.value.store(rhi::lazy_state::unloaded);
        {
            std::lock_guard lock(this->contract_buffers_mutex);
            value->identity = this->resource_identity.fetch_add(1, std::memory_order_relaxed);
            this->contract_buffers.insert(value.get());
        }
        return rhi::unique_object<rhi::lazy_buffer>(value.release());
    }

    // ==== core.lazy_resources.cpp:45-99 ====

    std::expected<rhi::unique_object<rhi::lazy_image>, rhi::error>
    core::resource_facet::create_lazy_image(rhi::lazy_image_desc const& request) {
        if (request.struct_size < sizeof(request) || request.resource.struct_size < sizeof(rhi::image_desc) ||
            !request.data || request.data->empty() || !request.resource.initial_bytes.empty()) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        auto desc = request.resource;
        auto const format = this->full_core.resolve_format(desc.format);
        auto const allowed = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::cube_compatible);
        if (!rhi::has_flag(desc.flags, rhi::image_flag::sampled) || (desc.flags & ~allowed) ||
            desc.format == rhi::image_format::depth || format == VK_FORMAT_UNDEFINED) {
            return std::unexpected(rhi::error::unsupported);
        }
        if (!desc.extent.width || !desc.extent.height || desc.extent.depth != 1 || !desc.array_layers ||
            (rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible) &&
             (desc.array_layers != 6 || desc.extent.width != desc.extent.height))) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        auto max_mips = static_cast<std::uint32_t>(std::bit_width(std::max(desc.extent.width, desc.extent.height)));
        if (!desc.mip_levels) {
            desc.mip_levels = max_mips;
        }
        if (desc.mip_levels > max_mips) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        auto packed = detail::packed_image_size(format, desc.extent.width, desc.extent.height, desc.array_layers, desc.mip_levels);
        if (!packed || *packed != request.data->size()) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        VkImageFormatProperties properties{};
        auto flags = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        auto supported = vkGetPhysicalDeviceImageFormatProperties(this->full_core.device.physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT,
                                                                  flags, &properties);
        if (supported != VK_SUCCESS) {
            return std::unexpected(rhi::error::unsupported);
        }
        if (desc.extent.width > properties.maxExtent.width || desc.extent.height > properties.maxExtent.height ||
            desc.array_layers > properties.maxArrayLayers || desc.mip_levels > properties.maxMipLevels) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        auto value = std::unique_ptr<core::image>(new (std::nothrow) core::image);
        if (!value) {
            return std::unexpected(rhi::error::out_of_host_memory);
        }
        value->parent = &this->full_core;
        value->source = request.data;
        value->description = desc;
        value->debug_name = desc.debug_name ? desc.debug_name : "lazy image";
        value->description.debug_name = value->debug_name.c_str();
        value->width = desc.extent.width;
        value->height = desc.extent.height;
        value->mip_levels = desc.mip_levels;
        value->array_layers = desc.array_layers;
        value->resolved_format = format;
        value->declared_format = desc.format;
        value->declared_flags = desc.flags;
        value->cube_compatible = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible);
        value->loading.value.store(rhi::lazy_state::unloaded);
        {
            std::lock_guard lock(this->contract_images_mutex);
            this->contract_images.insert(value.get());
        }
        return rhi::unique_object<rhi::lazy_image>(value.release());
    }

    // ==== core.resource_groups.cpp:76-258 ====

    std::expected<rhi::resource_group_ref, rhi::error> core::resource_facet::create_resource_group(rhi::resource_group_desc const& desc) {
        auto fail = [](rhi::error error) -> std::expected<rhi::resource_group_ref, rhi::error> { return std::unexpected(error); };
        if (desc.struct_size < sizeof(desc) || desc.entries.empty() || desc.entries.size() > rhi::resource_group_capacity) {
            return fail(rhi::error::invalid_argument);
        }
        if (!this->descriptor_heaps.ready() || this->heap_grid_offset == VK_WHOLE_SIZE) {
            return fail(rhi::error::unsupported);
        }
        auto group = std::unique_ptr<core::resource_group>(new (std::nothrow) core::resource_group);
        if (!group) {
            return fail(rhi::error::out_of_host_memory);
        }
        group->parent = &this->full_core;
        group->entries.assign(desc.entries.begin(), desc.entries.end());
        std::sort(group->entries.begin(), group->entries.end(), [](auto const& a, auto const& b) { return a.binding < b.binding; });
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(this->full_core.device.physical_device, &properties);
        for (auto const& entry : group->entries) {
            if (!entry.resource.retains_lifetime()) {
                return fail(rhi::error::invalid_argument);
            }
            if (entry.kind == rhi::resource_kind::uniform_buffer || entry.kind == rhi::resource_kind::storage_buffer) {
                std::unique_lock lock(this->contract_buffers_mutex);
                auto found = std::find_if(this->contract_buffers.begin(), this->contract_buffers.end(), [&](auto* p) { return static_cast<rhi::object const*>(p) == entry.resource.get(); });
                if (found == this->contract_buffers.end()) {
                    return fail(rhi::error::invalid_argument);
                }
                auto const& value = *static_cast<core::buffer const*>(*found);
                if (value.source) {
                    lock.unlock();
                    group->lazy_references.push_back(this->full_core.depend_on(value));
                    group->deferred |= value.state() != rhi::lazy_state::ready;
                }
            } else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image) {
                std::unique_lock lock(this->contract_images_mutex);
                auto found = std::find_if(this->contract_images.begin(), this->contract_images.end(), [&](auto* p) { return static_cast<rhi::object const*>(p) == entry.resource.get(); });
                if (found == this->contract_images.end()) {
                    return fail(rhi::error::invalid_argument);
                }
                auto const& value = *static_cast<core::image const*>(*found);
                if (value.source) {
                    lock.unlock();
                    group->lazy_references.push_back(this->full_core.depend_on(value));
                    group->deferred |= value.state() != rhi::lazy_state::ready;
                }
            } else if (entry.kind == rhi::resource_kind::texture_group) {
                std::lock_guard lock(this->texture_groups.mutex);
                if (!this->texture_groups.tokens_by_group.contains(static_cast<rhi::texture_group const*>(entry.resource.get()))) {
                    return fail(rhi::error::invalid_argument);
                }
                auto const& value = *static_cast<core::texture_group const*>(entry.resource.get());
                group->deferred |= std::any_of(value.deferred_images.begin(), value.deferred_images.end(), [](auto const& p) { return bool(p); });
            }
        }
        for (std::size_t index = 0; index < group->entries.size(); ++index) {
            auto const& entry = group->entries[index];
            if (!entry.resource.retains_lifetime() || entry.kind > rhi::resource_kind::acceleration_structure ||
                (index && entry.binding == group->entries[index - 1].binding)) {
                return fail(rhi::error::invalid_argument);
            }
            auto const* entry_resource = entry.resource.get();
            if (entry.kind == rhi::resource_kind::texture_group) {
                std::lock_guard lock(this->texture_groups.mutex);
                auto found = std::find_if(this->texture_groups.tokens_by_group.begin(), this->texture_groups.tokens_by_group.end(),
                                          [entry_resource](auto const& item) { return static_cast<rhi::object const*>(item.first) == entry_resource; });
                if (found == this->texture_groups.tokens_by_group.end()) {
                    return fail(rhi::error::invalid_argument);
                }
                group->values.push_back(found->second);
                continue;
            }
            VkDeviceSize address = 0, size = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            switch (entry.kind) {
            case rhi::resource_kind::uniform_buffer:
            case rhi::resource_kind::storage_buffer: {
                std::lock_guard lock(this->contract_buffers_mutex);
                auto found = std::find_if(this->contract_buffers.begin(), this->contract_buffers.end(),
                                          [entry_resource](auto* item) { return static_cast<rhi::object const*>(item) == entry_resource; });
                if (found == this->contract_buffers.end()) {
                    return fail(rhi::error::invalid_argument);
                }
                auto* buffer = *found;
                auto const& concrete = *static_cast<core::buffer const*>(buffer);
                bool uniform = entry.kind == rhi::resource_kind::uniform_buffer;
                bool usage = uniform ? (concrete.declared_usage == rhi::buffer_usage::uniform_gpu_only ||
                                        concrete.declared_usage == rhi::buffer_usage::uniform_coherent || concrete.declared_usage == rhi::buffer_usage::uniform_cached)
                                     : (concrete.declared_usage == rhi::buffer_usage::storage_gpu_only || concrete.declared_usage == rhi::buffer_usage::storage_coherent);
                if (!usage || !concrete.addressable) {
                    return fail(rhi::error::unsupported);
                }
                if (entry.offset >= concrete.size_bytes) {
                    return fail(rhi::error::invalid_argument);
                }
                size = entry.size ? entry.size : concrete.size_bytes - entry.offset;
                auto alignment = uniform ? properties.limits.minUniformBufferOffsetAlignment : properties.limits.minStorageBufferOffsetAlignment;
                auto limit = uniform ? properties.limits.maxUniformBufferRange : properties.limits.maxStorageBufferRange;
                if (size > concrete.size_bytes - entry.offset || size > limit || (alignment && entry.offset % alignment)) {
                    return fail(rhi::error::invalid_argument);
                }
                address = this->full_core.internal_address(concrete, entry.offset);
                // Uniforms lower to native storage descriptors; satisfy both sets of constraints.
                if ((!group->deferred && !address) || (address && address % properties.limits.minStorageBufferOffsetAlignment) ||
                    size > properties.limits.maxStorageBufferRange || (address && size > UINT64_MAX - address)) {
                    return fail(rhi::error::invalid_argument);
                }
                // Heap ConstantBuffer is an OpBufferPointerEXT StorageBuffer in the current shader ABI.
                type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            }
            case rhi::resource_kind::sampled_image:
            case rhi::resource_kind::storage_image: {
                std::lock_guard lock(this->contract_images_mutex);
                if (std::none_of(this->contract_images.begin(), this->contract_images.end(),
                                 [entry_resource](auto* item) { return static_cast<rhi::object const*>(item) == entry_resource; })) {
                    return fail(rhi::error::invalid_argument);
                }
                auto const& image = *static_cast<core::image const*>(entry_resource);
                auto const& view = entry.view;
                bool sampled = entry.kind == rhi::resource_kind::sampled_image;
                if (view.struct_size < sizeof(view) || view.base_mip >= image.mip_levels || view.base_layer >= image.array_layers ||
                    (view.mip_count && view.mip_count > image.mip_levels - view.base_mip) ||
                    (view.layer_count && view.layer_count > image.array_layers - view.base_layer)) {
                    return fail(rhi::error::invalid_argument);
                }
                if (view.role != (sampled ? rhi::view_role::sampled : rhi::view_role::storage) ||
                    !rhi::has_flag(image.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage)) {
                    return fail(rhi::error::unsupported);
                }
                break;
            }
            case rhi::resource_kind::sampler:
            case rhi::resource_kind::acceleration_structure: {
                std::lock_guard lock(this->group_members_mutex);
                if (!this->group_members.contains(entry_resource)) {
                    return fail(rhi::error::invalid_argument);
                }
                if (entry.kind == rhi::resource_kind::sampler) {
                    if (!rhi::is_concrete(*entry_resource, concrete::sampler)) {
                        return fail(rhi::error::invalid_argument);
                    }
                } else {
                    if (!rhi::is_concrete(*entry_resource, concrete::acceleration_structure)) {
                        return fail(rhi::error::invalid_argument);
                    }
                    auto const& concrete = *static_cast<core::acceleration_structure const*>(entry_resource);
                    if (!concrete.top_level || !concrete.address || !concrete.structure_size) {
                        return fail(rhi::error::invalid_argument);
                    }
                    address = concrete.address;
                    size = concrete.structure_size;
                    type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                }
                break;
            }
            default:
                return fail(rhi::error::invalid_argument);
            }
            if (group->deferred) {
                group->values.push_back(0);
                continue;
            }
            bool sampler = entry.kind == rhi::resource_kind::sampler;
            VkDeviceSize offset;
            {
                std::lock_guard lock(this->resource_groups_mutex);
                auto& free = sampler ? this->sampler_slots : this->resource_slots;
                if (!free.empty()) {
                    offset = free.back();
                    free.pop_back();
                } else if (sampler) {
                    if (!this->next_sampler) {
                        this->next_sampler = heap_sampler_base + static_cast<std::uint32_t>(this->shared_sampler_infos.size());
                    }
                    offset = VkDeviceSize(this->next_sampler) * 32u;
                    if (offset + 32u > this->descriptor_heaps.sampler_size()) {
                        return fail(rhi::error::out_of_device_memory);
                    }
                    ++this->next_sampler;
                } else {
                    offset = this->descriptor_heaps.reserve_bytes(64u, 64u);
                    if (offset == VK_WHOLE_SIZE) {
                        return fail(rhi::error::out_of_device_memory);
                    }
                }
            }
            group->allocations.push_back({offset, sampler});
            if (sampler) {
                auto const& info = static_cast<core::sampler const*>(entry_resource)->info;
                if (!this->descriptor_heaps.write_samplers(offset, std::span(&info, 1))) {
                    return fail(rhi::error::operation_failed);
                }
            } else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image) {
                auto type = entry.kind == rhi::resource_kind::sampled_image ? rhi::descriptor_type::sampled_image : rhi::descriptor_type::storage_image;
                rhi::heap_image_write_info info{.offset = offset, .resource = static_cast<rhi::image const*>(entry_resource), .view = &entry.view, .type = type};
                auto status = this->heap_view.write_image_native(info, true);
                if (status != rhi::error::ok) {
                    return fail(status);
                }
            } else if (!address || !this->descriptor_heaps.write_buffer(offset, address, size, type)) {
                return fail(rhi::error::operation_failed);
            }
            group->values.push_back(static_cast<std::uint32_t>(offset / (sampler ? 32u : 64u)));
        }
        {
            std::lock_guard lock(this->resource_groups_mutex);
            this->resource_groups.insert(group.get());
        }
        return rhi::resource_group_ref(group.release());
    }

    // ==== core.texture_groups.cpp:228-317 ====

    rhi::texture_group_ref core::resource_facet::assign_texture_group(rhi::texture_group_info const& info, rhi::error* result) {
        this->full_core.poll_submissions();
        auto fail = [result](rhi::error value) -> rhi::texture_group_ref {
            if (result != nullptr) {
                *result = value;
            }
            return {};
        };
        if (result != nullptr) {
            *result = rhi::error::invalid_argument;
        }
        if (info.struct_size < sizeof(info)) {
            return {};
        }
        if (this->full_core.device.logical_device == VK_NULL_HANDLE) {
            return fail(rhi::error::not_ready);
        }

        deren::utility::vessel::array<rhi::shared_object<rhi::image>, 16> images{};
        {
            std::lock_guard lock(this->contract_images_mutex);
            // Validate every input before acquiring or allocating any group resources.
            for (auto const& input : info.textures) {
                auto* image = input.value_or(nullptr);
                if (image == nullptr) {
                    continue;
                }
                if (!this->contract_images.contains(image)) {
                    return {};
                }
                auto const& concrete = *static_cast<core::image const*>(image);
                auto const valid = this->full_core.validate_texture_group_image(concrete);
                if (valid == rhi::error::unsupported) {
                    return fail(rhi::error::unsupported);
                }
                if (valid != rhi::error::ok) {
                    return {};
                }
            }
            for (std::size_t i = 0; i < images.size(); ++i) {
                if (auto* image = info.textures[i].value_or(nullptr)) {
                    images[i] = image->share();
                }
            }
        }
        auto initialized = this->full_core.initialize_texture_groups();
        if (initialized != rhi::error::ok) {
            return fail(initialized);
        }
        auto answer = std::unique_ptr<core::texture_group>(new (std::nothrow) core::texture_group);
        if (!answer) {
            return fail(rhi::error::out_of_host_memory);
        }
        answer->parent = &this->full_core;
        rhi::error status = rhi::error::ok;
        {
            std::lock_guard lock(this->texture_groups.mutex);
            std::erase_if(this->texture_groups.cache, [](auto const& item) { return item.second.expired(); });
            if (std::any_of(images.begin(), images.end(), [](auto const& image) { return bool(image); })) {
                auto free = std::find(this->texture_groups.groups.begin(), this->texture_groups.groups.end(), false);
                if (free == this->texture_groups.groups.end()) {
                    status = rhi::error::out_of_device_memory;
                } else {
                    answer->allocation = static_cast<std::uint32_t>(free - this->texture_groups.groups.begin());
                    *free = true;
                }
            }
            for (std::size_t i = 0; status == rhi::error::ok && i < images.size(); ++i) {
                if (!images[i]) {
                    continue;
                }
                status = this->full_core.write_group_texture_locked(*answer, static_cast<std::uint32_t>(i), *images[i]);
            }
            if (status == rhi::error::ok && answer->allocation < this->texture_groups.groups.size()) {
                // THE POSITION BECOMES REFERENCEABLE only after every slot it covers was written: the token
                // and its generation enter the map together, so `descriptor_space::write` cannot see a
                // half-filled group.
                answer->generation = ++this->texture_groups.next_generation;
                this->texture_groups.groups_by_token[answer->allocation + 1u] = answer.get();
                this->texture_groups.tokens_by_group[answer.get()] = answer->allocation + 1u;
            }
        }
        if (status != rhi::error::ok) {
            return fail(status);
        }
        if (result != nullptr) {
            *result = rhi::error::ok;
        }
        return rhi::texture_group_ref(answer.release());
    }

} // namespace deren::vulkan
