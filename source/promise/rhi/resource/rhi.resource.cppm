// -*- C++ -*-
// module: deren.promise.rhi:resource
module;

#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:resource;
export import deren.vstd;
export import deren.utility.shared;

import :contract;

export namespace deren::promise::rhi {

    struct image_view;

    /// The width of one index buffer element. APPENDED as its enum (the plan's §2): two values,
    /// because those are the two the renderer's index buffers are built from.
    enum class index_type : std::uint32_t {
        uint16 = 0,
        uint32 = 1,
    };

    /// WHAT A BUFFER IS FOR, as the contract names it: the memory intent, not a Vulkan memory property.
    ///
    /// The vocabulary is the renderer's census of buffer uses (twelve creation sites today, counted
    /// in source/backends/vulkan/runtime, readback, ray_tracing and acceleration_structure), and every value says what
    /// the CALLER does with the buffer - "the host writes it every frame", "only the GPU touches it",
    /// "an acceleration structure lives in it". The backend maps each one to memory properties, tiling and
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
        /// sampled-read access bit for it (the shipped recipe's bit).
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
        /// IMPLEMENTATION's copy-out (`image::get_content()`) or to a read-back. IT IS THE ROLE THE CENSUS
        /// FOUND MISSING ("no HOST-access masks ... stay in the escape bucket, runtime.probes.cppm /
        /// ray_tracing.cpp"), and it is why a probe that records its read-back needed a raw
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
    /// agree with every reader. The channel counts here are the enum's, and a format added to the enum
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

    // ---- RELEASE: WHAT IT IS, AND WHAT IT IS NOT ---------------------------------------------------
    //
    // EVERY handle a factory returns (`buffer`, `image`, `sampler`, `shader`, `pipeline`)
    // is handed out to ONE caller, who calls `release()` on it EXACTLY ONCE. The call runs INSIDE
    // the backend - that is the whole reason it is a
    // virtual instead of the caller reaching for `delete`: the allocator, the registry entry and the
    // count of live handles all stay on the side that created them, and the C ABI never has to hand out an
    // allocator.
    //
    // RELEASE IS NOT NECESSARILY DESTRUCTION, AND THAT IS WHY IT IS NOT CALLED `destroy()`. A backend
    // MAY SERVE THE SAME RESOURCE TO MORE THAN ONE CALLER - this one does it whenever two requests
    // carry identical content, by keeping a content-keyed (XXH3-128) registry and a count of live handles
    // behind these handles (the note on `release()` in source/backends/vulkan/core/vma/vma.cppm) - so `release()`
    // accounts for ONE caller, and the device object dies only when the LAST caller releases. Two consequences a
    // caller has to live with:
    //
    //   - THE CALL MAY FREE NOTHING. A caller that needs the memory back must not infer it from this
    //     call; the backend's bookkeeping decides (`vma_allocator::log_statistics()`, and the
    //     registry sweep in its `destroy()`).
    //   - THE HANDLE IS DEAD AFTERWARDS EVEN IF THE OBJECT LIVES. Touching the pointer again is a
    //     caller bug: this call has already released the caller's one claim, so another `release()` would release
    //     somebody else's.
    //
    // HOW A SECOND HANDLE IS OBTAINED: BY ASKING A FACTORY AGAIN. THE FACTORY IS WHERE A HANDLE COMES
    // FROM - a `create_image()` whose content and parameters match a live object answers with THAT object
    // and a NEW handle, and the caller's single `release()` balances it. There is deliberately NO
    // raw "add a reference to this handle" call would let a caller manufacture
    // handles for objects it never received, which is exactly the over-release path the backend
    // removed when it made its `free`/`add` private and handed out injectable wrappers instead
    // (the note on `release()` in vma_allocator).
    //
    // A VIEW IS NOT A HANDLE FROM A FACTORY, and this is the other half of the release model.
    // `frame_context::frame_image()`, `frame_context::frame_readback_buffer()` and `frame_context::begin_commands()`
    // answer with objects the BACKEND creates and destroys for a window (a frame, or until the next
    // acquire). Those are VIEWS, not factory products: they never came from a factory, and `release()` on one is a
    // lifecycle PRECONDITION violation that TERMINATES (the handle wrappers introduced in abi 41 are
    // what makes "who releases" unambiguous; a caller that reaches a view's release ignored it).
    //
    // A FRAME IMAGE THE BACKEND MAKES FOR ONE WINDOW MAY STILL HAND OUT A FACTORY PRODUCT: `frame_image()->make_view(desc)` answers a new factory
    // product, released like any other. The rule is a lifetime one: a view made over a swapchain image dies
    // with that image, so the caller releases every such view before `swapchain::recreate()` - and the range is
    // the swapchain image's shape (one layer, one mip); anything outside it is REFUSED, not clamped.
    // ----------------------------------------------------------------------------------------------

