# Coding standard

This page is the normative standard for first-party code. It says how a file is written, not why the
project exists; the naming and module-layout decisions that carry their own history stay in
\ref md_docs_2conventions "Conventions", and the process of landing a change is
`CONTRIBUTING.md`. Where a rule here and one there disagree, the more specific page wins and this page
gets fixed.

Two properties matter more than any single rule:

- **The tools are the formatting standard.** Layout is whatever `clang-format` produces from
  `.clang-format`, analysis is whatever `clang-tidy` accepts with the configured subset, and the C#
  formatting is `.editorconfig` plus `dotnet format`. Nobody reviews indentation or brace placement by
  hand; a reviewer asks whether the diff is formatted, and the answer is a tool run.
- **A rule is explained where it is not obvious.** This repository's comments spend lines on the
  failure a piece of code prevents, not on what the code restates. The standard asks for the same in
  reviews: if a rule needs a reason at the call site, the reason goes into the comment, not into the
  reviewer's memory.

## The gates that enforce it

| Gate | Checks | Where |
| --- | --- | --- |
| `clang-format --dry-run --Werror` on changed files | layout, include order | local gate, CI |
| `clang-tidy` on the changed-file subset | the checks left enabled in `.clang-tidy` | local gate, CI |
| compiler warnings | no new warnings in the touched files | local gate, CI |
| `dotnet format` / IDE analyzers | C# style and analyzer diagnostics | `.editorconfig` |
| `test_docs` | the documents are ASCII, listed, and internally consistent | CTest |
| CTest, backend-boundary ratchet, ASan/UBSan jobs | behavior and architecture | CI |

Passing a tool is necessary, not sufficient. The rules below are the part no tool checks.

### Lint cleanliness

A change leaves the lint result no worse than it found it. On the lines it touches it adds no
`clang-format` fix, no `clang-tidy` diagnostic, no compiler warning, and no IDE analyzer diagnostic.
The fix is the code. A suppression is allowed when it is justified - the check is wrong for this call
site, the platform forces the shape, the third-party type owns the spelling - and then the reason is
written where the suppression lives: a comment over the disable entry in `.clang-tidy`, a sentence
above the `.editorconfig` section, an attribute beside the declaration. An unexplained suppression is
the mistake, not the suppression itself.

The enabled check set is part of the standard, not a per-change dial. Widening `.clang-tidy` or
tightening `.editorconfig` is its own change, measured against the whole tree, never a side effect of
a feature.

## C++ is the default language

### One unit, one file

- An interface unit is `.cppm` and declares `export module deren.<area>[.<feature>];` or an explicit
  partition `export module deren.<area>:<partition>;`. An implementation unit is `.cpp` and repeats the
  module name without `export`. A partition's name equals its file stem, and one file holds one
  partition: the build treats the stem as the identity, so "two partitions in one file" is a
  disagreement the build eventually reports as a missing module.
- `.hpp` is reserved for what cannot be a module: the C ABI surface (`abi_export.hpp`, the host
  entry points), platform shims, and the small neutral header-only helpers whose whole point is to be
  included before any module is imported. Engine types do not live in headers; a type that needs an
  include guard is a type that belongs in an interface unit.
- Templates live in the interface unit that exports them. There is no inline-heavy header layer,
  and no pimpl: a class owns its state directly. The GUI is the precedent that was checked - a plain
  class with direct members, with the reason stated at the declaration (`gui.cppm:37`: ImGui's context
  lives in ImGui's global storage, so the wrapper tracks only initialization and its panel list rather
  than hiding state behind a pointer).

### The top of a file

Every substantial interface unit starts with a banner that states what the unit is, why it exists,
what it deliberately does not do, and what changed. The reusable module sets carry a `module version`
annotation; the annotation is authoritative, and it is bumped by the change that alters the exported
surface, not as a side effect of anything else (`README.md:92` is the policy).

```cpp
// ============================================================================
// module: deren.utility.shared
// module version: 1.5.0
//
// THE PROCESS-WIDE HALF OF utility: one log sink, one panic convergence, one
// allocator hook per process. WHY A SEPARATE MODULE: a DLL carries its copy of
// whatever it links, so two copies mean two ofstreams on one deren-debug.log and
// two startup rotations, and rotate_previous_log() truncates the file the other
// copy is still writing.
//
// 1.5.0: add the memory allocation service.
```

- Changelog lines go newest first, immediately after the reason.
- A file that is not independently reusable (an app-internal unit) may skip the version, but it keeps
  the banner and the reason.
- The banner is documentation, so it is English and ASCII like everything else in the tree.

### Modules, the std library, and their vstd

- The standard library is reached through `deren.vstd`, the libc++ mirror that also re-exports the
  standard names on MSVC. An interface unit writes `export import deren.vstd;`; an implementation unit
  writes `import deren.vstd;`. Raw `#include <vector>` in first-party code is a leftover, not a style.
