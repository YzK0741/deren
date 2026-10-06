// -*- C++ -*-
// ============================================================================
// file: vulkan/core/core.api_core.cpp
//
// THE RHI CONTRACT'S VIRTUALS, DEFINED FOR THE REAL BACKEND (plan_rhi_v4.md §11.4, S1-C; the
// recording surface, the escape and the read-back slot are S2 batch 2).
// `core` derives from `deren::promise::rhi::api_core` (vulkan/core/core.declarations.cppm), so the
// object that owns the instance / device / swapchain IS the object a host gets from
// deren_make_api_core() - there is no wrapper type to keep in step with it.
//
// WHAT IS REAL HERE, AND WHAT IS DELIBERATELY NOT:
//
//   - `abilities()`: A BIT MEANS THE CONTRACT CAN SERVE IT, not that the device has the feature. For
//     every set bit, `query_extension(kind)` has to answer with an object that can carry out every
//     operation that ability declares, on the objects this backend can produce. THIS BACKEND REPORTS
//     `vulkan_escape` AND NOTHING ELSE: every handle the escape hands out is a member that already
//     exists, so the bit is servable in the strict sense. `device_address` and `host_image_copy` are
//     still NOT reported - both need a producible `buffer`/`image`, and the factories still answer
//     nullptr because their descriptors are S3.
//   - `query_extension()`: the escape object for `vulkan_escape`, nullptr for everything else -
//     exactly the two-way invariant the startup gate (core.constructor.cppm, G2) and
//     tests/test_dynamic_link.cpp (G1) check.
//   - the factories: nullptr. Their descriptors are still forward-declared (the resource model is
//     §6.4/S3), so building them here would mean inventing S3. A factory that cannot honour a
//     descriptor answers nullptr (§4.2: no throwing path across the boundary).
//   - the recording surface and the frame views: REAL, and they are the only `rhi` handles that can
//     exist while the factories answer nullptr - borrowed views of objects this class already owns
//     (the frame's primary command buffer, the swapchain image the frame draws into, the read-back
//     slot). See the view types in core.declarations.cppm.
//   - the shadow gate (plan §8.3, gate A5): `use()` derives the barrier from the contract's role pair
//     and the asserts right below prove - field by field, AT COMPILE TIME - that it lands exactly on
//     the two recipes this renderer shipped by hand. The same dump is emitted once per pair at run
//     time, so the log carries both sides.
//   - the frame calls: the machinery this file's class already had - `wait_frame_slot`,
//     `image_available_semaphores`, `submit()`, `present()`, `wait_idle()`. `frame_begin()` also
//     performs the acquire, which is the one piece that used to live in the runtime
//     (runtime.frames.cppm:115): the device and the swapchain are the backend's, so the acquire is
//     the backend's too.
// ============================================================================
module;

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.core;

