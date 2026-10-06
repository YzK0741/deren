// -*- C++ -*-
// ============================================================================
// file: vulkan/core/core.entry.cpp
//
// THE REAL BACKEND'S C ABI SURFACE (plan_rhi_v4.md §4.1 item 3, abi 18): the ONE
// `extern "C"` symbol promise/rhi/backend_entry.hpp declares, DEFINED here for
// deren::vulkan::core - the object that already owns the instance, the device
// and the swapchain, and that already derives from deren::promise::rhi::api_core
// (vulkan/core/core.declarations.cppm:271). There is no wrapper type: the object the
// engine gets back from deren_make_api_core() IS the core.
//
// WHY A PLAIN TU AND NOT A MODULE IMPLEMENTATION: the probe backend's lesson
// (tests/probe_backend.cpp is a plain TU for the same reason). `extern "C"` symbols
// declared inside a module purview get module attachment, and the whole point of this one
// is that it is reachable BY NAME from another image that does not import any module of
// this target. So the file imports what it needs, includes the declared surface, and
// defines it at global scope.
//
// THE ABI'S OWN SHAPE (backend_entry.hpp) IS TAKEN AS GIVEN, AND SINCE abi 18 IT IS ONE
// FUNCTION: the version arrives as the first ARGUMENT (a mismatch is refused here, before any
// object exists, with the backend's own number written into the diagnostic), the creation
// descriptor arrives by pointer and is borrowed for the length of the call, and OWNERSHIP
// TRAVELS IN THE RETURNED `shared_ptr` - whose control block holds the deleter, so the delete
// runs in THIS image without the host ever naming a deleter symbol. The two former entry points
// are gone by consequence, not by taste: `deren_destroy_api_core` had nothing left to do, and
// `deren_abi_version()` became an input plus a field of the refusal.
//
// The `-Wreturn-type-c-linkage` warning (a C++ class returned from a C-linkage function) is
// silenced here as well as at the declaration - the C linkage is for the NAME, an undecorated
// symbol a host can resolve as a string.
// ============================================================================

// DO NOT DELETE (clang 22.1.8, MEASURED): the aligned `operator new` overloads must be VISIBLE in
// this translation unit. `std::make_shared<deren::vulkan::core>` below instantiates libc++'s
// `allocate_shared`, and without this include the frontend stops with `call to 'operator new' is
// ambiguous` (candidate list printed twice at __new/global_new_delete.h) - the same class of failure
// utility/utility.cpp, toon_screen_rim.cpp and upscale.cpp carry this include for.
#include <new>

import deren.promise.rhi;
import deren.vulkan.core;

// After the imports it names: the header declares deren::promise::rhi types (see its note).
#include "../../promise/rhi/backend_entry.hpp"

namespace rhi = deren::promise::rhi;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

extern "C" DEREN_API_EXPORT std::shared_ptr<rhi::api_core>
deren_make_api_core(std::uint32_t abi_version, rhi::create_info const* desc, rhi::error_info* out_error_info) {
    // THE HANDSHAKE FIRST, BEFORE ANY OBJECT EXISTS (plan §4.2): an engine built against a
    // different contract learns "we disagree" as an empty `shared_ptr`, at the only moment where
    // nothing has been allocated yet and nothing has to be cleaned up. THE BACKEND'S OWN NUMBER
    // travels in `native_code` - that is what the deleted `deren_abi_version()` used to answer, and
    // the refusal is the only place a host needs it.
    if (abi_version != rhi::abi_version) {
        if (out_error_info != nullptr) {
            *out_error_info = deren::vulkan::failed(rhi::error::abi_mismatch, static_cast<std::int32_t>(rhi::abi_version),
                                                    "the caller's abi_version is not this backend's");
        }
        return {};
    }
    if (desc == nullptr) {
        // "I have no creation parameters" is a caller bug, not a request for the standard context:
        // `create_info{}` is how that is spelled, and defaulting here would hide the bug.
        if (out_error_info != nullptr) {
            *out_error_info = deren::vulkan::failed(rhi::error::invalid_argument, 0, "the creation descriptor is null");
        }
        return {};
    }
    if (out_error_info != nullptr) {
        *out_error_info = rhi::error_info{}; // ok: a zeroed diagnostic says ok, api unknown - nobody failed
    }
    // OWNERSHIP TRANSFER, IN THE CONTROL BLOCK: `make_shared` allocates the object and its control
    // block inside THIS image, and every copy of the returned pointer carries that deleter with it -
    // so the destruction runs in the allocator that made the object even when the last reference is
    // dropped on the host's side. That is safe HERE for the measured reason the header states: one
    // C++ runtime and one heap (libc++.dll + UCRT) shared by every image, checked by the gate.
    //
    // The descriptor is read by the constructor and not kept: its `window_title` is borrowed only
    // until this call returns (GLFW copies the text into the window), which the contract's own note
    // states and the constructor's member note repeats.
    //
    // A failure INSIDE construction does not come back as an `error_info`: the backend's own
    // startup rule is a NAMED panic with the missing thing in the message (a required
    // extension, a required feature, a swapchain that cannot be created -
    // core.constructor.cppm's `panic` sites), and the spike confirmed that shape. So a non-empty
    // return means "the context exists"; there is no partially-built context to report on.
    return std::make_shared<deren::vulkan::core>(*desc);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
