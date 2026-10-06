// -*- C++ -*-
// ============================================================================
// file: promise/rhi/backend_entry.hpp
//
// The backend's C ABI surface: the ONE `extern "C"` symbol a backend exports and a
// host resolves (RHI plan v4, §1.15, §4.1 item 3; abi 18).
//
// WHY THIS IS NOT PART OF deren.promise.rhi (m03159), EVEN THOUGH IT LIVES IN ITS
// DIRECTORY (m04074): the contract is a collection of virtual base classes, and an
// entry point is not one. A backend DEFINES this, a host RESOLVES it, and neither
// side derives from anything to do it. So the file sits in `promise/rhi/` because it
// is the graphics-API boundary's surface and belongs with the contract it hands out -
// but it is NOT a module partition, never appears in the module's file set, and is not
// imported: it is an ordinary header that both sides `#include`, and a translation unit
// that includes it must first `import deren.promise.rhi;` for the types the declaration
// names.
//
// WHY ONE ENTRY POINT AND NOT THREE (abi 18). Until abi 17 this file declared three:
// a version query, a factory returning a RAW pointer, and the deleter by name (the
// plan's original §4.1 item 3 shape, chosen in m02244 because returning a
// `std::shared_ptr` needs `-Wreturn-type-c-linkage` silenced and makes "same standard
// library" a boundary premise). THAT PREMISE IS NOW MEASURED AND MACHINE-CHECKED: the
// executable imports libc++.dll and uses the UCRT heap (one C++ runtime, one heap for
// every image - `cxx deren.exe imports libc++.dll` in scripts/check_backend_boundary.py,
// which also covers the backend DLL), so a control block allocated inside the backend can
// be released by whoever drops the last reference. Two of the three entries therefore
// become unnecessary rather than wrong:
//   - `deren_destroy_api_core` is gone: ownership IS the shared_ptr's control block, whose
//     deleter was baked in by the backend - there is no deleter name left to resolve, and
//     no raw pointer for a host to delete;
//   - `deren_abi_version()` is gone: the version is an INPUT (argument one), so a
//     disagreement is refused on the call that would have used it, and the backend's own
//     number travels back in the `error_info` it fills (`native_code`) plus its message.
// WHAT DOES NOT CHANGE: an empty `shared_ptr` means "refused", exactly as a null raw
// pointer did, and `create_info` is still the contract's one creation structure, borrowed
// only until the call returns.
// ============================================================================
#pragma once

#include "../../utility/abi_export.hpp"

#include <cstdint>
#include <memory> // std::shared_ptr: the ownership the entry hands over

/**
 * @file promise/rhi/backend_entry.hpp
 * @brief the single `extern "C"` entry point that hands an `api_core` across the boundary.
 * @ingroup boundary
 *
 * The C ABI is deliberately tiny: an expected version number in, a context out, and a diagnostic
 * channel the caller owns. Everything else the engine and the backend share is a virtual base class
 * in `deren.promise.rhi`, compiled by both sides independently (§4.1 item 1), so no symbol other than
 * this one is exchanged.
 *
 * Include this after `import deren.promise.rhi;` - the declaration below names the contract's types,
 * and an `import` declaration cannot be written in a header.
 */

// THE RETURN TYPE IS A C++ CLASS AT A `extern "C"` FUNCTION, and clang warns about exactly that
// (`-Wreturn-type-c-linkage`). It is deliberate here (see the banner): the C ABI is used for the NAME -
// an undecorated symbol a host can resolve by string - not to promise that only C types travel. The
// warning is silenced at the declaration and at the definition rather than by weakening either.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

extern "C" {

/// THE one producer of an `api_core`, and the only symbol a backend exports.
///
/// `abi_version` is the CALLER's `deren::promise::rhi::abi_version`: a mismatch is refused (an empty
/// `shared_ptr`) and reported through `out_error_info` as `error::abi_mismatch`, with the BACKEND's own
/// number in `native_code` - the value `deren_abi_version()` used to answer, now carried by the
/// diagnostic that explains the refusal instead of by a second entry point.
///
/// `desc` is the contract's ONE creation structure (promise/rhi/rhi.core_desc.cppm), passed by
/// POINTER because it is an append-only POD whose size the caller declares in `struct_size` - the
/// backend reads only the bytes the caller says it compiled, which is what lets an older program
/// talk to a newer backend. It is never null in a well-formed call, and a null one is refused with
/// `error::invalid_argument` rather than defaulted, because "I have no creation parameters" is a
/// caller bug and not a request for the standard context (`create_info{}` is how that is spelled).
/// It is BORROWED ONLY UNTIL THIS CALL RETURNS: the backend copies what it keeps (the window title
/// lives in the window system's own storage after construction).
///
/// `out_error_info` is the abi 13 diagnostic channel: the CALLER provides the storage (an
/// out-parameter whose storage the caller owns can stay a frozen type - the by-value rule does not
/// bind here), and the backend fills the WHOLE structure - the decision code AND the diagnosis
/// (which graphics API produced it, the raw native code, static text, the failure point). On a
/// version mismatch it returns an EMPTY shared_ptr and fills `error::abi_mismatch`; a caller may not
/// assume the out-parameter was written on failure, but when it WAS written, it is the whole story.
///
/// OWNERSHIP: the returned `shared_ptr` carries the backend's own deleter, so the object is destroyed
/// inside the image that made it, whenever the last reference goes - the engine holds it as its
/// runtime's device root. A null return is the contract's "no context for you" and never a half-built
/// one: an internal failure is the backend's own named panic (its startup rule).
DEREN_API_EXPORT std::shared_ptr<deren::promise::rhi::api_core>
deren_make_api_core(std::uint32_t abi_version, deren::promise::rhi::create_info const* desc,
                    deren::promise::rhi::error_info* out_error_info);

} // extern "C"

/// The entry's C++ type, so a host that resolves the symbol BY NAME can name what it resolved
/// (the runtime's loader does: `reinterpret_cast<deren_make_api_core_fn>(address)`). Kept in this
/// header, beside the declaration it must match - a second spelling elsewhere would drift silently.
using deren_make_api_core_fn = std::shared_ptr<deren::promise::rhi::api_core> (*)(std::uint32_t,
                                                                                  deren::promise::rhi::create_info const*,
                                                                                  deren::promise::rhi::error_info*);

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