import deren.promise.rhi;
import deren.vulkan.constant_init;
import deren.vulkan.core.pipeline;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    namespace {

        /// The barrier one (from, to) role pair needs.
        ///
        /// THE DERIVATION FROM THE PAIR IS THE POINT of the shadow gate: it is spelled here in the
        /// contract's vocabulary (two roles), and the asserts below prove that it lands, field by
        /// field, on the renderer's own recipe constant for the same transition. A pair this backend
        /// cannot spell has no stages and no accesses at all, which is the honest answer for a
        /// transition nobody has defined yet - `use()` reports it once instead of recording nonsense.
        [[nodiscard]] constexpr VkImageMemoryBarrier2 barrier_for(rhi::image_use const from, rhi::image_use const to) noexcept {
            VkImageMemoryBarrier2 barrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = 0,
                .srcAccessMask = 0,
                .dstStageMask = 0,
                .dstAccessMask = 0,
                // EVERY IMAGE IN THIS RENDERER LIVES IN GENERAL: the layouts are an invariant, not a
                // parameter (docs/unified_image_layouts.md), which is exactly why the contract carries
                // no layout and why both sides of every transition here spell GENERAL.
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = VK_NULL_HANDLE,
                // The whole image, one mip, one layer: what the screenshot read-back copies, and what
                // the hand-written recipes carry.
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            if (from == rhi::image_use::color_attachment && to == rhi::image_use::transfer_source) {
                // what the frame wrote as a render target is about to be read by a transfer
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            } else if (from == rhi::image_use::transfer_source && to == rhi::image_use::color_attachment) {
                // the mirror: the copy is done and the frame gets its render target back (the present
                // transition that follows assumes GENERAL, see constant_init's present_transition)
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            }
            return barrier;
        }

        /// Every field of a VkImageMemoryBarrier2, for the shadow gate's equality proofs and its dump
        [[nodiscard]] constexpr bool same_barrier(VkImageMemoryBarrier2 const& a, VkImageMemoryBarrier2 const& b) noexcept {
            return a.sType == b.sType && a.pNext == b.pNext && a.srcStageMask == b.srcStageMask && a.srcAccessMask == b.srcAccessMask &&
                   a.dstStageMask == b.dstStageMask && a.dstAccessMask == b.dstAccessMask && a.oldLayout == b.oldLayout && a.newLayout == b.newLayout &&
                   a.srcQueueFamilyIndex == b.srcQueueFamilyIndex && a.dstQueueFamilyIndex == b.dstQueueFamilyIndex && a.image == b.image &&
                   a.subresourceRange.aspectMask == b.subresourceRange.aspectMask &&
                   a.subresourceRange.baseMipLevel == b.subresourceRange.baseMipLevel &&
                   a.subresourceRange.levelCount == b.subresourceRange.levelCount &&
                   a.subresourceRange.baseArrayLayer == b.subresourceRange.baseArrayLayer &&
                   a.subresourceRange.layerCount == b.subresourceRange.layerCount;
        }

        // ---- GATE A5, THE COMPILE-TIME HALF: THE DERIVED BARRIERS ARE THE SHIPPED RECIPES ----------
        // The two transitions this batch records must be the ones the renderer already had, and "the
        // same" here means all sixteen fields: stage/access masks, both layouts, queue family indices,
        // the image handle slot and the four subresource fields. A mismatch is a BUILD failure, not a
        // validation message at run time.
        constexpr VkImageMemoryBarrier2 derived_attachment_to_transfer = barrier_for(rhi::image_use::color_attachment, rhi::image_use::transfer_source);
        constexpr VkImageMemoryBarrier2 derived_transfer_to_attachment = barrier_for(rhi::image_use::transfer_source, rhi::image_use::color_attachment);
        static_assert(same_barrier(derived_attachment_to_transfer, deren::vulkan::color_attachment_to_transfer_transition),
                      "A5: use(color_attachment, transfer_source) must land field-for-field on color_attachment_to_transfer_transition");
        static_assert(same_barrier(derived_transfer_to_attachment, deren::vulkan::transfer_to_color_attachment_transition),
                      "A5: use(transfer_source, color_attachment) must land field-for-field on transfer_to_color_attachment_transition");

        /// every field of one barrier on one line, for the shadow gate's run-time dump (A5 asks for
        /// BOTH sides in the log, not just for "equal or not")
        [[nodiscard]] std::string barrier_text(VkImageMemoryBarrier2 const& barrier) {
            return std::format("sType={:#x} pNext={:#x} srcStage={:#x} srcAccess={:#x} dstStage={:#x} dstAccess={:#x} oldLayout={:#x} newLayout={:#x} "
                               "srcQFI={:#x} dstQFI={:#x} image={:#x} aspect={:#x} mip={}+{} layer={}+{}",
                               static_cast<std::uint32_t>(barrier.sType),
                               reinterpret_cast<std::uintptr_t>(barrier.pNext),
                               static_cast<std::uint64_t>(barrier.srcStageMask),
                               static_cast<std::uint64_t>(barrier.srcAccessMask),
                               static_cast<std::uint64_t>(barrier.dstStageMask),
                               static_cast<std::uint64_t>(barrier.dstAccessMask),
                               static_cast<std::uint32_t>(barrier.oldLayout),
                               static_cast<std::uint32_t>(barrier.newLayout),
                               static_cast<std::uint32_t>(barrier.srcQueueFamilyIndex),
                               static_cast<std::uint32_t>(barrier.dstQueueFamilyIndex),
                               reinterpret_cast<std::uintptr_t>(barrier.image),
                               static_cast<std::uint32_t>(barrier.subresourceRange.aspectMask),
                               barrier.subresourceRange.baseMipLevel,
                               barrier.subresourceRange.levelCount,
                               barrier.subresourceRange.baseArrayLayer,
                               barrier.subresourceRange.layerCount);
        }

    } // namespace

    // ---- THE APPEND-ONLY ABI GUARD FOR THE FACTORY DESCRIPTORS -------------------------------------
    // `core.constructor.cppm` applies the same rule to the context's creation structure (§11.2 of the
    // plan); the arithmetic itself lives in one place, `covered_by` in core.declarations.cppm, because
    // both descriptors are read through it.
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

        // ---- ABI 7'S IMAGE FACE: THE CONTRACT'S VOCABULARY IN THIS BACKEND'S ------------------------
        // The same one-to-one, written-out mapping rule create_buffer uses: a new contract value must
        // not silently become whatever the integer happens to mean here.

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

        /// the same ABI guard for `image_view_desc`.
        [[nodiscard]] deren::promise::rhi::image_view_desc sanitize_image_view_desc(deren::promise::rhi::image_view_desc const& desc) {
            using rhi_view_desc = deren::promise::rhi::image_view_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_view_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI image_view_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_view_desc options = {};
            if (covered_by(declared, offsetof(rhi_view_desc, base_layer), sizeof(rhi_view_desc::base_layer))) {
                options.base_layer = desc.base_layer;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, layer_count), sizeof(rhi_view_desc::layer_count))) {
                options.layer_count = desc.layer_count;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, base_mip), sizeof(rhi_view_desc::base_mip))) {
                options.base_mip = desc.base_mip;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, mip_count), sizeof(rhi_view_desc::mip_count))) {
                options.mip_count = desc.mip_count;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, role), sizeof(rhi_view_desc::role))) {
                options.role = desc.role;
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

    rhi::ability_bits core::abilities() const noexcept {
        // ---- TWO BITS NOW, AND EACH IS A PROMISE ABOUT SERVICE (batch-1 spec §4.1) -----------------
        //
        // `vulkan_escape` is the one ability this backend could serve from the start, and it is servable
        // for the strongest reason there is: every handle it hands out is a member that already exists
        // (`instance`, `physical_device`, `logical_device`, `graphics_queue_handle`, the frame's own
        // command buffer), so `query_extension()` answers with a real object and every operation the
        // ability declares is performable on objects this backend produces. The bit is therefore NOT
        // "the device has the feature" - a device-level fact is not an ability until something can
        // serve it (batch-1 §4.1) - it is "this backend can carry out everything vulkan_escape declares".
        //
        // `device_address` JOINED IT WHEN BUFFERS BECAME PRODUCTIBLE, which is the condition the previous
        // comment here recorded as missing: the ability now declares only `buffer_address()`, and a
        // buffer is something `create_buffer()` hands out. Its former acceleration-structure half moved
        // to `ray_tracing` (abi 5) precisely so this bit would stop being hostage to a resource no
        // backend could make - see rhi.extension.cppm's `device_address` note and the view's own.
        //
        // descriptor_heap只在heap真正可用时广播；其余未实现的能力继续不广播。
        // host_image_copy未接线；mesh_shader和ray_tracing仍由pass通过原生接口录制。
        return rhi::to_bits(rhi::extension_kind::vulkan_escape) | rhi::to_bits(rhi::extension_kind::device_address) |
               (this->heap_view.ready() ? rhi::to_bits(rhi::extension_kind::descriptor_heap) : 0u);
    }

    bool core::frame_heap::ready() const noexcept {
        return this->owner != nullptr && this->owner->descriptor_heaps.ready();
    }

    rhi::descriptor_heap_properties core::frame_heap::properties() const noexcept {
        if (this->owner == nullptr)
            return {};
        auto const& limits = this->owner->descriptor_heaps.limits();
        return {
            .resource_size = this->owner->descriptor_heaps.resource_size(),
            .max_resource_size = limits.max_resource_size,
            .max_sampler_size = limits.max_sampler_size,
            .resource_alignment = limits.resource_alignment,
            .sampler_alignment = limits.sampler_alignment,
            .resource_reserved = limits.resource_reserved,
            .sampler_reserved_with_embedded = limits.sampler_reserved_with_embedded,
            .buffer_descriptor_size = limits.buffer_descriptor_size,
            .image_descriptor_size = limits.image_descriptor_size,
            .sampler_descriptor_size = limits.sampler_descriptor_size,
            .max_push_data = limits.max_push_data,
            .max_embedded_samplers = limits.max_embedded_samplers,
        };
    }

    namespace {
        [[nodiscard]] constexpr VkDescriptorType heap_descriptor_type(rhi::descriptor_type const type) noexcept {
            switch (type) {
            case rhi::descriptor_type::sampled_image:
                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case rhi::descriptor_type::storage_image:
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case rhi::descriptor_type::uniform_buffer:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case rhi::descriptor_type::storage_buffer:
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case rhi::descriptor_type::acceleration_structure:
                return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            case rhi::descriptor_type::uniform_buffer_dynamic:
            case rhi::descriptor_type::storage_buffer_dynamic:
            case rhi::descriptor_type::combined_image_sampler:
                return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }

        // VK_EXT_descriptor_heap禁止把组合采样器或动态buffer描述符直接写入资源heap。
        static_assert(heap_descriptor_type(rhi::descriptor_type::combined_image_sampler) == VK_DESCRIPTOR_TYPE_MAX_ENUM);
        static_assert(heap_descriptor_type(rhi::descriptor_type::uniform_buffer_dynamic) == VK_DESCRIPTOR_TYPE_MAX_ENUM);
        static_assert(heap_descriptor_type(rhi::descriptor_type::storage_buffer_dynamic) == VK_DESCRIPTOR_TYPE_MAX_ENUM);

        [[nodiscard]] VkCommandBuffer heap_commands(core& owner, rhi::command_list* const commands,
                                                    rhi::structure_header const* const next, rhi::error& result) noexcept {
            if (next != nullptr) {
                if (next->s_type != rhi::structure_type::vulkan_command_buffer) {
                    result = rhi::error::unsupported;
                    return VK_NULL_HANDLE;
                }
                result = rhi::validate_structure(*next, rhi::structure_type::vulkan_command_buffer, sizeof(rhi::vulkan_command_buffer_info));
                if (result != rhi::error::ok)
                    return VK_NULL_HANDLE;
                auto const& native = *reinterpret_cast<rhi::vulkan_command_buffer_info const*>(next);
                if (commands != nullptr || native.context != static_cast<rhi::api_core const*>(&owner) || native.commands == nullptr) {
                    result = rhi::error::invalid_argument;
                    return VK_NULL_HANDLE;
                }
                // 原生命令缓冲的归属和录制状态仍是调用方前提；不强转frame_commands。
                return static_cast<VkCommandBuffer>(native.commands);
            }
            if (commands != &owner.commands_view) {
                result = rhi::error::invalid_argument;
                return VK_NULL_HANDLE;
            }
            if (!owner.frame_in_flight) {
                result = rhi::error::not_ready;
                return VK_NULL_HANDLE;
            }
            return owner.frame_command_buffer();
        }
    } // namespace

    rhi::error core::frame_heap::write_image(rhi::heap_image_write_info const& info) noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_image_write, sizeof(info), true);
        if (checked != rhi::error::ok)
            return checked;
        if (!this->ready())
            return rhi::error::not_ready;
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            return rhi::error::unsupported;
        auto const capacity = this->owner->descriptor_heaps.resource_size();
        auto const stride = this->owner->descriptor_heaps.descriptor_stride(type);
        if (info.offset > capacity || stride > capacity - info.offset)
            return rhi::error::invalid_argument;
        VkImageViewCreateInfo view{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        if (info.header.next != nullptr) {
            if (info.header.next->s_type != rhi::structure_type::vulkan_heap_image)
                return rhi::error::unsupported;
            rhi::error const native_checked = rhi::validate_structure(*info.header.next, rhi::structure_type::vulkan_heap_image, sizeof(rhi::vulkan_heap_image_info));
            if (native_checked != rhi::error::ok)
                return native_checked;
            auto const& native = *reinterpret_cast<rhi::vulkan_heap_image_info const*>(info.header.next);
            auto const& desc = native.view;
            if (info.resource != nullptr || info.view != nullptr || native.context != static_cast<rhi::api_core const*>(this->owner) ||
                desc.struct_size < sizeof(desc) || desc.native_image == nullptr)
                return rhi::error::invalid_argument;
            view = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = desc.view_flags,
                .image = static_cast<VkImage>(desc.native_image),
                .viewType = static_cast<VkImageViewType>(desc.view_type),
                .format = static_cast<VkFormat>(desc.format),
                .components = {static_cast<VkComponentSwizzle>(desc.components[0]), static_cast<VkComponentSwizzle>(desc.components[1]),
                               static_cast<VkComponentSwizzle>(desc.components[2]), static_cast<VkComponentSwizzle>(desc.components[3])},
                .subresourceRange = {desc.aspect_mask, desc.base_mip, desc.mip_count, desc.base_layer, desc.layer_count},
            };
            layout = static_cast<VkImageLayout>(native.layout);
        } else {
            if (info.resource == nullptr || info.view == nullptr || info.view->struct_size < sizeof(rhi::image_view_desc))
                return rhi::error::invalid_argument;
            // 类型标签相同不代表具体布局相同；先核实本core发出的活跃image，再访问owned_image。
            {
                std::lock_guard const lock(this->owner->contract_images_mutex);
                if (!this->owner->contract_images.contains(info.resource))
                    return rhi::error::invalid_argument;
            }
            auto const& image = *static_cast<owned_image const*>(info.resource);
            auto const& range = *info.view;
            if (range.base_layer >= image.array_layers || range.base_mip >= image.mip_levels)
                return rhi::error::invalid_argument;
            auto const layers = range.layer_count == 0 ? image.array_layers - range.base_layer : range.layer_count;
            auto const mips = range.mip_count == 0 ? image.mip_levels - range.base_mip : range.mip_count;
            if (layers > image.array_layers - range.base_layer || mips > image.mip_levels - range.base_mip)
                return rhi::error::invalid_argument;
            bool const sampled = type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if ((sampled && range.role != rhi::view_role::sampled) || (!sampled && range.role != rhi::view_role::storage) ||
                !rhi::has_flag(image.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage))
                return rhi::error::unsupported;
            bool const cube = image.cube_compatible && image.array_layers == 6 && range.base_layer == 0 && layers == 6;
            view = make_image_view_info(image.native_handle, image.resolved_format,
                                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : layers == 1 ? VK_IMAGE_VIEW_TYPE_2D
                                                                                     : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                        image.declared_format == rhi::image_format::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                        mips, layers);
            view.subresourceRange.baseMipLevel = range.base_mip;
            view.subresourceRange.baseArrayLayer = range.base_layer;
        }
        return this->owner->descriptor_heaps.write_image(info.offset, view, layout, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::write_buffer(rhi::heap_buffer_write_info const& info) noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info));
        if (checked != rhi::error::ok)
            return checked;
        if (!this->ready())
            return rhi::error::not_ready;
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
            return rhi::error::unsupported;
        auto const capacity = this->owner->descriptor_heaps.resource_size();
        auto const stride = this->owner->descriptor_heaps.descriptor_stride(type);
        if (info.address == 0 || info.size == 0 || info.size > UINT64_MAX - info.address ||
            info.offset > capacity || stride > capacity - info.offset)
            return rhi::error::invalid_argument;
        return this->owner->descriptor_heaps.write_buffer(info.offset, info.address, info.size, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::bind(rhi::heap_bind_info const& info) const noexcept {
        rhi::error result = rhi::validate_structure(info.header, rhi::structure_type::heap_bind, sizeof(info), true);
        if (result != rhi::error::ok)
            return result;
        if (!this->ready())
            return rhi::error::not_ready;
        VkCommandBuffer const commands = heap_commands(*this->owner, info.commands, info.header.next, result);
        if (result != rhi::error::ok)
            return result;
        this->owner->descriptor_heaps.record_bind(commands);
        return rhi::error::ok;
    }

    rhi::error core::frame_heap::push_data(rhi::heap_push_info const& info) const noexcept {
        rhi::error result = rhi::validate_structure(info.header, rhi::structure_type::heap_push, sizeof(info), true);
        if (result != rhi::error::ok)
            return result;
        if (!this->ready())
            return rhi::error::not_ready;
        result = rhi::validate_heap_push_range(info.offset, info.data.size(), this->properties().max_push_data);
        if (result != rhi::error::ok)
            return result;
        VkCommandBuffer const commands = heap_commands(*this->owner, info.commands, info.header.next, result);
        if (result != rhi::error::ok)
            return result;
        return this->owner->descriptor_heaps.push_data(commands, info.offset, info.data) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::heap_bindings core::frame_heap::bindings() const noexcept {
        if (!this->ready()) {
            return {};
        }
        VkBindHeapInfoEXT resource{}, sampler{};
        this->owner->descriptor_heaps.bind_infos(resource, sampler);
        return {
            .resource = {resource.heapRange.address, resource.heapRange.size, resource.reservedRangeOffset, resource.reservedRangeSize},
            .sampler = {sampler.heapRange.address, sampler.heapRange.size, sampler.reservedRangeOffset, sampler.reservedRangeSize},
        };
    }

    rhi::extension* core::query_extension(rhi::extension_kind const kind) noexcept {
        // The invariant is two-way and is checked in two places: at startup on the REAL backend
        // (core.constructor.cppm, gate G2) and on the probe backend in tests/test_dynamic_link.cpp
        // (G1). Every kind this backend announces answers with an object whose kind() is the kind that
        // was asked for, and every kind it does not announce answers nullptr.
        if (kind == rhi::extension_kind::vulkan_escape) {
            return &this->escape_view;
        }
        if (kind == rhi::extension_kind::descriptor_heap && this->heap_view.ready()) {
            return &this->heap_view;
        }
        if (kind == rhi::extension_kind::device_address) {
            return &this->address_view;
        }
        return nullptr;
    }

    rhi::swapchain* core::create_swapchain(rhi::swapchain_desc const& /*desc*/) {
        return nullptr;
    }

    rhi::buffer* core::create_buffer(rhi::buffer_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN THE DESCRIPTOR ------------------------------------------------
        // Same rule the context's descriptor follows (core.constructor.cppm): the caller declares how
        // many bytes of the structure IT compiled, and a field whose whole extent is not inside them
        // keeps this build's default. A shorter structure is an older caller; a longer one is a newer
        // caller whose appended tail this build has never heard of - either way only the prefix is read.
        rhi::buffer_desc const desc = sanitize_buffer_desc(declared_desc);

        if (desc.size == 0) {
            // A ZERO-BYTE BUFFER IS NOT A BUFFER, and the descriptor asked for nothing. Answering
            // nullptr is the contract's "this descriptor cannot be honoured" (§4.2: no throwing path).
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
        }

        // THE CAPABILITY FLAGS ARE THE ONLY VULKAN USAGE BITS THE CONTRACT NAMES, and they are named
        // for what they LET THE CALLER DO rather than for the bit: the renderer's twelve creation
        // sites ask for exactly these three (a device address, an acceleration-structure build input,
        // micromap storage) and for nothing else.
        uint32_t extra_usage = 0;
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
        //     with NO validation finding" (vulkan/runtime/runtime.constructor.cppm's own note).
        //   - `indirect`: a buffer that carries dispatch/draw commands.
        //   - `shader_binding_table`: a ray-tracing SBT.
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::storage)) {
            extra_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
        auto* const answer = new owned_buffer{};
        // `initial_bytes` is the contract's `std::byte` view and the allocator takes `uint8_t const*`:
        // the cast is the boundary between the contract's byte type and VMA's, spelled here so neither
        // side has to know the other's.
        answer->owned = this->vma.create_buffer(
            desc.initial_bytes.empty() ? nullptr : reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.size, type, extra_usage);
        if (!answer->owned.valid()) {
            deren::utility::log("rhi: create_buffer could not allocate {} B (usage {}, flags {:#x})", desc.size,
                                static_cast<std::uint32_t>(desc.usage), desc.flags);
            delete answer;
            return nullptr;
        }
        answer->size_bytes = desc.size;
        answer->addressable = rhi::has_flag(desc.flags, rhi::buffer_flag::device_address);
        answer->declared_flags = desc.flags;

        // THE DETAIL IS READ, NEVER KEPT: `get_buffer_detail` answers with a pointer into the
        // allocator's own map and has already released its lock by the time it returns, so what is
        // copied out here are the two VALUES the contract needs - the Vulkan handle and the mapped
        // address - not the pointer to the record (see the type's note).
        auto const* const detail = this->vma.get_buffer_detail(answer->owned.handle());
        if (detail != nullptr) {
            answer->native = detail->buffer;
            answer->mapped_bytes = detail->allocation_info.pMappedData;
        }
        deren::utility::log("rhi: create_buffer {} B usage {} flags {:#x} -> extra {:#x}, native {:#x}, mapped {}",
                            desc.size, static_cast<std::uint32_t>(desc.usage), desc.flags, extra_usage,
                            reinterpret_cast<std::uintptr_t>(answer->native), answer->mapped_bytes != nullptr);
        return answer;
    }

    std::uint64_t core::owned_buffer::size() const noexcept {
        return this->size_bytes;
    }

    std::span<std::byte> core::owned_buffer::mapped() noexcept {
        // EMPTY when this buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing (the same answer the
        // frame's read-back view gives).
        if (this->mapped_bytes == nullptr || this->size_bytes == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->mapped_bytes), static_cast<std::size_t>(this->size_bytes));
    }

    void core::owned_buffer::release() noexcept {
        // RELEASE IS THE ALLOCATOR'S REFERENCE-COUNT DECREMENT, and it happens inside the backend: the
        // destructor resets the `vk_buffer` owner, whose release lambda reaches `free_buffer` - which
        // destroys the buffer only if this was the last reference (rhi.api_core.cppm's ownership note;
        // vma_allocator's own comment says the same). A heap object because the FACTORY made it, so the
        // matching delete is the backend's own operator delete.
        delete this;
    }

    rhi::image* core::create_image(rhi::image_desc const& declared_desc) {
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
        VkFormat const native_format = native_image_format(desc.format, this->depth_attachment_format);
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
        vk_image const owned = this->vma.create_image(
            reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.initial_bytes.size(), create_info, type);
        if (owned.handle() == 0) {
            deren::utility::log("rhi: create_image {} refused: the allocator could not serve {}x{}, {} layer(s), {} mip(s)",
                                what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels);
            return nullptr;
        }

        // The detail lookup is an UNLOCKED BORROW: the VkImage is copied out NOW (the same rule
        // owned_buffer's comment states), never the pointer into the allocator's map.
        image_detail const* const detail = this->vma.get_image_detail(owned.handle());
        if (detail == nullptr) {
            deren::utility::log("rhi: create_image {} refused: the allocator holds no detail for the allocation", what);
            return nullptr;
        }
        auto* const answer = new owned_image();
        answer->owner = this;
        answer->owned = owned;
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
                            what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels, owned.handle());
        return answer;
    }

    rhi::sampler* core::create_sampler(rhi::sampler_desc const& declared_desc) {
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
        if (vkCreateSampler(this->logical_device, &info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_sampler refused: vkCreateSampler failed (mode {}, max lod {})",
                                static_cast<int>(desc.address_mode), desc.max_lod);
            return nullptr;
        }
        auto* const answer = new owned_sampler();
        answer->native_sampler_handle = handle;
        answer->device = this->logical_device;
        return answer;
    }

    // ---- THE OWNED IMAGE FACE'S OBJECTS (abi 7, §17's design) --------------------------------------

    rhi::image_extent core::owned_image::extent() const noexcept {
        return {.width = this->width, .height = this->height, .depth = 1};
    }

    rhi::image_format core::owned_image::format() const noexcept {
        return this->declared_format;
    }

    rhi::image_view* core::owned_image::make_view(rhi::image_view_desc const& declared_desc) {
        // The range is validated against THIS image's shape, per the contract's rule: a range the
        // image does not have is refused, not clamped - a clamped view would sample the wrong mip and
        // say nothing.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        uint32_t const layers = desc.layer_count == 0 ? this->array_layers - desc.base_layer : desc.layer_count;
        uint32_t const mips = desc.mip_count == 0 ? this->mip_levels - desc.base_mip : desc.mip_count;
        if (desc.base_layer >= this->array_layers || layers == 0 || desc.base_layer + layers > this->array_layers) {
            deren::utility::log("rhi: make_view refused: layer range [{} + {}) outside the image's {}", desc.base_layer, layers, this->array_layers);
            return nullptr;
        }
        if (desc.base_mip >= this->mip_levels || mips == 0 || desc.base_mip + mips > this->mip_levels) {
            deren::utility::log("rhi: make_view refused: mip range [{} + {}) outside the image's {}", desc.base_mip, mips, this->mip_levels);
            return nullptr;
        }

        // The role picks the aspect (the format is the image's own). The view TYPE follows the image's
        // shape: a cube-compatible six-layer image viewed whole IS the cube view; any other
        // multi-layer image is a 2D array; everything else a plain 2D.
        VkImageAspectFlags const aspect = this->declared_format == rhi::image_format::depth
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
        bool const whole_cube = this->cube_compatible && this->array_layers == 6 && desc.base_layer == 0 && layers == 6u;
        // THE RANGE DECIDES THE TYPE, because the shader's sampler must agree with it: a single-layer
        // range is a plain 2D view even on a layered image (the per-cascade shadow layer view samples
        // as texture2D), a whole six-layer cube-compatible image is the CUBE, and every other
        // multi-layer range is a 2D array.
        VkImageViewType const view_type = whole_cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                          : layers == 1 ? VK_IMAGE_VIEW_TYPE_2D
                                                        : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        VkImageViewCreateInfo const base_info = make_image_view_info(this->native_handle,
                                                                     this->resolved_format,
                                                                     view_type, aspect, mips, layers);
        // make_image_view_info always bases at 0; the contract's desc carries the base explicitly.
        VkImageViewCreateInfo view_info = base_info;
        view_info.subresourceRange.baseMipLevel = desc.base_mip;
        view_info.subresourceRange.baseArrayLayer = desc.base_layer;
        view_info.subresourceRange.levelCount = mips;
        view_info.subresourceRange.layerCount = layers;

        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->owner->logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view refused: vkCreateImageView failed");
            return nullptr;
        }
        auto* const answer = new owned_image_view();
        answer->native_view = handle;
        answer->device = this->owner->logical_device;
        return answer;
    }

    void core::owned_image::release() noexcept {
        {
            std::lock_guard const lock(this->owner->contract_images_mutex);
            this->owner->contract_images.erase(this);
        }
        // `delete this`: the destructor resets the `vk_image` RAII owner, which is the allocator's
        // reference-count decrement (rhi.api_core.cppm's ownership note - release, not necessarily
        // destruction: a content-deduplicated image dies when its LAST reference goes).
        delete this;
    }

    core::owned_image_view::~owned_image_view() noexcept {
        if (this->native_view != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroyImageView(this->device, this->native_view, nullptr);
            this->native_view = VK_NULL_HANDLE;
        }
    }

    void core::owned_image_view::release() noexcept {
        delete this;
    }

    core::owned_sampler::~owned_sampler() noexcept {
        if (this->native_sampler_handle != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroySampler(this->device, this->native_sampler_handle, nullptr);
            this->native_sampler_handle = VK_NULL_HANDLE;
        }
    }

    void core::owned_sampler::release() noexcept {
        delete this;
    }

    void core::owned_shader::release() noexcept {
        delete this;
    }

    void core::owned_pipeline::release() noexcept {
        delete this;
    }

    rhi::shader* core::create_shader(rhi::shader_desc const& declared_desc) {
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
        }
        (void)stage; // the module itself is stage-less; the stage rides the pipeline's stage info

        std::optional<vk_shader_module> module = ::deren::vulkan::make_shader_module(
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.code.data()), desc.code.size()),
            this->logical_device);
        if (!module.has_value()) {
            deren::utility::log("rhi: create_shader {} refused: vkCreateShaderModule failed", what);
            return nullptr;
        }
        auto* const answer = new owned_shader();
        answer->owned.emplace(std::move(module.value()));
        answer->native_handle = answer->owned->get();
        return answer;
    }

    rhi::pipeline* core::create_pipeline(rhi::pipeline_desc const& declared_desc) {
        rhi::pipeline_desc const desc = sanitize_pipeline_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed pipeline";

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S ------------------------------------------
        // Color formats one to one; the `depth` ROLE resolves to the device's own depth attachment
        // format; `unknown` as the depth format means NO depth attachment (make_pipeline's
        // VK_FORMAT_UNDEFINED spelling).
        std::vector<VkFormat> color_formats(desc.color_formats.size());
        for (std::size_t index = 0; index < desc.color_formats.size(); ++index) {
            color_formats[index] = native_image_format(desc.color_formats[index], this->depth_attachment_format);
            if (color_formats[index] == VK_FORMAT_UNDEFINED) {
                deren::utility::log("rhi: create_pipeline {} refused: color attachment {} is image_format::unknown", what, index);
                return nullptr;
            }
        }
        VkFormat const depth_format = desc.depth_format == rhi::image_format::unknown
                                          ? VK_FORMAT_UNDEFINED
                                          : native_image_format(desc.depth_format, this->depth_attachment_format);

        // The blend modes are the FOUR RECIPES the survey found; empty means every target is
        // overwritten (opaque), which is the default make_pipeline itself spells.
        std::vector<VkPipelineColorBlendAttachmentState> blends;
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
        VkCompareOp const compare = desc.compare == rhi::depth_compare::equal ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        if (desc.first_stage == rhi::shader_stage::mesh) {
            // the full overload takes the compare op; the simple one the mesh spelling routes through
            // carries the same default. Routed here because the mesh stage REPLACES the vertex stage.
        }
        std::expected<vk_pipeline, std::string_view> pipeline = make_pipeline(
            this->logical_device,
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
        auto* const answer = new owned_pipeline();
        answer->owned.emplace(std::move(pipeline.value()));
        answer->native_handle = answer->owned->get_pipeline();
        return answer;
    }

    rhi::query* core::create_query(rhi::query_desc const& /*desc*/) {
        return nullptr;
    }

    rhi::command_list* core::begin_commands() {
        // THE RECORDING VIEW OF THE FRAME IN FLIGHT, OR nullptr WHEN THERE IS NONE. The verb is not
        // literal yet: the frame's own vkBeginCommandBuffer/vkEndCommandBuffer still belong to the
        // engine, which also decides the present recipe - this call starts nothing, it hands out the
        // list the frame's recording is already going into. The `nullptr` when no frame is in flight
        // is the contract's way of saying "there is nothing to record into".
        if (!this->frame_in_flight) {
            return nullptr;
        }
        return &this->commands_view;
    }

    // ---- THE OWNED COMMAND BUFFER (abi 15) ----------------------------------------------------------

    rhi::command_buffer* core::create_command_buffer(rhi::command_buffer_desc const& declared_desc) {
        // THE ABI GUARD, the same rule every factory descriptor follows: only the prefix the caller
        // declares is read, so an older caller's structure keeps this build's default.
        rhi::command_buffer_kind kind = rhi::command_buffer_kind::primary;
        if (covered_by(declared_desc.struct_size, offsetof(rhi::command_buffer_desc, kind), sizeof(rhi::command_buffer_desc::kind))) {
            kind = declared_desc.kind;
        }
        // THE KIND IS THE ONE REFUSAL THIS FACTORY HAS: a value outside the two roles the contract
        // names is a caller bug, and the answer is nullptr plus the reason on the record (§4.2: no
        // throwing path).
        if (kind != rhi::command_buffer_kind::primary && kind != rhi::command_buffer_kind::secondary) {
            deren::utility::log("rhi: create_command_buffer refused: unknown command_buffer_kind {}", static_cast<std::uint32_t>(kind));
            return nullptr;
        }

        auto* const answer = new owned_command_buffer();
        answer->owner = this;
        // ONE POOL PER BUFFER, created and owned by the wrapper (handles/handles.cppm): the handle is a
        // self-contained device resource and the recording thread that owns it never shares a pool.
        answer->buffer = ::deren::vulkan::make_command_buffer(this->logical_device, this->graphics_queue_family_index,
                                                              kind == rhi::command_buffer_kind::secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY
                                                                                                          : VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        if (*answer->buffer == VK_NULL_HANDLE) {
            delete answer;
            deren::utility::log("rhi: create_command_buffer refused: the backend could not allocate the buffer");
            return nullptr;
        }
        // ITS RECORDING VIEW IS THIS BUFFER'S OWN (abi 15): the list the contract hands back knows which
        // buffer it records into, which is what keeps one `command_list` type serving both the frame's
        // list and every owned buffer.
        answer->list.owner = this;
        answer->list.target = *answer->buffer;
        {
            // THE PROVENANCE REGISTRY, the same shape `contract_images` uses: `execute()` and the
            // escape's native-handle answer must tell a buffer this backend made from a pointer a
            // caller holds, and the cast that reads it is only defined once that is known.
            std::lock_guard const lock(this->contract_command_buffers_mutex);
            this->contract_command_buffers.insert(answer);
        }
        deren::utility::log("rhi: create_command_buffer {} -> native {:#x} (its own command pool)",
                            kind == rhi::command_buffer_kind::secondary ? "secondary" : "primary",
                            reinterpret_cast<std::uintptr_t>(*answer->buffer));
        return answer;
    }

    void core::owned_command_buffer::release() noexcept {
        {
            std::lock_guard const lock(this->owner->contract_command_buffers_mutex);
            this->owner->contract_command_buffers.erase(this);
        }
        // `delete this`: the destructor releases the `vk_command_buffer`, whose own release DESTROYS THE
        // COMMAND POOL it created - the contract's one reference is the whole lifetime rule here (there
        // is no allocator registry behind this resource, unlike a buffer or an image).
        delete this;
    }

    rhi::error core::owned_command_buffer::begin_recording(rhi::command_buffer_begin_info const& declared_info) {
        // THE ABI GUARD for the begin info: only the prefix the caller declares is read.
        std::uint32_t const declared = declared_info.struct_size;
        rhi::command_buffer_flags usage = rhi::no_command_buffer_flags;
        rhi::structure_header const* next = nullptr;
        if (covered_by(declared, offsetof(rhi::command_buffer_begin_info, usage), sizeof(rhi::command_buffer_begin_info::usage))) {
            usage = declared_info.usage;
        }
        if (covered_by(declared, offsetof(rhi::command_buffer_begin_info, next), sizeof(rhi::command_buffer_begin_info::next))) {
            next = declared_info.next;
        }

        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // this handle holds no reference any more
        }

        VkCommandBufferInheritanceRenderingInfo rendering = {};
        VkCommandBufferInheritanceInfo inheritance = {};
        bool const inherits = next != nullptr;
        if (inherits) {
            // THE CHAIN IS READ, NEVER DROPPED (the rule the heap requests live by). Today's one
            // structure is the Vulkan attachment inheritance a `render_pass_continue` secondary MUST
            // declare; anything else is refused BY NAME below.
            if (next->s_type != rhi::structure_type::vulkan_command_buffer_inheritance ||
                rhi::validate_structure(*next, rhi::structure_type::vulkan_command_buffer_inheritance, sizeof(rhi::vulkan_command_buffer_inheritance_info)) != rhi::error::ok) {
                if (!this->refused_chain_logged) {
                    this->refused_chain_logged = true;
                    deren::utility::log("rhi: begin_recording refused a parameter chain of type {:#x} - this backend serves only "
                                        "vulkan_command_buffer_inheritance (a chain is never dropped silently)",
                                        static_cast<std::uint32_t>(next->s_type));
                }
                return rhi::error::unsupported;
            }
            auto const& declared_inheritance = *reinterpret_cast<rhi::vulkan_command_buffer_inheritance_info const*>(next);
            rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            rendering.pNext = nullptr;
            rendering.flags = 0;
            rendering.viewMask = declared_inheritance.view_mask;
            rendering.colorAttachmentCount = declared_inheritance.color_format_count;
            rendering.pColorAttachmentFormats = reinterpret_cast<VkFormat const*>(declared_inheritance.color_formats);
            rendering.depthAttachmentFormat = static_cast<VkFormat>(declared_inheritance.depth_format);
            rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
            rendering.rasterizationSamples = static_cast<VkSampleCountFlagBits>(declared_inheritance.samples);
            inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inheritance.pNext = &rendering;
        } else if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            // A CONTINUATION WITH NO INHERITANCE IS NOT EXPRESSIBLE: Vulkan requires the secondary to
            // declare the attachments it continues (dynamic rendering), and the backend cannot invent
            // them. Refused by name rather than begun into an unvalidated state.
            return rhi::error::unsupported;
        }

        VkCommandBufferUsageFlags flags = 0;
        if (rhi::has_flag(usage, rhi::command_buffer_usage::one_time_submit)) {
            flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        }
        if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        }
        if (rhi::has_flag(usage, rhi::command_buffer_usage::simultaneous_use)) {
            flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        }

        VkCommandBufferBeginInfo const begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = flags,
            .pInheritanceInfo = inherits ? &inheritance : nullptr,
        };
        return generic_error(vkBeginCommandBuffer(*this->buffer, &begin));
    }

    rhi::error core::owned_command_buffer::end_recording() noexcept {
        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        return generic_error(vkEndCommandBuffer(*this->buffer));
    }

    rhi::command_list* core::owned_command_buffer::recording() noexcept {
        if (*this->buffer == VK_NULL_HANDLE) {
            return nullptr; // no reference held: there is no view to lend
        }
        return &this->list;
    }

    rhi::error core::owned_command_buffer::execute(rhi::command_buffer& secondary) {
        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        {
            // PROVENANCE FIRST (the registry, so no pointer is cast before it is known to be ours):
            // a command buffer this backend did not hand out is a caller bug, refused by name.
            std::lock_guard const lock(this->owner->contract_command_buffers_mutex);
            if (!this->owner->contract_command_buffers.contains(&secondary)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& other = static_cast<core::owned_command_buffer const&>(secondary);
        VkCommandBuffer const native = *other.buffer;
        if (native == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // the secondary was released
        }
        // ONE SECONDARY PER CALL: vkCmdExecuteCommands takes an array, and the contract's unit is the
        // single secondary every API in this family executes. This records into THIS buffer, which the
        // caller promised is recording (the state itself is not queryable through Vulkan).
        vkCmdExecuteCommands(*this->buffer, 1, &native);
        return rhi::error::ok;
    }

    rhi::image* core::frame_image() noexcept {
        // THE IMAGE THE LAST ACQUIRE RETURNED, and it stays answerable AFTER the frame is submitted -
        // which is one step longer than the contract's minimum ("valid until that frame is submitted")
        // and is exactly what the read-back needs: its COPY is recorded inside the frame, but its READ
        // runs after the frame has landed and then asks this image for the extent and the format of the
        // frame it captured (runtime.readback.cppm). MEASURED: with the frame-scoped answer the read
        // stage got nullptr and every scenario reported "no screenshot produced". What the pointer may
        // not outlive is the CONTEXT; the frame-scoped window remains the caller-side rule for the
        // RECORDING verbs, and those still refuse when no frame is in flight.
        //
        // BEFORE THE FIRST ACQUIRE THERE IS NO "LAST" IMAGE, and that is a separate state from the index:
        // `acquired_image_index` starts at 0, which names a real swapchain image, so the index alone would
        // answer with image 0 before any frame ever began. `frame_acquired` is what tells the two apart.
        if (!this->frame_acquired || static_cast<std::size_t>(this->acquired_image_index) >= this->swap_chain_images.size()) {
            return nullptr;
        }
        return &this->frame_image_view;
    }

    rhi::buffer* core::frame_readback_buffer() noexcept {
        // THE BACKEND'S HOST-VISIBLE READ-BACK SLOT, at least one texel per frame pixel at four bytes
        // each (the formats the contract can unpack are all 8-bit RGBA/BGRA). Grown on demand: this is
        // the slot the read-back copy is recorded into, and its size follows the swapchain extent.
        VkExtent2D const extent = this->swap_chain_extent;
        VkDeviceSize const needed = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * 4u;
        if (needed == 0) {
            return nullptr; // no swapchain extent yet: there is nothing a copy could write into
        }
        bool const fits = this->readback_slot_buffer.valid() && this->readback_slot_size >= needed && this->readback_slot_mapped != nullptr;
        if (!fits) {
            // REPLACING AN ALLOCATION THE GPU MAY STILL BE READING FROM IS THE ONE THING THAT NEEDS A
            // WAIT, and it happens when the needed size GROWS - i.e. after a swapchain recreation,
            // whose extent is this slot's whole size. The wait is on the replacement only; the
            // steady-state call is the three comparisons above and adds nothing to the frame.
            if (this->readback_slot_buffer.valid()) {
                vkDeviceWaitIdle(this->logical_device);
            }
            this->readback_slot_buffer.reset();
            this->readback_slot_handle = VK_NULL_HANDLE;
            this->readback_slot_mapped = nullptr;
            this->readback_slot_size = 0;
            this->readback_slot_buffer = this->vma.create_buffer(nullptr, needed, buffer_type::readback_coherent);
            if (!this->readback_slot_buffer.valid()) {
                deren::utility::log("rhi: read-back slot creation failed ({} bytes)", needed);
                return nullptr;
            }
            auto const* const detail = this->vma.get_buffer_detail(this->readback_slot_buffer.handle());
            if (detail == nullptr) {
                this->readback_slot_buffer.reset();
                return nullptr;
            }
            this->readback_slot_handle = detail->buffer;
            this->readback_slot_mapped = detail->allocation_info.pMappedData;
            this->readback_slot_size = needed;
            deren::utility::log("rhi: read-back slot {}x{} -> {} B (host-visible, coherent, TRANSFER_DST)", extent.width, extent.height, needed);
        }
        return &this->readback_slot_view;
    }

    VkCommandBuffer core::frame_command_buffer() const noexcept {
        // The frame slot in progress: submit() and wait_frame_slot() use `current_frame` for it, and
        // to_next_frame() advances it.
        std::size_t const slot = static_cast<std::size_t>(this->current_frame);
        if (slot >= this->frame_command_buffers.size()) {
            return VK_NULL_HANDLE;
        }
        return this->frame_command_buffers[slot].get();
    }

    // ---- the contract's frame-domain views ---------------------------------------------------------
    // Every one of them answers for the core that owns it (`owner` is set in the constructor, and the
    // view object is a member of that core, so it can never outlive it).

    rhi::image_extent core::frame_image_slot::extent() const noexcept {
        VkExtent2D const extent = this->owner->swap_chain_extent;
        return rhi::image_extent{.width = extent.width, .height = extent.height, .depth = 1};
    }

    rhi::image_view* core::frame_image_slot::make_view(rhi::image_view_desc const& declared_desc) {
        // A BORROWED IMAGE HANDS OUT OWNED VIEWS (③-D/E item B, abi 16). What is borrowed is the IMAGE -
        // the swapchain image, owned by the presentation path - but a VIEW is a NEW backend object created
        // right here, so `release()` on it is real and the contract's ownership rule applies to it exactly
        // as it does to `create_image()->make_view()`: one reference, dropped once, inside the backend.
        //
        // THE LIFETIME RULE THE CALLER MUST KEEP (and the reason this used to refuse): a view made over a
        // swapchain image dies with that image, so the caller has to release every view it created from a
        // borrowed frame image BEFORE the swapchain is recreated (`swapchain::recreate()`, which the engine
        // drives from `runtime::on_swapchain_recreated`). A view that outlives its image is a stale handle
        // the validation layer reports; this backend does not track the caller's views, so it cannot paper
        // over it. In this renderer the window is exactly one frame's worth: the engine makes the view while
        // a frame is being recorded and releases it after the frame's submission, and a rebuild happens
        // between frames.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        VkImage const image = this->handle();
        if (image == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: no image has been acquired yet");
            return nullptr;
        }
        // THE SWAPCHAIN IMAGE'S SHAPE IS ONE LAYER AND ONE MIP, and that is a fact rather than a limit of
        // this implementation: the presentation image is what it is. A range outside it is refused, not
        // clamped - the same rule `owned_image::make_view` follows, for the same reason (a clamped view
        // samples something the caller did not ask for and says nothing).
        uint32_t const layers = desc.layer_count == 0 ? 1u : desc.layer_count;
        uint32_t const mips = desc.mip_count == 0 ? 1u : desc.mip_count;
        if (desc.base_layer != 0u || layers != 1u || desc.base_mip != 0u || mips != 1u) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: it is a single-layer, single-mip image "
                                "(asked for layer {} + {}, mip {} + {})",
                                desc.base_layer, layers, desc.base_mip, mips);
            return nullptr;
        }
        VkImageViewCreateInfo const view_info = make_image_view_info(image,
                                                                     this->owner->swap_chain_image_format,
                                                                     VK_IMAGE_VIEW_TYPE_2D,
                                                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                                                     mips,
                                                                     layers);
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->owner->logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: vkCreateImageView failed");
            return nullptr;
        }
        auto* const answer = new owned_image_view();
        answer->native_view = handle;
        answer->device = this->owner->logical_device;
        return answer;
    }

    void core::frame_image_slot::release() noexcept {
        // A BORROWED VIEW CARRIES NO REFERENCE, SO THERE IS NOTHING TO RELEASE - AND SAYING SO IS THE WHOLE
        // ANSWER. This object is a member of the core; the swapchain image behind it belongs to the core and
        // dies with the core's own teardown. A caller that reached `release()` here has wrapped something it
        // never held a reference to in `object_manager` - a bug in the caller rather than a leak - so the
        // answer is a ONE-TIME NAMED LOG, not a panic and not a silent no-op (rhi.api_core.cppm's ownership
        // note; the one-time shape is the one `frame_commands::use` already uses for an image this backend
        // did not hand out).
        if (!this->borrowed_release_logged) {
            this->borrowed_release_logged = true;
            deren::utility::log("rhi: release() on the frame image view - it is BORROWED from the backend and carries no reference, so nothing was released; "
                                "only factory-created handles hold a reference the caller may drop");
        }
    }

    rhi::image_format core::frame_image_slot::format() const noexcept {
        // The four 8-bit shapes a screenshot can be unpacked from; anything else is `unknown`, and the
        // engine's read stage turns that into the one-time "unsupported swapchain format" it already
        // had (runtime.readback.cppm).
        switch (this->owner->swap_chain_image_format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return rhi::image_format::bgra8_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return rhi::image_format::bgra8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return rhi::image_format::rgba8_srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return rhi::image_format::rgba8_unorm;
        default:
            return rhi::image_format::unknown;
        }
    }

    VkImage core::frame_image_slot::handle() const noexcept {
        core const& self = *this->owner;
        std::size_t const index = static_cast<std::size_t>(self.acquired_image_index);
        return index < self.swap_chain_images.size() ? self.swap_chain_images[index] : VK_NULL_HANDLE;
    }

    std::uint64_t core::frame_readback_slot::size() const noexcept {
        return static_cast<std::uint64_t>(this->owner->readback_slot_size);
    }

    void core::frame_readback_slot::release() noexcept {
        // BORROWED, same answer as the frame image view's: the read-back slot is the core's own allocation
        // (grown on demand by frame_readback_buffer(), released by the core's teardown), so a caller's
        // `release()` here drops a reference the caller never held and gets one named log.
        if (!this->borrowed_release_logged) {
            this->borrowed_release_logged = true;
            deren::utility::log("rhi: release() on the frame read-back buffer view - it is BORROWED from the backend and carries no reference, so nothing was released; "
                                "only factory-created handles hold a reference the caller may drop");
        }
    }

    std::span<std::byte> core::frame_readback_slot::mapped() noexcept {
        // EMPTY when the buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing.
        if (this->owner->readback_slot_mapped == nullptr || this->owner->readback_slot_size == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->owner->readback_slot_mapped),
                                    static_cast<std::size_t>(this->owner->readback_slot_size));
    }

    VkBuffer core::frame_readback_slot::handle() const noexcept {
        return this->owner->readback_slot_handle;
    }

    rhi::error core::frame_commands::use(rhi::image const& resource, rhi::image_use const from, rhi::image_use const to) noexcept {
        core* const self = this->owner;
        if (self == nullptr || !self->frame_in_flight) {
            // NO FRAME IS BEING RECORDED, so there is nowhere to record the transition - and SAYING SO is
            // the point of the return value: a silently dropped barrier is a frame whose image is in a
            // state nobody declared, which is the failure class task-148 measured.
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE FRAME-SCOPED VERBS BELONG TO THE FRAME'S LIST (abi 15): `use()` declares the FRAME
            // image's role pair, and a list that records into a buffer the caller created has no frame
            // image in it. `not_ready` is the contract's own word for it ("or the list is not this
            // frame's" - the window `use()` and `begin_commands()` share).
            return rhi::error::not_ready;
        }
        if (static_cast<void const*>(&resource) != static_cast<void const*>(&self->frame_image_view)) {
            // The factories still answer nullptr, so `frame_image()` is the ONLY image that can reach this
            // call today. A foreign object is the caller's bug: the answer says so, and the reason is
            // logged once.
            if (!this->unexpected_use_logged) {
                this->unexpected_use_logged = true;
                deren::utility::log("rhi: use() was handed an image this backend did not hand out; nothing was recorded");
            }
            return rhi::error::invalid_argument;
        }
        VkImageMemoryBarrier2 barrier = barrier_for(from, to);
        if (barrier.srcStageMask == 0 && barrier.srcAccessMask == 0 && barrier.dstStageMask == 0 && barrier.dstAccessMask == 0) {
            if (!this->unexpected_use_logged) {
                this->unexpected_use_logged = true;
                deren::utility::log("rhi: use({}, {}) is not a transition this backend can spell; nothing was recorded",
                                    static_cast<std::uint32_t>(from),
                                    static_cast<std::uint32_t>(to));
            }
            return rhi::error::unsupported;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        barrier.image = self->frame_image_view.handle();

        // ---- GATE A5, THE RUN-TIME HALF: BOTH SIDES OF THE BARRIER, ONCE PER PAIR ------------------
        // The static_asserts above prove the equality at compile time; this dump is what puts the two
        // field lists in the run's own log (spec §6.1 A5 asks for both sides, not for a verdict).
        std::uint32_t const pair_bit = (from == rhi::image_use::color_attachment) ? 1u : 2u;
        if ((this->shadow_gate_dumped & pair_bit) == 0) {
            this->shadow_gate_dumped |= pair_bit;
            VkImageMemoryBarrier2 const& recipe = (from == rhi::image_use::color_attachment) ? deren::vulkan::color_attachment_to_transfer_transition
                                                                                             : deren::vulkan::transfer_to_color_attachment_transition;
            deren::utility::log("rhi shadow gate (A5) use({}, {}) image={:#x}", static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to),
                                reinterpret_cast<std::uintptr_t>(barrier.image));
            deren::utility::log("rhi shadow gate (A5)   derived: {}", barrier_text(barrier));
            deren::utility::log("rhi shadow gate (A5)   recipe : {}", barrier_text(recipe));
        }

        VkDependencyInfo const dependency = make_image_dependency_info(1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::copy_image_to_buffer(rhi::buffer& destination,
                                                          rhi::image const& source,
                                                          rhi::image_copy_region const& region) noexcept {
        core* const self = this->owner;
        // WHAT CAN SAY NO, in the contract's own vocabulary (batch-2 spec §6.1): `unsupported` = this
        // surface cannot be a copy source at all; `invalid_argument` = the region does not fit, or the
        // destination is too small; `not_ready` = there is no frame to record into.
        if (self == nullptr || !self->frame_in_flight) {
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE FRAME-SCOPED VERBS BELONG TO THE FRAME'S LIST (abi 15): the copy's source is the
            // FRAME image and its destination the frame's read-back slot, so a list that records into
            // some other buffer has neither (the same `not_ready` window `use()` answers in).
            return rhi::error::not_ready;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!self->swapchain_transfer_src_supported) {
            // The swapchain images lack TRANSFER_SRC, so copying out of one would violate
            // VUID-vkCmdCopyImageToBuffer-srcImage-00186. The ENGINE owns the one-time log and the F12
            // shutdown that go with this answer (runtime.frames.cppm); the judgement itself is the
            // backend's, because the swapchain is.
            return rhi::error::unsupported;
        }
        if (static_cast<void const*>(&destination) != static_cast<void const*>(&self->readback_slot_view) ||
            static_cast<void const*>(&source) != static_cast<void const*>(&self->frame_image_view)) {
            return rhi::error::invalid_argument;
        }
        VkExtent2D const extent = self->swap_chain_extent;
        if (region.extent.width == 0 || region.extent.height == 0 || region.extent.depth != 1) {
            return rhi::error::invalid_argument;
        }
        if (region.mip_level != 0 || region.base_array_layer != 0 || region.array_layer_count != 1 || region.offset_z != 0) {
            return rhi::error::invalid_argument; // the frame image has one mip, one layer, and is 2D
        }
        if (static_cast<std::uint64_t>(region.offset_x) + region.extent.width > extent.width ||
            static_cast<std::uint64_t>(region.offset_y) + region.extent.height > extent.height) {
            return rhi::error::invalid_argument;
        }
        std::uint64_t const needed = static_cast<std::uint64_t>(region.extent.width) * region.extent.height * 4u;
        if (static_cast<std::uint64_t>(destination.size()) < needed) {
            return rhi::error::invalid_argument;
        }

        // The copy the screenshot path recorded by hand, field for field (batch-2 spec §3.3 item 2):
        // tightly packed, whole subresource, at the source's CURRENT layout (GENERAL everywhere here).
        VkBufferImageCopy copy = {};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mip_level, region.base_array_layer, region.array_layer_count};
        copy.imageOffset = {static_cast<int32_t>(region.offset_x), static_cast<int32_t>(region.offset_y), static_cast<int32_t>(region.offset_z)};
        copy.imageExtent = {region.extent.width, region.extent.height, region.extent.depth};
        vkCmdCopyImageToBuffer(command_buffer, self->frame_image_view.handle(), VK_IMAGE_LAYOUT_GENERAL, self->readback_slot_handle, 1, &copy);
        return rhi::error::ok;
    }

    // ---- tier-2: device_address --------------------------------------------------------------------

    std::uint64_t core::buffer_address_view::buffer_address(rhi::buffer const& resource, std::uint64_t const offset) const noexcept {
        // THE PRECONDITION IS THE CONTRACT'S OWN RULE, STATED RATHER THAN GUESSED AT: a caller may only
        // ask about a buffer THIS backend handed out (rhi.api_core.cppm says the same about every handle
        // it gives back). `frame_commands::use()` can afford a stricter check because its views are
        // singletons it compares addresses against; factory-created buffers are heap objects, so
        // recognising "one of mine" without RTTI would mean keeping a registry of them - and this path
        // does not need one, because the answer it computes (`vkGetBufferDeviceAddress`) is only defined
        // for a buffer this device created in the first place. So: a `static_cast` on a documented
        // precondition, not a hopeful assumption.
        auto const* const owned = static_cast<owned_buffer const*>(&resource);
        if (this->owner == nullptr || !owned->addressable) {
            // ASKED FOR NOTHING, GET NOTHING: a buffer created without `buffer_flag::device_address` has
            // no address to report (Vulkan only allows the call for a buffer created with that usage),
            // and 0 is the contract's spelling of "none" - the module's own guard, not a failure.
            return 0ull;
        }
        VkBufferDeviceAddressInfo const query = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = owned->native};
        std::uint64_t const address = static_cast<std::uint64_t>(vkGetBufferDeviceAddress(this->owner->logical_device, &query));
        if (address == 0u) {
            deren::utility::log("rhi: buffer_address() answered 0 for native {:#x} (usage flags {:#x}) - the address is only defined when the device enabled "
                                "bufferDeviceAddress, which is what the flag promised",
                                reinterpret_cast<std::uintptr_t>(owned->native), owned->declared_flags);
        }
        return address + offset;
    }

    // ---- tier-2: vulkan_escape ---------------------------------------------------------------------

    void* core::frame_escape::native_instance() const noexcept {
        return reinterpret_cast<void*>(this->owner->instance);
    }

    void* core::frame_escape::native_physical_device() const noexcept {
        return reinterpret_cast<void*>(this->owner->physical_device);
    }

    void* core::frame_escape::native_device() const noexcept {
        return reinterpret_cast<void*>(this->owner->logical_device);
    }

    void* core::frame_escape::native_queue() const noexcept {
        return reinterpret_cast<void*>(this->owner->graphics_queue_handle);
    }

    void* core::frame_escape::native_command_buffer(rhi::command_list& commands) const noexcept {
        // THE SAME WINDOW `begin_commands()` ANSWERS IN for the FRAME's list: outside the frame there is
        // no command buffer to name, and answering with "the slot that would be next" would be a lie an
        // escaping pass could record into.
        core& self = *this->owner;
        if (static_cast<void const*>(&commands) == static_cast<void const*>(&self.commands_view)) {
            if (!self.frame_in_flight) {
                return nullptr;
            }
            return reinterpret_cast<void*>(self.frame_command_buffer());
        }
        // ... AND AN OWNED BUFFER'S LIST (abi 15): resolved through the SAME provenance registry
        // `execute()` uses, so no caller pointer is cast before it is known to be one of ours. Outside a
        // frame is legitimate here - a buffer the caller owns exists on its own (the read-back's
        // one-shot buffer is created and recorded after the frame has landed), and it names itself
        // rather than "the slot that would be next".
        std::lock_guard const lock(self.contract_command_buffers_mutex);
        for (deren::promise::rhi::command_buffer const* const candidate : self.contract_command_buffers) {
            auto const* const owned = static_cast<owned_command_buffer const*>(candidate);
            if (static_cast<void const*>(&owned->list) == static_cast<void const*>(&commands)) {
                return reinterpret_cast<void*>(*owned->buffer);
            }
        }
        return nullptr; // not a list this backend handed out
    }

    std::span<char const* const> core::frame_escape::enabled_instance_extensions() const noexcept {
        return std::span<char const* const>(this->owner->instance_extension_names);
    }

    std::span<char const* const> core::frame_escape::enabled_device_extensions() const noexcept {
        return std::span<char const* const>(this->owner->device_extension_names);
    }

    void* core::frame_escape::native_buffer(rhi::buffer const& resource) const noexcept {
        // BORROWED, AND THE SAME PRECONDITION `buffer_address()` STATES: a caller may only ask about a
        // buffer THIS backend handed out (the contract's own rule for every handle it gives back).
        // Without RTTI there is no honest way to check that a heap-allocated factory object is one of
        // ours, so the cast rests on the documented precondition rather than on a hopeful assumption -
        // and a RELEASED buffer must not reach here at all (touching a released handle is a caller bug
        // the contract names; this would turn it into a crash instead of a wrong answer).
        auto const* const owned = static_cast<owned_buffer const*>(&resource);
        return reinterpret_cast<void*>(owned->native);
    }

    void* core::frame_escape::native_image(rhi::image const& resource) const noexcept {
        // THE SAME BORROWED-HANDLE RULE native_buffer states (abi 7's image face): the precondition is
        // the caller's - only images this backend's create_image handed out reach here, and a released
        // one must not. The VkImage was copied into the owned object at creation, so no detail lookup
        // runs per call.
        auto const* const owned = static_cast<owned_image const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    void* core::frame_escape::native_image_view(rhi::image_view const& resource) const noexcept {
        auto const* const owned = static_cast<owned_image_view const*>(&resource);
        return reinterpret_cast<void*>(owned->native_view);
    }

    void* core::frame_escape::native_sampler(rhi::sampler const& resource) const noexcept {
        auto const* const owned = static_cast<owned_sampler const*>(&resource);
        return reinterpret_cast<void*>(owned->native_sampler_handle);
    }

    std::uint32_t core::frame_escape::native_image_format(rhi::image const& resource) const noexcept {
        auto const* const owned = static_cast<owned_image const*>(&resource);
        return static_cast<std::uint32_t>(owned->resolved_format);
    }

    void* core::frame_escape::native_pipeline(rhi::pipeline const& resource) const noexcept {
        auto const* const owned = static_cast<owned_pipeline const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    void* core::frame_escape::native_shader_module(rhi::shader const& resource) const noexcept {
        auto const* const owned = static_cast<owned_shader const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    VkResult core::acquire_next_image(uint32_t& image_index) {
        // The frame slot in progress: submit() and wait_frame_slot() use `current_frame` for it, and
        // to_next_frame() advances it, so the acquire semaphore below is the one that slot's submission
        // will wait on (VUID-vkAcquireNextImageKHR-semaphore-01779 needs the slot to be idle - the
        // caller's wait_frame_slot() is what guarantees that).
        uint32_t const slot = static_cast<uint32_t>(this->current_frame);
        VkResult const result = vkAcquireNextImageKHR(this->logical_device, this->swap_chain, UINT64_MAX,
                                                      this->image_available_semaphores[slot], VK_NULL_HANDLE, &image_index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            // THE FRAME IN FLIGHT IS NOW THIS IMAGE, and that is all `frame_in_flight` means: from here
            // until submit() hands the frame over, `begin_commands()` and `frame_image()` answer for it.
            // Both spellings go through this one primitive - the contract's frame_begin() and the
            // runtime's own pacing path - so neither can be "in flight" without the other knowing.
            this->acquired_image_index = image_index;
            this->frame_in_flight = true;
            // and this is what makes `frame_image()` answer at all: before the first successful acquire
            // there is no last-acquired image, only a default index that happens to name one.
            this->frame_acquired = true;
        }
        return result;
    }

    rhi::submit_info core::frame_begin() {
        uint32_t image_index = 0;
        VkResult const acquired = this->acquire_next_image(image_index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            // tier-1's frame_begin() has no error channel (plan §3.3): a zeroed answer is the contract's
            // way of saying "no frame started", and the caller's own pacing path owns the diagnosis.
            return rhi::submit_info{};
        }
        // The contract's "index in the frame-in-flight ring" IS this core's frame slot.
        return rhi::submit_info{.frame_index = static_cast<std::uint32_t>(this->current_frame), .image_index = image_index};
    }

    rhi::error core::present() {
        // The contract's present() has no argument: it presents the image its own acquire took, and
        // the PRESENT call site's translation answers (out_of_date => the caller rebuilds; a failed
        // presentation never reports silence - the abi 14 note on the verb).
        //
        // NO ACQUIRED FRAME IS A NAMED REFUSAL, not a queue call: `acquired_image_index` starts at 0,
        // which NAMES a real image, so forwarding it before any acquire would reach
        // vkQueuePresentKHR waiting on a present-ready semaphore nothing has signalled (and present
        // image 0, which this frame never wrote). `frame_acquired` is what tells "the last acquire"
        // from "there has never been one" - the same flag `frame_image()` answers on - so a caller
        // that presents before opening a frame is told `not_ready` instead of being run into the WSI.
        if (!this->frame_acquired) {
            return rhi::error::not_ready;
        }
        return this->present(this->acquired_image_index);
    }

    void core::wait_idle() {
        // The const facade call (vulkan/core/core.cpp) is the implementation; the contract's virtual is
        // not const, so this overload exists to forward into it.
        core const& self = *this;
        self.wait_idle();
    }

    rhi::swapchain* core::frame_swapchain() noexcept {
        return &this->swapchain_view_;
    }

    rhi::error core::submit(rhi::command_list& commands) {
        // The list must be THIS frame's recording view - the same two-way check every frame verb
        // makes (a foreign list is a caller bug, refused by name, never guessed at).
        if (&commands != static_cast<rhi::command_list*>(&this->commands_view)) {
            return rhi::error::invalid_argument;
        }
        if (!this->frame_in_flight) {
            return rhi::error::not_ready;
        }
        // The image and the present-ready semaphore are this backend's own acquire state - never
        // caller data (the contract's note). The raw failure becomes the generic translation; the
        // device-level codes travel as themselves.
        return generic_error(this->submit_frame(this->frame_command_buffer(), this->acquired_image_index));
    }

    // ---- the frame face (abi 13) ----------------------------------------------------------------
    //
    // THE ACCESSORS hand out this core's two borrowed views - the same object every call, the
    // constructor set their `owner`. The views' methods compose the machinery this class already
    // owned: the walker is today's frame prologue fused (wait -> latch timings -> acquire -> report),
    // the profiler reads the timing snapshot the walker's latch produced.

    rhi::frame_walker* core::walk_frames() noexcept {
        return &this->frames_view;
    }

    rhi::gpu_profiler* core::profiler() noexcept {
        return &this->profiler_view;
    }

    std::uint32_t core::frame_walker_view::slot_count() const noexcept {
        return static_cast<std::uint32_t>(this->owner->MAX_FRAMES_IN_FLIGHT);
    }

    std::uint32_t core::frame_walker_view::position() const noexcept {
        // THE one authority: this is the backend's own cursor, read without advancing.
        return static_cast<std::uint32_t>(this->owner->current_frame);
    }

    rhi::frame_open_info core::frame_walker_view::wait_and_acquire() {
        // THE FIVE STEPS in the order the contract's note spells out - each position is load-bearing.
        uint32_t const slot = static_cast<uint32_t>(this->owner->current_frame);

        // 1 + 2. the cursor (read, not advanced) and its timeline. A never-submitted slot (value 0)
        // has nothing to wait for - VK_SUCCESS by definition, the same early return as today.
        VkResult const waited = this->owner->wait_frame_slot_result(slot);
        if (waited != VK_SUCCESS) {
            // The wait's VkResult, REPORTED instead of dropped (today `wait_frame_slot` voids it):
            // the code says what class of failure, the message names the step, native_code carries
            // the raw VkResult for whoever needs the exact number.
            return {.frame = {}, .result = deren::vulkan::failed(deren::vulkan::generic_error(waited), waited, "waiting the frame slot's timeline failed")};
        }

        // 3. LATCH this slot's previous frame's GPU timings, here between the wait and the acquire -
        // exactly where the engine's collect_gpu_timings sits today, so the wait already guarantees
        // the timestamps are readable AND a frame the acquire kills on OUT_OF_DATE is still
        // collected. Behaviour unchanged; the position is the backend's now.
        this->owner->profiler_view.latch(slot);

        // 4. the acquire - the same primitive `frame_begin()` goes through (one mechanism, two
        // spellings), translated AT THIS CALL SITE: SUBOPTIMAL is ok here (the acquired image
        // renders), OUT_OF_DATE is the rebuild signal the caller's classifier acts on.
        uint32_t image_index = 0;
        VkResult const acquired = this->owner->acquire_next_image(image_index);
        rhi::error const opened = deren::vulkan::acquire_error(acquired);
        if (opened != rhi::error::ok) {
            // The zero-frame rule: `frame` is zeroed and unusable, and the caller now knows WHY
            // (out_of_date => rebuild and skip; device_lost => fatal; ...) - the information
            // `frame_begin()`'s zeroed submit_info could not carry.
            return {.frame = {}, .result = deren::vulkan::failed(opened, acquired, "acquiring the next swapchain image failed")};
        }

        // 5. the frame is open: this slot, the acquired image, and a zeroed diagnostic saying ok.
        return {.frame = {.frame_index = slot, .image_index = image_index}, .result = {}};
    }

    void core::frame_walker_view::walk_to_next() noexcept {
        // The present-side close: advance ONE slot. It does not promise "the frame is over" - only
        // that the ring moved (the engine calls this after its present recipe).
        this->owner->to_next_frame();
    }

    void core::gpu_profiler_view::latch(uint32_t const slot) noexcept {
        // Non-const on purpose: the latch writes the read-once guard (a slot's timings are fetched
        // at most once per submission), the same bookkeeping read_gpu_timings keeps.
        core& self = *this->owner;
        this->latch_failed = false;
        if (!self.gpu_timing_supported) {
            this->latched_mark_count = 0; // the device cannot timestamp: the report is honestly empty
            return;
        }
        uint64_t const submitted = self.frame_done_values[slot];
        if (submitted == 0 || submitted <= self.gpu_timing_read_value[slot]) {
            return; // nothing new since the previous latch: the snapshot stays as it was
        }
        self.gpu_timing_read_value[slot] = submitted; // this submission is now accounted for (once)
        uint32_t const marks = self.gpu_timing_marks[slot];
        this->latched_mark_count = marks;
        if (marks == 0) {
            return; // a submission with no marks has nothing to fetch
        }
        std::array<uint64_t, gpu_timing_mark_capacity> ticks = {};
        VkResult const status = vkGetQueryPoolResults(self.logical_device, self.timestamp_query_pool, slot * gpu_timing_mark_capacity, marks,
                                                      sizeof(uint64_t) * marks, ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (status == VK_NOT_READY) {
            // The timeline wait already said the submission completed, so this is the same defensive
            // reading read_gpu_timings has: report no measurement rather than waiting or stalling.
            this->latched_mark_count = 0;
            return;
        }
        if (status != VK_SUCCESS) {
            // A real read-back failure: the mark count is host-known, the ticks are not - the
            // getters answer `device_lost` for it instead of reporting silence.
            this->latch_failed = true;
            return;
        }
        this->latched_ticks = ticks;
        for (uint32_t mark = 0; mark < marks; ++mark) {
            // The names are by-value string_views of the engine's static text (the `window_title`
            // rule), copied out at latch time - the array below may be overwritten by the next
            // recording, the snapshot must not move with it.
            this->latched_names[mark] = self.gpu_timing_names[slot][mark];
        }
    }

    std::uint32_t core::gpu_profiler_view::stage_count() const noexcept {
        if (!this->owner->gpu_timing_supported) {
            return 0; // "no timing => always 0": the caller reads this as unsupported via get_stage_info
        }
        return this->latched_mark_count;
    }

    rhi::error core::gpu_profiler_view::get_stage_info(uint32_t const index, std::string_view* const name,
                                                       uint64_t* const duration_ns) const noexcept {
        if (!this->owner->gpu_timing_supported) {
            return rhi::error::unsupported; // the device or configuration has no timing to report
        }
        if (index >= this->latched_mark_count) {
            return rhi::error::invalid_argument;
        }
        if (this->latch_failed) {
            return rhi::error::device_lost; // the timestamp read-back failed; the count is all we know
        }
        if (index + 1 >= this->latched_mark_count) {
            return rhi::error::not_ready; // the last mark closes the frame and opens no stage
        }
        if (name != nullptr) {
            *name = this->latched_names[index];
        }
        if (duration_ns != nullptr) {
            // The same masking the milliseconds reader does: the counter is a modulo-2^valid_bits
            // ring, so the delta is taken inside the width (a wrap inside the span comes out right),
            // and the device's timestampPeriod converts ticks to nanoseconds (1 ns/tick with 64
            // valid bits on the device this backend ships on - the integer is lossless there).
            uint64_t const mask = this->owner->timestamp_valid_bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << this->owner->timestamp_valid_bits) - 1);
            uint64_t const delta = (this->latched_ticks[index + 1] - this->latched_ticks[index]) & mask;
            *duration_ns = static_cast<uint64_t>(static_cast<double>(delta) * static_cast<double>(this->owner->timestamp_period_ns));
        }
        return rhi::error::ok;
    }

} // namespace deren::vulkan
