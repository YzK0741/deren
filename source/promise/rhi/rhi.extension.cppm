// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:extension
//
// tier-2 of the promise contract: the abilities a backend may or may not have
// beyond the portable tier-1 surface (RHI plan v4, §1.8, §1.9, §1.11, §3.4, §3.5).
//
// The split is the plan's answer to "should the base class keep DX12 in mind?":
// `rhi::api_core` carries only what DX12 also has, and everything a backend
// may lack lives here, announced as one bit in `api_core::abilities()`. Nothing an
// ability does may be needed by the portable path: a pass that needs one declares
// it with `required_capabilities()`, and a backend that does not have it is a
// NAMED failure at startup, not a silent downgrade (§1.9, §3.6).
//
// Three properties make this the shape the plan chose:
//
//   1. `abilities()` returns a BITMASK and one `extension_kind` value is one bit,
//      so the engine learns everything without a virtual call per ability, and the
//      consistency gate can iterate the bits: for every set bit, query_extension()
//      must return the matching usable object. No additional C export is required.
//
//      A BIT IS ANNOUNCED ONLY IF THE ABILITY OBJECT EXISTS **AND** CAN SERVE THE OBJECTS THIS
//      BACKEND PRODUCES: `query_extension(kind)` must answer, and every operation the ability
//      declares must be performable on something the backend can hand out. A device-level fact
//      ("this device is 1.2, so bufferDeviceAddress exists") is not an ability until there is a
//      `buffer` to ask about - the strict form of "置位 ⇒ 取得到 且 用得上".
//   2. Every ability derives from `extension`, whose only virtual is `kind()`.
//      Adding a sixth ability therefore cannot disturb the vtable of the other
//      five, and `query_extension()` stays a one-line lookup in the backend.
//   3. These interfaces carry only the common immutable identity (ABI12), no owning state, no non-inline definition, no
//      `deren::utility::vessel::string`, no exception across the boundary (§4.2). The engine and the
//      backend each compile this partition; neither exports a module symbol for it.
//
// 能力入口只有 query_extension；这些虚接口沿用同工具链的 C++ ABI。
// 不再增加一套 deren_ext_* C 函数表；该边界不承诺供非 C++ 宿主使用。
// ============================================================================
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

export module deren.promise.rhi:extension;
export import deren.utility.shared;
export import deren.promise.rhi.synchronization;

import :contract;

/**
 * @file source/promise/rhi/rhi.extension.cppm
 * @brief tier-2 of the promise contract: the abilities a backend may or may not have.
 * @ingroup promise
 *
 * DX12 does not have every capability Vulkan reaches through an extension (and a GL backend has
 * fewer still), so "the base class keeps DX12 in mind" cannot mean "the base class has everything"
 * (§1.5). What DX12 also has stays in `deren::promise::rhi::api_core`; the rest is declared here, one
 * class per ability, and announced as one bit in `api_core::abilities()`.
 *
 * The ability objects are obtained through query_extension() after the core ABI handshake;
 * the consistency gate checks reported bits against those objects, without extra C exports.
 */

export namespace deren::promise::rhi {

    // The tier-1 objects the abilities below take by reference. They are declared by
    // source/promise/rhi/rhi.api_core.cppm: an ability is a tier-2 view of the same backend, so it
    // speaks about the same objects.
    struct api_core;
    struct radix_sorter;
    struct image_view_desc;
    struct descriptor_heap_properties;
    struct heap_bindings;
    struct heap_image_write_info;
    struct heap_buffer_write_info;
    struct heap_bind_info;
    struct heap_push_info;
    struct buffer;
    struct command_buffer;
    struct image;
    struct image_view;
    struct texture_group;
    struct mergeable_command_buffer;
    struct sampler;
    struct pipeline;
    struct shader;

    // Descriptor shapes that are still S1 design surface (§5's capability table and
    // §6.4's usage/barrier decisions). A reference to an incomplete type is all a
    // virtual declaration needs, which is what lets the abilities be reviewed before
    // the shapes they carry are frozen.
    // `acceleration_structure` AND `acceleration_structure_desc` WERE FORWARD-DECLARED HERE, for the
    // `ray_tracing` ability's three acceleration-structure verbs. Both moved to TIER-1 in abi 26 (they are
    // `rhi.api_core` furniture now, next to `buffer` and `image`), so this file needs neither name - and
    // the ability that carried them is retired where its bit is defined.
    struct image_copy_region;

