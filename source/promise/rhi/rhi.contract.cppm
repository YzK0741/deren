// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:contract
// ============================================================================
module;

#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:contract;
export import deren.vstd;

export import :error;
export import :object;
export import :interop;

export namespace deren::promise::rhi {

    enum class structure_type : std::uint32_t {
        unknown = 0,
        heap_image_write = 1,
        heap_buffer_write = 2,
        heap_bind = 3,
        heap_push = 4,                  ///< retired in ABI 52; never reused
        command_buffer_inheritance = 5, ///< attachment formats in the portable RHI vocabulary
        /// appended with the descriptor space ability: one portable sampled-descriptor write into a
        /// `descriptor_position` (see `descriptor_write`). A value, not a vtable, so appending it moves
        /// no `abi_version` - the same rule the `vulkan_*` tags below follow.
        descriptor_write = 6,
        descriptor_image_write = 7,
        descriptor_buffer_write = 8,
        descriptor_acceleration_structure_write = 9,
        descriptor_bind = 10,
        /// RETIRED in abi 34: the heap image write carries no native view any more. The VALUE stays (a tag,
        /// like an ability bit, is never reused) and the request validation refuses it.
        vulkan_heap_image = 0x10000,
        /// RETIRED in abi 34: a recording verb takes a contract command_buffer, never a raw VkCommandBuffer.
        vulkan_command_buffer = 0x10001,
        /// RETIRED in abi 42: the native attachment-inheritance chain is gone; the portable
        /// `command_buffer_inheritance_info` is the only spelling. The VALUE stays (values are never reused).
        vulkan_command_buffer_inheritance = 0x10002,
        /// RETIRED in abi 42: `api_basis` (and the escape's legacy SBT path) is gone; the portable
        /// `shader_group_access` answers that data. The VALUE stays.
        vulkan_device_basis = 0x10003,
    };

    /// `next` is borrowed only until the synchronous call returns. Since ABI 34 there is no
    /// "silently drop an uninterpreted chain" request: except for
    /// `vulkan_command_buffer_inheritance` (the secondary recording attachment format, which has no
    /// portable spelling), every request carrying `next` is refused
    /// (`validate_structure(..., allow_next=false)` answers `unsupported`).
    struct structure_header {
        structure_type s_type = structure_type::unknown;
        std::uint32_t struct_size = sizeof(structure_header);
        structure_header const* next = nullptr;
    };

    [[nodiscard]] constexpr error validate_structure(structure_header const& header, structure_type const expected,
                                                     std::size_t const required_size, bool const allow_next = false) noexcept {
        if (header.struct_size < sizeof(structure_header) || header.struct_size < required_size || header.s_type != expected) {
            return error::invalid_argument;
        }
        if (!allow_next && header.next != nullptr) {
            return error::unsupported;
        }
        return error::ok;
    }

} // namespace deren::promise::rhi