- Includes that genuinely need textual inclusion go in a global module fragment: `module;` first, then
  the includes (C headers, Vulkan, GLFW, platform headers), then the module declaration. The fragment
  exists so the module never imports a header's macros into its interface.
- Import order is: module declaration, `export import`s, plain `import`s, then local aliases. An import
  whose purpose is not obvious from the name gets a one-line comment saying what type or behavior it
  brings.

### Formatting

`clang-format` (LLVM base, configured in `.clang-format`) is the authority: 4-space indentation, no
tabs, attached braces, braces inserted for one-line bodies, east `const` (`QualifierAlignment: Right`),
left-aligned pointers, namespaces and their contents indented, access modifiers outdented, a column
limit of zero (no wrapping; a line is broken by the author where it reads better), include blocks
regrouped and sorted case-sensitively. Run it on the files a change touches; do not reflow a file that
the change does not otherwise touch.

The layout is not negotiable; the order of declarations is the author's, with one convention:
public surface first, then protected, then private; constructors, the destructor, factories, queries,
mutators.

### Naming, in one line of shapes

Every first-party identifier is `lower_snake_case`: types, functions, fields, constants, modules,
namespaces, enumerators, local names, and file names. A mutable operation is prefixed with a verb
(`make`, `create`, `build`, `write`, `set`, `update`, `prepare`, `record` - the eight in
`docs/conventions.md`), a query reads as a predicate (`ready()`, `is_empty()`, `resource_size()`), and
an enum is `enum class <name> : std::uint32_t` with an explicit underlying width and unprefixed
enumerators. Two exceptions, and only two:

- Template parameters keep the usual C++ spelling (`T`, `Fetch`, `Parameters`). A parameter stands for
  a type the caller supplies, not for a name this code owns.
- A name that belongs to an external library keeps that library's spelling (`GLFWwindow`, the
  `GLFW_INCLUDE_NONE` switch). Renaming it would be a lie about who owns it.

The same shape holds in every language this page covers; the C# section repeats the rule with its
analyzer behind it, and the Slang structs and Python classes follow it as well. Names that predate
this page are brought into shape when their file is next edited, not in a rename-only sweep.

No Hungarian notation, no trailing underscores on members: `this->resource_heap_size`, not `m_size`.
Member access is spelled with `this->` throughout the backend (more than six thousand sites); follow
the file being edited rather than mixing both spellings in one class.

### Declarations and types

- East `const`: `VkDeviceSize const stride`, `std::span<std::byte const> const data`.
- Fixed-width integers from `<cstdint>` in first-party interfaces; the three documented exceptions
  (`main`'s `int`, `timespec`, `strtol`) are in `docs/conventions.md`.
- `[[nodiscard]]` on every pure query and every fallible result. A function whose return value can be
  ignored without harm is the only function without it.
- `noexcept` on everything that cannot throw through it. The default for the Vulkan backend is that an
  operation reports failure, it does not throw.
- `constexpr` for values that are compile-time facts, and `static_assert` for layout facts that cross a
  boundary.
- Aggregates initialize with designated initializers (`.sType = ...`, `.address = ...`), which keeps a
  Vulkan struct readable and prevents ordering mistakes.
- Lock guards use CTAD: `std::lock_guard lock(reservations);`. The named-type spelling is noise.
- Value records are `struct`; a type that maintains an invariant and deletes or validates its own
  copies is `class`. A class that must not be copied says so at the declaration with a message rather
  than leaving the default: `descriptor_heap(descriptor_heap const&) = delete ("...")`.
- Ownership is RAII, expressed in the RHI's own handles or in the utility's smart pointers. `new`,
  `delete`, `malloc`, and `free` do not appear in first-party code outside the allocator implementation.

### Control flow

- Early return over nested conditions; guard clauses first, the happy path last and unindented.
- Braces always, even for a one-line body; the tool inserts them and the review expects them.
- Validate arguments at the boundary of a unit, then trust them inside it. The failure of a broken
  contract inside the engine is `panic`, not a returned error: a caller that passed an impossible value
  is a bug, not a runtime condition. Recoverable failure is returned through the module's error value
  (`rhi::error`), logged once where the measured detail is known, and translated at the boundary that
  decides what the user sees. The tree has one failure policy page (`docs/error_handling.md`); a second
  representation of the same failure in the same call is the mistake that page forbids.
- Iterate with range-for and spans; index loops appear where the index is part of the algorithm.

### Comments and the generated reference

- A comment explains the constraint, the measurement, or the alternative that was rejected. It does not
  narrate the next line.
- Doxygen form on the exported surface: `@file`, `@defgroup`, `@brief`, `@param`, `@return`, `@note`,
  and `@ingroup` on entities that belong to a group. One-liners use `///`.
- Every comment is written in English and ASCII. The documents are held to ASCII by `test_docs`
  (LaTeX renders them, and one non-ASCII byte aborts that step); a source file that still carries
  older non-ASCII text is brought in line when it is next edited, not left as precedent.
- Numbers that a comment cites are measurements from this repository, with the measurement named.
  A comment that says "faster" without a number is a comment that will be questioned.

### Tests

- A test is `source/tests/test_<area>.cpp`, built as its own target, registered with `add_test`, and
  runnable headless. Its banner states the failure it prevents and, where a number is claimed, the
  number that was measured.
- Tests are deterministic. A test that needs a device reports that need and skips with exit code 77
  through `vk_test.h`, so a CI machine without one reports "skipped", not "passed".
- Assertions compare the whole value (bytes, layouts, counts), not a proxy for it.

### Third-party and mirrors

`source/third_party/` and the libc++ mirror in `source/vstd/` are read-only for first-party changes.
They are updated by their own import procedure; "fixing" a line in them silently breaks the next
import.

## C# - the managed scripting layer

The managed layer is a script SDK compiled into an assembly the game loads. Its style is fixed by the
repository's `.editorconfig` (`[*.cs]`): 4-space indents, K&R braces, braces required (IDE0011 is an
error), every identifier `lower_snake_case` (IDE1006 is an error).