    /// One bit per ability a backend can report through `api_core::abilities()`.
    ///
    /// The bit values are the contract: the consistency gate queries an object
    /// per set bit, so a value must never move and a new
    /// ability takes the next free bit. Zero means "no abilities" and is spelled
    /// `no_abilities`.
    enum class extension_kind : std::uint32_t {
        device_address = 1u << 0,  ///< buffer/acceleration-structure addresses (plan §5: 35 mentions, 9 files)
        descriptor_heap = 1u << 1, ///< VK_EXT_descriptor_heap: push data, write descriptors, bind heaps
        mesh_shader = 1u << 2,     ///< vkCmdDrawMeshTasksEXT and its indirect form
        ray_tracing = 1u << 3,     ///< RETIRED in abi 26: the acceleration-structure half became TIER-1 (`acceleration_structure` + the recording verbs), and "can this device trace rays" is answered by `device_capabilities`. The BIT stays (never reused).
        host_image_copy = 1u << 4, ///< vkCopyImageToMemoryEXT
        vulkan_escape = 1u << 5,   ///< the frame command buffer a third-party renderer (the GUI overlay) records into
        /// APPENDED (a new BIT; the six before it do not move - see the static_assert below): WHAT THE DEVICE
        /// CAN DO, in the contract's own vocabulary. A backend announces it when it can answer every method,
        /// the same "a set bit is a promise about service" rule the others live by. The engine used to derive
        /// these facts ITSELF - `vkGetPhysicalDeviceFeatures2` for the two features,
        /// `vkGetPhysicalDeviceProperties` for the push-constant limit, `vkGetPhysicalDeviceQueueFamilyProperties`
        /// + `vkGetDeviceQueue` to recover the graphics queue's family, and a second properties query for the
        /// acceleration-structure numbers - while the BACKEND had already made the same queries to decide what
        /// to enable. Asking it is both the smaller code and the only shape a second backend can serve.
        device_capabilities = 1u << 6,
        gpu_sorting = 1u << 8,             ///< stable ascending uint32 key/value radix sort
        shader_group_access = 1u << 7,     ///< portable SBT group data for a pipeline
        async_presentation = 1u << 9,      ///< snapshot and enqueue presentation without blocking the caller
        scoped_synchronization = 1u << 10, ///< explicit stage/access scopes and bound resource ranges
        frame_resources = 1u << 11,        ///< immutable acquired-image snapshots for scoped recording
        /// APPENDED (a new BIT; the twelve before it do not move - see the static_assert below): THE
        /// LOGICAL DESCRIPTOR SPACE a backend publishes descriptors into, seen as positions rather than
        /// byte offsets. It is the servable face of the `descriptor_heap` detail: the heap answers
        /// capacities/strides, this answers "where an immutable texture group lives" (`descriptor_position`)
        /// and "write one sampled descriptor there portably" (`descriptor_write`). Announced only when the
        /// backend can serve a texture group - the object the position is about - the same rule the other
        /// bits live by.
        descriptor_space = 1u << 12,
    };

    /// What `api_core::abilities()` returns: a set of `extension_kind` bits.
    using ability_bits = std::uint32_t;

    /// The empty set, spelled out where the engine compares a returned bitmask.
    inline constexpr ability_bits no_abilities = 0u;

    /// The same value as a bitmask, for use in `abilities()` implementations.
    [[nodiscard]] constexpr auto to_bits(extension_kind kind) noexcept -> ability_bits {
        return static_cast<ability_bits>(kind);
    }

    /// Whether a backend that reported `abilities` has this ability.
    [[nodiscard]] constexpr auto has_ability(ability_bits abilities, extension_kind kind) noexcept -> bool {
        return (abilities & to_bits(kind)) != no_abilities;
    }

    /// Every bit defined here: what a backend with all twelve abilities reports, and the
    /// set the plan's §8 consistency gate iterates.
    [[nodiscard]] constexpr auto all_abilities() noexcept -> ability_bits {
        // `extension_kind::ray_tracing` IS NOT HERE ANY MORE (abi 26): its whole shape moved to tier-1, so no
        // bit of the retired kind is defined. The ENUMERATOR stays (bits are never reused), which is why this
        // set is no longer "every enumerator" - the note on that enumerator says so.
        return to_bits(extension_kind::device_address) | to_bits(extension_kind::descriptor_heap) |
               to_bits(extension_kind::mesh_shader) |
               to_bits(extension_kind::host_image_copy) | to_bits(extension_kind::vulkan_escape) |
               to_bits(extension_kind::device_capabilities) | to_bits(extension_kind::shader_group_access) | to_bits(extension_kind::gpu_sorting) | to_bits(extension_kind::async_presentation) | to_bits(extension_kind::scoped_synchronization) | to_bits(extension_kind::frame_resources) | to_bits(extension_kind::descriptor_space);
    }

