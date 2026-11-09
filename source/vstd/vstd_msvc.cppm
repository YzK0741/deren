// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// vstd - MSVC dialect of the project's STL module.
//
// VENDORED FROM THE MSVC TOOLCHAIN
// -------------------------------
// The body below is the MSVC STL's modules/std.ixx, copied from Visual Studio
// 2022 17.14 / MSVC 14.44.35207 and renamed from `std` to `deren.vstd`. It includes
// the full standard-library header set inside the module purview; the
// `_BUILD_STD_MODULE` define makes those headers export their entities from this
// module. Build it as a normal source module - no `import std;`, no CMake
// CXX_MODULE_STD path, and no experimental import-std gate.
//
// WHY NOT vstd.cppm ON MSVC
// -------------------------
// source/vstd/vstd.cppm is the libc++-derived 71-partition re-export. cl.exe 19.44
// crashes with C1001 on every TU that instantiates std::span / std::array /
// std::tuple through it (7 of 326 objects on the real tree); see source/vstd/README.md
// for the measured evidence. Using the MSVC STL's module source avoids that
// construction while keeping the project-facing module name `deren.vstd`.
//
// WHAT THAT COSTS - STATED PLAINLY, NOT HIDDEN
// --------------------------------------------
// * Under MSVC the standard library is the MSVC STL, with MSVC semantics, not the
//   libc++ that source/vstd/vstd.cppm is generated from and byte-bound to. The
//   clang64 build - the one this repository verifies, ships and benchmarks - is
//   untouched by this file.
// * The module exports the whole standard library, not the trimmed header whitelist
//   of vstd.cppm, and it is bound to the installed MSVC STL version. Refresh this
//   copy when the toolchain is upgraded.
// * The VSTD_* portability macros (source/vstd/vstd_compat.inc) are not part of this
//   module's face; consumers only use the standard-library names.
//
// VERSIONING: this file does not change the shipped (clang64) module's face, so
// source/vstd/vstd.cppm's banner remains the versioned interface. If the project
// decides the MSVC dialect is part of vstd's public interface, bump both banners
// together.

// In a module-file, the optional `module;` must appear first; see [cpp.pre].
module;

// This named module expects to be built with classic headers, not header units.
#define _BUILD_STD_MODULE

// The subset of "C headers" [tab:c.headers] corresponding to
// the "C++ headers for C library facilities" [tab:headers.cpp.c]
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchar.h>
#include <wchar.h>
#include <wctype.h>

// <intrin.h> defines some types outside of `extern "C"` or `extern "C++"`.
#include <intrin.h>

export module deren.vstd;

#pragma warning(push)
#pragma warning(disable : 5244) // '#include <meow>' in the purview of module 'std' appears erroneous.

// "C++ library headers" [tab:headers.cpp]
#include <algorithm>
#if _HAS_STATIC_RTTI
#include <any>
#endif // _HAS_STATIC_RTTI
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <bitset>
#include <charconv>
#include <chrono>
#include <codecvt>
#include <compare>
#include <complex>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <execution>
#if _HAS_CXX23
#include <expected>
#endif // _HAS_CXX23
#include <filesystem>
#include <format>
#include <forward_list>
#include <fstream>
#include <functional>
#include <future>
#if _HAS_CXX23
#include <generator>
#endif // _HAS_CXX23
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <latch>
#include <limits>
#include <list>
#include <locale>
#include <map>
#if _HAS_CXX23
#include <mdspan>
#endif // _HAS_CXX23
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numbers>
#include <numeric>
#include <optional>
#include <ostream>
#if _HAS_CXX23
#include <print>
#endif // _HAS_CXX23
#include <queue>
#include <random>
#include <ranges>
#include <ratio>
#include <regex>
#include <scoped_allocator>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <source_location>
#include <span>
#if _HAS_CXX23
#include <spanstream>
#endif // _HAS_CXX23
#include <sstream>
#include <stack>
#if _HAS_CXX23
#include <stacktrace>
#endif // _HAS_CXX23
#include <stdexcept>
#if _HAS_CXX23
#include <stdfloat>
#endif // _HAS_CXX23
#include <stop_token>
#include <streambuf>
#include <string>
#include <string_view>
#include <strstream>
#include <syncstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <variant>
#include <vector>
#include <version>

// "C++ headers for C library facilities" [tab:headers.cpp.c]
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfenv>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <clocale>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuchar>
#include <cwchar>
#include <cwctype>

#pragma warning(pop)
