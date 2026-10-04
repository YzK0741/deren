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
#include <format>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.core;

import deren.promise.rhi;
import deren.vulkan.constant_init;

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
        // STILL NOT REPORTED, and each for a stated reason: `host_image_copy` (no `image` can be
        // produced until the image batch), `descriptor_heap` (the operations exist on this backend, but
        // the ability's contract face - `push_data` against a `command_list` - is not wired yet),
        // `mesh_shader` and `ray_tracing` (the passes record these through the escape today).
        return rhi::to_bits(rhi::extension_kind::vulkan_escape) | rhi::to_bits(rhi::extension_kind::device_address);
    }

    rhi::extension* core::query_extension(rhi::extension_kind const kind) noexcept {
        // The invariant is two-way and is checked in two places: at startup on the REAL backend
        // (core.constructor.cppm, gate G2) and on the probe backend in tests/test_dynamic_link.cpp
        // (G1). Every kind this backend announces answers with an object whose kind() is the kind that
        // was asked for, and every kind it does not announce answers nullptr.
        if (kind == rhi::extension_kind::vulkan_escape) {
            return &this->escape_view;
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

        VkSamplerCreateInfo const info = make_texture_sampler_info(mode, desc.max_lod);
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

    rhi::shader* core::create_shader(rhi::shader_desc const& /*desc*/) {
        return nullptr;
    }

    rhi::pipeline* core::create_pipeline(rhi::pipeline_desc const& /*desc*/) {
        return nullptr;
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

    rhi::image_view* core::frame_image_slot::make_view(rhi::image_view_desc const& /*desc*/) {
        // A BORROWED VIEW DOES NOT MAKE VIEWS (abi 7's make_view on the frame image): the swapchain
        // image's views belong to the backend's own presentation path. The one-time log is the same
        // borrowed-view rule the release override below lives by.
        if (!this->borrowed_make_view_logged) {
            this->borrowed_make_view_logged = true;
            deren::utility::log("rhi: make_view on the borrowed frame image answers nullptr - the swapchain image has no contract views");
        }
        return nullptr;
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
        VkCommandBuffer const command_buffer = self->frame_command_buffer();
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
        VkCommandBuffer const command_buffer = self->frame_command_buffer();
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

    rhi::extension_kind core::buffer_address_view::kind() const noexcept {
        return rhi::extension_kind::device_address;
    }

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

    rhi::extension_kind core::frame_escape::kind() const noexcept {
        return rhi::extension_kind::vulkan_escape;
    }

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
        // THE SAME WINDOW `begin_commands()` ANSWERS IN: outside the frame there is no command buffer to
        // name, and answering with "the slot that would be next" would be a lie an escaping pass could
        // record into. The key is the contract's own list: the ONE list this backend hands out belongs to
        // the frame in flight, so "which command buffer is this list" is an identity comparison.
        core const& self = *this->owner;
        if (!self.frame_in_flight) {
            return nullptr;
        }
        if (static_cast<void const*>(&commands) != static_cast<void const*>(&self.commands_view)) {
            return nullptr;
        }
        return reinterpret_cast<void*>(self.frame_command_buffer());
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

    void core::present() {
        // The contract's present() has no argument: it presents the image its own frame_begin()
        // acquired. The const overload next to it is the one that talks to vkQueuePresentKHR.
        static_cast<void>(this->present(this->acquired_image_index));
    }

    void core::wait_idle() {
        // The const facade call (vulkan/core/core.cpp) is the implementation; the contract's virtual is
        // not const, so this overload exists to forward into it.
        core const& self = *this;
        self.wait_idle();
    }

} // namespace deren::vulkan