    /// A buffer the backend creates and destroys; the caller releases it through `release()`.
    /// WHAT KIND OF ACCELERATION STRUCTURE: the two levels the hardware API has, named the contract's way.
    enum class acceleration_structure_type : std::uint32_t {
        bottom_level = 0, ///< one geometry's structure (a BLAS), built from triangles
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
    /// contract's way; the backend asserts the values against its enum (it is the only side that names both).
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
    /// The layout is the API's (the backend asserts it), because a caller fills an array of these.
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
    /// `struct_size` guard is the usual one, and every span is read only until the call returns.
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
    /// THE BACKEND BUILDS AND DESTROYS its SETUP MEMORY (the attributes, the records and the index array it was built from) and the
    /// structure's storage: a caller that has the handle cannot get the alignment or the index array wrong,
    /// which is exactly what the engine used to do by hand.
    struct micromap : object {

        micromap() noexcept = default;
        ~micromap() noexcept override = default;

        /// Release the caller's one handle (see `buffer::release()`): the backend destroys the structure and
        /// gives its memory back.
        void release() noexcept override = 0;
    };

    /// ONE TRIANGLE GEOMETRY of a bottom-level structure: where its vertices and indices are, and how many.    ///
    /// THE ADDRESSES ARE DEVICE ADDRESSES (the contract's `uint64_t`, as `shader_binding_table_region` spells
    /// them), which is what a build reads: a caller that wants an address asks `device_address::buffer_address()`
    /// for the buffer it created, so the engine half never names the driver's `VkDeviceAddress`.
    ///
    /// THE STRUCTURE IS BUILT ONCE FROM THIS (the separate addresses and counts are part identity); a structure
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
        /// ONE HANDLE AND NOTHING ELSE, deliberately: the micromap knows its usage record and carries the index
        /// array the traversal reads (the backend built both), so a caller cannot get either wrong - which is
        /// exactly the pair the engine used to carry across this boundary by hand.
        micromap* opacity_micromap = nullptr;
    };

    /// ONE TOP-LEVEL INSTANCE, as the CALLER writes it. THE LAYOUT IS THE CONTRACT'S, and the backend asserts
    /// it against the driver's structure (the same rule `mesh_task_command` follows): the caller is the one
    /// that fills these records, so their shape cannot live only in one API's header.
    struct acceleration_structure_instance {
        float transform[12] = {};                             ///< 3x4 ROW-major (the fourth column is implicit)
        std::uint32_t instance_custom_index = 0;              ///< what a shader reads back through the hit
        std::uint32_t mask = 0xFFu;                           ///< the visibility mask the traversal tests
        std::uint32_t shader_binding_table_record_offset = 0; ///< which hit record this instance's hits use
        std::uint32_t flags = 0;                              ///< the caller's instance bits
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
        acceleration_structure_geometry const* geometries = nullptr; ///< BOTTOM: referenced until the call returns
        std::uint32_t geometry_count = 0;                            ///< BOTTOM: how many
        std::uint32_t instance_capacity = 0;                         ///< TOP: how many instances must fit
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
    /// WHO ALLOCATES AND FREES THE MEMORY: the BACKEND. This is the whole point of the tier-1 shape - the storage the
    /// structure lives in, the scratch a build needs, its alignment and the per-geometry offsets are the
    /// backend's business, and a caller that never sees them cannot depend on them. A second backend with no
    /// explicit acceleration structures at all can therefore answer `create_acceleration_structure` and
    /// `build_acceleration_structure` however it must.
    ///
    /// ONE HANDLE, released through `release()`, like every factory product in this contract.
    struct acceleration_structure : object {

        acceleration_structure() noexcept = default;
        ~acceleration_structure() noexcept override = default;

        /// Release the caller's one handle (see `buffer::release()`).
        void release() noexcept override = 0;

        /// THE ADDRESS a shader (or an instance record's `structure_reference`) uses to reach this structure.
        /// Zero means the backend could not report one - which a top-level structure never means, since an
        /// address is exactly what an instance stores.
        [[nodiscard]] virtual std::uint64_t device_address() const noexcept = 0;

        /// THE BYTES this structure occupies. A caller needs it to describe the structure to something that
        /// carries address ranges (the descriptor heap's address-range descriptor is the measured case);
        /// zero means the backend did not answer.
        [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;

        /// TOP LEVEL ONLY: replace the instance list the NEXT build reads. Host work on memory the backend
        /// allocates and frees, and the records are `acceleration_structure_instance` - so the caller of a top-level structure
        /// never allocates the array the traversal reads, which is the same release rule as everywhere else
        /// here. Refused by name when the structure is a bottom-level one or the list is longer than the
        /// capacity it was created with.
        [[nodiscard]] virtual error write_instances(std::span<acceleration_structure_instance const> instances) = 0;
    };

    struct buffer : object {

        buffer() noexcept = default;
        ~buffer() noexcept override = default;

