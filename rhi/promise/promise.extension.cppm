// -*- C++ -*-
// ============================================================================
// module: deren.rhi.promise:extension
//
// tier-2 of the promise contract: the abilities a backend may or may not have
// beyond the portable tier-1 surface (RHI plan v4, §1.8, §1.9, §1.11, §3.4, §3.5).
//
// The split is the plan's answer to "should the base class keep DX12 in mind?":
// `promise::api_core` carries only what DX12 also has, and everything a backend
// may lack lives here, announced as one bit in `api_core::abilities()`. Nothing an
// ability does may be needed by the portable path: a pass that needs one declares
// it with `required_capabilities()`, and a backend that does not have it is a
// NAMED failure at startup, not a silent downgrade (§1.9, §3.6).
//
// Three properties make this the shape the plan chose:
//
//   1. `abilities()` returns a BITMASK and one `extension_kind` value is one bit,
//      so the engine learns everything without a virtual call per ability, and the
//      §8 consistency gate can iterate the bits: for every set bit, the matching
//      `deren_ext_<ability>_v1` symbol must resolve with a matching abi.
//   2. Every ability derives from `extension`, whose only virtual is `kind()`.
//      Adding a sixth ability therefore cannot disturb the vtable of the other
//      five, and `query_extension()` stays a one-line lookup in the backend.
//   3. These are pure interfaces: no data member, no non-inline definition, no
//      `std::string`, no exception across the boundary (§4.2). The engine and the
//      backend each compile this partition; neither exports a module symbol for it.
//
// Deliberately NOT here yet: the C function tables that §3.5 pairs with each
// ability (`deren_ext_<ability>_v1` returning a POD struct of function pointers).
// That is the S2 half of the same decision. The classes below are the
// compile-time face of the same five abilities, and they are what the probe
// backend implements today (tests/probe_backend.cpp).
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <span>

export module deren.rhi.promise:extension;

import :contract;

/**
 * @file rhi/promise/promise.extension.cppm
 * @brief tier-2 of the promise contract: the abilities a backend may or may not have.
 * @ingroup promise
 *
 * DX12 does not have every capability Vulkan reaches through an extension (and a GL backend has
 * fewer still), so "the base class keeps DX12 in mind" cannot mean "the base class has everything"
 * (§1.5). What DX12 also has stays in `deren::rhi::promise::api_core`; the rest is declared here, one
 * class per ability, and announced as one bit in `api_core::abilities()`.
 *
 * The five abilities below are the compile-time face of plan §3.5's model. The run-time face - a
 * `deren_ext_<ability>_v1` C function table per ability, resolved and ABI-checked at load time - is
 * the S2 half of the same decision, and the §8 consistency gate walks `abilities()`'s set bits to
 * check exactly that pairing.
 */

export namespace deren::rhi::promise {

    // The tier-1 objects the abilities below take by reference. They are declared by
    // rhi/promise/promise.api_core.cppm: an ability is a tier-2 view of the same backend, so it
    // speaks about the same objects.
    struct buffer;
    struct command_list;
    struct image;

    // Descriptor shapes that are still S1 design surface (§5's capability table and
    // §6.4's usage/barrier decisions). A reference to an incomplete type is all a
    // virtual declaration needs, which is what lets the abilities be reviewed before
    // the shapes they carry are frozen.
    struct acceleration_structure;
    struct acceleration_structure_desc;
    struct image_copy_region;

    /// One bit per ability a backend can report through `api_core::abilities()`.
    ///
    /// The bit values are the contract (§3.5): the consistency gate resolves
    /// `deren_ext_<ability>_v1` per set bit, so a value must never move and a new
    /// ability takes the next free bit. Zero means "no abilities" and is spelled
    /// `no_abilities`.
    enum class extension_kind : std::uint32_t {
        device_address = 1u << 0,  ///< buffer/acceleration-structure addresses (plan §5: 35 mentions, 9 files)
        descriptor_heap = 1u << 1, ///< VK_EXT_descriptor_heap: push data, write descriptors, bind heaps
        mesh_shader = 1u << 2,     ///< vkCmdDrawMeshTasksEXT and its indirect form
        ray_tracing = 1u << 3,     ///< acceleration structures, RT pipelines, trace
        host_image_copy = 1u << 4, ///< vkCopyImageToMemoryEXT
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