    /// The same twelve, as a LIST: what a gate walks to check one bit at a time.
    ///
    /// `all_abilities()` is the set as a bitmask; this is the enumeration the consistency gates and
    /// the tests iterate (`abilities()` set => `query_extension()` non-null, and the reverse), so the
    /// list is spelled in ONE place instead of per caller.
    [[nodiscard]] constexpr auto all_extension_kinds() noexcept -> deren::utility::vessel::array<extension_kind, 12> {
        return {
            extension_kind::device_address,
            extension_kind::descriptor_heap,
            extension_kind::mesh_shader,
            extension_kind::host_image_copy,
            extension_kind::vulkan_escape,
            extension_kind::device_capabilities,
            extension_kind::shader_group_access,
            extension_kind::gpu_sorting,
            extension_kind::async_presentation,
            extension_kind::scoped_synchronization,
            extension_kind::frame_resources,
            extension_kind::descriptor_space,
        };
    }

    static_assert(to_bits(extension_kind::device_address) == 0x1u, "the ability bits are ABI: they do not move");
    static_assert(all_abilities() == 0x1FF7u, "twelve active abilities; retired bits are never reused");

    /// The common root of the tier-2 abilities.
    ///
    /// `kind()` is the only virtual every ability shares, and that is the point: the
    /// engine asks `query_extension()` for what it wants and then `static_cast`s to
    /// that ability's type. It does NOT use `dynamic_cast`: the repository builds with
    /// `-fno-rtti`, and it would not be needed anyway - the caller asked for a
    /// specific kind, so it already knows the type (plan §3.3).
    struct extension : object {
    protected:
        explicit extension(std::string_view const name) noexcept
            : object(name) {
        }

    public:
        virtual ~extension() noexcept = default;
        [[nodiscard]] virtual extension_kind kind() const noexcept = 0;
    };

    /// Snapshots pin the core, but the acquire/submit/resize protocol still owns native image lifetime.
    /// Scoped describe/record rejects a snapshot once its acquire epoch ends; retain_image is empty outside a frame.
    struct frame_resources : extension {
        static constexpr extension_kind extension_id = extension_kind::frame_resources;
        explicit frame_resources(std::string_view name) noexcept
            : extension(name) {
        }
        extension_kind kind() const noexcept final {
            return extension_id;
        }
        [[nodiscard]] virtual std::shared_ptr<image> retain_image() noexcept = 0;
    };
    struct scoped_image_info {
        uint32_t mip_levels = 0, array_layers = 0;
        synchronization::aspect aspects = static_cast<synchronization::aspect>(0);
        uint32_t usage_mask = 0;
        uint64_t canonical_object = 0; // Stable allocation identity across wrappers, shared by image/buffer domain.
    };
    struct scoped_buffer_info {
        uint64_t byte_size = 0;
        uint32_t usage_mask = 0;
        uint64_t canonical_object = 0; // Stable allocation identity across wrappers, shared by image/buffer domain.
    };
    struct scoped_image_barrier {
        image const* resource = nullptr;
        synchronization::image_transition transition;
    };
    struct scoped_buffer_barrier {
        buffer const* resource = nullptr;
        synchronization::buffer_transition transition;
    };
    /// A new optional interface; existing command-buffer vtables and role-pair PODs do not change.
    struct scoped_synchronization : extension {
        static constexpr extension_kind extension_id = extension_kind::scoped_synchronization;
        explicit scoped_synchronization(std::string_view name) noexcept
            : extension(name) {
        }
        extension_kind kind() const noexcept final {
            return extension_id;
        }
        [[nodiscard]] virtual synchronization::stage supported_stages() const noexcept = 0;
        /// Describe only objects owned by this core. Foreign objects are rejected before casting.
        [[nodiscard]] virtual error describe(image const&, scoped_image_info&) const noexcept = 0;
        [[nodiscard]] virtual error describe(buffer const&, scoped_buffer_info&) const noexcept = 0;
        /// Validate every entry without recording any commands.
        [[nodiscard]] virtual error validate(std::span<scoped_image_barrier const>, std::span<scoped_buffer_barrier const>) const noexcept = 0;
        /// Validate the complete group, then record it atomically. Empty groups still check command ownership/state.
        [[nodiscard]] virtual error record(command_buffer&, std::span<scoped_image_barrier const>, std::span<scoped_buffer_barrier const>) const noexcept = 0;
    };

