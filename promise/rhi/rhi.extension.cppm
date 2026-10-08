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
//      `std::string`, no exception across the boundary (§4.2). The engine and the
//      backend each compile this partition; neither exports a module symbol for it.
//
// 能力入口只有 query_extension；这些虚接口沿用同工具链的 C++ ABI。
// 不再增加一套 deren_ext_* C 函数表；该边界不承诺供非 C++ 宿主使用。
// ============================================================================
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

export module deren.promise.rhi:extension;

import :contract;

/**
 * @file promise/rhi/rhi.extension.cppm
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
    // promise/rhi/rhi.api_core.cppm: an ability is a tier-2 view of the same backend, so it
    // speaks about the same objects.
    struct api_core;
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
        vulkan_escape = 1u << 5,   ///< raw Vulkan handles, for the code the contract cannot express yet
        /// APPENDED (a new BIT; the six before it do not move - see the static_assert below): WHAT THE DEVICE
        /// CAN DO, in the contract's own vocabulary. A backend announces it when it can answer every method,
        /// the same "a set bit is a promise about service" rule the others live by. The engine used to derive
        /// these facts ITSELF - `vkGetPhysicalDeviceFeatures2` for the two features,
        /// `vkGetPhysicalDeviceProperties` for the push-constant limit, `vkGetPhysicalDeviceQueueFamilyProperties`
        /// + `vkGetDeviceQueue` to recover the graphics queue's family, and a second properties query for the
        /// acceleration-structure numbers - while the BACKEND had already made the same queries to decide what
        /// to enable. Asking it is both the smaller code and the only shape a second backend can serve.
        device_capabilities = 1u << 6,
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

    /// Every bit defined here: what a backend with all seven abilities reports, and the
    /// set the plan's §8 consistency gate iterates.
    [[nodiscard]] constexpr auto all_abilities() noexcept -> ability_bits {
        // `extension_kind::ray_tracing` IS NOT HERE ANY MORE (abi 26): its whole shape moved to tier-1, so no
        // bit of the retired kind is defined. The ENUMERATOR stays (bits are never reused), which is why this
        // set is no longer "every enumerator" - the note on that enumerator says so.
        return to_bits(extension_kind::device_address) | to_bits(extension_kind::descriptor_heap) |
               to_bits(extension_kind::mesh_shader) |
               to_bits(extension_kind::host_image_copy) | to_bits(extension_kind::vulkan_escape) |
               to_bits(extension_kind::device_capabilities);
    }

    /// The same seven, as a LIST: what a gate walks to check one bit at a time.
    ///
    /// `all_abilities()` is the set as a bitmask; this is the enumeration the consistency gates and
    /// the tests iterate (`abilities()` set => `query_extension()` non-null, and the reverse), so the
    /// list is spelled in ONE place instead of per caller.
    [[nodiscard]] constexpr auto all_extension_kinds() noexcept -> std::array<extension_kind, 6> {
        return {extension_kind::device_address, extension_kind::descriptor_heap, extension_kind::mesh_shader,
                extension_kind::host_image_copy, extension_kind::vulkan_escape,
                extension_kind::device_capabilities};
    }

    static_assert(to_bits(extension_kind::device_address) == 0x1u, "the ability bits are ABI: they do not move");
    static_assert(all_abilities() == 0x77u, "six abilities now: `ray_tracing` (bit 3 = 0x08) is RETIRED in abi 26 and a retired bit is never reused");

    /// The common root of the tier-2 abilities.
    ///
    /// `kind()` is the only virtual every ability shares, and that is the point: the
    /// engine asks `query_extension()` for what it wants and then `static_cast`s to
    /// that ability's type. It does NOT use `dynamic_cast`: the repository builds with
    /// `-fno-rtti`, and it would not be needed anyway - the caller asked for a
    /// specific kind, so it already knows the type (plan §3.3).
    struct extension : object {
    protected:
        explicit extension(interface_type const type) noexcept
            : object(type) {
        }

    public:
        virtual ~extension() noexcept = default;
        [[nodiscard]] virtual extension_kind kind() const noexcept = 0;
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
    struct device_address : extension {
        static constexpr interface_type interface_id = interface_type::device_address;
        static constexpr extension_kind extension_id = extension_kind::device_address;
        device_address() noexcept
            : extension(interface_id) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// The device address of `resource`, `offset` bytes into it; 0 when the buffer was not created
        /// with `buffer_flag::device_address` (the caller asked for no address, so there is none to give).
        [[nodiscard]] virtual std::uint64_t buffer_address(buffer const& resource, std::uint64_t offset) const noexcept = 0;
    };

    /// 可选的bindless heap服务；使用通用资源/地址/命令语义。原生参数只能显式放在next中。
    /// 不承诺所有后端支持；广播此能力必须提供可用实现，不能用空操作冒充。
    struct descriptor_heap : extension {
        static constexpr interface_type interface_id = interface_type::descriptor_heap;
        static constexpr extension_kind extension_id = extension_kind::descriptor_heap;
        descriptor_heap() noexcept
            : extension(interface_id) {
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
        static constexpr interface_type interface_id = interface_type::mesh_shader;
        static constexpr extension_kind extension_id = extension_kind::mesh_shader;
        mesh_shader() noexcept
            : extension(interface_id) {
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
        static constexpr interface_type interface_id = interface_type::host_image_copy;
        static constexpr extension_kind extension_id = extension_kind::host_image_copy;
        host_image_copy() noexcept
            : extension(interface_id) {
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
    /// seven questions from ITS own caps, and the engine keeps compiling.
    ///
    /// THE METHODS ARE THE MEASURED SET, not a device dump: each one is read by the engine today (the call
    /// sites are named in `docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md` §2.20). A fact nobody reads is dead
    /// vocabulary, so a later need adds a method rather than this type carrying a `VkPhysicalDeviceProperties`.
    struct device_capabilities : extension {
        static constexpr interface_type interface_id = interface_type::device_capabilities;
        static constexpr extension_kind extension_id = extension_kind::device_capabilities;
        device_capabilities() noexcept
            : extension(interface_id) {
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

    /// 原生Vulkan视图的过渡参数，只能用于vulkan_heap_image_info；不冒充通用格式。
    struct vulkan_heap_image_desc {
        std::uint32_t struct_size = sizeof(vulkan_heap_image_desc);
        void* native_image = nullptr;
        std::uint32_t view_flags = 0;
        std::uint32_t view_type = 0;
        std::uint32_t format = 0;
        std::array<std::uint32_t, 4> components = {};
        std::uint32_t aspect_mask = 0;
        std::uint32_t base_mip = 0;
        std::uint32_t mip_count = 1;
        std::uint32_t base_layer = 0;
        std::uint32_t layer_count = 1;
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

    // 通用语义与原生参数分开；普通请求不解释Vulkan枚举。
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
    };
    struct heap_push_info {
        structure_header header{structure_type::heap_push, sizeof(heap_push_info), nullptr};
        command_buffer* commands = nullptr;
        std::uint32_t offset = 0;
        std::span<std::byte const> data;
    };

    /// 推送常量以4字节为单位；先比较offset再相减，避免大跨度输入溢出。
    [[nodiscard]] constexpr error validate_heap_push_range(std::uint32_t const offset, std::uint64_t const size,
                                                           std::uint32_t const limit) noexcept {
        return size == 0 || (offset & 3u) != 0 || (size & 3u) != 0 || offset > limit || size > limit - offset
                   ? error::invalid_argument
                   : error::ok;
    }

    /// 过渡的Vulkan参数通过next显式借用；不是通用格式/句柄的别名。
    struct vulkan_heap_image_info {
        structure_header header{structure_type::vulkan_heap_image, sizeof(vulkan_heap_image_info), nullptr};
        vulkan_heap_image_desc view;
        std::uint32_t layout = 0;
        api_core const* context = nullptr;
    };
    struct vulkan_command_buffer_info {
        structure_header header{structure_type::vulkan_command_buffer, sizeof(vulkan_command_buffer_info), nullptr};
        void* commands = nullptr;
        api_core const* context = nullptr;
    };

    /// THE ATTACHMENT INHERITANCE a SECONDARY command buffer declares (abi 15, the
    /// `vulkan_command_buffer_inheritance` structure type).
    ///
    /// `command_buffer_begin_info::next` carries THIS when the recording is a `render_pass_continue`
    /// one, because that is the one begin fact no portable vocabulary covers: Vulkan's dynamic
    /// rendering requires the secondary to declare the attachment formats it will render against
    /// (VUID-vkBeginCommandBuffer-commandBuffer-00050 and its pNext-chain companions), and which
    /// formats a device serves is a property of the API, not of a recording session. It rides the SAME
    /// tagged mechanism `vulkan_command_buffer_info` / `vulkan_heap_image_info` use - a
    /// `structure_header` first, exactly one layer, borrowed only until the call returns - so the
    /// portable `command_buffer_begin_info` stays free of one API's attachment model.
    ///
    /// The formats are NATIVE (`VkFormat` as a `std::int32_t`, the way Vulkan spells it): this struct
    /// exists precisely to name what the contract cannot. `color_formats` is borrowed for the call and
    /// must hold @p color_format_count entries; a count of 0 with a null pointer means "no colour
    /// attachment", and `depth_format == 0` (VK_FORMAT_UNDEFINED) means "no depth attachment".
    struct vulkan_command_buffer_inheritance_info {
        structure_header header{structure_type::vulkan_command_buffer_inheritance, sizeof(vulkan_command_buffer_inheritance_info), nullptr};
        std::uint32_t color_format_count = 0;
        std::int32_t const* color_formats = nullptr; ///< native VkFormat values
        std::int32_t depth_format = 0;               ///< native VkFormat; 0 = no depth attachment
        std::uint32_t samples = 1;                   ///< VkSampleCountFlagBits; 1 = single-sampled
        std::uint32_t view_mask = 0;                 ///< VkCommandBufferInheritanceRenderingInfo::viewMask
    };

    [[nodiscard]] constexpr interface_type extension_interface_type(extension_kind const kind) noexcept {
        switch (kind) {
        case extension_kind::device_address:
            return interface_type::device_address;
        case extension_kind::descriptor_heap:
            return interface_type::descriptor_heap;
        case extension_kind::mesh_shader:
            return interface_type::mesh_shader;
        case extension_kind::ray_tracing: // RETIRED: no interface answers to it any more (see its bit's note)
            return interface_type::ray_tracing;
        case extension_kind::host_image_copy:
            return interface_type::host_image_copy;
        case extension_kind::vulkan_escape:
            return interface_type::vulkan_escape;
        case extension_kind::device_capabilities:
            return interface_type::device_capabilities;
        }
        return interface_type::unknown;
    }

    /// tier-2 ability: the raw Vulkan handles a pass needs when the contract has no concept for what it
    /// does. ONLY a Vulkan backend can answer these, which is exactly why this is an ability and not
    /// tier-1; a non-Vulkan backend does not announce the bit, and G1/G2 then require `nullptr` from
    /// `query_extension(vulkan_escape)`.
    ///
    /// A pass that uses it MUST declare it (`required_capabilities()`): a backend that does not announce
    /// it is a NAMED failure at startup, never a silent skip (plan §1.9/§3.6).
    ///
    /// DELIBERATELY ABSENT: get_instance_proc / get_device_proc. The front end resolves its own entry
    /// points through `vkGetDeviceProcAddr` on `native_device()` - the repository's documented way and
    /// the only one that works for extension commands on this toolchain. Keeping a loader out of the
    /// contract is the point of the rule.
    ///
    /// THE ESCAPE IS CURRENTLY REQUIRED BY THE ENGINE, and that is a TRANSITIONAL state, not a designed
    /// exception: the engine still records its own frame by hand (until S3 moves the passes), so a
    /// Vulkan backend that did not announce this bit would make the engine fail at startup by name.
    /// Once the passes record through the contract, the escape shrinks to the few calls the contract has
    /// no concept for.
    /**
     * @brief THE BASIS OF A BACKEND: the token that carries a BASIC HANDLE (a device above all) through the
     *        engine without naming it - `core::get_basis()` answers one, and a method that needs a basic handle
     *        takes `api_basis&` and passes it back to the backend that owns it.
     *
     * NO INTERFACE AT ALL, AND THAT IS THE POINT: there is no virtual, no ownership, no data beyond the TAG -
     * an implementer owes this type NOTHING (a backend derives an empty struct, sets `s_type`, and is done).
     * The fact being passed is "which device owns this work", and the contract deliberately does not name
     * `VkDevice`: it is a dispatchable pointer whose type belongs to the backend. `void*` was the previous
     * carrier, and it costs nothing and checks nothing; a TAGGED token is the improvement - the receiver asks
     * the same question every other tagged structure in this contract is asked (`structure_header`'s `s_type`
     * convention, one level up in the skeleton rather than in a pNext chain).
     *
     * WHY NOT A VIRTUAL: this build is `-fno-rtti`, and a virtual would buy nothing that the tag does not -
     * the callee is the BACKEND, which already knows what it handed out and only has to check that the token is
     * the one it expects. An empty base + `s_type` is also the only shape that keeps the contract free of a
     * vtable it would then have to keep stable across releases.
     *
     * IT IS NOT A BASE OF `api_core` AND NOT A BASE OF THE BACKEND'S OWN TYPE (the ruling this was built
     * under): the backend COMPOSES one and hands it out by name, so a signature that takes the contract does
     * not also take "the thing handles hang off". The handle itself never travels IN the token (that would make
     * it the native type leaking through the contract again) - the backend reads it from its own state.
     *
     * `struct_size` AND `next` ARE DELIBERATELY ABSENT, unlike `structure_header`: nothing crosses a boundary
     * by value here (the token is passed by reference between engine and backend), so there is no caller/callee
     * layout to guard, and there is no chain to walk.
     */
    struct api_basis {
        /// WHICH KIND OF BASIS THIS IS - the tag a receiver checks (`structure_type::vulkan_device_basis` for
        /// the Vulkan backend's logical device). The only content this type has.
        structure_type s_type = structure_type::unknown;
    };

    struct vulkan_escape : extension {
        static constexpr interface_type interface_id = interface_type::vulkan_escape;
        static constexpr extension_kind extension_id = extension_kind::vulkan_escape;
        vulkan_escape() noexcept
            : extension(interface_id) {
        }
        [[nodiscard]] extension_kind kind() const noexcept final {
            return extension_id;
        }
        /// VkInstance / VkPhysicalDevice / VkDevice / VkQueue as their own types. All four are
        /// DISPATCHABLE handles (pointers), so `void*` carries them without naming a Vulkan type in the
        /// contract. Null before the device exists; the context's lifetime covers them.
        [[nodiscard]] virtual void* native_instance() const noexcept = 0;
        [[nodiscard]] virtual void* native_physical_device() const noexcept = 0;
        [[nodiscard]] virtual void* native_device() const noexcept = 0;
        /// the queue the backend submits the frame's command buffer on
        [[nodiscard]] virtual void* native_queue() const noexcept = 0;

        /// The PRIMARY command buffer of the frame `commands` belongs to, as VkCommandBuffer; nullptr
        /// when no frame is in flight. This is what lets an escaping pass record raw Vulkan into the
        /// frame it is part of.
        [[nodiscard]] virtual void* native_command_buffer(command_buffer& commands) const noexcept = 0;

        /// The instance/device extensions this context ENABLED, NUL-terminated names, in the backend's
        /// own storage (valid for the context's lifetime). The escape's guard rail: a pass checks the
        /// extension it wants is here BEFORE it resolves an entry point for it.
        ///
        /// THE ELEMENT TYPE IS `char const* const`, AND THAT IS A COMPILE-LEVEL FACT RATHER THAN A STYLE
        /// CHOICE (this was "corrected" to `std::span<char const*>` once and had to be changed back): a
        /// span's ELEMENT type may be const-qualified - what may not be const is the span itself. The
        /// backend answers from a container IT OWNS, reached through a const member function, so the span
        /// is built over a CONST container, and `std::span<char const*>`'s range constructor is
        /// constrained away for a non-borrowed const range unless `is_const_v<element_type>` holds - the
        /// non-const-element spelling does not compile at the backend's return statements. The alternative,
        /// a const_cast, would misstate what the caller may do with the view.
        [[nodiscard]] virtual std::span<char const* const> enabled_instance_extensions() const noexcept = 0;
        [[nodiscard]] virtual std::span<char const* const> enabled_device_extensions() const noexcept = 0;

        /// The native buffer behind a contract `buffer`, as the backend's own handle; nullptr when the
        /// buffer carries none.
        ///
        /// BORROWED, AND VALID ONLY WHILE `resource` HOLDS ITS REFERENCE: this hands out the handle, not
        /// a share of the object, so a released buffer must not be passed (the contract's ownership note
        /// says touching a released handle is a caller bug - and this is the call that would make it
        /// fatal rather than merely wrong).
        ///
        /// WHY IT EXISTS: a pass records raw Vulkan through the escape, and the raw calls that take a
        /// buffer (`vkCmdBindVertexBuffers`, `VkDescriptorBufferInfo`, an acceleration structure's build
        /// geometry) need the handle itself, which a device ADDRESS cannot stand in for.
        /// `device_address::buffer_address()` answers the addressable case; this is the escape hatch for
        /// the rest, and it is why the engine can stop reaching for the allocator's detail map.
        [[nodiscard]] virtual void* native_buffer(buffer const& resource) const noexcept = 0;

        /// The native image behind a contract `image`; same borrowed-handle rule as `native_buffer`
        /// (valid only while `resource` holds its reference). APPENDED IN ABI 7 with the image face
        /// (§17): the engine's raw recording and descriptor writes need the `VkImage` itself.
        [[nodiscard]] virtual void* native_image(image const& resource) const noexcept = 0;

        /// The native `VkImageView` behind a contract `image_view` - the handle a descriptor write and
        /// a raw `vkCmdBindDescriptorSets` consume. Same lifetime rule as `native_buffer`: borrowed,
        /// valid only while the view holds its reference.
        [[nodiscard]] virtual void* native_image_view(image_view const& resource) const noexcept = 0;

        /// The native `VkSampler` behind a contract `sampler`; same lifetime rule as the two above.
        [[nodiscard]] virtual void* native_sampler(sampler const& resource) const noexcept = 0;

        /// The concrete format behind a contract `image`, as the integer VkFormat spells it - the one
        /// image FACT a `native_image` consumer still needs while the heap's image descriptors are
        /// raw `VkImageViewCreateInfo` (the `depth` ROLE resolved to the device's own format inside
        /// the backend, and only the backend knows which). Same borrowed rule as the three above.
        /// TRANSITIONAL by the same measure the escape is: the raw heap path is Face D's to remove.
        [[nodiscard]] virtual std::uint32_t native_image_format(image const& resource) const noexcept = 0;

        /// The native `VkPipeline` behind a contract `pipeline` - what a raw `vkCmdBindPipeline` and
        /// the compute/ray-tracing dispatch paths consume. Same borrowed rule as the image handles.
        /// APPENDED IN ABI 8 with the pipeline face.
        [[nodiscard]] virtual void* native_pipeline(pipeline const& resource) const noexcept = 0;

        /// The native `VkShaderModule` behind a contract `shader` - what a raw
        /// `VkPipelineShaderStageCreateInfo` (the compute and ray-tracing pipelines the engine still
        /// assembles) consumes. Same borrowed rule.
        [[nodiscard]] virtual void* native_shader_module(shader const& resource) const noexcept = 0;

        /// The swapchain image's format AS THE BACKEND RESOLVED IT, as the integer `VkFormat` spells it.
        ///
        /// WHY IT IS ON THE CONTEXT RATHER THAN ASKED OF AN IMAGE: this is a SESSION-STABLE fact (the
        /// surface's format does not change for a given surface), and the engine needs it BEFORE any
        /// frame exists - the pipelines that render INTO the presentation image are created up front.
        /// `native_image_format(image const&)` cannot answer then: the only swapchain image a caller can
        /// name is `api_core::frame_image()`, and that is deliberately nullptr until an acquire has
        /// happened. VkFormat::VK_FORMAT_UNDEFINED when the backend has no swapchain (nothing has been
        /// presented into yet, or the context was created without one).
        ///
        /// APPENDED IN ABI 17 with the contract-only runtime's merged slice (③-D/E step 2): a virtual on
        /// an existing tier-2 interface, which is the case the abi number exists for.
        [[nodiscard]] virtual std::uint32_t native_swapchain_image_format() const noexcept = 0;

        /// THE BASIS THIS ESCAPE HANDS OUT (abi 22), as the contract's tagged `api_basis` - see its own note for
        /// why the base carries no handle and no interface. A caller that has to pass "the device this work
        /// belongs to" passes THIS, and the method below takes it back. Null before the device exists.
        [[nodiscard]] virtual api_basis* get_basis() const noexcept = 0;

        // THE `device_proc` SLOT IS GONE (abi 24), AND ITS LAST CALLER IS THE MEASUREMENT: it resolved an
        // allocated entry point against the basis's device (`vkCmdTraceRaysKHR` above all), and the ray-tracing
        // launch is the recording face's verb now - the BACKEND resolves that pointer once at startup
        // (`core::ray_trace_launch`), so nothing in the engine asks a device for an entry point any more. The
        // engine's own ray-tracing module resolves the acceleration-structure and micromap commands through
        // `native_device`, which is a raw handle it already holds rather than a basis round trip. Removing it is
        // an interface change - `shader_group_handles` below shifts down one slot - which is what the abi number
        // is for; a facility nobody calls is dead vocabulary, and this file has deleted one before
        // (`pass::native_commands`) rather than keep it "in case".

        /// THE SHADER-BINDING-TABLE GROUPS of a ray-tracing `pipeline`: `group_count` handles starting at
        /// `first_group`, written into `out` (the device-side query `vkGetRayTracingShaderGroupHandlesKHR`).
        /// false when the basis carries the wrong tag, the pipeline has no native handle, or the query failed.
        ///
        /// APPENDED IN ABI 22 WITH `get_basis` AND `device_proc`, and it is the SBT half of the same escape
        /// bucket: the group handles are per-pipeline DEVICE data whose layout (size, alignment, the regions a
        /// launch is given) is exactly what the contract has no vocabulary for. `pass::shader_group_handles` is
        /// the pass-layer spelling of this call, and it is what let `ray_traced_shadow.cpp` stop naming a
        /// `VkDevice` - and, with the region and properties types promoted in abi 24, stop including a Vulkan
        /// header at all.
        [[nodiscard]] virtual bool shader_group_handles(api_basis& basis, pipeline const& resource, std::uint32_t first_group, std::uint32_t group_count,
                                                        std::span<std::uint8_t> out) const noexcept = 0;
    };

} // namespace deren::promise::rhi
