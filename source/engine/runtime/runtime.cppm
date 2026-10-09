// ============================================================================
// module: deren.engine.runtime  - THE CONTRACT-ONLY RUNTIME (dynamic-backend migration, step 2)
//
// WHY A SECOND RUNTIME EXISTS: the flip turns `deren_vulkan` into a library loaded by name, and from that
// moment no engine file may import a module the backend owns. The runtime that names `core` is the biggest
// such client, so this is the runtime that never names it: it is constructed through `deren_make_api_core()`,
// holds the contract's `rhi::api_core` behind a `shared_ptr` with the BACKEND's own deleter, and reaches
// every raw Vulkan handle through `query_extension<rhi::vulkan_escape>()`.
//
// THE MODULE NAME AND THE PARTITION NAMES ARE THE LEGACY RUNTIME'S, deliberately: `main.cpp`, `chores`, the
// passes and the tests import `deren.engine.runtime` and must not change a line when the file list switches
// to this runtime (`-DVR_RUNTIME=dynamic`). One runtime per BUILD TREE, because two BMIs of one module name
// cannot both be visible to one translation unit - and the legacy tree stays the CONTROL for this one: both
// must read the same fourteen render hashes (docs/dynamic_runtime.md, section 1).
//
// WHAT EXISTS HERE NOW: S1, the scaffolding - this umbrella, `:declarations` (the class and its interface)
// and `:constructor` (construction through the C entry, the face, the escape, teardown). The partitions the
// port grows into are `:frames` (the frame loop), `:probes` (the two startup probes) and `:readback` (the
// screenshot path), in the order and under the acceptance docs/dynamic_runtime.md records.
//
// THE PRIMARY INTERFACE IS DELIBERATELY THIS SMALL, for the reason the legacy runtime.cppm states: under
// -Werror clang rejects a primary that imports its own implementation partitions, and it does not need to -
// CMake compiles every partition in the module's file set, so their definitions are archived and the linker
// finds them.
// ============================================================================
export module deren.engine.runtime;
export import :declarations;