        /// RELEASE THE CALLER'S ONE HANDLE. Runs inside the backend, whatever the compiler's `delete`
        /// would have done, and **may free nothing**: the resource can be shared/reused, so the object
        /// dies only when its last caller releases (see the release note above). Do not touch the handle
        /// afterwards. A frame view never came from a factory, so there is nothing to release: callers MUST
        /// NOT invoke release() on it, and the backend MAY terminate on that lifecycle precondition violation.
        void release() noexcept override = 0;

        [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
        /// Direct bytes of a host-visible buffer; empty when it cannot be mapped. Successful host-mapped
        /// intents require no explicit flush/invalidate calls. The span points into the buffer's storage and
        /// follows the lifetime of the buffer and of the frame it was mapped in. Calling mapped() neither waits nor inserts a barrier.
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

    /// An image the backend creates and destroys; the caller releases it through `release()`.
    struct image : object {

        image() noexcept = default;
        ~image() noexcept override = default;

        /// see `buffer::release()`: runs inside the backend; the object may survive
        /// because a backend is allowed to serve identical content from one shared, content-keyed entry.
        void release() noexcept override = 0;

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

        /// Copy a shared handle without consuming the caller's factory handle.
        /// Frame images and backends without shared storage return empty.
        /// The api_core and backend library must outlive every shared handle.
        /// While shared handles exist, repeated calls use the same control block.
        /// Use this instead of constructing an independent `shared_object` from a raw image.
        [[nodiscard]] virtual shared_object<image> share() {
            return {};
        }
    };

    /// A view of an image, created by the backend: the contract's substitute for a raw `VkImageView`.
    /// Backends that share parent state keep an independent handle to the image alongside the factory handle.
    /// Views of frame images keep no parent valid and must respect the frame lifetime.
    /// No native view crosses the contract (the escape's `native_image_view` was deleted in abi 42).
    struct image_view : object {

        image_view() noexcept = default;
        ~image_view() noexcept override = default;
        /// Release the view; the backend destroys the native view and then the parent handle it kept.
        void release() noexcept override = 0;
        /// Copy the parent handle; empty for frame images or unsupported backends.
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
        char const* debug_name = nullptr;         ///< what the backend logs on refusal; not copied by the backend
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
    /// the ENGINE's sampler set needs NEAREST minification (the G-buffer's stored surface is read at exact
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
    struct sampler : object {

        sampler() noexcept = default;
        ~sampler() noexcept override = default;
        /// see `buffer::release()`
        void release() noexcept override = 0;
    };

    /// How many sampled slots one immutable texture group carries (`texture_group_info::textures`).
    inline constexpr std::uint32_t texture_group_capacity = 16u;

    struct texture_group_info {
        std::uint32_t struct_size = sizeof(texture_group_info);
        deren::utility::vessel::array<std::optional<image*>, texture_group_capacity> textures{};
    };
    /// Immutable sampled 2D slots. The device and backend must outlive every `texture_group`.
    struct texture_group : object {
        texture_group() noexcept = default;
        ~texture_group() noexcept override = default;
        void release() noexcept override = 0;
    };
    using texture_group_ref = shared_object<texture_group>;

    /// Immutable composition: texture_group is one member kind, not the general binding container.
    /// `resource` must be a shared handle of the kind declared (a factory product or an alias). Buffer size 0 means
    /// the remainder after offset. View selects an image subresource. Spans are read until return.
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
    /// The group keeps its members valid; recorded/pending commands keep the group valid. Core/backend must
    /// outlive every group. Immutability covers bindings, not resource content or synchronization.
    struct resource_group : object {
        resource_group() noexcept = default;
        ~resource_group() noexcept override = default;
        void release() noexcept override = 0;
    };
    using resource_group_ref = shared_object<resource_group>;
    /// Resource creation interface reached by pointer. Empty products mean creation was refused/failed.
    /// Descriptors are read until return; products follow their existing release rules.
    /// Shared immutable payload. Other aliases must not mutate bytes during the resource lifetime.
    using lazy_data = std::shared_ptr<deren::utility::vessel::vector<std::uint8_t> const>;
    enum class lazy_state : std::uint32_t { unloaded,
                                            loading,
                                            ready,
                                            failed,
    };
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
            desc.data->size() != desc.resource.size) {
            return error::invalid_argument;
        }
        auto const usage = desc.resource.usage;
        if (usage == buffer_usage::vertex || usage == buffer_usage::index || usage == buffer_usage::uniform_gpu_only ||
            usage == buffer_usage::storage_gpu_only) {
            return error::ok;
        }
        return error::unsupported;
    }
    struct lazy_buffer : buffer {
        using buffer::buffer;
        [[nodiscard]] virtual lazy_state state() const noexcept = 0;
        /// Nonblocking: not_ready while loading, or while a view, binding, or recording still needs the native object.
        /// Source and metadata survive; the next native demand reloads the initial bytes.
        [[nodiscard]] virtual error unload() noexcept = 0;
    };
    struct lazy_image : image {
        using image::image;
        [[nodiscard]] virtual lazy_state state() const noexcept = 0;
        [[nodiscard]] virtual error unload() noexcept = 0;
    };

} // namespace deren::promise::rhi
