// -*- C++ -*-
// ============================================================================
// module: deren.rhi.promise:contract
//
// The two pieces every other partition of deren.rhi.promise needs: the ABI number the
// backend and the engine compare, and the error enum that turns a failure into a
// return value instead of an exception (RHI plan v4, §4.1 item 3 and §4.2).
//
// `deren::rhi::promise::abi_version` is the compile-time constant whose twin is the
// backend's `deren_abi_version()`. Plan §8.1 row 4 makes the comparison a tested
// check rather than a convention: the loader resolves the symbol and the test
// asserts the returned number equals this constant. The value is part of the C
// ABI: it moves only when the shape of `api_core` or of a tier-2 ability changes
// in a way an older engine could not survive.
//
// Nothing here allocates, throws, or names a container. Both sides of the boundary
// compile this partition (plan §4.1 item 1); neither exports a symbol for it.
// ============================================================================
module;

#include <cstdint>

export module deren.rhi.promise:contract;

/**
 * @file rhi/promise/promise.contract.cppm
 * @brief the ABI number the backend and the engine compare, and the `error` enum every promise entry
 *        point reports through.
 * @ingroup promise
 *
 * Both are part of the C ABI rather than of any implementation: `abi_version` is the number
 * `deren_abi_version()` returns and `deren_make_api_core()` refuses to build against, and `error`
 * is what a failure travels in because an exception must not cross the boundary (§4.2).
 * Deliberately the smallest partition in the contract: both sides compile it, so anything added here
 * is added to both compilations at once.
 */

export namespace deren::rhi::promise {

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

} // namespace deren::rhi::promise