    /// tier-2 ability: where a buffer lives on the device.
    ///
    /// A backend without it cannot serve `vkGetBufferDeviceAddress`-style rendering; a backend with it
    /// (DX12 has the same concept, GL does not) answers the call.
    ///
    /// IT DECLARES BUFFER ADDRESSES AND NOTHING ELSE, and that split is deliberate rather than a
    /// narrowing: an ACCELERATION STRUCTURE ADDRESS BELONGS TO `ray_tracing`, because it takes an
    /// `acceleration_structure` and only that ability can produce one. While the two shared this
    /// interface the bit could not be announced at all - the rule at the top of this file ("every
    /// operation the ability declares must be performable on something the backend can hand out") held
    /// it hostage to a resource the backend could not yet make. One ability per operand stays honest;
    /// bundling an unreachable operand with a reachable one gets the bit announced never, or wrongly.
    /// Optional presentation overlap. begin() snapshots the submitted image before returning.
    /// Collect the result before submitting another frame. wait_idle() and teardown drain calls.
    /// Completion means the host call returned, not that the GPU or display is idle.
    struct async_presentation : extension {
        static constexpr extension_kind extension_id = extension_kind::async_presentation;
        explicit async_presentation(std::string_view type) noexcept
            : extension(type) {
        }
        extension_kind kind() const noexcept override {
            return extension_kind::async_presentation;
        }
        virtual std::shared_future<error> begin() = 0;
    };

    struct gpu_sorting : extension {
        static constexpr extension_kind extension_id = extension_kind::gpu_sorting;
        explicit gpu_sorting(std::string_view name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// Returns one owning reference, or null on unsupported/allocation failure.
        [[nodiscard]] virtual radix_sorter* create_radix_sorter() = 0;
    };

    struct device_address : extension {

        static constexpr extension_kind extension_id = extension_kind::device_address;
        explicit device_address(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// The device address of `resource`, `offset` bytes into it; 0 when the buffer was not created
        /// with `buffer_flag::device_address` (the caller asked for no address, so there is none to give).
        [[nodiscard]] virtual std::uint64_t buffer_address(buffer const& resource, std::uint64_t offset) const noexcept = 0;
    };

    /// Backend-specific SBT record bytes queried through a portable pipeline operand.
    /// Record size comes from device_capabilities::shader_binding_table(). Zero groups,
    /// out-of-range groups, insufficient output and foreign/non-RT pipelines are invalid_argument.
    struct shader_group_access : extension {

        static constexpr extension_kind extension_id = extension_kind::shader_group_access;
        explicit shader_group_access(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        [[nodiscard]] virtual error read(pipeline const& resource, std::uint32_t first_group,
                                         std::uint32_t group_count, std::span<std::uint8_t> out) const noexcept = 0;
    };

    /// 可选的bindless heap服务；使用通用资源/地址/命令语义。原生参数只能显式放在next中。
    /// 不承诺所有后端支持；广播此能力必须提供可用实现，不能用空操作冒充。
    struct descriptor_heap : extension {

        static constexpr extension_kind extension_id = extension_kind::descriptor_heap;
        explicit descriptor_heap(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        [[nodiscard]] virtual bool ready() const noexcept = 0;
        [[nodiscard]] virtual descriptor_heap_properties properties() const noexcept = 0;
        [[nodiscard]] virtual heap_bindings bindings() const noexcept = 0;
        [[nodiscard]] virtual error write_image(heap_image_write_info const& info) noexcept = 0;
        [[nodiscard]] virtual error write_buffer(heap_buffer_write_info const& info) noexcept = 0;
        [[nodiscard]] virtual error bind(heap_bind_info const& info) const noexcept = 0;
        [[nodiscard]] virtual error push_data(heap_push_info const& info) const noexcept = 0;
    };

    /// ONE SHADER-BINDING-TABLE REGION: where a ray-tracing launch reads one table's records, how many bytes
    /// that table spans, and how far apart consecutive records are.
    ///
    /// WHY THIS IS A GENERAL RHI TYPE RATHER THAN A BACKEND STRUCT: the three numbers are the whole of what a
    /// launch must be told about a table, and every API in this family has a spelling of them - Vulkan's
    /// `VkStridedDeviceAddressRegionKHR` is one, and it is the type this was promoted FROM (the engine's
    /// ray-traced shadow pass held four of those as native members, which put a Vulkan type in a pass's own state
    /// for no reason: the DATA is a device range, not a driver structure). A region is also the unit the CALLER
    /// BUILDS - the records come from a pipeline's shader-group handles, which only the pipeline's creator can
    /// read back - so it has to travel through the contract's vocabulary like every other descriptor.
    ///
    /// IT LIVES IN THIS PARTITION (not `:api_core`) because `:extension` is the module's VOCABULARY partition -
    /// the one `:api_core` imports rather than the other way round - the same reason `descriptor_type` and the
    /// heap write PODs are here. Its consumers are the recording face's `command_buffer::trace_rays` (abi 24) and
    /// the pass that builds the table; the `ray_tracing` ability's own copy of the launch was MOVED to the
    /// recording face rather than kept here (see that verb's note).
    ///
    /// AN ALL-ZERO REGION (`address == 0`) IS THE "NO RECORDS" SPELLING, and it is the honest one for a table a
    /// given pipeline has no shaders for: a caller of `command_buffer::trace_rays` passes it rather than a null
    /// pointer, and the backend decides what a launch does with it (Vulkan DEREFERENCES the region pointer, so
    /// "empty table" is a zeroed region, never nullptr).
    ///
    /// FROZEN LIKE THE OTHER PODs ONCE SHIPPED (`image_copy_region`, `submit_info`): it is passed BY VALUE, so a
    /// field addition moves `abi_version`.
    struct shader_binding_table_region {
        std::uint64_t address = 0; ///< device address of this region's first record (0 = no records)
        std::uint64_t size = 0;    ///< bytes this region spans
        std::uint64_t stride = 0;  ///< bytes between consecutive records (the device's own alignment asks for it)
    };

