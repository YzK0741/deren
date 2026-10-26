// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:api_core
//
// tier-1 of the promise contract: the backend's context and its factories (RHI
// plan v4, §1.11, §3.3, §4.1).
//
// `api_core` is the ONE object that crosses the boundary as a C++ type, and it is
// reached through a single C function, `deren_make_api_core` - declared in
// source/promise/rhi/backend_entry.hpp, the boundary's one C entry point, because an
// entry point is the ABI surface rather than a virtual base class (m03159). Everything
// below follows from the cross-boundary rules in §4.2:
//
//   - operations are virtual, the destructor is `virtual ... noexcept`, and the
//     common object root carries a static concrete implementation name (ABI30). No owning
//     data or non-inline definition crosses this interface: the vtable is the boundary,
//     and destruction has to reach the backend's own `operator delete`;
//   - an OWNED handle carries ONE REFERENCE, dropped by `release()`, once, inside the
//     backend; `unique_object<T>` is the owner spelling of that call (RAII, move-only).
//     RELEASE IS NOT NECESSARILY DESTRUCTION: the backend may serve the same resource to
//     several callers (a content-keyed registry + a reference count sit behind these
//     handles), so the object dies with its LAST reference and a caller must not read the
//     call as "the memory is free now" - nor touch the handle afterwards;
//   - a BORROWED view (`frame_image()`, `frame_readback_buffer()`, `begin_commands()`)
//     holds no reference at all and must never be managed;
//   - the backend hands the ROOT out as the contract's own `shared_object<api_core>` (abi 44),
//     whose control block's last copy calls the root's `release()` - the drop runs inside the
//     backend, so no call site and no host ever names a deleter: the entry point returns the handle,
//     and nothing in the engine's ownership path frees DLL memory with the executable's allocator;
//   - `abilities()` is the tier-2 bit set (source/promise/rhi/rhi.extension.cppm) and
//     `query_extension()` is the only way to reach an ability, so adding one does
//     not grow this vtable;
//   - parameters are PODs, `std::span`, or opaque handles. No `deren::utility::vessel::string`, no
//     container, no exception, no `dynamic_cast` (`-fno-rtti`);
//   - the portable code touches nothing else, and this interface must never absorb
//     engine concepts: no scene, no pass, no camera, no toon (§4.1 item 2).
//
// The `*_desc` structs that have landed are defined here, and their widths are part of the ABI - the
// `struct_size` guards let callers omit trailing descriptor fields safely.
// ============================================================================
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected> // std::expected: `image::get_content()` answers the bytes OR the named reason
#include <optional>
#include <span>        // std::span: buffer::mapped() hands the caller the bytes of a host-visible buffer
#include <string_view> // std::string_view: gpu_profiler's stage names ride the boundary as views
#include <vector>      // deren::utility::vessel::vector: the CONTENT `image::get_content()` returns

export module deren.promise.rhi:api_core;
export import deren.utility.shared;

import :extension;

/**
 * @file source/promise/rhi/rhi.api_core.cppm
 * @brief tier-1 of the promise contract: the backend's context and its factories.
 * @ingroup promise
 *
 * `api_core` is the one object that crosses the boundary as a C++ type, and it is reached through a
 * single C function, `deren_make_api_core()` in `source/promise/rhi/backend_entry.hpp` (§1.11: the way to
 * obtain the context stays C++, the rest of the boundary is C). It is also the one interface the
 * plan lets grow "big" - it is the backend's whole context and factory set - under the one
 * restriction that it never learns an engine concept: no scene, no pass, no camera (§4.1 item 2).
 *
 * Applications and passes reach optional features through `abilities()` + `query_extension()`
 * (tier-2, `source/promise/rhi/rhi.extension.cppm`) rather than through new virtuals here, which is what
 * keeps this vtable stable as the backend gains extensions.
 */

export namespace deren::promise::rhi {

    // ================================ ABI 53 CONTRACT CLARIFICATIONS ================================
    // These are normative for every backend and complement the per-method comments. They clarify behavior
    // that was previously only in the reference backend or in docs, without changing the ABI 53 vtable.
    //
    // OWNERSHIP. Every handle derives from `object`; `release()` drops the caller's ONE reference and the
    // object dies with the last reference. A borrowed view carries no caller-owned reference: callers MUST
    // NOT invoke its underlying release(), and the backend MAY terminate on that ownership violation. A
    // borrow() wrapper's destruction/reset drops no reference. A backend MAY keep the underlying resource
    // alive for shared/content-keyed reuse, but MUST NOT expose a dead handle. Directly bound pipelines and
    // borrowed buffer/image operands remain caller-owned through pending CPU recording and GPU completion;
    // only verbs with explicit retention guarantees transfer that lifetime responsibility to the backend.
    // The backend core MUST outlive every handle, group, command buffer and pending submission.
    //
    //
    // RECORDING ERROR RULE. A handle-taking verb answers `error`; a value-only verb answers `void`. A void
    // verb latches its failure into the recording, and `end_recording()`/`apply_operations()` reports it and
    // makes the recording non-executable. Callers MUST serialize calls on one recorder; the contract does not
    // grant concurrent calls on the same command buffer.
    //
    // COMMAND DATA. `copy_buffer` MUST reject size==0, offsets past either buffer, or a range past either
    // buffer with `invalid_argument`; a backend MUST NOT pass an overrun to the native API. `bind_vertex_buffer`
    // and `bind_index_buffer` MUST reject an offset >= the buffer size. `clear_color_image`'s four components
    // are RGBA, and an all-zero subresource range means every mip/layer of the image. `mark_gpu_timing`'s
    // label text is borrowed static text that MUST stay valid for the process lifetime; the backend does not
    // copy it. `begin_rendering`/`end_rendering` MUST be paired and MUST NOT nest; an open scope at
    // `end_recording` fails that recording and nothing is published. `dispatch`/`draw_mesh_tasks` require an
    // open recording with the matching pipeline; a device that cannot run mesh work is reported through
    // `device_capabilities`, and a void mesh verb latches `invalid_argument` rather than drawing nothing
    // silently.
    //
    // API CORE. `wait_idle()` waits for all submitted work; it has no error channel (device loss surfaces on
    // later calls) and is legal outside a recording (inside one it does not end the recording).
    // `main_context()` is optional: null means the backend exposes no native/plugin interop context; a
    // non-null pointer is borrowed until api_core::release() and must be validated by tag before casting.
    //
    // FRAME VIEWS. `begin_commands()` returns the frame's primary (null outside recording) borrowed until the
    // frame is submitted; `frame_image()` is valid from acquire through submit until the next acquire;
    // `frame_readback_buffer()` is borrowed for the same window. Using any of them outside its window is a
    // contract violation the backend may refuse.
    //
    // ASSIGN TEXTURE GROUP. On success the returned shared handle owns one reference and `*result` (when
    // non-null) is `ok`; on failure the returned handle is empty and `*result` is the named error. Passing a
    // null `result` is allowed; empty (all-null) slots are legal and preserve group capacity. A backend that
    // does not support groups returns an empty handle with `unsupported`.
    //
    // RADIX SORT. `requirements(count)` reports scratch bytes, scratch alignment and scratch buffer flags; a
    // nonzero count with scratch_bytes==0 means the backend cannot serve that count. `record` validates the
    // operands and returns `invalid_argument` for a null/foreign buffer, an offset/size out of range or a
    // scratch size below the requirement. Internal native bindings may be overwritten, but the recorder's
    // logical pipeline/group/push state is preserved and re-established for later RHI consumers. All operand
    // buffers are borrowed through GPU completion.
    //
    // ==============================================================================================

    /// WHAT A BUFFER IS FOR, as the contract names it: the memory intent, not a Vulkan memory property.
    ///
    /// The vocabulary is the renderer's own census of buffer uses (twelve creation sites today, counted
    /// in source/backends/vulkan/runtime, readback, ray_tracing and acceleration_structure), and every value says what
    /// the CALLER does with the buffer - "the host writes it every frame", "only the GPU touches it",
    /// "it holds an acceleration structure". The backend maps each one to memory properties, tiling and
    /// an upload strategy, which is exactly the knowledge the contract is not allowed to have.
    ///
    /// VALUES ARE APPENDED, NEVER MOVED OR REUSED: a value is part of the ABI the moment it ships (the
    /// same rule the `error` codes and the ability bits follow).
    enum class buffer_usage : std::uint32_t {
        vertex = 0,                         ///< vertex data; GPU-only, uploaded once at creation
        index = 1,                          ///< index data; GPU-only, uploaded once at creation
        uniform_gpu_only = 2,               ///< uniforms the host updates rarely (or never)
        uniform_coherent = 3,               ///< per-frame uniforms: host-visible and coherent
        uniform_cached = 4,                 ///< host-visible/coherent; cached memory is preferred for host reads
        storage_coherent = 5,               ///< a storage buffer the host also writes (e.g. a table the GPU reads)
        readback_coherent = 6,              ///< the destination of a GPU -> CPU copy; created WITHOUT contents
        acceleration_structure_storage = 7, ///< memory an acceleration structure lives in; allocate-only
        acceleration_structure_scratch = 8, ///< an acceleration structure build's scratch space; allocate-only
        storage_gpu_only = 9,               ///< a storage buffer only the GPU touches; allocate-only
        /// Host-visible STAGING: the SOURCE of a copy recorded into a device buffer inside a frame. Its twin is
        /// `readback_coherent` (the destination of a GPU -> CPU copy), and it exists because NO other intent carries
        /// TRANSFER_SRC by default - so without it a staging buffer needs an explicit transfer_source flag, and the
        /// recording face's `copy_buffer` would be refused for its source. See the viewer's frame-ordered upload.
        staging_source = 10,
    };

    /// Capabilities a buffer needs BEYOND its memory intent.
    ///
    /// These are the three things the renderer's creation sites ask for that are not about memory at
    /// all: the buffer's DEVICE ADDRESS has to be obtainable (bufferDeviceAddress), an acceleration
    /// structure build has to be able to READ it, and the opacity-micromap extension has to be able to
    /// use it as storage. Naming them here rather than passing raw Vulkan usage flags is the whole
    /// point of the contract: a caller says WHAT IT NEEDS, and the backend knows which bits that is -
    /// and a backend that cannot serve one says so by refusing the descriptor instead of silently
    /// allocating something that will be read the wrong way.
    enum class buffer_flag : std::uint32_t {
        device_address = 1u << 0,               ///< `vkGetBufferDeviceAddress`-style addresses must work
        acceleration_structure_input = 1u << 1, ///< readable by an acceleration structure build
        micromap_storage = 1u << 2,             ///< usable as `VK_EXT_opacity_micromap` storage
        storage = 1u << 3,                      ///< the GPU reads/writes it as a storage buffer (SSBO)
        indirect = 1u << 4,                     ///< it carries indirect dispatch/draw commands
        shader_binding_table = 1u << 5,         ///< it carries a ray-tracing shader binding table
        micromap_build_input = 1u << 6,         ///< readable by an opacity-micromap build
        transfer_destination = 1u << 7,         ///< receives GPU fill/update/copy commands
        transfer_source = 1u << 8,              ///< permits diagnostic GPU buffer readback copies
    };

    /// A set of `buffer_flag` bits, spelled the way the ability set is (`to_bits` + `has_flag`).
    using buffer_flags = std::uint32_t;

    /// The empty flag set, spelled where a caller passes "none".
    inline constexpr buffer_flags no_buffer_flags = 0u;

    /// The same value as a bitmask, for a caller composing a set.
    [[nodiscard]] constexpr auto to_bits(buffer_flag flag) noexcept -> buffer_flags {
        return static_cast<buffer_flags>(flag);
    }

    /// Whether @p flags has @p flag set.
    [[nodiscard]] constexpr auto has_flag(buffer_flags flags, buffer_flag flag) noexcept -> bool {
        return (flags & to_bits(flag)) != no_buffer_flags;
    }

    /// How a buffer is created: its memory intent, the capabilities it needs, its size, and - when the
    /// caller already has the bytes - its initial contents.
    ///
    /// THE INITIAL BYTES ARE PART OF THE DESCRIPTOR, not a second upload call, because that is where the
    /// renderer's deduplication lives: a backend that keys its resources on CONTENT (this one does, by
    /// XXH3-128 - see `release()`'s note) can only recognise identical content if it is handed the
    /// content at creation. `initial_bytes` empty means ALLOCATE ONLY: the buffer is created with no
    /// contents (a GPU-only target, a read-back slot, an acceleration structure's storage), and a
    /// backend that deduplicates must never match one of those against another.
    ///
    /// `struct_size` is the same ABI guard `create_info` carries and follows the same append-only rule:
    /// a field whose whole extent is not inside the bytes the caller declares keeps this build's
    /// default, so an older program talking to a newer backend is detected instead of mis-read.
    struct buffer_desc {
        std::uint32_t struct_size = sizeof(buffer_desc);     ///< size of this structure as the CALLER compiled it
        std::uint64_t size = 0;                              ///< bytes
        buffer_usage usage = buffer_usage::storage_gpu_only; ///< what the caller does with it (see above)
        buffer_flags flags = no_buffer_flags;                ///< capabilities beyond the memory intent
        std::span<std::byte const> initial_bytes = {};       ///< empty = allocate only (undefined contents); otherwise exactly size bytes
    };

    /// Allocation-only is valid for every memory intent; nonempty initialization covers exactly the buffer.
    /// Mechanism/capability refusal is separate from this shared shape validation.
    [[nodiscard]] constexpr error validate_buffer_initialization(std::uint64_t size, std::uint64_t initial_size) noexcept {
        return size != 0 && (initial_size == 0 || initial_size == size) ? error::ok : error::invalid_argument;
    }

    /// A 3D extent in texels. POD, passed by value.
    struct image_extent {
        std::uint32_t width = 0;
        std::uint32_t height = 1;
        std::uint32_t depth = 1;
    };