    /// Every bit defined here: what a backend with all five abilities reports, and the
    /// set the plan's §8 consistency gate iterates.
    [[nodiscard]] constexpr auto all_abilities() noexcept -> ability_bits {
        return to_bits(extension_kind::device_address) | to_bits(extension_kind::descriptor_heap) |
               to_bits(extension_kind::mesh_shader) | to_bits(extension_kind::ray_tracing) |
               to_bits(extension_kind::host_image_copy);
    }

    static_assert(to_bits(extension_kind::device_address) == 0x1u, "the ability bits are ABI: they do not move");
    static_assert(all_abilities() == 0x1fu, "five abilities, five bits, one bit each (plan §3.5)");

    /// The common root of the tier-2 abilities.
    ///
    /// `kind()` is the only virtual every ability shares, and that is the point: the
    /// engine asks `query_extension()` for what it wants and then `static_cast`s to
    /// that ability's type. It does NOT use `dynamic_cast`: the repository builds with
    /// `-fno-rtti`, and it would not be needed anyway - the caller asked for a
    /// specific kind, so it already knows the type (plan §3.3).
    struct extension {
        virtual ~extension() noexcept = default;
        [[nodiscard]] virtual extension_kind kind() const noexcept = 0;
    };

    /// tier-2 ability: where a buffer or an acceleration structure lives on the device.
    ///
    /// A backend without it cannot serve `vkGetBufferDeviceAddress`-style rendering;
    /// a backend with it (DX12 has the same concept, GL does not) answers both calls.
    struct device_address : extension {
        /// The device address of `resource`, `offset` bytes into it.
        [[nodiscard]] virtual std::uint64_t buffer_address(buffer const& resource, std::uint64_t offset) const noexcept = 0;

        /// The device address of `structure`, as the backend reports it for
        /// `vkGetAccelerationStructureDeviceAddressKHR`.
        [[nodiscard]] virtual std::uint64_t acceleration_structure_address(acceleration_structure const& structure) const noexcept = 0;
    };

    /// tier-2 ability: the bindless descriptor heap (VK_EXT_descriptor_heap).
    struct descriptor_heap : extension {
        /// Append `data` to the heap the recording command list will see.
        virtual void push_data(command_list& commands, std::span<std::byte const> data) = 0;

        // Plan §5's census counts four more entries for this ability:
        // write_resource_descriptors (6), write_sampler_descriptors (4),
        // bind_resource_heap (2) and bind_sampler_heap (2). Their signatures land with
        // S1, together with the handover shape (docs/descriptor_heap_handover.md); the
        // names are listed here so this ability is not mistaken for "push_data and
        // nothing else".
    };

    /// tier-2 ability: mesh and task shaders.
    struct mesh_shader : extension {
        /// Record `groups_x` x `groups_y` x `groups_z` mesh workgroups into `commands`
        /// (plan §5: 19 `vkCmdDrawMeshTasksEXT` calls plus 4 indirect ones).
        virtual void dispatch_mesh(command_list& commands, std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) = 0;
    };

    /// tier-2 ability: acceleration structures and ray tracing.
    struct ray_tracing : extension {
        /// Allocate an acceleration structure (plan §5: 47 creates and 21 destroys go
        /// through the generic `create`/`destroy` members, which is why the census
        /// undercounts this ability by name).
        [[nodiscard]] virtual acceleration_structure* create_acceleration_structure(acceleration_structure_desc const& desc) = 0;

        /// Record the build of `target` into `commands`.
        virtual void build_acceleration_structure(command_list& commands, acceleration_structure& target) = 0;

        /// Record a trace of `width` x `height` pixels, `depth` rays deep.
        virtual void trace_rays(command_list& commands, std::uint32_t width, std::uint32_t height, std::uint32_t depth) = 0;

        // Micromaps (3 mentions), RT pipeline creation (3) and shader group handles (1)
        // are the remaining entries in §5's row for this ability; they land with S1.
    };

    /// tier-2 ability: copying an image into memory the app chose.
    ///
    /// This one is HOST-side work rather than a recorded command: the implementation
    /// performs the copy, nothing is recorded, nothing is submitted - so the caller has
    /// to make the image's writes visible to the host first (a barrier and a wait, see
    /// docs/host_image_copy.md).
    struct host_image_copy : extension {
        /// Copy `region` of `source` into `destination`, which must be at least as
        /// large as the region the backend resolves.
        [[nodiscard]] virtual error copy_image_to_memory(image const& source, std::span<std::byte> destination,
                                                         image_copy_region const& region) noexcept = 0;
    };

} // namespace deren::rhi::promise