    /// THE THREE DEVICE FACTS A SHADER BINDING TABLE IS BUILT FROM: how many bytes one shader-group handle is,
    /// how far apart consecutive handles are in the table, and how the table itself must be aligned.
    ///
    /// IT TRAVELS WITH THE REGION TYPE and for the same reason: a pass that builds a table needs the numbers, and
    /// it has no physical device to ask (the pass layer deliberately holds none) - so the SESSION carries them,
    /// exactly as it carries `swap_chain_image_format` and `depth_format`. Vulkan spells them inside
    /// `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`; naming that structure in a pass is what this type
    /// removes, and it is what let the ray-traced shadow pass stop including a Vulkan header at all.
    ///
    /// ZEROED IS THE "NO RAY TRACING" SPELLING: a device without the pipeline extension answers zeros, which the
    /// one caller reads as "build no table" (its own check, because zero is a legal value in no other sense).
    struct shader_binding_table_properties {
        std::uint32_t handle_size = 0;      ///< bytes per shader-group handle
        std::uint32_t handle_alignment = 0; ///< bytes between consecutive handles in the table
        std::uint32_t base_alignment = 0;   ///< the table's own alignment
    };

    /// tier-2 ability: mesh and task shaders.
    struct mesh_shader : extension {

        static constexpr extension_kind extension_id = extension_kind::mesh_shader;
        explicit mesh_shader(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// Record `groups_x` x `groups_y` x `groups_z` mesh workgroups into `commands`
        /// (plan §5: 19 `vkCmdDrawMeshTasksEXT` calls plus 4 indirect ones).
        virtual void dispatch_mesh(command_buffer& commands, std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) = 0;
    };

    // ---- `struct ray_tracing : extension` STOOD HERE, AND IT IS RETIRED (abi 26) ---------------------
    //
    // IT CARRIED THREE ACCELERATION-STRUCTURE VERBS whose operands (`acceleration_structure` /
    // `acceleration_structure_desc`) were never more than FORWARD DECLARATIONS in this file - so the ability
    // announced a service no backend could implement, and its own note admitted it ("the backend still does
    // not serve the verb"). The launch left first (abi 24, it is a recording command); now the rest has:
    //
    //   * `acceleration_structure` is TIER-1 FURNITURE (`rhi.api_core`, next to `buffer` and `image`), with
    //     `api_core::create_acceleration_structure()` and the recording face's
    //     `command_buffer::build_acceleration_structure()` / `refit_acceleration_structure()`;
    //   * "CAN THIS DEVICE TRACE RAYS" - the only question the ability still answered - is a device fact, and
    //     `device_capabilities` answers it (`ray_query()` today, a pipeline bit when a caller needs one);
    //   * the `extension_kind::ray_tracing` BIT and the `interface_type::ray_tracing` VALUE stay, marked
    //     RETIRED in place: a bit and a number are never reused.
    //
    // The micromap half this note used to promise arrives with the same tier-1 treatment (plan S1's P4): a
    // micromap is a resource a caller creates, exactly as an acceleration structure is.

    /// tier-2 ability: copying an image into memory the app chose.
    ///
    /// This one is HOST-side work rather than a recorded command: the implementation
    /// performs the copy, nothing is recorded, nothing is submitted - so the caller has
    /// to make the image's writes visible to the host first (a barrier and a wait, see
    /// docs/host_image_copy.md).
    struct host_image_copy : extension {

        static constexpr extension_kind extension_id = extension_kind::host_image_copy;
        explicit host_image_copy(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// Copy `region` of `source` into `destination`, which must be at least as
        /// large as the region the backend resolves.
        [[nodiscard]] virtual error copy_image_to_memory(image const& source, std::span<std::byte> destination,
                                                         image_copy_region const& region) noexcept = 0;
    };