    /// Which role a resource is used in. The vocabulary is the plan's §6.4/§6.5 list, which is exactly
    /// the set of roles DX12 resource states and this renderer's barriers both name; Vulkan's catch-all
    /// layout `GENERAL` is deliberately NOT a member (it is a Vulkan spelling, not a use).
    enum class image_use : std::uint32_t {
        color_attachment = 0, ///< written as a render target (and read back as one)
        transfer_source = 1,  ///< read by a copy out of the image        /// APPENDED for the recording face: the state an image is in
        /// before anything has declared a use for it, which is what a barrier's `from` says at the top
        /// of a pass. A new VALUE, never a renumbering - the rule at `abi_version`.
        undefined = 2,
        /// APPENDED with the record series (abi 20, plan §9.4): the state a pass hands a WRITTEN
        /// target to the samplers in - the post chain's bloom levels read the level above them, and
        /// the barrier that says so is a (color_attachment, shader_read) pair. The backend derives the
        /// sampled-read access bit for it (the shipped recipe's own bit).
        shader_read = 3,
        /// ---- APPENDED BY THE RECORDING FACE'S VOCABULARY BATCH (the lead's ruling on the transition census)
        ///
        /// THE SIX BELOW COME FROM ONE MEASUREMENT, not from a wish list: every transition recipe the engine
        /// still records itself (`source/backends/vulkan/constant_init/constant_init.cppm`, 20 of them) was read as a role
        /// PAIR, and these are the roles that were missing. Adding VALUES is not an abi change (the rule at
        /// `abi_version`), so this batch carries no bump - and the pilot's `shader_read` above stays as it was.
        ///
        /// THE MEASUREMENT'S SHAPE, which is why ROLES and not layouts decide: this renderer keeps every image
        /// in `GENERAL` (`docs/unified_image_layouts.md`), so the recipes' `oldLayout`/`newLayout` are almost
        /// all GENERAL -> GENERAL and carry NO information. THE SEMANTICS LIVE IN THE STAGE/ACCESS MASKS:
        /// `hdr_sampling_transition` is COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_WRITE ->
        /// FRAGMENT_SHADER|COMPUTE_SHADER/SHADER_SAMPLED_READ, while `general_to_sampling_transition` is
        /// COMPUTE_SHADER/SHADER_WRITE -> that same destination. A later reader who "fixes" a layout will
        /// change the wrong thing; THE PAIR, AND THEREFORE ITS MASKS, IS THE FACT.
        shader_write = 4, ///< written by a shader: `undefined_to_general_transition`,
                          ///< `sampling_to_general_transition`, and the source of `general_to_sampling` /
                          ///< `general_to_transfer_src`
        /// Written AND read by the same shader stage in one barrier: `compute_storage_transition`
        /// (COMPUTE_SHADER/SHADER_WRITE -> COMPUTE_SHADER/SHADER_READ|SHADER_WRITE, a read-modify-write UAV
        /// barrier). IT IS A ROLE, NOT A LAYOUT, and it exists because neither half can stand for it:
        /// `shader_write` alone would DROP the read half (a race on the one site that needs it), and
        /// widening every `shader_write` would over-synchronise every other site. A second combination
        /// role is added only when a measurement produces one - never pre-stacked.
        shader_read_write = 5,
        transfer_destination = 6, ///< written by a copy in: `undefined_to_transfer_dst_transition`,
                                  ///< `sampling_to_transfer_dst_transition`
        depth_attachment = 7,     ///< written as the DEPTH attachment: `depth_attachment_transition`,
                                  ///< `sampling_to_depth_attachment_transition`
        depth_read = 8,           ///< sampled as depth: `undefined_to_depth_sampling_transition` (a shadow
                                  ///< map read back after being written)
        present = 9,              ///< handed to the presentation engine: `present_transition`,
                                  ///< `undefined_to_present_transition`
        /// APPENDED for plan X4: READ BY THE HOST - the state a written target is handed to the
        /// IMPLEMENTATION's own copy-out (`image::get_content()`) or to a read-back. IT IS THE ROLE THE CENSUS
        /// FOUND MISSING ("no HOST-access masks ... stay in the escape bucket, runtime.probes.cppm /
        /// ray_tracing.cpp"), and it is why a probe that records its own read-back needed a raw
        /// `vkCmdPipelineBarrier2`: the pair (color_attachment, host_read) is declared in the backend's table now.
        host_read = 10,
    };

    /// One subresource of one image, tightly packed: what a copy reads.
    struct image_copy_region {
        image_extent extent = {}; ///< texels to copy; width, height and depth all non-zero
        std::uint32_t mip_level = 0;
        std::uint32_t base_array_layer = 0;
        std::uint32_t array_layer_count = 1;
        std::uint32_t offset_x = 0;
        std::uint32_t offset_y = 0;
        std::uint32_t offset_z = 0;
    };

    /// THE CONTENT OF AN IMAGE, IN HOST MEMORY: what `image::get_content()` answers with.
    ///
    /// THE LAYOUT IS PART OF THE CONTRACT, because the caller has to be able to unpack it without asking
    /// the backend anything else: ROW-MAJOR, TIGHTLY PACKED (`row_pitch == extent.width * bytes_per_pixel`,
    /// which is why there is no pitch field), TOP-LEFT origin, channel order decided by the image's
    /// `image_format` (so `bgra8_*` really is B,G,R,A in memory), and layers/mips laid out in the order the
    /// region asked for them.
    ///
    /// THE INVARIANT: `bytes.size() == extent.width * extent.height * extent.depth * bytes_per_pixel`.
    /// A backend that cannot answer exactly that must answer an ERROR instead of a partial buffer - the
    /// caller's unpacking loop trusts this and nothing else.
    struct image_content {
        image_extent extent = {};                             ///< the texels `bytes` covers
        std::uint32_t bytes_per_pixel = 0;                    ///< `bytes_per_pixel(image_format)`, repeated here so the caller does
                                                              ///< not have to look up the image's format to stride the rows
        deren::utility::vessel::vector<std::byte> bytes = {}; ///< the content, row-major and tightly packed
    };

    // ================================================================================================
    // THE RECORDING FACE'S DESCRIPTORS. They are plain contract
    // PODs: ADDING THEM CHANGES NO INTERFACE, so no abi bump follows from this block - the verbs that
    // take them are the interface change and land with the backend that
    // translates them.
    //
    // WHAT THE VOCABULARY DELIBERATELY DOES NOT CARRY, each one a MEASURED verdict rather
    // than an omission: no HOST-ACCESS masks (the two host-visible barrier sites were raw through the
    // escape when this block was written, runtime.probes.cppm / ray_tracing.cpp - the `host_read` roles
    // carry them now), no QUEUE-FAMILY ownership-transfer field (there
    // is no cross-queue submission; the 46 VK_QUEUE_FAMILY_* tokens are IGNORED initialisations), no
    // `resolve`/`resolveMode` and no `viewMask` (no path resolves MSAA in a rendering scope and none
    // uses multi-view - and both can be APPENDED later because `rendering_info` carries `struct_size`),
    // and no `stage_hint` (add it only if a migrated site demonstrably needs a wider mask).
    // ================================================================================================

    /// A region of a 2D target: what `set_scissor` and a rendering scope's area are made of.
    ///
    /// IT DID NOT EXIST WHEN THE PLAN WAS WRITTEN (the plan cites it as "rect EXISTS"); the engine's
    /// sources name `VkRect2D` in nine places and the contract had no spelling for it at all, so the
    /// vocabulary is created here rather than the plan's premise being assumed. Signed offsets because
    /// a scissor may be placed at a negative origin (Vulkan clamps it), unsigned extent because a
    /// zero-width scissor is a legitimate "record nothing" and must be expressible.
    struct rect {
        std::int32_t offset_x = 0;
        std::int32_t offset_y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    /// One image's subresources: which mips and array layers a barrier covers.
    /// ALL-ZERO means "the whole image" (the plan's spelling), which is what almost every site wants.
    /// FROZEN (plan §6): a by-value contract POD, so a new field would be an abi change; the two
    /// growable descriptors below are the ones that carry `struct_size` instead.
    struct subresource_range {
        std::uint32_t base_mip = 0;
        std::uint32_t mip_count = 0;
        std::uint32_t base_layer = 0;
        std::uint32_t layer_count = 0;
    };

    /// One image's transition, in the vocabulary `command_buffer::use()` already speaks: THE PAIR is the
    /// information (the caller knows what it recorded, the backend knows what the pair costs), which is
    /// why this is not a single "new state". `resource` is a contract handle, not a native one.
    struct image_barrier {
        image* resource = nullptr;
        image_use from = image_use::undefined;
        image_use to = image_use::undefined;
        subresource_range range = {};
    };

    /// Which role a BUFFER is used in. APPENDED as a type of its own (the plan's §3), and SMALL ON
    /// PURPOSE: the engine has four buffer barrier sites, so the roles are the four the renderer
    /// actually names plus `undefined`. Not merged with `image_use`: a buffer is never a colour
    /// attachment and an image is never a vertex source, and one enum would invite both.
    enum class buffer_use : std::uint32_t {
        undefined = 0,
        shader_read,
        shader_write,
        transfer_source,
        transfer_destination,
        /// APPENDED (abi 21, and adding VALUES is explicitly not an abi change - see `abi_version`): the one
        /// reader whose stage is not a shader stage. The renderer's compute-skinning job writes vertices and
        /// the ACCELERATION-STRUCTURE BUILD reads them back, and no existing role named that reader - the
        /// measured site is `source/engine/pass/compute_skin.cpp`'s build-ordering barrier, whose raw masks were
        /// COMPUTE_SHADER/SHADER_WRITE -> ACCELERATION_STRUCTURE_BUILD/SHADER_READ.
        acceleration_structure_read,
        /// APPENDED WITH ITS MEASUREMENT (the ninth/tenth batch, adding a VALUE - no abi change): the
        /// ACCELERATION-STRUCTURE BUILD as a WRITER. `source/engine/ray_tracing/ray_tracing.cpp`'s build-ordering barrier
        /// waits for every bottom level one build wrote before the next build reads them, and its raw masks were
        /// ACCELERATION_STRUCTURE_BUILD/ACCELERATION_STRUCTURE_WRITE_KHR -> the same stage/ACCELERATION_STRUCTURE_READ.
        /// `acceleration_structure_read` named the reader alone; a pair needs both halves, and neither
        /// `shader_write` (a compute stage) nor `transfer_destination` says "a build wrote this".
        acceleration_structure_write,
        /// APPENDED WITH THEIR MEASUREMENT (the SBT batch, values again - no abi change): the OPACITY MICROMAP
        /// BUILD as a writer and the ACCELERATION-STRUCTURE BUILD as its reader. `source/engine/ray_tracing/ray_tracing.cpp`
        /// records the spec's own ordering barrier between them, whose raw masks are
        /// MICROMAP_BUILD_BIT_EXT/MICROMAP_WRITE_BIT_EXT -> ACCELERATION_STRUCTURE_BUILD_BIT_KHR/MICROMAP_READ_BIT_EXT.
        /// Neither half is a shader stage and neither is a transfer, so no earlier role named them - the same
        /// measurement rule that produced `acceleration_structure_read`/`acceleration_structure_write`.
        ///
        /// THE MICROMAP'S *OTHER* BARRIER IS THE BACKEND'S, and that is a deliberate split rather than an
        /// omission: its SOURCE is HOST_WRITE, and this enum carries no host-WRITE role (the host-visible
        /// barrier was raw through the escape when this block was written, and `build_micromap` records it
        /// inside the backend now - a host write is not a stage the recording series can order).
        micromap_write,
        micromap_read,
        indirect_read,
        shader_read_write,
        host_read,
    };

    /// One buffer's transition. `size == 0` means "the whole buffer" (the plan's spelling).
    struct buffer_barrier {
        buffer* resource = nullptr;
        buffer_use from = buffer_use::undefined;
        buffer_use to = buffer_use::undefined;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    /// A GLOBAL memory barrier's two roles: NO resource, because the ordering rule is about every write and
    /// every read of a stage pair rather than about one buffer or image.
    ///
    /// WHY A ROLE PAIR AND NOT A MASK: the same argument the image pair carries - the caller knows WHICH WAY
    /// the memory moved (it recorded it), the backend knows what that costs. The measured site is the
    /// compute-skinning job's build-ordering barrier (see `buffer_use::acceleration_structure_read`), and it
    /// is the ONLY global barrier in the engine: a second one should be added here only with its own
    /// measurement, the way the roles above were.
    struct memory_barrier {
        buffer_use from = buffer_use::undefined;
        buffer_use to = buffer_use::undefined;
    };

    /// WHICH SHADER STAGE a pair's shader side belongs to, for the sites whose producer or consumer is NOT the
    /// stage the shipped recipe names.
    ///
    /// ADDED WITH ITS MEASUREMENT (abi 21), which is what the barrier model's own note demands: the 20
    /// transition recipes name the stage their shader side runs at (the storage-image recipes say
    /// COMPUTE_SHADER), and the ray-traced shadow's visibility image is written by a traceRays LAUNCH rather
    /// than by a dispatch - a barrier whose masks do not cover the stage that actually ran leaves the writes
    /// unsynchronised, and the measured symptom was "half the model lost its sun". The pair is still the
    /// vocabulary (WHICH WAY the image moves); this says only WHICH STAGE did it, so it does not re-open the
    /// question the pair answers.
    ///
    /// `none` IS THE DEFAULT AND THE OVERWHELMING CASE: every site but one uses the recipe's own stages, and a
    /// hint of `none` is what an older caller's bytes decode to (the field is behind `struct_size`).
    enum class stage_hint : std::uint32_t {
        none = 0,
        vertex,
        fragment,
        compute,
        /// the mesh stage, which carries the TASK stage with it: a mesh pipeline that dispatches work runs both
        /// entry points, and no site separates them.
        mesh,
        ray_tracing,
    };

    /// THE FORWARD DECLARATION THE BARRIER BATCH NEEDS: `acceleration_structure` is tier-1 and declared further
    /// down (where its build/address surface lives), but a barrier names it by POINTER only, which is all an
    /// incomplete type must support - the same shape `:extension`'s `image`/`buffer` forward declarations have.
    struct acceleration_structure;

    /// THE ACCELERATION-STRUCTURE SIDE OF THE BARRIER MODEL (RT stage 1): what a structure is being used AS.
    ///
    /// ADDED WITH ITS MEASUREMENT SITE: the ray-traced shadow and MegaLights traces read the top-level structure
    /// their BUILD wrote, and the contract had no object-level use pair for that edge - the existing
    /// `buffer_use::acceleration_structure_*` roles name the build's reads and writes of the GEOMETRY BUFFERS
    /// (`source/engine/ray_tracing/ray_tracing.cpp`'s ordering barriers), which is a different resource. This
    /// vocabulary is the object-level one: the structure itself, between the build that wrote it and the shaders
    /// that traverse it.
    enum class acceleration_structure_use : std::uint32_t {
        undefined = 0,
        /// THE BUILD WROTE IT: the acceleration-structure build stage, with the extension's
        /// ACCELERATION_STRUCTURE_WRITE_KHR access.
        build_write,
        /// A SHADER TRAVERSES IT: an RT pipeline's raygen/miss/hit stages, or a ray query inside a
        /// compute/fragment shader. The access is ACCELERATION_STRUCTURE_READ_KHR; the STAGE comes from the
        /// batch's `stage_hint` (`none` = every shader stage, which is correct and conservative).
        shader_read,
    };

    /// One acceleration structure's transition in a batch. NO layout and NO subresource range: a structure has
    /// neither, which is why the batch carries it in its own span (below) and why the backend records it as a
    /// memory barrier rather than an image/buffer barrier.
    struct acceleration_structure_barrier {
        acceleration_structure* resource = nullptr;
        acceleration_structure_use from = acceleration_structure_use::undefined;
        acceleration_structure_use to = acceleration_structure_use::undefined;
    };

