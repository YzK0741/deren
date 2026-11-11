// ============================================================================
// module: vstd
// module version: 0.1.2a  (independent of the app version in CMakeLists project(VERSION))
//
// The public facade of the project's STL module. Consumers always write
// `import deren.vstd;`; the backend behind it is selected by the build:
//
//   clang64: deren.vstd.libcxx  (source/vstd/vstd_libcxx.cppm, modified from libc++)
//   MSVC:    deren.vstd.msvc    (source/vstd/vstd_msvc.cppm, vendored MSVC STL)
//
// Both backends export the same standard-library surface. This file only
// re-exports the selected one, so the public module name and its version stay
// independent of the toolchain.
//
// extend: vstd is the natural home for project-local STL extensions
//         (vstd-only additions beyond libc++) later on - bump the version
//         below when they land.
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features
//         (including extensions), PATCH on internal fixes - independently of
//         the rest of the project.
// ============================================================================

/**
 * @file vstd.cppm
 * @defgroup vstd vstd STL Module
 * @brief the project's STL module: consumed via `import deren.vstd;`.
 *
 * @details
 * - This is a facade over one selected backend; it does not define any
 *   standard-library entity itself.
 * - clang64 selects `deren.vstd.libcxx` (trimmed libc++ re-exports).
 * - MSVC selects `deren.vstd.msvc` (vendored MSVC STL module source).
 * - The backend is chosen by `source/vstd/vstd.cmake` through
 *   `DEREN_VSTD_LIBCXX` or `DEREN_VSTD_MSVC`.
 * @version 0.1.2a
 */

export module deren.vstd;

#if defined(DEREN_VSTD_MSVC)
export import deren.vstd.msvc;
#elif defined(DEREN_VSTD_LIBCXX)
export import deren.vstd.libcxx;
#else
#error "vstd: no STL backend selected (define DEREN_VSTD_MSVC or DEREN_VSTD_LIBCXX)"
#endif