    /// tier-2 ability: WHAT THE DEVICE CAN DO, in the contract's own vocabulary.
    ///
    /// WHY AN ABILITY RATHER THAN A METHOD ON `api_core`: the extension mechanism already IS the channel for
    /// "a capability a backend may or may not serve" - `abilities()` announces it with a bit, `query_extension()`
    /// hands back the object, and "a set bit is a promise about service" holds for it exactly as it does for
    /// `vulkan_escape` or `descriptor_heap`. It also keeps the TIER-1 vtable untouched: a new ability moves no
    /// existing slot, which is why this batch needs no `abi_version` change while a `facts()` method on
    /// `api_core` would have.
    ///
    /// EVERY METHOD IS A SEMANTIC FACT, NOT AN API QUERY, and the distinction is the whole point:
    /// `ray_query()` means "this device can run ray queries", which in this backend is "the extension is
    /// ENABLED and its feature is present" - the TWO halves the engine used to re-derive itself from the
    /// enabled-extension list (by naming `VK_KHR_ray_query`) and from `vkGetPhysicalDeviceFeatures2`. Asking
    /// the backend for the answer it already computed to make its own enabling decision is smaller, cannot
    /// disagree with it, and is the only shape a second backend can serve: a D3D12 backend answers the same
    /// eight questions from ITS own caps, and the engine keeps compiling.
    ///
    /// THE METHODS ARE THE MEASURED SET, not a device dump: each one is read by the engine today. A fact nobody reads is dead
    /// vocabulary, so a later need adds a method rather than this type carrying a `VkPhysicalDeviceProperties`.
    struct device_capabilities : extension {

        static constexpr extension_kind extension_id = extension_kind::device_capabilities;
        explicit device_capabilities(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }

        /// Whether this device can run a MESH pipeline: the engine gates `evaluate_mesh_shaders()` and the
        /// mesh-dispatch path on it (the vertex form is gone - docs/mesh_shaders.md step 4).
        [[nodiscard]] virtual bool mesh_shader() const noexcept = 0;

        /// Whether this device can run RAY QUERIES: the ray-traced lighting path's gate, and what the
        /// acceleration-structure input usage on a buffer is decided by.
        [[nodiscard]] virtual bool ray_query() const noexcept = 0;

        /// The largest push block the device accepts, in bytes: the stage block's own limit (a mesh stage
        /// needs more than a vertex one, so this is the number its availability is checked against).
        [[nodiscard]] virtual std::uint32_t max_push_constants_size() const noexcept = 0;

        /// The queue family the backend's GRAPHICS queue belongs to. A command pool created by anyone else
        /// (the probes' pools are the measured example) must name this family, and the backend is the only
        /// object that knows it - the engine used to walk the device's families and compare each one's queue
        /// against the escape's, which is a derivation of a fact the backend already had.
        [[nodiscard]] virtual std::uint32_t graphics_queue_family() const noexcept = 0;

        /// The shader-binding-table numbers a pass builds its table from: the same POD the pass context
        /// carries, answered here so a caller that is NOT a pass (and has no context) can ask for it.
        [[nodiscard]] virtual shader_binding_table_properties shader_binding_table() const noexcept = 0;

        /// The alignment an acceleration structure's scratch offset must obey. The acceleration-structure
        /// module reads it when it sizes and offsets its scratch buffers; zero means "the device did not
        /// answer", which its callers already treat as 1.
        [[nodiscard]] virtual std::uint64_t acceleration_structure_scratch_alignment() const noexcept = 0;

        /// The largest number of instances one acceleration structure may hold: the module refuses a frame
        /// whose instance count passes it rather than asking the driver to fail.
        [[nodiscard]] virtual std::uint64_t max_acceleration_structure_instances() const noexcept = 0;
    };

    /// ABI9: heap 查询只返回值，不暴露后端 heap_limits 类型或引用。
    struct descriptor_heap_properties {
        std::uint64_t resource_size = 0;
        std::uint64_t max_resource_size = 0;
        std::uint64_t max_sampler_size = 0;
        std::uint64_t resource_alignment = 0;
        std::uint64_t sampler_alignment = 0;
        std::uint64_t resource_reserved = 0;
        std::uint64_t sampler_reserved_with_embedded = 0;
        std::uint32_t buffer_descriptor_size = 0;
        std::uint32_t image_descriptor_size = 0;
        std::uint32_t sampler_descriptor_size = 0;
        std::uint32_t max_push_data = 0;
        std::uint32_t max_embedded_samplers = 0;
    };

    struct heap_binding {
        std::uint64_t address = 0;
        std::uint64_t size = 0;
        std::uint64_t reserved_offset = 0;
        std::uint64_t reserved_size = 0;
    };

