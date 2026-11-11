# vstd — the project's STL module

> **What this is**: `vstd` is **modified from libc++** (LLVM's C++ standard
> library, the C++23 library shipped with clang). It is *not* a rewrite of
> the STL, *not* derived from any other std implementation (e.g. GNU
> libstdc++), and *not* an independent implementation: it is libc++'s
> generated `std` module, renamed to `vstd` and **trimmed to the headers this
> project actually uses**. The exported entities are the libc++ entities,
> re-exported with `using`; the definitions come from the toolchain's libc++
> headers at compile time.
>
> The whole module is therefore **byte-bound to the matching libc++** of the
> MSYS2 clang64 toolchain — keep it in sync on toolchain upgrades (see below).
>
> **UPDATE (portability layer)**: the binding is now confined to ONE file. The partitions
> name `VSTD_*` instead of `_LIBCPP_*`, and `source/vstd/vstd_compat.inc` defines each of those
> names — under libc++ *as* the `_LIBCPP_*` macro it replaces, and under another standard
> library from that library's feature-test macros. The partitions themselves contain
> no implementation-specific macro, and no internal namespace: see "Portability layer"
> below for what that buys and what it does not.

## Layout

```
source/vstd/
  vstd.cppm      public facade: export module deren.vstd; and a conditional
                 export import of the selected backend
  vstd_libcxx.cppm
                 libc++ backend: global-fragment #include <X> lines
                 (pull libc++ definitions) + export module deren.vstd.libcxx;
                 + one #include "std/X.inc" per used header (editable whitelist)
  vstd_msvc.cppm MSVC backend: vendored copy of the MSVC STL's modules/std.ixx,
                 renamed to deren.vstd.msvc - see "MSVC backend"
  std/           the .inc partitions referenced by vstd_libcxx.cppm (71 today),
                 copied from libc++'s module output - do not hand-edit
  LICENSE        LLVM project license (Apache-2.0 with LLVM exceptions),
                 covering the .inc content
  README.md      this file
```

Each `.inc` partition is a pure re-export block derived from the libc++
original: `export namespace std { using std::vector; ... }`. The full
upstream `std.cppm` / `std.compat.cppm` are **not** vendored here; only the
used partitions are kept.

## Backends

The public module is `deren.vstd` (`vstd.cppm`). It is a facade: CMake selects exactly one backend,
defines `DEREN_VSTD_LIBCXX` or `DEREN_VSTD_MSVC`, and the facade re-exports that backend with
`export import`. Consumers never name a backend.

### libc++ backend (`vstd_libcxx.cppm`)

The clang64 build compiles `vstd_libcxx.cppm`, which is the trimmed copy of libc++'s generated
`std` module described above. Its module name is `deren.vstd.libcxx`; the facade re-exports it as
`deren.vstd`.

### MSVC backend (`vstd_msvc.cppm`)

The MSVC build compiles `vstd.cppm` and `vstd_msvc.cppm`; it does **not** compile
`vstd_libcxx.cppm`. `vstd_msvc.cppm` is a vendored copy of the MSVC STL's `modules/std.ixx` from
Visual Studio 2022 17.14 / MSVC 14.44.35207. The body is upstream's unchanged except for the module
name:

```c++
export module deren.vstd.msvc;
```

The facade then re-exports it as `deren.vstd`. The C headers are included in the global module
fragment, then the full C++ library header set is included inside the module purview; the
`_BUILD_STD_MODULE` define makes those headers export their entities from `deren.vstd.msvc`. There is
no `import std;` declaration, no `CXX_MODULE_STD`, and no experimental import-std gate in the build:
this is an ordinary source module.

This is not a preference, it is a workaround for a measured compiler bug: with the libc++-derived
re-export construction, `cl.exe` 19.44 dies with `fatal error C1001` (internal compiler error,
three different signatures) on every TU that instantiates `std::span`, `std::array` or
`std::tuple` through the module - 7 of the tree's 326 objects, every one of them an importer of
`vstd`. Using the MSVC STL's module source avoids that construction. Minimal reproductions, a
partition-level bisection and the counter-experiments live in
`build-release-clang64/msvc/lead_lab/ICE_FINDINGS.md`.

What the MSVC backend costs, stated rather than hidden:

- under MSVC the standard library is the **MSVC STL**, not the libc++ that `vstd_libcxx.cppm` is
  generated from and byte-bound to. The clang64 build - the verified, shipped, benchmarked one -
  does not compile this file and its module face is unchanged.
- the vendored copy exports the **whole** standard library, not the trimmed header whitelist of
  `vstd_libcxx.cppm`, and it is bound to the installed MSVC STL version. Refresh the copy when the
  toolchain is upgraded, retaining Microsoft's copyright and SPDX header.
- the `VSTD_*` portability macros are not part of the MSVC backend's face. Measured safe today:
  no source outside `source/vstd/` names a `VSTD_*` macro, and no consumer writes `vstd::` for a
  project-local extension - consumers only want standard names.

The version banner in `vstd.cppm` is deliberately **not** bumped by the MSVC backend: it does not
change the clang64 module's face. If the project ever treats the MSVC backend as part of `vstd`'s
public interface, bump the facade banner and this comment together.

## Portability layer

`source/vstd/vstd_compat.inc` is included once, after `<__config>` and before the partitions. It gives every
implementation-specific macro the partitions use one portable spelling:

