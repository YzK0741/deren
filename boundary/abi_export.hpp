// -*- C++ -*-
// ============================================================================
// file: boundary/abi_export.hpp
//
// The export/import keywords for the boundary's C ABI (RHI plan v4, §1.15, §4.1).
//
// The backend boundary is a set of `extern "C"` entry points, but the keyword that
// makes them visible across a DLL boundary is not portable and depends on how the
// translation unit is being built:
//
//   - static build (no DEREN_API_SHARED): ordinary C++ symbols, no attribute at all.
//   - shared build, compiling the backend (DEREN_API_SHARED + DEREN_API_BUILD):
//     dllexport on Windows, default visibility on ELF.
//   - shared build, importing the backend (DEREN_API_SHARED, no DEREN_API_BUILD):
//     dllimport on Windows, nothing on ELF.
//
// DEREN_API_HIDE is the mirror image: it keeps a symbol OUT of the dynamic export
// table on a platform that would otherwise export everything (ELF without a
// visibility preset). Measured with these two macros (plan §10.3, round 4 and the
// earlier rounds): the probe DLL exports exactly its three entry points, and a
// statically linked test binary exports none of them.
//
// The backend that will export those three symbols is the module under vulkan/ once
// the boundary lands (plan §7.4); today the only user is the probe backend
// tests/probe_backend.cpp. Keeping the macro names here - next to the entry points
// they are applied to in boundary/backend_entry.hpp - is the point: one header owns
// the question "is this build shared?".
// ============================================================================
#pragma once

/**
 * @file boundary/abi_export.hpp
 * @brief the export/import keyword the backend's C ABI is declared with, chosen from the build mode
 *        rather than from a platform `#ifdef` at every declaration.
 * @ingroup boundary
 *
 * The boundary is a handful of `extern "C"` entry points, and whether one of them is visible across
 * a DLL boundary depends on how the translation unit in front of it is being built:
 *
 * | build | `DEREN_API_EXPORT` |
 * | --- | --- |
 * | linked in statically (no `DEREN_API_SHARED`) | nothing - an ordinary C++ symbol |
 * | compiling the backend (`DEREN_API_SHARED` + `DEREN_API_BUILD`) | `dllexport` / `visibility("default")` |
 * | importing the backend (`DEREN_API_SHARED`, no `DEREN_API_BUILD`) | `dllimport` / nothing |
 *
 * @details
 * - `DEREN_API_HIDE` is the mirror image, for a platform that would otherwise export everything:
 *   on Windows it is empty by construction (nothing is exported unless marked `dllexport`).
 * - Measured (plan §10.3): the probe DLL exports exactly its three entry points and the statically
 *   linked test binary exports none of them, which is what the two macros promise.
 * - CMake defines the macros per target (`tests/probe_backend.cpp` is compiled twice from one
 *   source file for exactly this reason), so no source file has to know which build it is in.
 */

/**
 * @defgroup boundary The Boundary (the C ABI a backend exports)
 * @brief the three `extern "C"` symbols a backend exports and a host resolves, and the keyword that
 *        makes them visible across a DLL boundary.
 *
 * The C ABI is deliberately tiny: a version number, a factory whose ownership transfer is spelled
 * out by the deleter it returns, and that deleter (see `boundary/backend_entry.hpp`). Everything
 * else the engine and the backend share is a virtual base class in `deren.promise`, compiled by
 * both sides independently, so no C++ type is passed by value and no symbol other than these three
 * is exchanged. It is a group of its own rather than part of `deren.promise` because an entry point
 * is not a virtual base class (m03159).
 */

#if defined(_WIN32)
#if defined(DEREN_API_SHARED)
#if defined(DEREN_API_BUILD)
#define DEREN_API_EXPORT __declspec(dllexport)
#else
#define DEREN_API_EXPORT __declspec(dllimport)
#endif
#else
#define DEREN_API_EXPORT
#endif
// Windows has no "hide" for a single symbol: nothing is exported unless it is
// marked dllexport, so the hide macro is empty by construction.
#define DEREN_API_HIDE
#else
#if defined(DEREN_API_SHARED)
#define DEREN_API_EXPORT __attribute__((visibility("default")))
#define DEREN_API_HIDE __attribute__((visibility("hidden")))
#else
#define DEREN_API_EXPORT
#define DEREN_API_HIDE
#endif
#endif
