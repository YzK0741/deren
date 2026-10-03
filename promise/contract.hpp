// -*- C++ -*-
// ============================================================================
// file: promise/contract.hpp
//
// The two pieces every promise/ header needs: the ABI number the backend and the
// engine compare, and the error enum that turns a failure into a return value
// instead of an exception (RHI plan v4, §4.1 item 3 and §4.2).
//
// `deren::promise::abi_version` is the compile-time constant whose twin is the
// backend's `deren_abi_version()`. Plan §8.1 row 4 makes the comparison a tested
// check rather than a convention: the loader resolves the symbol and the test
// asserts the returned number equals this constant. The value is part of the C
// ABI: it moves only when the shape of `api_core` or of a tier-2 ability changes
// in a way an older engine could not survive.
//
// Nothing here allocates, throws, or names a container. Both sides of the
// boundary compile this header (plan §4.1 item 1); neither exports a symbol for
// it.
// ============================================================================
#pragma once

#include <cstdint>

/**
 * @file promise/contract.hpp
 * @brief the ABI number the backend and the engine compare, and the `error` enum every promise entry
 *        point reports through.
 * @ingroup promise
 *
 * Both are part of the C ABI rather than of any implementation: `abi_version` is the number
 * `deren_abi_version()` returns and `deren_make_api_core()` refuses to build against, and `error`
 * is what a failure travels in because an exception must not cross the boundary (§4.2).
 * Deliberately the smallest header in the contract: it is included by both sides, so anything added
 * here is added to both compilations at once.
 */

/**
 * @defgroup promise The Promise Contract (backend boundary)
 * @brief the abstract interfaces the engine and the backend share, and the C ABI that hands them
 *        out - compiled by both sides, exported by neither (RHI plan v4 §3.1, §4.1).
 *
 * The files that make up the contract:
 *
 * | header | what it is |
 * | --- | --- |
 * | `promise/contract.hpp` | the ABI number and the `error` enum every entry point reports through |
 * | `promise/extension.hpp` | tier-2: the five abilities a backend may or may not have |
 * | `promise/api_core.hpp` | tier-1: the context, its factories, the frame calls, and the three C entry points |
 * | `promise/abi_export.hpp` | the `DEREN_API_*` keywords the three entry points are declared with |
 *
 * @details
 * - **Two tiers, one rule.** `api_core` carries only what every backend has a concept for (DX12
 *   included), and an ability a backend may lack lives in tier-2, announced as one bit in
 *   `api_core::abilities()` (§1.5, §1.8). A pass that needs an ability declares it; a backend that
 *   does not have it is a named failure, never a silent downgrade (§1.9).
 * - **Nothing here allocates, throws, or names a container** (§4.2): every interface is a vtable,
 *   every parameter is a POD, a `std::span`, or an opaque handle, and every failure is a returned
 *   `error`.
 * - **These are not modules and export no symbols.** Both sides compile these headers into their
 *   own translation units, which is what keeps the engine off the DLL's import library; the only
 *   exchanged names are the three `extern "C"` entry points in `promise/api_core.hpp`.
 * - **Ownership is spelled out, not implied.** An object made inside the backend is deleted inside
 *   the backend: the engine builds its `std::shared_ptr` around `deren_destroy_api_core`, resolved
 *   from the backend it loaded (§4.1 item 3).
 */

namespace deren::promise {

    /// The C ABI number of this build of the contract.
    ///
    /// `deren_abi_version()` in the backend returns it and `deren_make_api_core()`
    /// refuses any other value, which is the "engine and backend disagree" failure
    /// path the skeleton keeps testable on purpose (plan §4.2).
    inline constexpr std::uint32_t abi_version = 1u;

    /// Why a promise entry point could not do what it was asked.
    ///
    /// An exception must not cross the boundary (plan §4.2), so every failure a
    /// backend can report travels as one of these through an out-parameter.
    ///
    /// `abi_mismatch` = 7 is not a system error number: it is the code the minimal
    /// use case measured in plan §10.3 reports, and it is kept here so that the
    /// refusal stays observable from the engine side.
    enum class error : std::uint32_t {
        ok = 0,           ///< the call did what it was asked
        abi_mismatch = 7, ///< the caller's abi_version is not the backend's
    };

} // namespace deren::promise