    struct heap_bindings {
        heap_binding resource;
        heap_binding sampler;
    };

    // 请求只含可移植语义。THE `next` CHAIN IS REFUSED (abi 34): the native view/command-buffer parameters the
    // transition carried are DELETED, so a request that still chains a tag is answered `unsupported` instead
    // of having the chain walked - the portable fields below are the whole request.
    enum class descriptor_type : std::uint32_t {
        sampled_image = 0,
        storage_image = 1,
        combined_image_sampler = 2,
        uniform_buffer = 3,
        storage_buffer = 4,
        acceleration_structure = 5,
        uniform_buffer_dynamic = 6,
        storage_buffer_dynamic = 7,
    };

    struct heap_image_write_info {
        structure_header header{structure_type::heap_image_write, sizeof(heap_image_write_info), nullptr};
        std::uint64_t offset = 0;
        image const* resource = nullptr;
        image_view_desc const* view = nullptr;
        descriptor_type type = descriptor_type::sampled_image;
    };
    struct heap_buffer_write_info {
        structure_header header{structure_type::heap_buffer_write, sizeof(heap_buffer_write_info), nullptr};
        std::uint64_t offset = 0;
        std::uint64_t address = 0;
        std::uint64_t size = 0;
        descriptor_type type = descriptor_type::storage_buffer;
    };
    struct heap_bind_info {
        structure_header header{structure_type::heap_bind, sizeof(heap_bind_info), nullptr};
        command_buffer* commands = nullptr;
        /// ABI 36: the batch-only recording target, so a mergeable segment can publish its heap binding
        /// without a command_buffer round trip. Exactly one of the two targets is set.
        mergeable_command_buffer* mergeable_commands = nullptr;
    };
    struct heap_push_info {
        structure_header header{structure_type::heap_push, sizeof(heap_push_info), nullptr};
        command_buffer* commands = nullptr;
        std::uint32_t offset = 0;
        std::span<std::byte const> data;
        /// ABI 36: same rule as `heap_bind_info::mergeable_commands` - exactly one target is set.
        mergeable_command_buffer* mergeable_commands = nullptr;
    };

    /// 推送常量以4字节为单位；先比较offset再相减，避免大跨度输入溢出。
    [[nodiscard]] constexpr error validate_heap_push_range(std::uint32_t const offset, std::uint64_t const size,
                                                           std::uint32_t const limit) noexcept {
        return size == 0 || (offset & 3u) != 0 || (size & 3u) != 0 || offset > limit || size > limit - offset
                   ? error::invalid_argument
                   : error::ok;
    }

    /// A LOGICAL POSITION inside a backend's descriptor space - what an immutable texture group was
    /// registered at.
    ///
    /// `space` names the space (0 = the renderer's main bindless heap, the one space v1 defines);
    /// `base` is the group's shader-facing identity (the token a push carries); `count` is the number of
    /// sampled slots the group covers; `generation` changes when the backend REUSES the position for
    /// another group, which is what makes a stale position distinguishable from a live one.
    ///
    /// FROZEN BY VALUE: a caller may store it in a material record and cross the boundary with it, so
    /// every field addition moves `abi_version` (the `submit_info` rule). The ENCODING of `base` is the
    /// backend's; the contract promises identity and validity, never address arithmetic.
    struct descriptor_position {
        std::uint32_t space = 0;
        std::uint32_t base = 0;
        std::uint32_t count = 0;
        std::uint32_t generation = 0;
    };
    static_assert(sizeof(descriptor_position) == 16u, "a descriptor position crosses by value; the layout is frozen");
    static_assert(std::is_trivially_copyable_v<descriptor_position>);
    static_assert(std::is_standard_layout_v<descriptor_position>);

    /// The shape of one backend's descriptor space, as answers rather than byte offsets.
    struct descriptor_space_properties {
        std::uint32_t group_capacity = 0;     ///< live immutable groups the space can hold
        std::uint32_t texture_capacity = 0;   ///< sampled descriptor slots it can hold
        std::uint32_t group_record_bytes = 0; ///< the shader-facing record one group occupies
        std::uint32_t texture_slot_bytes = 0; ///< stride of one sampled descriptor, in bytes
    };

    /// ONE PORTABLE SAMPLED-DESCRIPTOR WRITE: slot `index` of the group positioned at `where`.
    ///
    /// `view == nullptr` means the group profile's own view (all mips, one array layer, sampled role).
    /// The write is HOST-SIDE and must happen before the position is consumed by recorded work; the
    /// position's generation has to still be live (`invalid_argument` otherwise).
    struct descriptor_write {
        structure_header header{structure_type::descriptor_write, sizeof(descriptor_write), nullptr};
        descriptor_position where{};
        std::uint32_t index = 0;
        descriptor_type type = descriptor_type::sampled_image;
        image* resource = nullptr;
        image_view_desc const* view = nullptr;
    };

