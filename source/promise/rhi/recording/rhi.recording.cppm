// -*- C++ -*-
// module: deren.promise.rhi:recording
module;

#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:recording;
export import deren.vstd;
export import deren.utility.shared;

import :contract;
import :extension;
import :pipeline;
import :resource;

export namespace deren::promise::rhi {

    // ================================================================================================
    // THE RECORDING FACE'S DESCRIPTORS. They are plain contract
    // PODs: ADDING THEM CHANGES NO INTERFACE, so no abi bump follows from this block - the verbs that
    // take them are the interface change and land with the backend that
    // translates them.
    //
    // WHAT THE VOCABULARY DELIBERATELY DOES NOT CARRY, each one a MEASURED verdict rather
    // than an omission: no HOST-ACCESS masks (the two host-visible barrier sites were raw through the
    // escape when this block was written, runtime.probes.cppm / ray_tracing.cpp - the `host_read` roles
    // carry them now), no QUEUE-FAMILY handover field (there
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

    /// Which role a BUFFER is used in. APPENDED as A separate type (the plan's §3), and SMALL ON
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
        /// records the spec's ordering barrier between them, whose raw masks are
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
    /// is the ONLY global barrier in the engine: a second one should be added here only with its
    /// measurement, the way the roles above were.
    struct memory_barrier {
        buffer_use from = buffer_use::undefined;
        buffer_use to = buffer_use::undefined;
    };

    /// WHICH SHADER STAGE a pair's shader side belongs to, for the sites whose producer or consumer is NOT the
    /// stage the shipped recipe names.
    ///
    /// ADDED WITH ITS MEASUREMENT (abi 21), which is what the barrier model's note demands: the 20
    /// transition recipes name the stage their shader side runs at (the storage-image recipes say
    /// COMPUTE_SHADER), and the ray-traced shadow's visibility image is written by a traceRays LAUNCH rather
    /// than by a dispatch - a barrier whose masks do not cover the stage that actually ran leaves the writes
    /// unsynchronised, and the measured symptom was "half the model lost its sun". The pair is still the
    /// vocabulary (WHICH WAY the image moves); this says only WHICH STAGE did it, so it does not re-open the
    /// question the pair answers.
    ///
    /// `none` IS THE DEFAULT AND THE OVERWHELMING CASE: every site but one uses the recipe's stages, and a
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
    /// neither, which is why the batch carries it in its span (below) and why the backend records it as a
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
    /// so a later addition is additive rather than an abi break. The spans are read for the call
    /// only, the same rule every other array in this contract follows.
    struct barrier_group {
        std::uint32_t struct_size = sizeof(barrier_group);
        std::span<image_barrier const> images = {};
        std::span<buffer_barrier const> buffers = {};
        /// WHICH STAGE the images' shader side is (see `stage_hint`); `none` = the recipes' stages. IT
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

    /// Which triangle winding a pipeline culls. APPENDED as its enum: the four spellings every
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
    /// Stable ascending uint32 key/value sort. Operands must stay valid until GPU completion.
    /// Caller publishes writes before record and publishes output reads afterward.
    /// Internal native bindings may be overwritten; logical recorder state is preserved for later RHI consumers.
    struct radix_sorter : object {
        radix_sorter() noexcept = default;
        ~radix_sorter() noexcept override = default;
        void release() noexcept override = 0;
        [[nodiscard]] virtual radix_sort_requirements requirements(std::uint32_t count) const noexcept = 0;
        [[nodiscard]] virtual error record(command_recorder&, radix_sort_info const&) = 0;
    };

    // ---- THE COMMAND BUFFER: THE RECORDING HANDLE THE CALLER CREATES (abi 15) ----------------------
    //
    // `command_buffer` used to be a view of a recording with no `release()` at all (abi
    // 1/2/14); what the contract never had was the RESOURCE behind it - a command buffer the caller can
    // create, keep, begin, end and execute, which is what the engine's per-slot primaries, its
    // per-cascade and per-worker secondaries and the read-back's one-shot buffer need. Since abi 15
    // `command_buffer` IS that resource, in the shape `buffer` / `image` established: the
    // factory hands out ONE handle, `release()` runs inside the backend, and
    // `rhi::unique_object<T>` is the wrapper spelling. The frame's list is still the ONE instance of the
    // type that belongs to the frame (`begin_commands()` answers it; its `release()` panics).
    //
    // WHAT DOES NOT CROSS THIS BOUNDARY, and it is the decision this type rests on: the ALLOCATOR. A
    // command buffer is allocated from an API-specific pool/allocator whose lifetime is entangled with
    // GPU progress (it may be reset only once the device is done with it), and neither Vulkan's
    // `VkCommandPool` nor D3D12's allocator nor Metal's queue is a portable concept. This backend's
    // wrapper carries its POOL as a member (source/backends/vulkan/core/handles/handles.cppm), so the one lifetime rule is the
    // handle's and no allocator, pool or reset verb appears here; a backend that needs one keeps
    // it behind `begin_recording()` / `release()`.
    // ----------------------------------------------------------------------------------------------