    /// A batch of transitions recorded together - what `vkCmdPipelineBarrier2` receives as the arrays.
    ///
    /// GROWS BY `struct_size` (plan §6): it is its FIRST member, and the backend reads a field only when
    /// the caller's declared size covers it (the `covered_by` rule `sanitize_sampler_desc` already uses),
    /// so a later addition is additive rather than an abi break. The spans are BORROWED for the call
    /// only, the same rule every other array in this contract follows.
    struct barrier_group {
        std::uint32_t struct_size = sizeof(barrier_group);
        std::span<image_barrier const> images = {};
        std::span<buffer_barrier const> buffers = {};
        /// WHICH STAGE the images' shader side is (see `stage_hint`); `none` = the recipes' own stages. IT
        /// APPLIES TO THE ACCELERATION-STRUCTURE BARRIERS' SHADER SIDE TOO (below), which is how an RT-pipeline
        /// traversal and a ray query in a compute shader are told apart.
        /// APPENDED, so a caller that declares the older prefix keeps `none` and the shipped behaviour.
        stage_hint stage = stage_hint::none;
        /// THE GLOBAL MEMORY BARRIER of this batch, recorded WITH the resource barriers above and in the same
        /// `vkCmdPipelineBarrier2` (the call takes the arrays at once). `has_memory` is the "present" flag,
        /// because an all-`undefined` pair is a legal refusal rather than "no barrier": a batch with neither
        /// images, buffers nor memory is a no-op the backend answers `ok` for.
        bool has_memory = false;
        memory_barrier memory = {};
        /// THE ACCELERATION-STRUCTURE BARRIERS OF THE SAME BATCH (RT stage 1), appended AFTER `memory` so an
        /// older caller's bytes decode to an empty span. The backend folds them into the same
        /// `vkCmdPipelineBarrier2` as a memory barrier: a structure has no layout, and the extension synchronizes
        /// it through ACCELERATION_STRUCTURE_READ/WRITE accesses.
        std::span<acceleration_structure_barrier const> acceleration_structures = {};
    };

    /// What a rendering scope does with an attachment it does not need to keep.
    enum class load_op : std::uint32_t {
        load = 0, ///< keep what is there (three sites CLEAR, the rest LOAD - the plan's census)
        clear,
        dont_care,
    };

    /// What a rendering scope leaves behind in an attachment.
    enum class store_op : std::uint32_t {
        store = 0,
        dont_care,
    };

    /// One colour attachment of a rendering scope. The view, not the image: the engine's attachments
    /// are views (a mip, a layer, a swizzle), and the plan's census found 1-2 per call.
    struct color_attachment {
        image_view* view = nullptr;
        load_op load = load_op::load;
        store_op store = store_op::store;
        /// read only when `load == load_op::clear`; the required RGBA order is the caller's.
        float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    /// The depth (and optional stencil) attachment of a rendering scope.
    struct depth_attachment {
        image_view* view = nullptr;
        load_op load = load_op::load;
        store_op store = store_op::store;
        bool read_only = false; ///< depth read vs depth+stencil attachment (a shadow map's two phases)
        bool has_stencil = false;
        float clear_depth = 1.0f;
        std::uint32_t clear_stencil = 0;
    };

    /// A rendering scope: what `vkCmdBeginRendering` describes, once per begin (29) / end (22).
    ///
    /// GROWS BY `struct_size` (plan §6), the same rule as `barrier_group`. `secondary_contents` says the
    /// scope will run secondary command buffers (six sites do), and `layer_count` is 6 sites' non-1 case.
    struct rendering_info {
        std::uint32_t struct_size = sizeof(rendering_info);
        rect area = {};
        std::uint32_t layer_count = 1;
        std::span<color_attachment const> colors = {};
        depth_attachment depth = {};
        bool has_depth = false;
        bool secondary_contents = false;
    };

    /// A viewport: what `set_viewport` states, in the vocabulary every graphics API shares.
    /// FROZEN (plan §6): a by-value contract POD.
    struct viewport {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float min_depth = 0.0f;
        float max_depth = 1.0f;
    };

    /// The width of one index buffer element. APPENDED as its own enum (the plan's §2): two values,
    /// because those are the two the renderer's index buffers are built from.
    enum class index_type : std::uint32_t {
        uint16 = 0,
        uint32 = 1,
    };

    /// Which triangle winding a pipeline culls. APPENDED as its own enum: the four spellings every
    /// graphics API names, because the renderer's 11 `set_cull_mode` sites use exactly these.
    enum class cull_mode : std::uint32_t {
        none = 0,
        front = 1,
        back = 2,
        front_and_back = 3,
    };

    /// One image-to-image copy: the two handles and each side's region (extent, offsets, mip and
    /// layers), relative to each image. FROZEN (plan §6). Both layouts are the images' CURRENT ones -
    /// this renderer keeps every image in GENERAL (docs/unified_image_layouts.md), so the descriptor
    /// carries no layout pair for the same reason `copy_image_to_buffer` does not.
    struct image_copy {
        image* source = nullptr;
        image* destination = nullptr;
        image_copy_region source_region = {};
        image_copy_region destination_region = {};
    };

    /// The format of an image, as far as the contract names it: the four 8-bit shapes a screen read-back
    /// can be unpacked from, plus `unknown` ("the backend cannot describe it"). Values are only ever
    /// APPENDED: the one decision the engine makes from a format is the BGRA/RGBA swizzle of a
    /// screenshot and whether it can be encoded at all.
    ///
    /// 7 appends the creation formats: the values whose BYTE LAYOUT
    /// matters to the caller (CPU-uploaded content that a sampler interprets) are named one by one, and
    /// `depth` is a ROLE, not a byte layout - "a depth attachment" is all a caller needs to say, because
    /// which concrete depth format a device serves is the backend's capability question, not the
    /// caller's. The judge is §17's: does the caller need to know the byte layout? Named formats say
    /// yes; `depth` says no.
    ///
    /// `r16_sfloat` is APPENDED (③-D/E A1.2, 2026-10-06) - one half-float channel, the format the
    /// ray-traced sun visibility image is created with: the pass writes a single visibility factor and
    /// the lighting stage multiplies the sun term by it, so one channel is what the byte layout IS
    /// (RGBA16F here would be three quarters padding). It is spelled with an EXPLICIT value AFTER the
    /// `depth` role so that every enumerator that already existed keeps the number it had - "only ever
    /// APPENDED" is about the numbers, and no caller compares format values by order. No `abi_version`
    /// bump follows for the same reason the appended `error` values did not need one: nothing that
    /// already crossed the boundary changed. A backend built against the older enum refuses the value
    /// (its image-format switch has no case) instead of misreading it - the documented answer
    /// to a descriptor a backend cannot honour.
    ///
    /// `r32_uint` is the NEXT APPEND (2026-10-17, same high-bit rule): one 32-bit unsigned channel, the
    /// format the visibility buffer is created with - the main view's geometry raster writes one packed
    /// (item, meshlet, triangle) id per pixel and a resolve pass reads it back. An unsigned channel is the
    /// BYTE LAYOUT the caller needs (the id is arithmetic, not a colour), which is the §17 test for naming
    /// a format instead of a role. It also gives the resolve pass a sampled read of the same image without a
    /// byte reinterpretation, and no `abi_version` bump follows for the same reason as `r16_sfloat`.
    ///
    /// AND THE CONVENTION THAT APPEND ESTABLISHED, for the next value someone adds here: an append to an
    /// enum whose range ENDS in a role/sentinel enumerator (this one's `depth = 0x7FFFFFFF`) takes an
    /// explicit value in the HIGH BITS at the END of the list, never a slot beside the named values. The
    /// marker makes "this one is new" self-evident, and - the part that actually matters - NO EXISTING
    /// ENUMERATOR'S NUMBER MOVES, which is the entire content of "values are only ever APPENDED". Where an
    /// enum's values are contiguous there is nothing to mark (see `error`: its six general values continue
    /// the run at 13-18); the rule is about the numbers, not about the syntax.
    ///
    /// EVERY VALUE IS WRITTEN OUT, which is the belt to that braces: with the sequence implicit, inserting
    /// a value in the middle would renumber everything after it and the "only appended" rule would be
    /// broken by a one-line edit that looks harmless. It also answers clang-tidy's
    /// `readability-enum-initial-value`, which the mixture of implicit and explicit values tripped.
    enum class image_format : std::uint32_t { unknown = 0,
                                              rgba8_unorm = 1,
                                              rgba8_srgb = 2,
                                              bgra8_unorm = 3,
                                              bgra8_srgb = 4,
                                              r16g16_sfloat = 5,        ///< two half-float channels (BRDF LUT)
                                              r16g16b16a16_sfloat = 6,  ///< four half-float channels (environment/irradiance cubes)
                                              r32g32b32_sfloat = 7,     ///< three 32-bit float channels
                                              depth = 0x7FFFFFFFu,      ///< ROLE: a depth attachment the backend shapes
                                              r16_sfloat = 0x80000000u, ///< APPENDED: one half-float channel (ray-traced visibility)
                                              r32_uint = 0x80000001u,   ///< APPENDED: one 32-bit unsigned channel (visibility buffer)
    };

    /// How many bytes ONE texel of @p format takes in host memory. `unknown` answers 0 (a format the
    /// contract cannot describe), and `depth` answers 0 as well: a depth ROLE has no host spelling here,
    /// which is the same answer `image::get_content()` gives that content a named error for.
    ///
    /// IT LIVES IN THE CONTRACT because it is a fact about the FORMAT, not about any API: a caller that
    /// receives `image_content` needs it to stride rows, and a backend that fills `bytes_per_pixel` must
    /// agree with every reader. The channel counts here are the enum's own, and a format added to the enum
    /// must give its size here in the same change - `image_content`'s invariant is
    /// `bytes.size() == width * height * depth * bytes_per_pixel`.
    [[nodiscard]] constexpr std::uint32_t bytes_per_pixel(image_format const format) noexcept {
        switch (format) {
        case image_format::rgba8_unorm:
        case image_format::rgba8_srgb:
        case image_format::bgra8_unorm:
        case image_format::bgra8_srgb:    // 4 x 8-bit
        case image_format::r16g16_sfloat: // 2 x half-float: the same four bytes, so one branch
            return 4u;
        case image_format::r16g16b16a16_sfloat:
            return 8u; // four half-floats
        case image_format::r32g32b32_sfloat:
            return 12u; // three 32-bit floats
        case image_format::r16_sfloat:
            return 2u; // one half-float
        case image_format::r32_uint:
            return 4u; // one 32-bit unsigned word
        case image_format::unknown:
        case image_format::depth:
            break;
        }
        return 0u;
    }

    // ---- OWNERSHIP: WHAT `release()` IS, AND WHAT IT IS NOT ---------------------------------------
    //
    // EVERY handle a factory returns (`buffer`, `image`, `sampler`, `shader`, `pipeline`)
    // is handed out WITH ONE REFERENCE, and the caller releases that reference by calling
    // `release()` EXACTLY ONCE. The call runs INSIDE the backend - that is the whole reason it is a
    // virtual instead of the caller reaching for `delete`: the allocator, the registry entry and the
    // reference count all stay on the side that created them, and the C ABI never has to hand out an
    // allocator.
    //
    // RELEASE IS NOT NECESSARILY DESTRUCTION, AND THAT IS WHY IT IS NOT CALLED `destroy()`. A backend
    // MAY SERVE THE SAME RESOURCE TO MORE THAN ONE CALLER - this one does it whenever two requests
    // carry identical content, by keeping a content-keyed (XXH3-128) registry and a reference count
    // behind these handles (source/backends/vulkan/core/vma/vma.cppm's ownership note) - so `release()` drops ONE
    // reference, and the device object dies only when the LAST reference goes. Two consequences a
    // caller has to live with:
    //
    //   - THE CALL MAY FREE NOTHING. A caller that needs the memory back must not infer it from this
    //     call; the backend's own bookkeeping decides (`vma_allocator::log_statistics()`, and the
    //     registry sweep in its `destroy()`).
    //   - THE HANDLE IS DEAD AFTERWARDS EVEN IF THE OBJECT LIVES. Touching the pointer again is a
    //     caller bug: the caller no longer holds a reference, so another `release()` would drop
    //     somebody else's.
    //
    // HOW A SECOND REFERENCE IS TAKEN: BY ASKING A FACTORY AGAIN. THE FACTORY IS THE RETAIN - a
    // `create_image()` whose content and parameters match a live object answers with THAT object and a
    // NEW reference, and the caller's single `release()` balances it. There is deliberately NO
    // `retain()` virtual: a raw "add a reference to this handle" call would let a caller manufacture
    // references for handles it never received, which is exactly the over-release path the backend
    // removed when it made its free/retain private and handed out injectable owners instead
    // (vma_allocator's ownership comment).
    //
    // A BORROWED VIEW IS NOT AN OWNED HANDLE, and this is the other half of the ownership model.
    // `frame_context::frame_image()`, `frame_context::frame_readback_buffer()` and `frame_context::begin_commands()`
    // answer with objects the BACKEND owns and lends for a window (a frame, or until the next
    // acquire). Those are VIEWS, not factory products: they carry no reference, and `release()` on one is a
    // lifecycle PRECONDITION violation that TERMINATES (the container-based ownership introduced in abi 41 is
    // what makes "who releases" unambiguous; a caller that reaches a view's release ignored it).
    //
    // A BORROWED IMAGE MAY STILL HAND OUT AN OWNED VIEW: `frame_image()->make_view(desc)` answers a new factory
    // product, released like any other. The rule is a lifetime one: a view made over a swapchain image dies
    // with that image, so the caller releases every such view before `swapchain::recreate()` - and the range is
    // the swapchain image's own shape (one layer, one mip); anything outside it is REFUSED, not clamped.
    // ----------------------------------------------------------------------------------------------

    /// A buffer, owned by the backend and released by the caller through `release()`.
    /// WHAT KIND OF ACCELERATION STRUCTURE: the two levels the hardware API has, named the contract's way.
    enum class acceleration_structure_type : std::uint32_t {
        bottom_level = 0, ///< one geometry's own structure (a BLAS), built from triangles
        top_level = 1,    ///< the instance list a ray launch traverses (a TLAS)
    };

    /// What a description may ask for beyond its type.
    enum class acceleration_structure_flag : std::uint32_t {
        allow_update = 1u << 0, ///< the structure may be REFIT in place (addresses and counts stay fixed)
    };
    /// The bits an `acceleration_structure_desc` carries (the same shape `buffer_flags` has).
    using acceleration_structure_flags = std::uint32_t;
    inline constexpr acceleration_structure_flags no_acceleration_structure_flags = 0u;
    [[nodiscard]] constexpr auto to_bits(acceleration_structure_flag const flag) noexcept -> acceleration_structure_flags {
        return static_cast<acceleration_structure_flags>(flag);
    }
    [[nodiscard]] constexpr auto has_flag(acceleration_structure_flags const flags, acceleration_structure_flag const flag) noexcept -> bool {
        return (flags & to_bits(flag)) != no_acceleration_structure_flags;
    }

    /// WHICH OPACITY STATES A MICROMAP'S MICRO-TRIANGLES CARRY. The two the hardware API defines, named the
    /// contract's way; the backend asserts the values against its own enum (it is the only side that names both).
    enum class micromap_format : std::uint32_t {
        two_state = 1,  ///< opaque / transparent, one bit per micro-triangle
        four_state = 2, ///< opaque / transparent / unknown / ... two bits per micro-triangle
    };

    /// HOW MANY MICRO-TRIANGLES A MICROMAP RESERVES, and in what format: the record the geometry that consults it
    /// declares, because the build has to reserve them before the traversal can look any of them up.
    struct micromap_usage {
        std::uint32_t count = 0;             ///< micro-triangles in this format
        std::uint32_t subdivision_level = 0; ///< 0 = one micro-triangle per triangle
        micromap_format format = micromap_format::four_state;
    };

