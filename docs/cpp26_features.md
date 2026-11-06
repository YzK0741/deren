# C++26 features in use

This page is the C++26 adoption register. It records language and library features
that first-party code actually uses, not every feature the toolchain advertises. Add
a row when a change adopts one; keep the tables honest rather than turning this page
into a wish list.

## Baseline

- Compile mode: `CMAKE_CXX_STANDARD 26` (`-std=c++26`).
- Measured toolchain: clang 23.1.4 with libc++ 23.1.4.
- C++26 library features are not assumed. A call site is enabled only when the
  matching `__cpp_lib_*` macro is defined, and `deren.vstd` exports only names the
  installed libc++ declares. See `docs/compiler_tolerance.md` for the toolchain
  limits.

## Language features

| Feature | First use | Notes |
|---------|-----------|-------|
| `= delete("reason")` | `source/promise/rhi/contract/rhi.object.cppm:72` | `rhi::unique_object` explains its move-only ownership rule in the compiler diagnostic. All first-party deleted special members now carry a reason; vendored `source/third_party` is untouched. |
| Placeholder variable `_` | `source/utility/shared/shared_utility.cppm:168` | `allocator::deallocate` marks its unused size argument `_` instead of naming it `count`; a placeholder variable is intentionally discarded and exempt from unused-parameter warnings. |
| Structured binding declaration as a condition | `source/tests/test_utility.cpp:49` | Compile-time probe decomposes a bool-convertible result directly in the `if` condition. This validates the language path; production adoption still needs a suitable result type. |

## Library features

| Feature | First use | Notes |
|---------|-----------|-------|
| None recorded yet. | - | Do not list a feature here until a `__cpp_lib_*` guard and a call site exist. |

## Adding an entry

Name the feature, the first file or change that uses it, and the feature-test macro
(or compiler version) that proves it. A feature that only exists behind a
`__cpp_lib_*` probe belongs in the library table, not the language table. When the
first real entry lands, delete the placeholder row for that table.
