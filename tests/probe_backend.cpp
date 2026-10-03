// -*- C++ -*-
// ============================================================================
// file: tests/probe_backend.cpp
//
// The probe backend of tests/test_dynamic_link.cpp: the smallest stand-in for the
// promise/ C ABI (RHI plan v4 §7.4, §10.3 round 4). CMake builds this one source
// file twice - as a DLL (with DEREN_API_SHARED + DEREN_API_BUILD) and as a static
// library (with neither) - and the test links the static half while opening the DLL
// half at run time, so both branches of promise/abi_export.hpp are exercised.
//
// Three exported entry points and no C++ type crosses the boundary by value:
//   deren_abi_version()           -> unsigned int  (1 today)
//   deren_make_api_core(abi, err) -> api_core*     (nullptr + *err = 7 on ABI mismatch)
//   deren_destroy_api_core(core)  -> void          (delete through api_core)
//
// `api_core` is an abstract class whose definition is duplicated in the test on
// purpose: the object is created and destroyed inside the backend, and the front end
// only ever calls it through the base class, which is exactly the ownership rule the
// plan puts on the boundary (plan §7.4.4).
// ============================================================================
#include "../promise/abi_export.hpp"

struct api_core {
    virtual ~api_core() = default;
    virtual int value() const noexcept = 0;
};

namespace {
    struct impl final : api_core {
        int value() const noexcept override {
            return this->stored;
        }

        int stored = 0;
    };
} // namespace

extern "C" DEREN_API_EXPORT unsigned int deren_abi_version() {
    return 1u;
}

extern "C" DEREN_API_EXPORT struct api_core* deren_make_api_core(unsigned int abi, int* out_error) {
    if (abi != deren_abi_version()) {
        if (out_error != nullptr) {
            *out_error = 7; // the plan's "ABI mismatch" code, deliberately not a system error
        }
        return nullptr;
    }
    if (out_error != nullptr) {
        *out_error = 0;
    }
    auto* const core = new impl();
    core->stored = 42;
    return core;
}

extern "C" DEREN_API_EXPORT void deren_destroy_api_core(struct api_core* core) {
    delete core;
}
