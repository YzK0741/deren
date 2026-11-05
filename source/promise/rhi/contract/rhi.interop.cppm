// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:interop
// ============================================================================
module;

#include <string_view>

export module deren.promise.rhi:interop;

import :object;

export namespace deren::promise::rhi {

    /// A BORROWED, BACKEND-OWNED MAIN-THREAD CONTEXT (abi 39).
    ///
    /// The device-side state a non-engine consumer on the main thread brings itself up from - the GUI
    /// overlay is the caller, and it is the reason this exists: that consumer needs the API's own handles,
    /// and handing it ONE opaque context is the alternative to growing `vulkan_escape` with one virtual per
    /// handle.
    ///
    /// IT IS OPAQUE HERE ON PURPOSE. The contract carries only the concrete-identity tag (`object::get_tag()`),
    /// no native vocabulary and no handle members: an API-specific consumer checks the tag with
    /// `is_concrete()` and casts the handle to the concrete layout ITS API's backend and plugin share (for
    /// Vulkan: `deren::vulkan::main_context` in `denen.vulkan.main_context`, versioned with that
    /// backend/plugin pair rather than with this contract). A contract that named the layout would put the
    /// native vocabulary right back.
    ///
    /// OWNERSHIP: the `api_core` owns it; `api_core::main_context()` answers a BORROWED pointer valid for
    /// the context's lifetime. There is no `release()` and no reference to keep - the caller must never
    /// delete it.
    struct main_context : object {
        explicit main_context(std::string_view const name) noexcept
            : object(name) {
        }
    };

} // namespace deren::promise::rhi
