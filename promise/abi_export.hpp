// -*- C++ -*-
// ============================================================================
// file: promise/abi_export.hpp
//
// The export/import keywords for the promise/ C ABI (RHI plan v4, §1.15, §4.1).
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
// Today the only user is the probe backend tests/probe_backend.cpp; the real
// backend under promise/ is what will consume it when the boundary lands
// (plan §7.4). Keeping the macro names here - next to where that backend will
// live - is the point: one header owns the question "is this build shared?".
// ============================================================================
#pragma once

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