| partition says | libc++ (`_LIBCPP_*`) | another standard library |
|---|---|---|
| `VSTD_STD_VER` | `_LIBCPP_STD_VER` | derived from `__cplusplus`, in libc++'s numbering |
| `VSTD_HAS_*` (13 names) | the `_LIBCPP_HAS_*` flag | the standard's feature-test macro, else `__has_include` of the header |
| `VSTD_ENABLE_*` (3 opt-ins) | the `_LIBCPP_ENABLE_*` opt-in | never defined (nothing else has those entities) |
| `VSTD_USING_IF_EXISTS` | `__has_attribute(using_if_exists)` | expands to nothing |

**THE RULE THAT MATTERS: a `VSTD_*` name is defined ONLY WHEN THE CAPABILITY IS THERE, never as 0.**
The partitions test both ways — `#if VSTD_HAS_THREADS` and `#ifdef VSTD_ENABLE_EXPERIMENTAL` — and
only "defined iff capable" makes both mean what the libc++ original meant. The first version of the
layer defined them as 0 and silently compiled libc++'s *experimental* time zone database into the
module, because `#ifdef` was true while the toolchain's opt-in was not defined.

**What the layer cannot do**, so nobody expects otherwise: a name the other library has not
implemented still fails where a partition says `using std::that_name;`. That residue is found by
compiling against that library and guarded there, line by line; and `VSTD_USING_IF_EXISTS` softens it
only on a compiler with `__attribute__((using_if_exists))` (clang — GCC and MSVC do not, so there the
`using` is hard).

**What is NOT allowed back in**: a partition may not name a libc++ internal namespace. Four did —
`compare.inc`, `concepts.inc`, `iterator.inc`, `ranges.inc` re-exported the standard CPOs as
`using std::__cpo::...` / `using std::ranges::__cpo::...` inside an `inline namespace __cpo`. Those
are inline namespaces in libc++, so the public spelling (`std::partial_order`, `std::ranges::begin`,
…) names the same entity, and that is what the partitions say now.

## Portability notes (inspected)

The partitions are mechanically generated from libc++ and were audited:

- every line is a `using`-declaration inside `export namespace std` (plus
  comments/blank lines); no pragmas, no `#elif`/`#else`; `#if`/`#endif` pairs match 1:1.
- the only conditionals are the `VSTD_*` names above and `__has_builtin`; measured over the 71
  partitions: zero `_LIBCPP_*`, zero `_MSC_VER`/`__linux__`/`__APPLE__`/`__GLIBCXX`.
- **one platform branch remains, and it is upstream's**: `vstd_libcxx.cppm` wraps the
  `__has_include(<debugging>)` / `__has_include(<text_encoding>)` sanity check in `#ifndef _WIN32`,
  because on Windows the MSVC STL headers are on the search path and would make `__has_include` lie.
  (An earlier revision of this file claimed "zero `_WIN32` hits"; measured, there are two, both that
  guard.) It is harmless and necessary.

So the content is **toolchain-bound, not platform-bound**: it compiles
against the matching libc++ headers and must be regenerated with them, but it
has no Windows/Linux/compiler-specific separate forks.

## Versioning

`vstd` follows the same independent version scheme as every other module in the
repo (banner at the top of `vstd.cppm`, independent of the app version in
`project(VERSION)`):

- current: **0.1.2a**
- MAJOR: breaking interface changes
- MINOR: additive features — including project-local **extensions** (`vstd`
  is the natural home for STL additions beyond libc++; the module is not just a
  re-export shim, so bump MINOR when one lands)
- PATCH: internal fixes (trim changes, partition refreshes on a libc++ patch
  upgrade)

## Editing / trimming

`vstd_libcxx.cppm` keeps one `#include <X>` + `#include "std/X.inc"` pair per used
header, in the upstream order.

- **Adding** an STL feature: the compiler reports the missing entity; add that
  header's pair to `vstd_libcxx.cppm` and copy its partition into `std/` (from the
  toolchain's upstream libc++ module output).
- **Removing**: drop both lines and the partition file.

## Toolchain upgrade

The snapshot must match the installed libc++ (byte-compatible headers), as
always. On a clang64 upgrade:

1. From the new toolchain's libc++ include tree, obtain the fresh upstream
   module output (the generated `std.cppm` and its `std/` partition set - the
   same source this repo originally modified).
2. For every header currently listed in `vstd_libcxx.cppm`, copy the fresh `X.inc`
   over `std/X.inc` (contents may change between libc++ versions) and confirm
   the `#include <X>` lines still exist upstream; **rename every `_LIBCPP_*`
   macro in the fresh partition to its `VSTD_*` spelling** (the names are listed
   in `source/vstd/vstd_compat.inc`), then re-verify the trim with a full build.
3. If a fresh partition names a macro the compat layer does not define yet, the
   build fails on that name - the intended failure mode, not something to work
   around: add its `VSTD_*` spelling to `source/vstd/vstd_compat.inc` (defined only when
   the capability is there) and rebuild.
4. `source/vstd/std/compare.inc`, `concepts.inc`, `iterator.inc` and `ranges.inc` carry
   a locally written `PORTABILITY LAYER` note where libc++'s generated output
   re-exported an internal namespace (`std::__cpo`, `std::ranges::__cpo`). The
   fresh copy will bring those lines back: replace them with the public spelling
   again (see "Portability layer").
