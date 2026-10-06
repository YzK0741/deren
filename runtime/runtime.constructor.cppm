// ============================================================================
// module: deren.vulkan.runtime:constructor  - construction and teardown (step 2, S1)
//
// THE ONE CONSTRUCTION, AND THE LINE THE WHOLE FLIP IS FOR: the contract's creation structure goes to the C
// entry `deren_make_api_core()`, and the object comes back as an `api_core*` this side never deletes. With
// the backend still STATIC in this tree that symbol is resolved by the LINKER, exactly as the legacy
// runtime's `std::make_shared<core>` was; the loader (`deren.utility.dynamic_link`, resolving the same three
// names by hand from a library) joins at the flip, and no call site of this file changes when it does.
// ============================================================================
module;

#include <cstdint>
#include <memory> // std::shared_ptr

module deren.vulkan.runtime:constructor;

import :declarations;
import deren.promise.rhi;
import deren.utility;

// THE THREE C ENTRY POINTS, included AFTER the contract import because their declarations name the
// contract's types (the header says so itself). They are ordinary `extern "C"` symbols with C language
// linkage, so a declaration in a module purview is attached to the GLOBAL module - the backend's definition
// and this declaration are the same entity, which is what the linker sees.
#include "../promise/rhi/backend_entry.hpp"

namespace deren::vulkan {

    runtime::runtime()
        : runtime(rhi::create_info{}) {
        // The empty creation is the contract's own "standard context" - the same spelling the legacy runtime
        // hands to the backend constructor.
    }

    runtime::runtime(rhi::create_info const& options)
        : core_owner{} {
        // THE ABI HANDSHAKE IS THE CALL'S FIRST ARGUMENT, and it is the constant BOTH sides compiled from
        // the contract module: a mismatch is refused by the backend (`error::abi_mismatch`) rather than
        // survived - the one failure mode a by-name load makes possible.
        rhi::error_info status{};
        rhi::api_core* const raw = deren_make_api_core(rhi::abi_version, &options, &status);
        if (raw == nullptr) {
            // THE REFUSAL IS THE BACKEND'S DIAGNOSIS, not a bare null: `status` carries the decision code,
            // the API, the native code, the backend's own text and the failure's source location (abi 13).
            // This is the one place the engine turns "the factory refused" into the startup panic the rest
            // of the runtime is written against, so nothing downstream has to hold a maybe-device.
            deren::utility::panic(std::source_location::current(),
                                  "runtime: deren_make_api_core refused the creation (abi {}, error code {}, native {}): {}",
                                  rhi::abi_version,
                                  static_cast<std::uint32_t>(status.code),
                                  status.native_code,
                                  status.message);
        }
        this->core_owner = std::shared_ptr<rhi::api_core>{raw, &deren_destroy_api_core};
    }

    runtime::~runtime() = default;

    rhi::api_core& runtime::rhi_face() const noexcept {
        // §18's rule in one line: the interface reference of the SAME object. Before the flip the vtable
        // points into this archive; after it, into the loaded library - the call sites cannot tell.
        return *this->core_owner;
    }

    rhi::vulkan_escape& runtime::escape() const noexcept {
        // `query_extension` is a CONTRACT virtual: this call emits no backend symbol no matter which side of
        // the boundary the object lives on. The escape is announced by `abilities()`, and the backend's own
        // test binary asserts that announcement (tests/spike_backend_boundary.cpp), so the dereference here
        // is the same promise the legacy runtime's accessor makes.
        return *rhi::query_extension<rhi::vulkan_escape>(this->rhi_face());
    }
} // namespace deren::vulkan