    /// tier-2 ability: the backend's DESCRIPTOR SPACE, addressed by positions instead of byte offsets.
    ///
    /// IT EXISTS BESIDE `descriptor_heap`, not instead of it: the heap interface answers the DEVICE's
    /// numbers (capacities, alignments, strides, push-data limit), while this answers the ENGINE's
    /// vocabulary - an immutable `texture_group`, the position it was registered at, and a portable
    /// descriptor write. A caller stores the position (a frozen POD) instead of leaking an absolute heap
    /// slot into material data, and `write()` is the one path that publishes the group's sampled
    /// descriptors (the backend's group-table record follows it atomically).
    ///
    /// ANNOUNCED ONLY WHEN A GROUP CAN BE POSITIONED AND WRITTEN: the ability's rule is the same one the
    /// other bits follow ("a set bit is a promise about service"), so a backend that cannot serve a
    /// texture group answers null here rather than announcing a shape it cannot fill.
    struct descriptor_space : extension {
        static constexpr extension_kind extension_id = extension_kind::descriptor_space;
        explicit descriptor_space(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        [[nodiscard]] virtual bool ready() const noexcept = 0;
        [[nodiscard]] virtual descriptor_space_properties properties() const noexcept = 0;
        /// The position `group` was registered at, or `count == 0` for anything this space does not own
        /// (a foreign group, an already-released one, another core's).
        [[nodiscard]] virtual descriptor_position position(texture_group const& group) const noexcept = 0;
        /// Write one sampled descriptor into a live position. See `descriptor_write` for the rules.
        [[nodiscard]] virtual error write(descriptor_write const& info) noexcept = 0;
    };

    // `vulkan_command_buffer_inheritance_info` STOOD HERE AND IS DELETED in abi 42: it was the native
    // (`VkFormat`) spelling of an attachment-inheritance declaration. The portable spelling in
    // `:api_core` (`command_buffer_inheritance_info`, contract image formats) is the one every caller uses,
    // so the native struct and its tag are retired (the tag VALUE stays - values are never reused).

    /// tier-2 ability: the ONE raw native handle the contract cannot express - the frame's command buffer,
    /// which the Dear ImGui Vulkan backend needs to record its own draw data. ONLY a Vulkan backend can
    /// answer it, which is why this is an ability and not tier-1; a non-Vulkan backend does not announce
    /// the bit, and G1/G2 then require `nullptr` from `query_extension(vulkan_escape)`.
    ///
    /// THE ENGINE DOES NOT USE IT ANY MORE, and neither does the content pipeline: every engine pass records
    /// through the contract, the GUI plugin's device root is the opaque `rhi::main_context` (abi 39), and
    /// the resource-handle borrows were deleted with their last test callers (abi 42). What is left is the
    /// GUI overlay's per-frame recording hand-off, which is a third-party API's own requirement.
    ///
    /// DELIBERATELY ABSENT: get_instance_proc / get_device_proc. A front end resolves its own entry points
    /// through the loader and the `main_context` device; keeping a loader out of the contract is the point.
    // `api_basis` STOOD HERE AND IS DELETED in abi 42: it was the tagged "device this work belongs to"
    // token behind `vulkan_escape::get_basis()`. Nothing outside the escape's own legacy SBT path ever
    // needed a basis (`shader_group_access` answers the SBT data portably), and both slots are gone.

    struct vulkan_escape : extension {

        static constexpr extension_kind extension_id = extension_kind::vulkan_escape;
        explicit vulkan_escape(std::string_view const name) noexcept
            : extension(name) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }

        // WHAT IS LEFT, AND WHY: this used to be the raw-handle hatch for passes that recorded Vulkan by
        // hand. The engine records through the contract now, the GUI plugin takes the device root as the
        // opaque `rhi::main_context` (abi 39), and the resource-handle borrows went with the tests that
        // were their only callers (abi 40/42). ONE slot survives because one caller genuinely needs it:
        // the Dear ImGui Vulkan backend records its own draw data into the frame's command buffer, and
        // ImGui's API takes a `VkCommandBuffer` - there is no contract verb for "hand a third-party
        // renderer the recording target".

        /// The PRIMARY command buffer of the frame `commands` belongs to, as VkCommandBuffer; nullptr
        /// when no frame is in flight. This is what lets an escaping pass record raw Vulkan into the
        /// frame it is part of.
        [[nodiscard]] virtual void* native_command_buffer(command_buffer& commands) const noexcept = 0;
    };

} // namespace deren::promise::rhi