    /// HOW A COMMAND BUFFER IS USED (abi 15).
    ///
    /// A PRIMARY buffer is handed to the device as a unit of work; it is the ONLY role a caller can
    /// create in abi 37 (the secondary role is retired - see the VALUE below). The multithreaded
    /// recording paths record into `mergeable_command_buffer` segments instead and merge them in
    /// batches; those segments are contract separate handles (abi 35), handed out by
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
    /// than dropping it, which is the rule the heap requests already follow ("never silently drop the chain").
    ///
    /// `struct_size` is the same ABI guard `command_buffer_desc` carries; `next` is read only
    /// until the call returns.
    /// Attachment compatibility for a secondary recording. Formats use RHI values;
    /// `depth` requests the backend's chosen depth format, `unknown` means no depth.
    /// Read only until begin_recording returns. Appending a tagged POD changes no vtable.
    struct command_buffer_inheritance_info {
        static constexpr std::string_view tag = "deren.rhi.structure.command_buffer_inheritance";
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
        /// secondary's begin. The engine's segments live on the heap face and want it (the default); a
        /// THIRD-PARTY recorder that binds its classic descriptor sets - the Dear ImGui Vulkan backend is
        /// the measured case - must NOT inherit a heap bind, or every a separate one binds trips
        /// VUID-...-11295/11296 inside the continuation.
        bool inherit_descriptor_heaps = true;
    };

    /// ONE INDIRECT MESH-TASK RECORD: the three group counts, and nothing else.
    ///
    /// WHY THE LAYOUT IS THE CONTRACT'S: `command_buffer::draw_mesh_tasks_indirect()` takes a STRIDE, and the
    /// caller that fills the argument buffer is the one that has to know what a record looks like. Leaving that
    /// shape to the caller put the driver's structure (`VkDrawMeshTasksIndirectCommandEXT` - which the
    /// engine named both to size its buffer and to write its slot) in the engine half, so the shape lives here,
    /// where both halves can name it. The backend that serves the verb static_asserts its structure against
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
    /// called it and the lane refused recording). Same alignment/overflow contract, its minimum stride.
    [[nodiscard]] constexpr error validate_mesh_task_indirect_range(std::uint64_t size, std::uint64_t offset, std::uint32_t count,
                                                                    std::uint32_t stride) noexcept {
        return validate_indirect_range(size, offset, count, stride, mesh_task_command_size);
    }

    /// The batch-only recording handle (defined after `command_buffer`): forward-declared here so the
    /// recording interface can name the batch its `execute()` merges.
    struct mergeable_command_buffer;
    /// The segment's shared handle: a `shared_object` over the batch-only handle (abi 44).
    using mergeable_command_buffer_ref = shared_object<mergeable_command_buffer>;

    /// Shared recording vocabulary (ABI 49). Every recording handle exposes these verbs directly.
    /// This base carries no individual submission or batch execution permission.
    struct command_recorder : object {

        command_recorder() noexcept = default;
        ~command_recorder() noexcept override = default;

        /// RELEASE THE CALLER'S ONE HANDLE (see `buffer::release()`): the recording session and the
        /// allocation behind it die when the last caller releases, inside the backend.
        void release() noexcept override = 0;

        /// Open this buffer's recording session.
        ///
        /// `ok` = recording; `not_ready` = this handle has already been released, or the backend's
        /// recording context is not available; `unsupported` = a usage bit or a chain entry this
        /// backend cannot serve (REFUSED BY NAME, never silently dropped); device-level failures
        /// (`device_lost`, `out_of_*_memory`) travel as themselves.
        [[nodiscard]] virtual error begin_recording(command_buffer_begin_info const& info) = 0;