    /// ONE MICRO-TRIANGLE'S ATTRIBUTE: where its bits are in the data array, and how they are laid out.
    /// The layout is the API's own (the backend asserts it), because a caller fills an array of these.
    struct micromap_triangle {
        std::uint32_t data_offset = 0;       ///< byte offset of this triangle's attributes in `desc.data`
        std::uint16_t subdivision_level = 0; ///< 0 = this triangle is not subdivided
        std::uint16_t format = 0;            ///< a `micromap_format` value
    };
    inline constexpr std::uint32_t micromap_triangle_size = sizeof(micromap_triangle);

    /// WHAT TO BUILD: the attributes, the per-triangle records and the index array a micromap is built from.
    ///
    /// THE MEMORY IS THE BACKEND'S, as it is for an acceleration structure, and so are the setup buffers these
    /// spans are copied into - including the 256-byte ADDRESS alignment the build requires of them, which is a
    /// requirement on an address rather than on a buffer and therefore cannot be the caller's problem. The
    /// `struct_size` guard is the usual one, and every span is borrowed only until the call returns.
    struct micromap_desc {
        std::uint32_t struct_size = sizeof(micromap_desc);
        std::uint32_t triangle_count = 0;                  ///< how many micro-triangles
        std::span<std::byte const> data = {};              ///< the attributes, `data_stride` bytes apart
        std::uint32_t data_stride = 4;                     ///< e.g. 4 for one 4-state pair per triangle
        std::span<micromap_triangle const> triangles = {}; ///< one record per micro-triangle
        std::span<std::uint32_t const> indices = {};       ///< the micro-triangle indices
        micromap_format format = micromap_format::four_state;
    };

    /// AN OPACITY MICROMAP (tier-1, like the acceleration structure it is attached to).
    ///
    /// WHAT IT IS FOR: a micro-triangle's opacity becomes the traversal's business instead of a shader's - an
    /// opaque one is committed without any-hit work, a transparent one is skipped, and an UNKNOWN one invokes
    /// the any-hit shader. That is why a micromap that says "unknown" everywhere is a no-op and a decisive test
    /// at the same time, and it is what the mask bake uses it for.
    ///
    /// IT OWNS ITS OWN SETUP MEMORY (the attributes, the records and the index array it was built from) and the
    /// structure's storage: a caller that holds the handle cannot get the alignment or the index array wrong,
    /// which is exactly what the engine used to do by hand.
    struct micromap : object {

        explicit micromap(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~micromap() noexcept = default;

        /// Release the caller's one reference (see `buffer::release()`): the backend destroys the structure and
        /// gives its memory back.
        virtual void release() noexcept = 0;
    };

    /// ONE TRIANGLE GEOMETRY of a bottom-level structure: where its vertices and indices are, and how many.    ///
    /// THE ADDRESSES ARE DEVICE ADDRESSES (the contract's `uint64_t`, as `shader_binding_table_region` spells
    /// them), which is what a build reads: a caller that wants an address asks `device_address::buffer_address()`
    /// for the buffer it created, so the engine half never names the driver's `VkDeviceAddress`.
    ///
    /// THE STRUCTURE IS BUILT ONCE FROM THIS (the addresses and counts are part of its identity); a structure
    /// whose BYTES change behind the same addresses is REFIT, which is what `acceleration_structure_flag::
    /// allow_update` declares at creation.
    struct acceleration_structure_geometry {
        std::uint64_t vertex_address = 0;             ///< first vertex, already offset into its buffer
        std::uint32_t vertex_stride = 0;              ///< bytes per vertex (the caller's layout)
        std::uint32_t vertex_count = 0;               ///< vertices the geometry spans
        std::uint64_t index_address = 0;              ///< first index (0 = the geometry is not indexed)
        index_type index_format = index_type::uint32; ///< what an index is
        std::uint32_t index_count = 0;                ///< indices; triangles = index_count / 3

        /// OPTIONAL: THE OPACITY MICROMAP this geometry consults, or null when no micromap is attached.
        /// A missing micromap still permits the hit group's any-hit alpha test.
        ///
        /// ONE HANDLE AND NOTHING ELSE, deliberately: the micromap knows its own usage record and owns the index
        /// array the traversal reads (the backend built both), so a caller cannot get either wrong - which is
        /// exactly the pair the engine used to carry across this boundary by hand.
        micromap* opacity_micromap = nullptr;
    };

    /// ONE TOP-LEVEL INSTANCE, as the CALLER writes it. THE LAYOUT IS THE CONTRACT'S, and the backend asserts
    /// it against the driver's own structure (the same rule `mesh_task_command` follows): the caller is the one
    /// that fills these records, so their shape cannot live only in one API's header.
    struct acceleration_structure_instance {
        float transform[12] = {};                             ///< 3x4 ROW-major (the fourth column is implicit)
        std::uint32_t instance_custom_index = 0;              ///< what a shader reads back through the hit
        std::uint32_t mask = 0xFFu;                           ///< the visibility mask the traversal tests
        std::uint32_t shader_binding_table_record_offset = 0; ///< which hit record this instance's hits use
        std::uint32_t flags = 0;                              ///< the caller's own instance bits
        std::uint64_t structure_reference = 0;                ///< `acceleration_structure::device_address()`
    };
    inline constexpr std::uint32_t acceleration_structure_instance_size = sizeof(acceleration_structure_instance);

    /// THE INSTANCE BIT A SHADOW CASTER NEEDS: a shadow ray must be blocked by a surface it approaches from
    /// behind, which is what the raster shadow pass does for the casters whose pipeline disables culling - so
    /// the traversal must disable facing culling for the same instances, or every plane and every open mesh
    /// leaks light.
    ///
    /// IT IS A CONTRACT VALUE, NOT THE API'S MACRO, and the backend asserts the two agree (it is the only side
    /// that names both). The engine writes it into `acceleration_structure_instance::flags`.
    inline constexpr std::uint32_t acceleration_structure_instance_facing_cull_disable = 1u;

    /// WHAT TO BUILD: the geometries of a bottom-level structure, or the capacity of a top-level one.
    ///
    /// `struct_size` is the same ABI guard `buffer_desc` carries, so a caller compiled against an older
    /// description is read only as far as it declared.
    struct acceleration_structure_desc {
        std::uint32_t struct_size = sizeof(acceleration_structure_desc);
        acceleration_structure_type type = acceleration_structure_type::bottom_level;
        acceleration_structure_flags flags = no_acceleration_structure_flags;
        acceleration_structure_geometry const* geometries = nullptr; ///< BOTTOM: borrowed until the call returns
        std::uint32_t geometry_count = 0;                            ///< BOTTOM: how many
        std::uint32_t instance_capacity = 0;                         ///< TOP: how many instances it must hold
    };

    /// AN ACCELERATION STRUCTURE (tier-1, like `buffer` and `image`).
    ///
    /// WHY TIER-1 RATHER THAN AN ABILITY: the ability mechanism answers "can this backend serve this optional
    /// feature, or not", and an acceleration structure is not optional furniture to a renderer that has one -
    /// it is a RESOURCE the caller creates, reads an address from, and destroys, exactly as a buffer is. The
    /// `ray_tracing` ability carried these verbs until abi 26 and announced them for nobody (its shapes were
    /// forward declarations); with the object here, the ability is RETIRED and the capability question ("can
    /// this device run ray queries / a ray-tracing pipeline") is asked through `device_capabilities`.
    ///
    /// WHO OWNS THE MEMORY: the BACKEND. This is the whole point of the tier-1 shape - the storage the
    /// structure lives in, the scratch a build needs, its alignment and the per-geometry offsets are the
    /// backend's business, and a caller that never sees them cannot depend on them. A second backend with no
    /// explicit acceleration structures at all can therefore answer `create_acceleration_structure` and
    /// `build_acceleration_structure` however it must.
    ///
    /// ONE REFERENCE, dropped through `release()`, like every owned handle in this contract.
    struct acceleration_structure : object {

