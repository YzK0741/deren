// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi
// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))
//
// The RHI boundary's contract: the abstract interfaces the engine and the backend
// share, and nothing else (RHI plan v4, §3.1 and §4.1). One module, four
// partitions - `:contract` (the ABI number and the `error` enum), `:core_desc`
// (how the program asks for a context), `:extension` (tier-2: the abilities a
// backend may or may not have) and `:api_core` (tier-1: the context, its
// factories and the frame calls).
//
// WHY THE NAME IS deren.promise.rhi (m03716, corrected by m03931): the contract layer as a whole is
// `promise` - a family of abstract interfaces any boundary of this project can express in the same
// shapes - and the graphics-API instance of it is the `rhi` submodule inside it. So the outer
// segment names the family, the inner segment names the API the layer is written for, the module is
// deren.promise.rhi, the types are deren::promise::rhi::*, and the directory is source/promise/rhi/. Both
// segments are lower case on purpose: every other module segment in this repository is lower case
// and names the directory that holds it (`source/backends/vulkan/`, `source/utility/`, `source/vstd/`), module names end up in
// CMake/BMI paths and in doc page names, and two directories differing only in case are a hazard on
// the case-insensitive filesystems this project is built on. The acronym stays upper case in prose
// ("the RHI"), where it is a word rather than an identifier; all-caps identifiers here are macros
// (DEREN_API_*, VR_TEST_*).
//
// WHY A MODULE RATHER THAN A DIRECTORY OF HEADERS: this layer is a set of virtual
// base classes that BOTH sides compile into their own image (m03159; §4.1's rule is
// that neither side exports a module symbol for it). A module makes the two
// compilations come from one interface unit - listed once per target in CMake - so
// they cannot drift, and it takes the preprocessor out of the layer entirely: no
// partition below this line includes a header, so no macro can leak into the shape
// of the contract.
//
// WHAT IS DELIBERATELY NOT IN HERE:
//
//   - the `extern "C"` entry point (`deren_make_api_core`) and the
//     `DEREN_API_*` keywords it is declared with. It is the ABI SURFACE, not an
//     interface: a backend defines it and a host resolves it, and neither
//     derives from anything. `source/promise/rhi/backend_entry.hpp` declares it,
//     `source/utility/abi_export.hpp` owns the keyword. (This is the m03159 ruling:
//     promise is a collection of virtual base classes.)
//   - engine concepts: no scene, no pass, no camera, no renderer state - `api_core` and every
//     interface below it must not learn one, which is the plan's §4.1 item 2 rule.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================

export module deren.promise.rhi;

export import :contract;
export import :core_desc;
export import :extension;
export import :api_core;

/**
 * @file source/promise/rhi/rhi.cppm
 * @defgroup promise The Promise Contract (backend boundary)
 * @brief the abstract interfaces the engine and the backend share - a module of vtables, PODs and
 *        spans, compiled by both sides and exported by neither (RHI plan v4 §3.1, §4.1, m03159).
 *
 * The partitions that make up the contract, and the one file that is deliberately not part of it:
 *
 * | part | what it is |
 * | --- | --- |
 * | `source/promise/rhi/rhi.contract.cppm` | the ABI number, the `error` enum every entry point reports through, and the RHI ownership containers (`unique_object` / `shared_object`) |
 * | `source/promise/rhi/rhi.core_desc.cppm` | how the program asks for a context: the structure `deren_make_api_core()` takes |
 * | `source/promise/rhi/rhi.extension.cppm` | tier-2: the abilities a backend may or may not have |
 * | `source/promise/rhi/rhi.api_core.cppm` | tier-1: the context, its factories, the frame calls |
 * | `source/promise/rhi/backend_entry.hpp` | the `extern "C"` entry point that hands an `api_core` out (same directory, but neither a partition nor imported) |
 *
 * @details
 * - **Two tiers, one rule.** `api_core` carries only what every backend has a concept for (DX12
 *   included), and an ability a backend may lack lives in tier-2, announced as one bit in
 *   `api_core::abilities()` (§1.5, §1.8). A pass that needs an ability declares it; a backend that
 *   does not have it is a named failure, never a silent downgrade (§1.9).
 * - **No interface names engine state** (§4.2): every interface is a vtable, every parameter is a POD,
 *   a `std::span` or an opaque handle, and every failure is a returned `error`. The contract's own
 *   ownership containers (in `:contract`) are the one allocation: the `shared_object` control block.
 * - **A module, but not one that crosses the boundary as a symbol.** Both sides compile this
 *   interface unit into their own image, which is what keeps the engine off the DLL's import
 *   library; the only exchanged name is the `extern "C"` entry point in
 *   `source/promise/rhi/backend_entry.hpp` (the header, not the module).
 * - **Ownership is spelled out, not implied.** An object made inside the backend is dropped inside
 *   the backend: the entry hands out a `shared_object<api_core>` (abi 44) whose last copy calls the
 *   contract's `release()`, and the backend's implementation of that call destroys the object (§4.1 item 3).
 */