        /// Close the recording session: the buffer becomes executable by `command_context::submit()`. A
        /// `mergeable_command_buffer` segment becomes executable through `execute(span)` instead.
        [[nodiscard]] virtual error end_recording() noexcept = 0;

        /// Apply pending CPU operations in order, preserving final state for later recording.
        /// end_recording applies automatically. Deferred errors make the recording non-executable.
        /// Pipelines and buffers the caller binds must stay valid through pending recording AND GPU completion.
        [[nodiscard]] virtual error apply_operations() noexcept {
            return error::unsupported;
        }

        // ---- THE RECORDING SERIES, NOW ON THE HANDLE (user's ruling) ------------------------------
        // command_list is GONE: the recording verbs live on the one handle the caller has, so a
        // call site is `buffer->draw(...)` / `->barrier(...)` with no view in between. The
        // non-virtual forwarders this type used to carry are gone with it: these ARE the series.

        /// Declare that `resource` moves from one role to the other, and let the backend record what that
        /// needs. THE PAIR, not a single use, is deliberate: this is plan §6.4 option (a) ("explicit
        /// barrier") spelled in option (c)'s vocabulary - the caller knows what it just did (it recorded
        /// it), the backend knows what the pair costs. A single-use form cannot be derived here: the
        /// barrier describes what the CALLER just did, and the backend cannot know the "from" of a
        /// recording it was not told about.
        ///
        /// THE CORRECTNESS IS THE CALLER'S: a missing pair, or a reversed one, is a WRONG barrier - the
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
        // timestamp resolves at is a MEASUREMENT detail of the backend that allocates the query pool, not
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
        // THE RECORDING SURFACE'S VOCABULARY: the verbs this renderer's passes actually record,
        // replacing 140 raw `vkCmd*` call sites in the engine's sources. Declared ONCE on
        // `command_recorder`, which every recording handle wraps; each kind of handle keeps separate submit rules. Shader parameter pushes
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
        /// Validates before native recording; leaves the group valid through recorded and pending GPU use.
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
        /// refuse explicitly; the default operation copies no bytes and stores no pointer.
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
        /// APPENDED IN ABI 24 (a tier-1 slot). The regions are the contract's
        /// `shader_binding_table_region`; the verb answers NOTHING (`void`), like `draw` and `draw_mesh_tasks`, and
        /// a backend whose device published no `vkCmdTraceRaysKHR` records nothing - a pass that launches rays is
        /// only built when its pipeline could be created, which needs that extension.
        virtual void trace_rays(shader_binding_table_region const& raygen, shader_binding_table_region const& miss, shader_binding_table_region const& hit,
                                shader_binding_table_region const& callable, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept = 0;

        /// APPENDED IN ABI 26: record the BUILD of @p target - its geometries (a bottom-level structure) or its
        /// instance list (a top-level one), whichever it was created with, over the memory the backend allocates.
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
        /// them in, and the ordering this verb enforces (the barrier is the backend's, as it is for a build).
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

        /// ABI 31. Success leaves the group valid for recorded work; nullptr clears it.
        [[nodiscard]] virtual error load_texture_group(texture_group_ref const& /*group*/) {
            return error::unsupported;
        }
    };

    /// Individually submittable handle and receiver of mergeable batches.
    struct command_buffer : command_recorder {
        command_buffer() noexcept = default;
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

    /// Batch-only handle: ordered, self-contained segments valid until completion.
    /// Sibling of command_buffer; cannot receive execute() or be submitted individually.
    struct mergeable_command_buffer : command_recorder {
        mergeable_command_buffer() noexcept = default;
        ~mergeable_command_buffer() noexcept override = default;
    };
    struct command_context {
        command_context() noexcept = default;
        virtual ~command_context() noexcept = default;
        command_context(command_context const&) = delete ("command_context is an interface reached by pointer; use the pointer the runtime supplies");
        command_context& operator=(command_context const&) = delete ("command_context is an interface reached by pointer; use the pointer the runtime supplies");
        [[nodiscard]] virtual unique_object<command_buffer> create_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual shared_object<command_buffer> make_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual mergeable_command_buffer_ref make_mergeable_command_buffer(command_buffer_desc const& desc) = 0;
        [[nodiscard]] virtual error submit(command_buffer& commands) = 0;
        [[nodiscard]] virtual error submit(std::span<mergeable_command_buffer_ref const> batch) = 0;
    };

} // namespace deren::promise::rhi
