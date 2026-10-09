# Conventions

The rules this repository follows when a name has to be chosen, with the reason each one exists.
Where a rule has exceptions, the exception list is the reason.

## Module names

**A module name says what the module IS, in full words.** No abbreviations.

The tree has 56 modules. These keep a short spelling, and each one is a NAME rather than an abbreviation:

| keeps | why |
| --- | --- |
| `deren.vstd` | the project's own STL module name - documented in `vstd/README.md`, imported by 34 files |
| `deren.gltf_loader`, `deren.vulkan.core.pipeline:spirv_parser` | glTF and SPIR-V are **format** names |
| `deren.vulkan.core:vma`, `deren.utility:better_pmr` | VMA and PMR are the library's and the standard's own terms (Vulkan Memory Allocator, polymorphic memory resource) |
| `deren.vulkan.core:init_utils`, `deren.vulkan.init_utils`, `deren.vulkan.constant_init` | `init` was kept by request |
| `vulkan.pass:taa`, `vulkan.pass:fxaa` | the `-aa` pair was kept by request |
| `vulkan.pass.megalights_*` | a feature of this renderer, not a shortening of one |

The rule renamed `app_config` -> `application_configuration`, `vulkan.gui` ->
`deren.vulkan.graphical_user_interface` and `vulkan.pass.{rt_shadow,gbuffer_debug}` ->
`deren.vulkan.pass.{ray_traced_shadow,geometry_buffer_debug}`. The `app_config` STRUCT, the `gui` CLASS
and the `gbuffer_debug` / `rt_shadows` CONFIG KEYS kept their spelling, because none of them is a module
name.

### The rule that decides whether a submodule becomes a partition

**A partition cannot be imported from outside its module.** That is a hard constraint, not a preference, so
a dotted submodule becomes a `:partition` exactly when **nothing outside its own module imports it** -
measured per module rather than judged:

* it moved: `deren.vulkan.core:init_utils`, `:vma`, `:vma_handles`, `:descriptor_heap`,
  `deren.vulkan.core.pipeline:spirv_parser`, and `deren.utility:better_pmr`, `:bvh`, `:data_block`, `:frame_clock`,
  `:frame_stats`, `:thread_pool`;
* it stayed a module: `deren.vulkan.core.handles` (12 importers outside the family),
  `deren.vulkan.render_resource.shared` (10), `deren.vulkan.core.pipeline` (6), `deren.vulkan.core.filters` (1). Converting
  those would force every consumer to import the whole parent, which is the opposite of what the split is
  for.

## Verb prefixes

Eight verbs lead the names on a module's surface. A public name that starts with none of them is worth a
second look.

| prefix | means |
| --- | --- |
| `make_` | build a value and return it; pure, no GPU work |
| `create_` | create a GPU object (buffer, image, pipeline, pool) at runtime |
| `build_` | construct a pipeline or a structure from a description |
| `write_` | write into something that already exists (a heap descriptor, a buffer) |
| `set_` | change one tunable; no work beyond recording the choice |
| `update_` | refresh per-frame state |
| `ensure_` | idempotent lazy creation: make it if it is not there yet |
| `record_` | record commands into a command buffer |

They are not interchangeable, and the difference is what makes a call site readable at a glance:
`set_shadow_cascades` is a knob, `ensure_shadow_resources` allocates, `record_shadow_content` draws.

## Partitions

**One file per partition.** CMake names a partition's dyndep output after the MODULE, so a named partition
cannot also have a separate `.cpp` implementation unit: both spellings of that pair were measured to fail
(ninja reports `multiple rules generate ...<partition>.pcm`, or CMake reports that the file is not in a
`CXX_MODULES` file set). A module's own unnamed implementation unit (`utility.cpp`) is fine either way.

A partition **does not see the primary interface**, and imports are **not transitive**: each partition
imports `:declarations` and whatever else it calls. The primary **may not** import its own implementation
partitions either - clang 22 rejects it with `-Wimport-implementation-partition-unit-in-interface-unit` -
and listing them in the CMake module file set is what compiles them.

## Where a helper belongs

Beside the thing it is defined in terms of, and published once. Example: `core::heap_slot_offset` existed as
three copies because it multiplies `core::heap_slot_stride`; the helper and the constant now live together,
and moving the helper into the heap-plumbing partition instead would have closed a cycle
(`core.declarations.cppm` already re-exports `:descriptor_heap`).

## Arithmetic types

**Every arithmetic type the project declares is written as a fixed-width one**: `uint8_t`, `int32_t`, `uint32_t`,
`uint64_t`. A bare `int`, `unsigned`, `short`, `unsigned char` or `long long` states its width only by
convention, and the convention is not the same everywhere this is built.

The change that introduced the rule (83 files) was a **spelling** change and is verified as one: on these
platforms `int32_t` IS `int` and `uint8_t` IS `unsigned char`, so the render gate is byte-identical. What it
buys is that a width is stated where it used to be implied.

Three exceptions, each because somebody else's interface declares the type:

| keeps | why |
| --- | --- |
| `int main(int argc, char** argv)` | the C++ standard requires main's own signature to use the keyword: the one place where the language, not the project, decides |
| `timespec::tv_nsec` as `long` | it IS `long` in POSIX |
| `std::strtol`'s answer | `long`, whose width IS the platform's (32 bits on Windows, 64 on Linux), so the conversion is an explicit `static_cast<int32_t>` rather than an implicit one |

The rule does NOT reach `float` / `double` (no fixed-width equivalent in `<cstdint>`) or a bare `char`,
which is a CHARACTER type: only `unsigned char` / `signed char` are spelled `uint8_t` / `int8_t`.
`third_party/` and `vstd/` are out of scope - vendored, and the libc++ mirror.

## Portability

The rules above are about NAMES. The rules that follow from the toolchains this is built against are a separate
page, each with the measurement behind it: what a module interface may not do with an incomplete type, why
`NOMINMAX` is guarded before `<windows.h>`, and why the project owns `deren::utility::print`/`println` instead of using
`std::print`. See \ref md_docs_2compiler__tolerance "Compiler tolerance".