        explicit acceleration_structure(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~acceleration_structure() noexcept = default;

        /// Release the caller's one reference (see `buffer::release()`).
        virtual void release() noexcept = 0;

        /// THE ADDRESS a shader (or an instance record's `structure_reference`) uses to reach this structure.
        /// Zero means the backend could not report one - which a top-level structure never means, since an
        /// address is exactly what an instance stores.
        [[nodiscard]] virtual std::uint64_t device_address() const noexcept = 0;

        /// THE BYTES this structure occupies. A caller needs it to describe the structure to something that
        /// carries address ranges (the descriptor heap's own address-range descriptor is the measured case);
        /// zero means the backend did not answer.
        [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;

        /// TOP LEVEL ONLY: replace the instance list the NEXT build reads. Host work on memory the backend
        /// owns, and the records are `acceleration_structure_instance` - so the caller of a top-level structure
        /// never allocates the array the traversal reads, which is the same ownership rule as everywhere else
        /// here. Refused by name when the structure is a bottom-level one or the list is longer than the
        /// capacity it was created with.
        [[nodiscard]] virtual error write_instances(std::span<acceleration_structure_instance const> instances) = 0;
    };

    struct buffer : object {

        explicit buffer(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~buffer() noexcept = default;

        /// RELEASE THE CALLER'S ONE REFERENCE. Runs inside the backend, whatever the compiler's `delete`
        /// would have done, and **may free nothing**: the resource can be shared/reused, so the object
        /// dies only with its last reference (see the ownership note above). Do not touch the handle
        /// afterwards. A borrowed frame view carries no reference to drop: callers MUST NOT invoke
        /// release() on it, and the backend MAY terminate on that ownership precondition violation.
        virtual void release() noexcept = 0;

        [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
        /// Direct bytes of a host-visible buffer; empty when it cannot be mapped. Successful host-mapped
        /// intents require no explicit flush/invalidate calls. The span borrows the buffer's storage and
        /// obeys its owner/frame lifetime. Calling mapped() neither waits nor inserts a barrier.
        /// Finish CPU writes before submitting GPU readers, and do not overwrite a range still in use.
        /// Before reading GPU writes, publish them for host_read and establish completion with a matching
        /// submission token, the frame protocol or wait_idle. Coherence alone supplies neither ordering
        /// nor completion. A backend unable to provide this mapping guarantee must refuse creation.
        [[nodiscard]] virtual std::span<std::byte> mapped() noexcept = 0;
    };

    /// How a view's `VkImageView` will be USED - the backend maps it to the aspect and usage the
    /// descriptor needs, and a caller that asks for a role the image's format cannot serve
    /// (depth_attachment on a color image) gets a refusal, not a wrong handle.
    enum class view_role : std::uint32_t {
        sampled = 0,          ///< read by a sampler (textures, per-cascade depth layer views)
        storage = 1,          ///< read/written by a compute pass as an image
        color_attachment = 2, ///< rendered into as a color target
        depth_attachment = 3, ///< rendered into as a depth target
    };

    /// WHICH RANGE of an image a view covers: the slice selector of `image::make_view()`. It carries no
    /// Vulkan type and no format - the image already has both - and a caller that wants "layer i of the
    /// shadow cascade" says so with `base_layer = i, layer_count = 1`.
    struct image_view_desc {
        std::uint32_t struct_size = sizeof(image_view_desc); ///< ABI guard, same rule as `buffer_desc`
        std::uint32_t base_layer = 0;
        std::uint32_t layer_count = 1; ///< 0 means "all remaining layers", like VkImageViewCreateInfo
        std::uint32_t base_mip = 0;
        std::uint32_t mip_count = 1; ///< 0 means "all remaining mips"
        view_role role = view_role::sampled;
    };

    /// An image, owned by the backend and released by the caller through `release()`.
    struct image : object {

        explicit image(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~image() noexcept = default;

        /// see `buffer::release()`: drops one reference inside the backend; the object may survive
        /// because a backend is allowed to serve identical content from one shared, content-keyed entry.
        virtual void release() noexcept = 0;

        [[nodiscard]] virtual image_extent extent() const noexcept = 0;
        [[nodiscard]] virtual image_format format() const noexcept = 0;

        /// A view of this image: WHERE THE BACKEND MAKES THE `VkImageView`, the caller only says WHICH
        /// RANGE of the image it wants (§17's design: the engine wants "one array layer" and "the whole
        /// image", not a raw `VkImageViewCreateInfo`). The view is a factory product (`unique_object<image_view>`,
        /// released by its container) and no native view crosses the contract: the escape's
        /// `native_image_view` was deleted in abi 42. The backend refuses a descriptor that asks for a range
        /// the image does not have (a layer past `array_layers`, a mip past `mip_levels`) with
        /// `error::invalid_argument` rather than clamping.
        /// APPENDED IN ABI 7 (§17's image face): a new virtual on an existing interface moves the
        /// vtable's shape, which is exactly the case `abi_version` exists to number.
        [[nodiscard]] virtual unique_object<image_view> make_view(image_view_desc const& desc) = 0;

        /// READ THIS IMAGE BACK INTO HOST MEMORY - the image's CONTENT, not a handle to it.
        ///
        /// THE MECHANISM IS THE BACKEND'S, AND IT IS `VK_EXT_host_image_copy` IN THIS ONE: the implementation
        /// performs the copy (`vkCopyImageToMemoryEXT`), so there is NO staging buffer, NO copy command and NO
        /// submission in the caller's path - the bytes come back directly. A caller that must not allocate can
        /// still use the `host_image_copy` ability's span-shaped verb; this is the convenience the ENGINE uses,
        /// and it is the ONLY read-back shape the engine sees.
        ///
        /// @param region WHICH subresource and which texels of it; the default is the whole mip 0, every layer.
        /// @return the content, row-major and tightly packed with the invariant `image_content` states - or a
        ///         NAMED error, never a partial buffer. `error::unsupported` is the honest answer for an image
        ///         the backend cannot copy out of (not created for host transfer, a device without the
        ///         extension) and for a format the contract cannot describe; it is also the answer for the
        ///         FRAME image on a surface whose swapchain usage has no `HOST_TRANSFER` bit, which is a
        ///         property of the SURFACE and not of this backend.
        ///
        /// APPENDED IN ABI 25: a new virtual on an existing interface, the case `abi_version` numbers.
        /// The default argument is spelled here rather than in the backend for the reason every other
        /// contract default is: a caller compiled against an older contract and a callee compiled against a
        /// newer one must agree on what "no region" means, and that can only be the header's text.
        [[nodiscard]] virtual std::expected<image_content, error> get_content(image_copy_region const& region = {}) const = 0;

        /// Copy an owning reference without consuming the caller's factory reference.
        /// Borrowed images and backends without shared ownership return empty.
        /// The api_core and backend library must outlive all strong and weak resource references.
        /// While shared references exist, repeated calls use the same control block.
        /// Use this instead of constructing an independent `shared_object` from a raw image.
        [[nodiscard]] virtual shared_object<image> share() {
            return {};
        }
    };

    /// A view of an image, owned by the backend: the contract's substitute for a raw `VkImageView`.
    /// Backends supporting shared parent references retain owned images independently of the factory reference.
    /// Views of borrowed frame images retain no parent and must respect the frame lifetime.
    /// No native view crosses the contract (the escape's `native_image_view` was deleted in abi 42).
    struct image_view : object {

        explicit image_view(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~image_view() noexcept = default;
        /// Drop the view, including its retained parent reference after native view destruction.
        virtual void release() noexcept = 0;
        /// Copy the parent owner; empty for borrowed swapchain images or unsupported backends.
        [[nodiscard]] virtual shared_object<image> get_image() const noexcept {
            return {};
        }
    };

    /// Capabilities an image needs beyond its shape - same rule as `buffer_flag`: the caller says WHAT
    /// IT NEEDS, the backend knows the usage bits, and a descriptor the backend cannot serve is refused.
    /// The set is §17's survey of this renderer's creation sites, not an attempt to mirror VkImageUsage.
    enum class image_flag : std::uint32_t {
        sampled = 1u << 0,              ///< read by a sampler (every texture and the shadow maps)
        storage = 1u << 1,              ///< bound as a storage image to compute
        color_attachment = 1u << 2,     ///< rendered into as a color target
        depth_attachment = 1u << 3,     ///< rendered into as a depth target (the shadow cascades)
        transfer_source = 1u << 4,      ///< copy SOURCE (a probe's capture target)
        transfer_destination = 1u << 5, ///< copy DESTINATION (a capture's read-back path)
        cube_compatible = 1u << 6,      ///< six layers, creatable as a cube (the environment maps)
        host_transfer = 1u << 7,        ///< the implementation copies from it into host memory
                                        ///< (VK_EXT_host_image_copy; the startup probe's target)
    };

    /// A set of `image_flag` bits, spelled like `buffer_flags`.
    using image_flags = std::uint32_t;

    /// The empty flag set, spelled where a caller passes "none".
    inline constexpr image_flags no_image_flags = 0u;

    [[nodiscard]] constexpr auto to_bits(image_flag flag) noexcept -> image_flags {
        return static_cast<image_flags>(flag);
    }

    [[nodiscard]] constexpr auto has_flag(image_flags flags, image_flag flag) noexcept -> bool {
        return (flags & to_bits(flag)) != no_image_flags;
    }

    /// How an image is created: its shape, its format (named, or the `depth` ROLE), the capabilities it
    /// needs, and - when the caller already has the texels - their bytes. Same append-only `struct_size`
    /// guard as `buffer_desc`; the initial bytes are part of the descriptor for the same reason they are
    /// on `buffer_desc` (§17: the five IBL/LUT sites are "content + one upload", and a backend that
    /// deduplicates on content can only recognise it when creation sees it).
    struct image_desc {
        std::uint32_t struct_size = sizeof(image_desc); ///< size of this structure as the CALLER compiled it
        image_extent extent = {};                       ///< texels; width and height must be non-zero for a 2D image
        std::uint32_t mip_levels = 1;                   ///< full chain is `(bits width/height)+1`; 0 means "full chain"
        std::uint32_t array_layers = 1;                 ///< 6 with `cube_compatible`
        image_format format = image_format::unknown;    ///< a NAMED format, or `depth` ("a depth attachment")
        image_flags flags = no_image_flags;
        std::span<std::byte const> initial_bytes; ///< the texels to upload at creation; empty = allocate only
        char const* debug_name = nullptr;         ///< what the backend logs on refusal; not retained
    };

    /// How a sampler addresses outside its last mip/edge: the modes this renderer's creation sites
    /// actually ask for (make_texture_sampler_info's callers). Appended like every enum, never moved.
    enum class sampler_address_mode : std::uint32_t {
        repeat = 0,
        mirrored_repeat = 1,
        clamp_to_edge = 2,
        clamp_to_border = 3,
    };

    /// The two filters this renderer's creation sites choose between (Vulkan's NEAREST/LINEAR; CUBIC is not
    /// asked for anywhere, so it is not in the vocabulary).
    enum class sampler_filter : std::uint32_t {
        nearest = 0,
        linear = 1,
    };

    /// How a mip level is picked between two levels.
    enum class sampler_mipmap_mode : std::uint32_t {
        nearest = 0,
        linear = 1,
    };

    /// The depth-comparison ops the renderer asks for, plus the disabled spelling: a comparison turns a
    /// `sampler2DShadow` tap into the hardware PCF test, and the cascaded shadow map asks for `less_or_equal`
    /// (source/shaders/pbr.frag's "not deeper than stored depth"). `never` is what `compare_enable == false` means.
    enum class sampler_compare_op : std::uint32_t {
        never = 0,
        less_or_equal = 1,
    };

    /// How a sampler is created: the knobs this renderer's sampler sites turn - addressing, the two filters,
    /// the mipmap mode, the LOD it clamps at and (for the shadow map) a depth comparison. Everything else is
    /// the backend's device judgement: anisotropy off, border transparent black, min LOD 0 - measured, no
    /// creation site asks for anything else.
    ///
    /// WIDENED IN abi 16 (see `abi_version`): the first version carried `address_mode` + `max_lod` only, and
    /// the ENGINE's own sampler set needs NEAREST minification (the G-buffer's stored surface is read at exact
    /// texel centres; so is the composite's depth tap), NEAREST mipmapping and a compare op (the shadow map)
    /// to stay value-for-value equal to the samplers the backend used to create on its behalf. The fields are
    /// APPENDED and the defaults reproduce the old behaviour exactly (linear/linear/linear, no comparison), and
    /// `struct_size` stays the first member: a caller built against the previous revision is REFUSED by the
    /// guard rather than read out of bounds.
    struct sampler_desc {
        std::uint32_t struct_size = sizeof(sampler_desc); ///< ABI guard, same rule as `buffer_desc`
        sampler_address_mode address_mode = sampler_address_mode::repeat;
        float max_lod = 0.0f; ///< the mip the sampler clamps at (the shadow comparators use small values)
        sampler_filter mag_filter = sampler_filter::linear;
        sampler_filter min_filter = sampler_filter::linear;
        sampler_mipmap_mode mipmap_mode = sampler_mipmap_mode::linear;
        bool compare_enable = false; ///< depth comparison: turns a `sampler2DShadow` tap into a PCF test
        sampler_compare_op compare_op = sampler_compare_op::never;
    };

    /// The shader stage a `shader_desc`'s code is compiled for. Values are the renderer's actual
    /// stage vocabulary; appended, never moved.
    enum class shader_stage : std::uint32_t {
        vertex = 0,
        mesh = 1, ///< the stage that REPLACES the vertex stage (docs/mesh_shaders.md)
        fragment = 2,
        compute = 3,
        /// THE RAY-TRACING STAGES, APPENDED (abi 21 - adding VALUES is explicitly not an abi change, see
        /// `abi_version`). They name the five entry points a ray-tracing pipeline is built from, and they exist
        /// so a ray-tracing shader module is NOT mislabeled as a compute one: the module itself is stage-less,
        /// but the pipeline's stage info is what the group table indexes, and a caller that cannot say
        /// "raygen" cannot build a group table at all.
        ray_generation = 4,
        miss = 5,
        closest_hit = 6,
        any_hit = 7,
        intersection = 8,
    };

    /// How a shader is created: its stage and its SPIR-V. Same append-only `struct_size` guard.
    struct shader_desc {
        std::uint32_t struct_size = sizeof(shader_desc); ///< size of this structure as the CALLER compiled it
        shader_stage stage = shader_stage::vertex;
        std::span<std::byte const> code;  ///< the SPIR-V
        char const* debug_name = nullptr; ///< what the backend logs on refusal; not retained
    };

    /// THE FOUR BLEND RECIPES this renderer's pipelines actually use (the survey found exactly these
    /// four behind the raw `VkPipelineColorBlendAttachmentState` constants): a caller names the MODE,
    /// the backend spells the factors and ops. Appended, never moved.
    enum class blend_mode : std::uint32_t {
        opaque = 0,   ///< overwrite: the G-buffer's surface targets (alpha carries data there)
        alpha = 1,    ///< classic src-alpha over
        additive = 2, ///< accumulate: the HDR target's emissive accumulation
        multiply = 3, ///< multiply: the toon chain's light modulation
    };

    /// The depth compare operator a pipeline's depth test uses. The renderer's vocabulary today:
    /// the LESS_OR_EQUAL default and the character-forward pass's EQUAL overwrite.
    enum class depth_compare : std::uint32_t {
        less_or_equal = 0,
        equal = 1,
    };

    /// ONE STAGE OF A RAY-TRACING PIPELINE (appended with the ray-tracing spelling, abi 21).
    ///
    /// A ray-tracing pipeline is built from several entry points at once, and its GROUPS index this list - so
    /// the stage kind and the code travel together, and the ORDER here is what `ray_tracing_group`'s indices
    /// mean. `debug_name` is the backend's log text for a stage it refuses, exactly as it is for a shader.
    struct ray_tracing_stage {
        shader_stage stage = shader_stage::ray_generation;
        std::span<std::byte const> code;
        char const* debug_name = nullptr;
    };

    /// "THIS GROUP SLOT NAMES NO STAGE", the spelling a `ray_tracing_group` uses for the slots it leaves empty
    /// (`VK_SHADER_UNUSED_KHR` in the API's own vocabulary, as a value so the descriptor stays portable).
    inline constexpr std::uint32_t shader_group_none = 0xFFFFFFFFu;

    /// ONE SHADER GROUP of a ray-tracing pipeline: the STAGE INDICES it binds, in the order
    /// `ray_tracing_stages` lists them.
    ///
    /// A GENERAL group names ONE shader (`general`: a raygen, a miss or an intersection entry); a HIT group
    /// names up to three (`closest_hit`, `any_hit`, `intersection`) and its geometry kind. WHICH KIND a group is
    /// follows from which slots are filled, which is the rule the API's own group type states - so the
    /// descriptor does not carry a group-type enumerator that could disagree with the slots.
    ///
    /// THE GROUP ORDER IS THE CALLER'S SHADER BINDING TABLE ORDER: the caller fills its SBT regions in exactly
    /// this order (see `pass::rt_shadow_pass`), which is why the count travels back with the pipeline.
    struct ray_tracing_group {
        std::uint32_t general = shader_group_none;
        std::uint32_t closest_hit = shader_group_none;
        std::uint32_t any_hit = shader_group_none;
        std::uint32_t intersection = shader_group_none;
        /// whether a HIT group's geometry is TRIANGLES (the only kind this renderer traces); ignored by a
        /// general group. It exists because the API makes the caller state it, and this renderer's answer is
        /// the same at every site.
        bool triangles = true;
    };

    /// A shader AS binding backed by an existing descriptor heap range. No descriptor set is allocated.
    /// The backend translates the binding to its own heap mapping; array elements are stride bytes apart.
    struct acceleration_structure_heap_binding {
        shader_stage stage = shader_stage::ray_generation;
        std::uint32_t descriptor_set = 0;
        std::uint32_t binding = 0;
        std::uint32_t byte_offset = 0;
        std::uint32_t array_stride = 0;
        std::uint32_t array_count = 1;
    };

    /// How a pipeline is created - `make_pipeline`'s parameters in the contract's vocabulary.
    /// The COLOR and DEPTH formats are contract formats (a named value, or the `depth` ROLE for the
    /// depth attachment; `unknown` as the depth format means the pipeline has NO depth attachment).
    /// The blend modes are per color attachment in attachment order; EMPTY means every target is
    /// overwritten (opaque), which is what a G-buffer surface target needs. Same append-only guard.
    enum class texture_group_profile : std::uint32_t { sampled_2d_16 = 0 };
    struct texture_group_binding {
        bool enabled = false;
        std::uint32_t push_byte_offset = 0;
        // vertex=1, fragment=2, compute=4, mesh=8.
        std::uint32_t shader_stage_bits = 0;
        texture_group_profile profile = texture_group_profile::sampled_2d_16;
    };
    /// Logical resource kinds, independent of descriptor storage.
    enum class resource_kind : std::uint32_t {
        texture_group,
        sampled_image,
        storage_image,
        uniform_buffer,
        storage_buffer,
        sampler,
        acceleration_structure,
    };
    /// One shader-visible uint32 handle field. Its byte offset is a shader parameter location,
    /// never an offset in descriptor storage. Stage bits: vertex=1, fragment=2, compute=4, mesh=8.
    struct resource_group_binding {
        std::uint32_t binding = 0;
        resource_kind kind = resource_kind::texture_group;
        std::uint32_t push_byte_offset = 0;
        std::uint32_t shader_stage_bits = 0;
    };
    inline constexpr std::uint32_t resource_group_capacity = 32;
    [[nodiscard]] constexpr error validate_resource_group_layout(std::span<resource_group_binding const> layout) noexcept {
        if (layout.size() > resource_group_capacity) {
            return error::invalid_argument;
        }
        for (std::size_t i = 0; i < layout.size(); ++i) {
            auto const& a = layout[i];
            if (a.kind > resource_kind::acceleration_structure || a.shader_stage_bits == 0 ||
                (a.shader_stage_bits & ~15u) || (a.push_byte_offset & 3u) || a.push_byte_offset > UINT32_MAX - 4u) {
                return error::invalid_argument;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (a.binding == layout[j].binding || a.push_byte_offset == layout[j].push_byte_offset) {
                    return error::invalid_argument;
                }
            }
        }
        return error::ok;
    }
    struct pipeline_desc {
        std::uint32_t struct_size = sizeof(pipeline_desc); ///< size of this structure as the CALLER compiled it
        std::span<image_format const> color_formats;       ///< attachment order; empty = depth-only
        image_format depth_format = image_format::unknown; ///< `unknown` = no depth attachment
        std::span<std::byte const> vertex_code;            ///< the FIRST stage's SPIR-V (vertex, or mesh)
        std::span<std::byte const> fragment_code;          ///< the fragment stage's SPIR-V
        shader_stage first_stage = shader_stage::vertex;   ///< `mesh` REPLACES the vertex stage
        std::uint32_t sample_count = 1u;                   ///< the render instance's MSAA level (1, 2, 4...)
        bool depth_test = true;                            ///< test + DYNAMIC write state (per-draw)
        float depth_bias_constant_factor = 0.0f;
        float depth_bias_slope_factor = 0.0f;
        float depth_bias_clamp = 0.0f;
        std::span<blend_mode const> blend_modes; ///< per color attachment; empty = all opaque
        depth_compare compare = depth_compare::less_or_equal;
        char const* debug_name = nullptr; ///< what the backend logs on refusal; not retained
        /**
         * THE COMPUTE SPELLING (APPENDED, guarded by `struct_size`, so an older caller keeps the graphics
         * meaning it compiled against).
         *
         * WHY A FIELD AND NOT A SECOND DESCRIPTOR: a compute pipeline is the SAME request with ONE stage and
         * no attachment state, and `first_stage == shader_stage::compute` is what selects it - the colour
         * formats, the depth format, the blend modes, the sample count and the depth test are then IGNORED,
         * exactly as `fragment_code` is ignored for a stage that has no fragment shader. The name says what
         * the bytes ARE rather than reusing `vertex_code` for a shader that is not a vertex stage: the two
         * graphics fields are named for their stages, and this one is named for its own.
         *
         * THE ENTRY POINT IS NOT HERE, on purpose: every shader this renderer builds is entered at "main"
         * (the graphics path hardcodes the same name in `make_pipeline`), so a field for it would carry one
         * value forever. A backend that sees no entry-point field uses "main"; a caller with a differently
         * named entry point is not a case this engine has.
         *
         * THE HEAP FLAG IS THE BACKEND'S: a heap-native pipeline is created with a NULL layout (validation
         * refuses the alternative), and a create call in that shape must carry
         * `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` - a descriptor-heap capability of the backend, not a
         * caller decision, so the contract does not spell it.
         */
        std::span<std::byte const> compute_code;
        /**
         * THE RAY-TRACING SPELLING (APPENDED in the same batch, `struct_size`-guarded).
         *
         * NON-EMPTY `ray_tracing_stages` IS THE WHOLE SWITCH: a ray-tracing pipeline is the only kind built
         * from SEVERAL named entry points and a GROUP TABLE, so the presence of the stages is what says which
         * of the three paths `create_pipeline` takes (graphics, compute, ray tracing) - the colour, depth,
         * compute and blend fields are ignored, exactly as the compute spelling ignores the attachment state.
         *
         * WHAT IS NOT HERE: the SHADER BINDING TABLE. Its handles are per-pipeline data read back after
         * creation, and its regions are the CALLER's memory with the device's own stride rules - both are the
         * caller's side of the boundary (see the pass that fills one), and a descriptor carrying them would be
         * promising a lifetime this contract cannot state.
         */
        std::span<ray_tracing_stage const> ray_tracing_stages;
        /// ONE GROUP PER SHADER BINDING TABLE REGION, in the caller's order; empty = not a ray-tracing pipeline.
        std::span<ray_tracing_group const> ray_tracing_groups;
        /// the pipeline's ray recursion depth (`maxPipelineRayRecursionDepth`); 1 = "a ray may hit once".
        std::uint32_t max_ray_recursion = 1u;
        /// Optional AS shader bindings into the heap. Appended and guarded by struct_size;
        /// older callers keep an empty list. Currently supported by ray-tracing pipelines.
        std::span<acceleration_structure_heap_binding const> acceleration_structure_bindings;
        texture_group_binding texture_group{};
        std::span<resource_group_binding const> resource_group;
    };

    /// The descriptors of the remaining factories. Opaque until S1 (see the banner).

    /// The remaining tier-1 objects: samplers, shaders, pipelines, swapchains, queries
    /// and command lists.
    ///
    /// Each one is a polymorphic handle and not much more yet: the operations the engine
    /// performs on them (bind, draw, dispatch, barrier, present) are exactly the S1
    /// design surface, held back by §6.4's "explicit barrier first, then shadow mode"
    /// decision. What they carry today is ownership: destroying one through this base
    /// has to reach the backend's destructor, which is the property the boundary rests
    /// on.
    struct sampler : object {

        explicit sampler(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~sampler() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct shader : object {

        explicit shader(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~shader() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct pipeline : object {

        explicit pipeline(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~pipeline() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct swapchain : object {

        explicit swapchain(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~swapchain() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;

        // ---- the presentation verbs (abi 14) --------------------------------------------------
        // The swapchain was created once at startup and is rebuilt IN PLACE - never replaced - so
        // the verbs below are APPENDED to this interface rather than new methods on `api_core`: a
        // rebuild is the swapchain's own behaviour (the caller supplies nothing - the backend
        // re-derives the extent from the window it owns, which is the one place the current size
        // lives).
        /// Rebuild the swapchain and its images for the window's current size.
        /// `ok` = a new generation was built; `not_ready` = the window reports a zero extent (a
        /// minimized or not-yet-sized window), nothing was built, and the caller retries later -
        /// a deferred recreate is a STATE, not a failure, and `error` carries it as one; real
        /// build failures are the backend's named startup-style panics (the same rule the
        /// constructor's build path follows).
        [[nodiscard]] virtual error recreate() = 0;

        /// The extent of the swapchain's images, in pixels - what presentation shows. The caller
        /// scales it to its own render resolution (a render scale is a RENDERER decision, not a
        /// presentation fact, so the contract does not pre-multiply it).
        [[nodiscard]] virtual image_extent extent() const noexcept = 0;

        /// Presentation format available before acquiring an image (ABI 28).
        [[nodiscard]] virtual image_format format() const noexcept = 0;
    };

    /// For a nonzero count, scratch_bytes == 0 means the backend cannot serve that count.
    struct radix_sort_requirements {
        std::uint64_t scratch_bytes = 0, buffer_alignment = 1;
        buffer_flags scratch_flags = no_buffer_flags;
    };
    struct radix_sort_info {
        std::uint32_t element_count = 0;
        buffer* keys = nullptr;
        buffer* values = nullptr;
        buffer* scratch = nullptr;
        std::uint64_t keys_offset = 0, values_offset = 0, scratch_offset = 0;
    };
    /// Stable ascending uint32 key/value sort. Operands are borrowed until GPU completion.
    /// Caller publishes writes before record and publishes output reads afterward.
    /// Internal native bindings may be overwritten; logical recorder state is preserved for later RHI consumers.
    struct radix_sorter : object {
        explicit radix_sorter(std::string_view name) noexcept
            : object(name) {
        }
        virtual ~radix_sorter() noexcept = default;
        virtual void release() noexcept = 0;
        [[nodiscard]] virtual radix_sort_requirements requirements(std::uint32_t count) const noexcept = 0;
        [[nodiscard]] virtual error record(command_recorder&, radix_sort_info const&) = 0;
    };

    // ---- THE COMMAND BUFFER: THE OWNED RECORDING HANDLE (abi 15) ------------------------------------
    //
    // `command_buffer` used to be a BORROWED view of a recording with no `release()` of its own (abi
    // 1/2/14); what the contract never had was the RESOURCE behind it - a command buffer the caller can
    // create, keep, begin, end and execute, which is what the engine's per-slot primaries, its
    // per-cascade and per-worker secondaries and the read-back's one-shot buffer need. Since abi 15
    // `command_buffer` IS that resource, in the ownership shape `buffer` / `image` established: the
    // factory hands out ONE reference, `release()` drops it inside the backend, and
    // `rhi::unique_object<T>` is the owner spelling. The frame's own list is still the ONE borrowed
    // instance of the type (`begin_commands()` answers it; its `release()` panics).
    //
    // WHAT DOES NOT CROSS THIS BOUNDARY, and it is the decision this type rests on: the ALLOCATOR. A
    // command buffer is allocated from an API-specific pool/allocator whose lifetime is entangled with
    // GPU progress (it may be reset only once the device is done with it), and neither Vulkan's
    // `VkCommandPool` nor D3D12's allocator nor Metal's queue is a portable concept. This backend's
    // wrapper OWNS ITS OWN POOL (source/backends/vulkan/core/handles/handles.cppm), so the one lifetime rule is the
    // handle's own and no allocator, pool or reset verb appears here; a backend that needs one keeps
    // it behind `begin_recording()` / `release()`.
    // ----------------------------------------------------------------------------------------------

    /// HOW A COMMAND BUFFER IS USED BY ITS OWNER (abi 15).
    ///
    /// A PRIMARY buffer is handed to the device as a unit of work; it is the ONLY role a caller can
    /// create in abi 37 (the secondary role is retired - see the VALUE below). The multithreaded
    /// recording paths record into `mergeable_command_buffer` segments instead and merge them in
    /// batches; those segments are contract handles of their own (abi 35), handed out by
    /// `make_mergeable_command_buffer()` - the GUI overlay's two recording targets are the other caller.
    enum class command_buffer_kind : std::uint32_t {
        primary = 0, ///< submitted by the frame itself
        /// RETIRED in abi 37: a secondary `command_buffer` is no longer creatable - the multithreaded
        /// recording paths use `mergeable_command_buffer` segments, merged in batches. The VALUE stays
        /// (enum values are ABI, never reused); both factories refuse it by name.
        secondary = 1,
    };

    /// How a command buffer is created (abi 15).
    ///
    /// `struct_size` is the ABI guard every descriptor in this file carries and follows the same
    /// append-only rule: a field whose whole extent is not inside the bytes the caller declares keeps
    /// this build's default, so an older caller is detected instead of mis-read.
    struct command_buffer_desc {
        std::uint32_t struct_size = sizeof(command_buffer_desc); ///< size of this structure as the CALLER compiled it
        command_buffer_kind kind = command_buffer_kind::primary; ///< what the buffer is for (see above)
    };

    /// WHAT ONE RECORDING SESSION NEEDS BEYOND ITS KIND (abi 15): the three usage facts every API in
    /// this family has a spelling of. WHICH barriers, stages and layouts that implies stays the
    /// backend's - the contract states the intent, never the mechanism.
    enum class command_buffer_usage : std::uint32_t {
        none = 0,
        one_time_submit = 1u << 0,      ///< recorded once and submitted once: the backend may optimise
        render_pass_continue = 1u << 1, ///< recorded INSIDE a rendering instance (mergeable segments only)
        simultaneous_use = 1u << 2,     ///< the recorded buffer may have overlapping submissions/executions; recording stays caller-serialized
    };

    /// The flags of `command_buffer_usage`, spelled the way the buffer flag set is (`to_bits` + `has_flag`).
    using command_buffer_flags = std::uint32_t;

    /// The empty flag set, spelled where a caller passes "none".
    inline constexpr command_buffer_flags no_command_buffer_flags = 0u;

    /// The same value as a bitmask, for a caller composing a set.
    [[nodiscard]] constexpr auto to_bits(command_buffer_usage flag) noexcept -> command_buffer_flags {
        return static_cast<command_buffer_flags>(flag);
    }

    /// Whether @p flags has @p flag set.
    [[nodiscard]] constexpr auto has_flag(command_buffer_flags flags, command_buffer_usage flag) noexcept -> bool {
        return (flags & to_bits(flag)) != no_command_buffer_flags;
    }

    /// What one `begin_recording()` call is told (abi 15).
    ///
    /// PORTABLE BY CONSTRUCTION: the usage bits, a size guard, and a one-layer BACKEND PARAMETER CHAIN.
    /// The chain is the same tagged mechanism `descriptor_heap`'s `vulkan_*_info` structs ride
    /// (`structure_header` + a `structure_type` value): an attachment-inheritance model is one API's
    /// execution model, so it does not live in this structure - a backend that needs one names it in
    /// the chain. A backend that cannot serve a chain REFUSES THE CALL BY NAME (`unsupported`) rather
    /// than dropping it, which is the rule the heap requests already follow ("不静默丢链").
    ///
    /// `struct_size` is the same ABI guard `command_buffer_desc` carries; `next` is borrowed only
    /// until the call returns.
    /// Attachment compatibility for a secondary recording. Formats use RHI values;
    /// `depth` requests the backend's chosen depth format, `unknown` means no depth.
    /// Borrowed only until begin_recording returns. Appending a tagged POD changes no vtable.
    struct command_buffer_inheritance_info {
        structure_header header{structure_type::command_buffer_inheritance, sizeof(command_buffer_inheritance_info), nullptr};
        std::uint32_t color_format_count = 0;
        image_format const* color_formats = nullptr;
        image_format depth_format = image_format::unknown;
        std::uint32_t samples = 1;
        std::uint32_t view_mask = 0;
    };

    struct command_buffer_begin_info {
        std::uint32_t struct_size = sizeof(command_buffer_begin_info); ///< size of this structure as the CALLER compiled it
        command_buffer_flags usage = no_command_buffer_flags;          ///< see command_buffer_usage
        structure_header const* next = nullptr;                        ///< optional tagged backend parameters
        /// APPENDED in abi 45: whether the backend chains ITS descriptor-heap inheritance into this
        /// secondary's begin. The engine's own segments live on the heap face and want it (the default); a
        /// THIRD-PARTY recorder that binds its own classic descriptor sets - the Dear ImGui Vulkan backend is
        /// the measured case - must NOT inherit a heap bind, or every one of its binds trips
        /// VUID-...-11295/11296 inside the continuation.
        bool inherit_descriptor_heaps = true;
    };

    /// ONE INDIRECT MESH-TASK RECORD: the three group counts, and nothing else.
    ///
    /// WHY THE LAYOUT IS THE CONTRACT'S: `command_buffer::draw_mesh_tasks_indirect()` takes a STRIDE, and the
    /// caller that fills the argument buffer is the one that has to know what a record looks like. Leaving that
    /// shape to the caller put the driver's own structure (`VkDrawMeshTasksIndirectCommandEXT` - which the
    /// engine named both to size its buffer and to write its slot) in the engine half, so the shape lives here,
    /// where both halves can name it. The backend that serves the verb static_asserts its own structure against
    /// `mesh_task_command_size`: it is the only side that names BOTH, and that assert is what keeps the two from
    /// drifting apart silently.
    ///
    /// A FIELD ADDED HERE IS NOT A FREE CHANGE: this value is what a caller passes as the stride, so the layout
    /// is as frozen as the verb that consumes it.
    struct dispatch_indirect_command {
        std::uint32_t groups_x = 0u, groups_y = 1u, groups_z = 1u;
    };
    static_assert(sizeof(dispatch_indirect_command) == 12u);
    [[nodiscard]] constexpr error validate_dispatch_indirect_range(std::uint64_t size, std::uint64_t offset) noexcept {
        return offset % 4u != 0u || offset > size || size - offset < sizeof(dispatch_indirect_command)
                   ? error::invalid_argument
                   : error::ok;
    }

    /// One indirect-command range rule, parameterised by the family's record size. Callers must pass the
    /// size their argument record really has (16 B draw commands, 12 B mesh-task commands); using one
    /// family's minimum for another was the visibility-lane defect this parameter removes.
    [[nodiscard]] constexpr error validate_indirect_range(std::uint64_t size, std::uint64_t offset, std::uint32_t count,
                                                          std::uint32_t stride, std::uint32_t record_size) noexcept {
        if (record_size < 4u || offset % 4u != 0u || stride < record_size || stride % 4u != 0u || offset > size) {
            return error::invalid_argument;
        }
        if (count == 0u) {
            return error::ok;
        }
        std::uint64_t const remaining = size - offset;
        return remaining >= record_size && (static_cast<std::uint64_t>(count) - 1u) <= (remaining - record_size) / stride
                   ? error::ok
                   : error::invalid_argument;
    }

    struct draw_indirect_command {
        std::uint32_t vertex_count = 0, instance_count = 0, first_vertex = 0, first_instance = 0;
    };
    static_assert(sizeof(draw_indirect_command) == 16u);
    [[nodiscard]] constexpr error validate_draw_indirect_range(std::uint64_t size, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) noexcept {
        return validate_indirect_range(size, offset, count, stride, sizeof(draw_indirect_command));
    }

    struct mesh_task_command {
        std::uint32_t groups_x = 0;
        std::uint32_t groups_y = 0;
        std::uint32_t groups_z = 0;
    };
    inline constexpr std::uint32_t mesh_task_command_size = 12u;
    static_assert(sizeof(mesh_task_command) == mesh_task_command_size, "the record's size IS the stride callers pass");

    /// The mesh-task argument record is 12 B, not the 16 B draw-indirect record: a range check that reused the
    /// draw-indirect minimum rejected every mesh-task indirect dispatch (the visibility lane's item recorder
    /// called it and the lane refused recording). Same alignment/overflow contract, its own minimum stride.
    [[nodiscard]] constexpr error validate_mesh_task_indirect_range(std::uint64_t size, std::uint64_t offset, std::uint32_t count,
                                                                    std::uint32_t stride) noexcept {
        return validate_indirect_range(size, offset, count, stride, mesh_task_command_size);
    }

    /// How many sampled slots one immutable texture group carries (`texture_group_info::textures`).
    inline constexpr std::uint32_t texture_group_capacity = 16u;

    struct texture_group_info {
        std::uint32_t struct_size = sizeof(texture_group_info);
        deren::utility::vessel::array<std::optional<image*>, texture_group_capacity> textures{};
    };

    /// The batch-only recording handle (defined after `command_buffer`): forward-declared here so the
    /// recording interface can name the batch its `execute()` merges.
    struct mergeable_command_buffer;
    /// The segment's shared handle: a `shared_object` over the batch-only handle (abi 44).
    using mergeable_command_buffer_ref = shared_object<mergeable_command_buffer>;

    /// Immutable sampled 2D slots. The device and backend must outlive all references.
    struct texture_group : object {
        explicit texture_group(std::string_view name) noexcept
            : object(name) {
        }
        virtual ~texture_group() noexcept = default;
        virtual void release() noexcept = 0;
    };
    using texture_group_ref = shared_object<texture_group>;

    /// Immutable composition: texture_group is one member kind, not the general binding container.
    /// `resource` must be an owning/alias shared reference of the kind declared. Buffer size 0 means
    /// the remainder after offset. View selects an image subresource. Spans are borrowed until return.
    struct resource_group_entry {
        std::uint32_t binding = 0;
        resource_kind kind = resource_kind::texture_group;
        shared_object<object> resource;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        image_view_desc view{};
    };
    struct resource_group_desc {
        std::uint32_t struct_size = sizeof(resource_group_desc);
        std::span<resource_group_entry const> entries;
    };
    /// Group owns member references; recorded/pending commands retain the group. Core/backend must
    /// outlive every group. Immutability covers bindings, not resource content or synchronization.
    struct resource_group : object {
        explicit resource_group(std::string_view name) noexcept
            : object(name) {
        }
        virtual ~resource_group() noexcept = default;
        virtual void release() noexcept = 0;
    };
    using resource_group_ref = shared_object<resource_group>;

    /// Shared recording vocabulary (ABI 49). Owners expose these verbs directly.
    /// This base carries no individual submission or batch execution permission.
    struct command_recorder : object {

        explicit command_recorder(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~command_recorder() noexcept = default;

        /// RELEASE THE CALLER'S ONE REFERENCE (see `buffer::release()`): the recording session and the
        /// allocation behind it die with the last reference, inside the backend.
        virtual void release() noexcept = 0;

        /// Open this buffer's recording session.
        ///
        /// `ok` = recording; `not_ready` = this handle holds no reference any more, or the backend's
        /// recording context is not available; `unsupported` = a usage bit or a chain entry this
        /// backend cannot serve (REFUSED BY NAME, never silently dropped); device-level failures
        /// (`device_lost`, `out_of_*_memory`) travel as themselves.
        [[nodiscard]] virtual error begin_recording(command_buffer_begin_info const& info) = 0;

        /// Close the recording session: the buffer becomes executable by `command_context::submit()`. A
        /// `mergeable_command_buffer` segment becomes executable through `execute(span)` instead.
        [[nodiscard]] virtual error end_recording() noexcept = 0;

        /// Apply pending CPU operations in order, preserving final state for later recording.
        /// end_recording applies automatically. Deferred errors make the recording non-executable.
        /// Borrowed pipeline/buffer owners must live through pending recording AND GPU completion.
        [[nodiscard]] virtual error apply_operations() noexcept {
            return error::unsupported;
        }

        // ---- THE RECORDING SERIES, NOW ON THE OWNER (user's ruling) ------------------------------
        // command_list is GONE: the recording verbs live on the one handle a caller holds, so a
        // call site is `buffer->draw(...)` / `->barrier(...)` with no borrowed view in between. The
        // non-virtual forwarders this type used to carry are gone with it: these ARE the series.

        /// Declare that `resource` moves from one role to the other, and let the backend record what that
        /// needs. THE PAIR, not a single use, is deliberate: this is plan §6.4 option (a) ("explicit
        /// barrier") spelled in option (c)'s vocabulary - the caller knows what it just did (it recorded
        /// it), the backend knows what the pair costs. A single-use form cannot be derived here: the
        /// barrier describes what the CALLER just did, and the backend cannot know the "from" of a
        /// recording it was not told about.
        ///
        /// THE CALLER OWNS THE CORRECTNESS: a missing pair, or a reversed one, is a WRONG barrier - the
        /// one failure this contract cannot catch for you. The shadow gate compares the barriers this
        /// produces, field by field, against the recipes the renderer ships with.
        ///
        /// IT ANSWERS, IT DOES NOT DROP SILENTLY: a barrier that was not recorded leaves the image in a
        /// state nobody declared, which is exactly the validation failure this surface was built to avoid
        /// (task-148's). So the answer is an `error` and the caller has to look at it (`[[nodiscard]]`).
        /// `not_ready` = no frame is being recorded, or the list is not this frame's (the same window
        /// `begin_commands()` answers in); `unsupported` = a role pair this backend cannot spell;
        /// `invalid_argument` = an image this backend did not hand out.
        [[nodiscard]] virtual error use(image const& resource, image_use from, image_use to) noexcept = 0;

        /// Record a copy of `region` of `source` into `destination`, at the source's CURRENT layout (this
        /// renderer keeps every image in GENERAL - docs/unified_image_layouts.md).
        ///
        /// The destination is a DEVICE buffer, not host memory: "record it into this frame" and "read it
        /// on the host" are two moments, and the second one is `buffer::mapped()` once the frame lands.
        [[nodiscard]] virtual error copy_image_to_buffer(buffer& destination, image const& source, image_copy_region const& region) noexcept = 0;

        // ---- the GPU timing recording verbs (abi 14) -------------------------------------------
        // One mark's duration is the interval it OPENS: stage i runs from mark i to mark i + 1 and is
        // named by the name mark i carries (read back through `gpu_profiler`). WHICH PIPELINE STAGE a
        // timestamp resolves at is a MEASUREMENT detail of the backend that owns the query pool, not
        // a caller decision - the caller marks pass boundaries in order and names them; the contract
        // deliberately does not carry a pipeline-stage vocabulary (that would import one API's
        // execution model into every backend).
        /// Open the frame's timing range: reset this frame slot's queries on the recorded timeline.
        /// Call once per frame, before any mark. `unsupported` = this device cannot timestamp;
        /// `not_ready` = no frame is being recorded (the same window `use()` refuses in).
        [[nodiscard]] virtual error begin_gpu_timing() noexcept = 0;

        /// Write one timing mark into the frame's range, named for the stage it opens. @p mark_index
        /// must be the marks this frame has already written (the marks are POSITIONAL: an
        /// out-of-order index would mislabel every later interval, so it is refused with
        /// `invalid_argument` rather than accepted silently). `stage_name` is the caller's STATIC
        /// text - a literal outliving the frame (the same rule as the creation descriptor's
        /// `window_title`); the backend stores the view and reports it verbatim.
        [[nodiscard]] virtual error mark_gpu_timing(std::uint32_t mark_index, std::string_view stage_name) noexcept = 0;

        // ---- the portable record series (abi 20) ------------------------------------------------
        // THE RECORDING SURFACE'S OWN VOCABULARY: the verbs this renderer's passes actually record,
        // replacing 140 raw `vkCmd*` call sites in the engine's sources. Declared ONCE on the owning
        // `command_recorder`; sibling owners retain different submit rules. Shader parameter pushes
        // are recorded here too, independent of the backend's descriptor storage.
        //
        // THE ANSWERING RULE, and the one place it deliberately deviates from the obvious shape: a verb that
        // RECEIVES A CONTRACT HANDLE answers `error` - "a handle this backend did not hand out" is a
        // real, checkable refusal (`invalid_argument`), the same answer `use()` and
        // `copy_image_to_buffer()` already give, and a barrier or a binding that was silently not
        // recorded is the corruption this surface exists to make impossible. A verb that receives
        // ONLY VALUES generally answers `void`; push_constants answers error because its byte range and
        // reserved pipeline fields need explicit validation. For the remaining legacy void verbs, a
        // wrong-state call on a non-recording buffer is the validation layer's catch, exactly as it
        // is for the raw calls today. `not_ready` on an answering verb means this list is not
        // currently recording; `unsupported` means a mechanism this backend cannot serve.
        //
        // EVERY DESCRIPTOR here is the plan's measured vocabulary: what the 140 sites name, nothing
        // more. Layouts are the images' current ones (GENERAL - docs/unified_image_layouts.md); the
        // host-visible mask pairs and the queue-family transfers were raw through the escape when this
        // block was written, and the `host_read` roles (`image_use` / `buffer_use`) carry the
        // host-visible half now (the escape itself is deleted in abi 43).

        /// Immediate binding; exact binding-number/kind match with the current pipeline is required.
        /// Validates before native recording; retains group through recorded and pending GPU use.
        [[nodiscard]] virtual error load_resource_group(resource_group_ref const& /*unused*/) {
            return error::unsupported;
        }

        // render scope
        [[nodiscard]] virtual error begin_rendering(rendering_info const& info) = 0;
        virtual void end_rendering() noexcept = 0;

        /// Record shader parameter bytes immediately; the span is consumed before return.
        /// Require recording, nonempty four-byte units and containment within the capacity reported by
        /// device_capabilities::max_push_constants_size(). Reject overlap with bound pipeline resource
        /// group/texture token fields. A pipeline need not already be bound. Unsupported implementations
        /// refuse explicitly; no pointer or parameter storage is retained by the default operation.
        [[nodiscard]] virtual error push_constants(std::uint32_t /*unused*/, std::span<std::byte const> /*unused*/) noexcept {
            return error::unsupported;
        }

        // binding
        [[nodiscard]] virtual error bind_pipeline(pipeline const& handle) = 0;
        [[nodiscard]] virtual error bind_vertex_buffer(buffer const& handle, std::uint64_t offset) = 0;
        [[nodiscard]] virtual error bind_index_buffer(buffer const& handle, std::uint64_t offset, index_type type) = 0;

        // draw
        virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept = 0;
        virtual void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept = 0;

        [[nodiscard]] virtual error draw_indirect(buffer const& arguments, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) = 0;

        // compute + geometry
        virtual void dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept = 0;
        /// ABI48: one compute dispatch from a 12-byte argument record; unavailable backends refuse explicitly.
        [[nodiscard]] virtual error dispatch_indirect(buffer const& /*unused*/, std::uint64_t /*unused*/) {
            return error::unsupported;
        }
        virtual void draw_mesh_tasks(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept = 0;
        [[nodiscard]] virtual error draw_mesh_tasks_indirect(buffer const& argument_buffer, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) = 0;
        /// Record a ray-tracing LAUNCH over `width` x `height` pixels, `depth` rays deep, reading the shader
        /// binding table the CALLER built: one region per table - ray generation, miss, hit - plus the callable
        /// table a shader may invoke (an all-zero region when there are none).
        ///
        /// WHY THE LAUNCH IS A RECORD VERB AND NOT PART OF THE `ray_tracing` ABILITY: it is a recorded command
        /// exactly like `draw`, `dispatch` and `draw_mesh_tasks` - it takes a command buffer, it is ordered with
        /// the rest of a frame's commands, and it needs nothing the recording face does not already have. The
        /// `ray_tracing` ability declares the ACCELERATION-STRUCTURE half, whose descriptor shapes are still the
        /// S1 design surface and are deliberately incomplete - and a launch reachable only through an ANNOUNCED
        /// ability would be unreachable on a backend that serves the recording face but has not frozen those
        /// shapes yet, which is exactly this renderer's state.
        ///
        /// APPENDED IN ABI 24 (a tier-1 slot). The regions are the contract's own
        /// `shader_binding_table_region`; the verb answers NOTHING (`void`), like `draw` and `draw_mesh_tasks`, and
        /// a backend whose device published no `vkCmdTraceRaysKHR` records nothing - a pass that launches rays is
        /// only built when its pipeline could be created, which needs that extension.
        virtual void trace_rays(shader_binding_table_region const& raygen, shader_binding_table_region const& miss, shader_binding_table_region const& hit,
                                shader_binding_table_region const& callable, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept = 0;

        /// APPENDED IN ABI 26: record the BUILD of @p target - its geometries (a bottom-level structure) or its
        /// instance list (a top-level one), whichever it was created with, over the memory the backend owns.
        /// `ok` = recorded; `not_ready` = this buffer is not recording; `invalid_argument` = a handle this
        /// backend did not hand out; `unsupported` = a level or a shape it cannot serve (REFUSED BY NAME, never
        /// silently skipped - a missing build would produce an empty traversal, which is a wrong picture rather
        /// than a missing one).
        [[nodiscard]] virtual error build_acceleration_structure(acceleration_structure& target) = 0;

        /// APPENDED IN ABI 26: REFIT @p target in place, for the structures whose description declared
        /// `acceleration_structure_flag::allow_update` - the addresses and counts are unchanged and only the
        /// memory they point at has been rewritten (the zero-copy shape a compute-skinning frame produces).
        /// Refused by name when the structure was not created refittable, which is exactly what declaring it
        /// is for.
        [[nodiscard]] virtual error refit_acceleration_structure(acceleration_structure& target) = 0;

        /// APPENDED IN ABI 27: record the BUILD of @p target - the attributes, the per-triangle records and the
        /// index array its description was created from. `ok` = recorded; `not_ready` = this buffer is not
        /// recording; `invalid_argument` = a handle this backend did not hand out; `unsupported` = a shape it
        /// cannot serve, refused BY NAME.
        ///
        /// A MICROMAP MUST BE BUILT BEFORE THE GEOMETRY THAT CONSULTS IT: that is the order the caller records
        /// them in, and the ordering this verb owns (the barrier is the backend's, as it is for a build).
        [[nodiscard]] virtual error build_micromap(micromap& target) = 0;

        // dynamic state
        virtual void set_viewport(viewport const& vp) noexcept = 0;
        virtual void set_scissor(rect const& scissor) noexcept = 0;
        virtual void set_cull_mode(cull_mode mode) noexcept = 0;
        virtual void set_depth_write(bool enable) noexcept = 0;
        virtual void set_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept = 0;

        // synchronisation
        [[nodiscard]] virtual error barrier(barrier_group const& group) = 0;
        [[nodiscard]] virtual error barrier(image_barrier const& one) = 0;

        // copy + clear
        [[nodiscard]] virtual error copy_image(image_copy const& copy) = 0;
        [[nodiscard]] virtual error copy_buffer(buffer& destination, buffer const& source, std::uint64_t size, std::uint64_t source_offset, std::uint64_t destination_offset) = 0;
        [[nodiscard]] virtual error clear_color_image(image const& target, deren::utility::vessel::array<float, 4> const& color, subresource_range const& range) = 0;

        /// ABI 31. Success retains the group for recorded work; nullptr clears it.
        [[nodiscard]] virtual error load_texture_group(texture_group_ref const& /*group*/) {
            return error::unsupported;
        }
    };

    /// Individually submittable owner and receiver of mergeable batches.
    struct command_buffer : command_recorder {
        explicit command_buffer(std::string_view name) noexcept
            : command_recorder(name) {
        }
        ~command_buffer() noexcept override = default;
        /// ABI 36. Merge a BATCH of mergeable segments into THIS recording buffer - the recording-time
        /// counterpart of the submission-time batch. The batch rules are the same ones
        /// `command_context::submit(span)` states: span order, self-contained segments, no implicit memory
        /// barriers, whole-batch validation. `unsupported` = a backend whose merge must happen at submit
        /// time (the default body): that backend serves `submit(span)` instead.
        [[nodiscard]] virtual error execute(std::span<mergeable_command_buffer_ref const> /*batch*/) {
            return error::unsupported;
        }
    };

    /// Batch-only owner: ordered, self-contained segments retained until completion.
    /// Sibling of command_buffer; cannot receive execute() or be submitted individually.
    struct mergeable_command_buffer : command_recorder {
        explicit mergeable_command_buffer(std::string_view name) noexcept
            : command_recorder(name) {
        }
        ~mergeable_command_buffer() noexcept override = default;
    };

    /// What starting a frame hands back.
    ///
    /// §3.3 fixes the name and the fact that it is a value, not a handle; the fields the
    /// frame-in-flight ring and the timeline machinery need are §6.5's, so this is the
    /// smallest version that is still worth returning.
    struct submit_info {
        std::uint32_t frame_index = 0; ///< index in the frame-in-flight ring
        std::uint32_t image_index = 0; ///< the swapchain image this frame draws into
    };

    /// What opening one frame decided, AND WHY - the frame face's answer type (abi 13).
    ///
    /// BY VALUE, THEREFORE FROZEN: the same rule `submit_info` and `error_info` live under - a
    /// by-value POD's size is part of its calling convention, so any field addition moves
    /// `abi_version`. This type is the minimal repair of a measured information loss: today's
    /// `frame_begin()` squashes OUT_OF_DATE and every other failure into the same zeroed
    /// `submit_info`, so the caller cannot tell a resized window from a dead device, and
    /// `wait_frame_slot()` drops `vkWaitSemaphores`' result entirely. Both arrive here now:
    ///
    /// 1. `result.code == error::ok` => `frame` names the opened frame. ANYTHING else => `frame` is
    ///    zeroed and MUST NOT be used - the readable "zero means no frame" of today, but now the
    ///    caller also knows WHY (out_of_date => rebuild and skip; device_lost => fatal; ...).
    /// 2. `result` may come from EITHER half of the open - the slot's timeline wait, or the image
    ///    acquire. `result.message` names the step and `result.native_code` carries the raw VkResult;
    ///    `where` is the backend's failure point. This is where `wait_frame_slot()`'s dropped
    ///    `vkWaitSemaphores` result comes back.
    struct frame_open_info {
        submit_info frame = {}; ///< the opened frame; zeroed and invalid unless result.code == ok
        error_info result = {}; ///< ok, or the failure of whichever step refused
    };

    /// The frame-in-flight ring depth this ABI is written for (abi 15's follow-up, ③-D/E batch).
    ///
    /// DATA, NOT A VTABLE: an `inline constexpr`, so adding it cannot renumber `abi_version` (the same
    /// rule every appended `error` value follows). It exists because a compile-time number is what the
    /// engine needs for its per-slot C++ (arrays sized by the ring), which `frame_walker::slot_count()`
    /// - the RUNTIME truth, and the only authority - cannot provide. The two are therefore pinned to
    /// each other rather than trusted: the backend `static_assert`s its own ring against this constant,
    /// and the engine checks `slot_count()` against it once at startup and PANICS on a mismatch (a
    /// backend whose ring is a different size must fail loudly, never overrun a per-slot array).
    inline constexpr std::uint32_t max_frames_in_flight = 2u;

    /// THE PER-IMAGE ARRAY BOUND A BACKEND PROMISES (③-D/E item A1): the swapchain hands out no more than
    /// this many images, so a caller may size its per-image arrays (targets, views, per-image flags) as a
    /// C++ array of this many slots.
    ///
    /// DATA, NOT A VTABLE - the `max_frames_in_flight` rule, for the same reason: an integer whose only job
    /// is to size an array must not renumber `abi_version`. The alternative was a `swapchain::image_count()`
    /// virtual (abi 17), which buys a runtime number the caller would then have to allocate around.
    ///
    /// IT IS A PROMISE THE BACKEND KEEPS, NOT A GUESS ABOUT THE DRIVER: a driver that reports MORE images
    /// than this makes the backend REFUSE THE CREATION LOUDLY - never clamp, never resize past the
    /// constant, because the caller's arrays are already sized by it. The backend `static_assert`s this
    /// against its own frozen bound, and the ENGINE asserts an index before it uses one (a panic, never a
    /// silent out-of-range read). A backend that cannot keep this promise is what the abi-17
    /// `swapchain::image_count()` route is for.
    inline constexpr std::uint32_t max_swapchain_images = 4u;

    /// The frame ring's cursor, as a BORROWED VIEW - the `command_buffer` shape, not the owned-handle
    /// one: a view carries no reference, so `unique_object` must not wrap it (its release panics).
    /// `api_core` hands out the same object every call.
    ///
    /// WHY THE RING IS A CONTRACT FACE AT ALL: the cursor's one home is the backend's own frame
    /// state - the engine today keeps a SECOND copy of it (`current_frame` field reads, the
    /// `MAX_FRAMES_IN_FLIGHT` constant) plus three raw verbs (wait, acquire, advance), which is the
    /// "two sources of truth" shape this face exists to end. The engine borrows the view and walks;
    /// the backend stays the only authority. `position()` and a successful `wait_and_acquire()`'s
    /// `frame.frame_index` answer the SAME slot - the consistency is asserted, not hoped for.
    ///
    /// THE ORDER INSIDE `wait_and_acquire()` IS LOAD-BEARING (it is today's frame prologue, fused):
    /// read the cursor without advancing; wait that slot's timeline (value 0 = never submitted =>
    /// nothing to wait for); LATCH the slot's last completed frame's GPU timings - here, between the
    /// wait and the acquire, exactly where the engine's `collect_gpu_timings` sits today, so a frame
    /// that dies on OUT_OF_DATE is still collected (behaviour unchanged); acquire the next image;
    /// report. `walk_to_next()` advances after present, and it advances ONE step - it does not
    /// promise "the frame is over" (`end_frame` would), only that the ring moved.
    struct frame_walker {
        virtual ~frame_walker() noexcept = default;

        /// how many slots the ring has (today's `MAX_FRAMES_IN_FLIGHT` reads).
        [[nodiscard]] virtual std::uint32_t slot_count() const noexcept = 0;

        /// the slot THIS frame is being recorded into (today's `current_frame` field reads). Read-only:
        /// the cursor moves in `walk_to_next()`, never in a read.
        [[nodiscard]] virtual std::uint32_t position() const noexcept = 0;

        /// wait this slot's timeline, latch its previous frame's GPU timings, acquire the next image,
        /// and report the decision. See the type's note for the order and the zero-frame rule.
        [[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;

        /// advance the ring one slot - the present-side close of the frame (today's `to_next_frame()`).
        virtual void walk_to_next() noexcept = 0;
    };

    /// The GPU timing report of the LAST COMPLETED frame, as a BORROWED VIEW (no `release()`, same
    /// rule as `frame_walker`).
    ///
    /// The REPORT, not the recording: marks are written by the backend while a frame is recorded
    /// (a mark's semantic name rides the mark itself - the engine's static text, held as a pointer,
    /// the same rule as `window_title`), and the backend latches a slot's finished frame when its
    /// timeline is waited. Reading is then pure host-side work the CALLER orders: after
    /// `wait_and_acquire()` answers, this face describes the frame that JUST completed.
    ///
    /// A "stage" is the interval one mark OPENS: stage i runs from mark i to mark i + 1 and is named
    /// by the name mark i carried. The last mark closes the frame and opens nothing - asking for it
    /// is `not_ready`. Durations are NANOSECONDS (the backend's timestamp period converts; on the
    /// device this backend ships on it is 1 ns/tick with 64 valid bits, so the integer is lossless);
    /// the display layer converts to milliseconds.
    ///
    /// THE ERRORS ARE THE ERROR MECHANISM WORKING: `unsupported` = this device or configuration
    /// cannot timestamp (and `stage_count()` is then always 0); `invalid_argument` = an index at or
    /// past `stage_count()`; `device_lost` = the timestamp read-back failed; `not_ready` = no marks
    /// latched yet, or the index names the frame's last mark, which opens no stage.
    struct gpu_profiler {
        virtual ~gpu_profiler() noexcept = default;

        /// how many marks the last completed frame wrote (0 = nothing latched, or no timing support).
        [[nodiscard]] virtual std::uint32_t stage_count() const noexcept = 0;

        /// one stage's name and duration. `name` / `duration_ns` are caller-provided stores and may be
        /// nullptr individually (ask only what you need); the name is the static text the mark carried,
        /// valid as long as the process runs (the backend is never unloaded - invariant 4).
        [[nodiscard]] virtual error get_stage_info(std::uint32_t index, std::string_view* name, std::uint64_t* duration_ns) const noexcept = 0;
    };

    /// Borrowed resource creation interface. Empty products mean creation was refused/failed.
    /// Descriptors are borrowed until return; products retain their existing ownership rules.
    /// Shared immutable payload. Other aliases must not mutate bytes during the resource lifetime.
    using lazy_data = std::shared_ptr<deren::utility::vessel::vector<std::uint8_t> const>;
    enum class lazy_state : std::uint32_t { unloaded,
                                            loading,
                                            ready,
                                            failed };
    struct lazy_buffer_desc {
        std::uint32_t struct_size = sizeof(lazy_buffer_desc);
        buffer_desc resource;
        lazy_data data;
    };
    struct lazy_image_desc {
        std::uint32_t struct_size = sizeof(lazy_image_desc);
        image_desc resource;
        lazy_data data;
    };
    [[nodiscard]] inline error validate_lazy_buffer(lazy_buffer_desc const& desc) noexcept {
        if (desc.struct_size < sizeof(desc) || desc.resource.struct_size < sizeof(buffer_desc) ||
            !desc.data || desc.data->empty() || !desc.resource.initial_bytes.empty() ||
            desc.data->size() != desc.resource.size)
            return error::invalid_argument;
        switch (desc.resource.usage) {
        case buffer_usage::vertex:
        case buffer_usage::index:
        case buffer_usage::uniform_gpu_only:
        case buffer_usage::storage_gpu_only:
            return error::ok;
        default:
            return error::unsupported;
        }
    }
    struct lazy_buffer : buffer {
        using buffer::buffer;
        [[nodiscard]] virtual lazy_state state() const noexcept = 0;
        /// Nonblocking: not_ready while loading or retained by a view/binding/recording.
        /// Source and metadata survive; the next native demand reloads the initial bytes.
        [[nodiscard]] virtual error unload() noexcept = 0;
    };
    struct lazy_image : image {
        using image::image;
        [[nodiscard]] virtual lazy_state state() const noexcept = 0;
        [[nodiscard]] virtual error unload() noexcept = 0;
    };

    struct resource_factory {
        resource_factory() noexcept = default;
        virtual ~resource_factory() noexcept = default;
        resource_factory(resource_factory const&) = delete;
        resource_factory& operator=(resource_factory const&) = delete;
        [[nodiscard]] virtual std::expected<unique_object<lazy_buffer>, error> create_lazy_buffer(lazy_buffer_desc const&) {
            return std::unexpected(error::unsupported);
        }
        [[nodiscard]] virtual std::expected<unique_object<lazy_image>, error> create_lazy_image(lazy_image_desc const&) {
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
    struct command_context {
        command_context() noexcept = default;
        virtual ~command_context() noexcept = default;
        command_context(command_context const&) = delete;
        command_context& operator=(command_context const&) = delete;
        [[nodiscard]] virtual unique_object<command_buffer> create_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual shared_object<command_buffer> make_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual mergeable_command_buffer_ref make_mergeable_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual error submit(command_buffer& commands) = 0;
        [[nodiscard]] virtual error submit(std::span<mergeable_command_buffer_ref const> batch) = 0;
    };

    /// Borrowed frame/presentation interface, also present for offscreen frames (present is unsupported).
    /// Walker/profiler/surface are stable borrowed views: never release/delete them.
    struct frame_context {
        frame_context() noexcept = default;
        virtual ~frame_context() noexcept = default;
        frame_context(frame_context const&) = delete;
        frame_context& operator=(frame_context const&) = delete;
        /// Current frame's primary, or null outside recording. Borrowed until frame submission.
        [[nodiscard]] virtual command_buffer* begin_commands() = 0;
        /// Last acquired image; null before first acquire, valid through submit until the next acquire.
        [[nodiscard]] virtual image* frame_image() noexcept = 0;
        /// Host-visible slot used by the frame copy; completion/host visibility is the caller's responsibility.
        [[nodiscard]] virtual buffer* frame_readback_buffer() noexcept = 0;
        /// Legacy acquisition result; prefer walk_frames()->wait_and_acquire() for full error information.
        [[nodiscard]] virtual submit_info frame_begin() = 0;
        [[nodiscard]] virtual error present() = 0;
        [[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;
        [[nodiscard]] virtual gpu_profiler* profiler() noexcept = 0;
        [[nodiscard]] virtual swapchain* frame_swapchain() noexcept = 0;
        /// Submit the current frame using backend-owned acquire/presentation state.
        [[nodiscard]] virtual error submit() = 0;
    };

    /// Read-only binding to a core-owned interface. Only api_core may bind it during construction.
    /// No ownership, allocation, release, reset, rebinding, or implicit pointer conversion.
    /// Arrow access requires a fully constructed/bound core. Const freezes binding, not device activity.
    template <class T>
    class context_view final {
        T* target = nullptr;
        friend struct api_core;
        context_view() noexcept = default;
        void bind(T& value) noexcept {
            target = &value;
        }

    public:
        context_view(context_view const&) = delete;
        context_view& operator=(context_view const&) = delete;
        [[nodiscard]] T* operator->() const noexcept {
            return target;
        }
        [[nodiscard]] T* get() const noexcept {
            return target;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return target != nullptr;
        }
    };

    /// Device ownership, capabilities and global control. Public subviews borrow the same core.
    /// Derived constructors bind fully constructed interface implementations before publication.
    struct api_core : object {
        context_view<resource_factory> resources;
        context_view<command_context> commands;
        context_view<frame_context> frames;
        explicit api_core(std::string_view name) noexcept
            : object(name) {
        }
        virtual ~api_core() noexcept = default;
        virtual void release() noexcept = 0;
        [[nodiscard]] virtual ability_bits abilities() const noexcept = 0;
        [[nodiscard]] virtual extension* query_extension(extension_kind kind) noexcept = 0;
        virtual void wait_idle() = 0;
        [[nodiscard]] virtual std::uint32_t api_version() const noexcept = 0;
        [[nodiscard]] virtual main_context* main_context() noexcept {
            return nullptr;
        }

    protected:
        void bind_views(resource_factory& resource, command_context& command, frame_context& frame) noexcept {
            if (resources || commands || frames) {
                deren::utility::fast_panic("RHI core subviews already bound");
            }
            resources.bind(resource);
            commands.bind(command);
            frames.bind(frame);
        }
    };

    /// 在已完成ABI握手的有效对象上检查扩展身份；不能验证悬空指针或不可信后端。
    template <typename ability>
    [[nodiscard]] ability* query_extension(api_core& core) noexcept {
        extension* const result = core.query_extension(ability::extension_id);
        if (result == nullptr || result->kind() != ability::extension_id) {
            return nullptr;
        }
        return static_cast<ability*>(result);
    }

} // namespace deren::promise::rhi