- Use modern C# where it removes noise: file-scoped namespaces, collection expressions (`[]`,
  `[.. trimmed]`), target-typed `new()`, pattern matching, primary constructors for small records.
- `var` when the right-hand side names the type; the explicit type when the declaration is the only
  place the type appears.
- Nullability is part of the contract. `?.` is for values the contract admits as null, and
  `ExceptionObject.ToString()` is still nullable because a game type may override it to return null;
  a non-null contract with a `?.` is dead code pretending to be defensive.
- Interop mirrors are blittable structs at namespace scope with
  `[StructLayout(LayoutKind.Sequential)]`, fields never named like a static field of the enclosing
  class, and their size compared against `sizeof` at load (`byte_size != sizeof(layout)`).
  The host is x64 Windows only, so `[Cdecl]` furniture on `delegate* unmanaged` is noise: the platform
  already has one convention.
- Every fallible wrapper keeps its `out script_status` twin (`=> name(args, out _)`), because the short
  form and the checked form must not drift. Do not narrow a documented public entry point to satisfy a
  solution-wide analyzer: this assembly is a script SDK, its callers live outside this solution, and a
  `MemberCanBePrivate.Global` suggestion is not evidence. Suppressions go into `.editorconfig` with a
  comment that names the SDK reason.

## CMake

- `project(deren VERSION 0.7.0)` is the single version source; `CMAKE_CXX_STANDARD 26` is set once; do
  not add a `-std=` flag beside it.
- A directory's build logic lives in `<dir>.cmake` files included by the parent; each starts with
  `include_guard(GLOBAL)`. `CMakeLists.txt` wires directories and targets, it does not accumulate
  per-directory logic.
- Target names state the module or the module half they build (`deren_engine`, `deren_assets`,
  `static_utility` / `shared_utility`, `app_support`, `gaussian_loader`); interface units are listed
  with `FILE_SET CXX_MODULES`; compilation sources are named one by one, not globbed (the formatter's
  own file list is the one deliberate glob).
- A CMake comment explains the hazard (compiler quirk, dependency direction, measured cost), not the
  intent of the next command.

## Python - the scripts

- The scripts are build and verification helpers, not products. They run on the Python CMake finds on
  Windows: 3.8 syntax, standard library first, a dependency added only for a capability the standard
  library cannot reach (Vulkan tools, file hashing at a measured cost).
- 4-space indents, type hints on function signatures, `argparse` for options, a docstring on a script
  that says why it exists and what it refuses to do.
- Exit codes are the interface: 0 success, non-zero failure, 77 for a test that cannot run here. A
  script prints what it checked when it fails, not when it succeeds.
- A script writes only under `build/`, `tmp/`, or the path it was given; nothing else in the tree is a
  scratch directory.

## Shaders

Slang is the source language (`source/shaders/*.slang`); the GLSL files are the archived first form.
Shader files carry their stage content in one file per family, include from paths relative to
`source/shaders/`, and keep the descriptor records in step with the C++ structs that bind them - a
layout change on this side is a shader change in the same commit.

## The documents

`docs/*.md` is build input, not commentary: every page is listed in `Doxyfile`'s `INPUT`, the bytes are
ASCII, code fences and `@code`/`@endcode` are balanced, and every inline anchor link resolves inside its
own file. `test_docs` asserts those invariants because none of them fails visibly in a diff. A page that
describes history says so in its own words (the migration pages are the precedent); it does not update
the historical text to match today's API.

## What a reviewer looks at first

1. Does the diff's module boundary and file split match the unit rules above?
2. Are failures returning `error`/`bool` where recoverable and panicking where the contract is broken,
   with exactly one representation per call?
3. Do the new comments state a constraint or a measurement, in ASCII English?
4. Are the touched files formatted and warning-clean, and is the test that covers the change present?
5. Does the document that describes the behavior (`docs/`) change in the same commit as the behavior?
