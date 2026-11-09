# Core facets: splitting the Vulkan device root by RHI responsibility

Status: implemented and verified on `refactor/core-facets`; published as PR #55
(https://github.com/YzK0741/deren/pull/55). Phases 1-5 are complete: each facet contains
its Vulkan state and implements its RHI entry points; `core` keeps thin delegating entry points for its
remaining internal callers. The full Release build succeeds, CTest passes 85/85 with the 7 device-labelled tests skipped (CI runs 78),
and a 60-frame validation-layer sample reported no VUID or ERROR lines.

This document is the implementation plan and current design record for turning `deren.vulkan.core` from
one class that implements every RHI interface into a small composition root plus four named facets. It was
written before the first code move so the phases, the ABI constraints, and the acceptance checks could be
reviewed independently of the implementation.

## 1. Problem

Before Phase 1, `source/backends/vulkan/core/declarations/core.declarations.cppm:327` declared:

```cpp
export struct core : deren::promise::rhi::api_core,
                     deren::promise::rhi::resource_factory,
                     deren::promise::rhi::command_context,
                     deren::promise::rhi::frame_context,
                     deren::utility::enable_stack_destruct {
```

The three RHI interfaces are therefore inherited directly by the device root. Their implementations are
interleaved with instance/device/queue setup, swapchain state, frame state, resource factories, command
buffer allocation, cleanup code, and the global `api_core` methods. The flat member list is a symptom:
the class declares every kind of Vulkan object and every fact about every RHI partition at the same level.

The RHI already exposes the split the backend needs. `api_core` (`source/promise/rhi/rhi.api_core.cppm:197`)
exposes three subviews:

```cpp
context_view<resource_factory> resources;
context_view<command_context> commands;
context_view<frame_context> frames;
```

and `bind_views(...)` at `:220`. Before Phase 1, `core` bound all three views to itself
(`source/backends/vulkan/core/core.constructor.cppm:153`:

```cpp
bind_views(*this, *this, *this);
```

The goal is to make the objects behind those views named, single-responsibility facets instead of the
root object wearing four interfaces.

## 2. Terms and target shape

### 2.1 Facet, not domain or impl

The word is **facet**. `domain` was the working name; `facet` says what these objects are: a named face of
one device root, not an independent device or a second lifetime. No `_impl` suffix is used.

Target declaration shape:

```cpp
export struct core : deren::promise::rhi::api_core,
                     deren::utility::enable_stack_destruct {
    struct device_facet;
    struct resource_facet;
    struct command_facet;
    struct frame_facet;

    // ... global api_core state and methods ...

    resource_facet resource{*this};
    command_facet command{*this};
    frame_facet frame{*this};
    device_facet device{*this};
};
```

The definitions sit at the end of `core`, after the nested view/state types whose members they use;
only `device_facet` is declared early enough for `main_context()` code, and its definition is also at the
end. The member order is construction order: resource, command, frame, device. Each facet is a direct
member, so its address is stable for the lifetime of the core.

### 2.2 Responsibilities

| Facet | RHI interface | State after the migration |
| --- | --- | --- |
| `device_facet` | none (internal) | instance, physical/logical device, queues, surface, properties, extension entry points, VMA, `main_context_view` |
| `resource_facet` | `resource_factory` | buffers, images, acceleration structures, micromaps, samplers, shaders, pipelines, resource/texture groups, memory statistics |
| `command_facet` | `command_context` | command buffer allocation, mergeable command buffers, command submission |
| `frame_facet` | `frame_context` | swapchain, frame acquire/present, frame command buffer, frame image/readback, walker, profiler, per-frame synchronization |

`color_format` and `frames_in_flight` are frame facts. `frame_facet` is their authority.
`device_facet` only snapshots them into the opaque `main_context_view` when that view is requested.

The global `api_core` surface stays on `core`: `abilities()`, `query_extension()`, `wait_idle()`,
`api_version()`, `main_context()`, timing methods, root `release()`, initialization orchestration and
`do_cleanup()`.

### 2.3 Back pointer

Every facet stores a non-null `core& full_core`. The constructor injects it:

```cpp
explicit resource_facet(core& parent) noexcept : full_core(parent) {}
```

`full_core` is the facet's non-null back pointer. It does not extend any lifetime: neither the facets
nor the core destroy each other through it, and neither stores the other behind it. RHI views spell their
origin `parent` the same way; the facet pointer keeps its name because it cannot be null and no branch may
test it.

### 2.4 `main_context_view`

The RHI contract and the concrete Vulkan layout share the spelling
`main_context_view`:

* `source/promise/rhi/contract/rhi.interop.cppm:31` declares the opaque
  `deren::promise::rhi::main_context_view : object`.
* `api_core::main_context()` (`source/promise/rhi/rhi.api_core.cppm:208`) and
  `gui::create_info::main_context` (`source/promise/gui/gui.cppm:50`) use that
  RHI type.
* The concrete shared layout is `deren::vulkan::main_context_view` in
  `source/backends/vulkan/main_context/main_context.cppm`.

This is a source-level type-name change only. The RHI numeric ABI, the
`object` base, the concrete field order, the `final` qualifier, and the tag
string (`deren_main_context_vulkan`) stay unchanged. The module name
(`deren.vulkan.main_context`) and file name (`main_context.cppm`) stay
unchanged too; `main_context()` remains the method name.

`device_facet` declares one `main_context_view view`. `core::main_context()` delegates to the device facet,
which refreshes the handles and returns `&view`. The GUI plugin and tests continue to receive a plain
pointer with the same layout and tag; only the concrete type spelling changes.

## 3. Migration phases

The phases keep the build green at every step. Phase 1 changes no data lifetime and no Vulkan call order.

### Phase 1: forwarding facets plus `main_context_view`

1. Rename the RHI contract type and the concrete `main_context` type to `main_context_view`; update
   `api_core::main_context()`, `gui::create_info`, all `do_concrete` / `static_cast` call sites, and the
   GUI plugin.
2. Add the four nested facet declarations to `core`. In Phase 1 they are thin forwarders to the existing
   `core` methods.
3. Make `core` inherit only `api_core` and `enable_stack_destruct`; remove the three interface bases.
4. Convert the existing direct overrides of those three interfaces into ordinary `core` methods by
   removing `override` (the bodies and signatures do not change).
5. Bind the RHI views to the facets:
   `bind_views(resource, command, frame);`
6. Route `core::main_context()` through `device_facet`.
7. Build and run the targeted tests.

Phase 1 does **not** move members. The facets are adapters over the existing `core` state, so the change
can be reverted without data migration and every behavior stays byte-for-byte on the same call path.

### Phase 2: frame state

Swapchain handles, swapchain images/views, depth resources, frame command buffers, frame slots,
fences/timelines, acquire/present state, frame walker/profiler views, `color_format`, and
`frames_in_flight` live in `frame_facet`. Its frame virtual entry points are implemented in
`facet/core.frame_facet.cpp`; `core` keeps delegating entry points for its internal callers. `do_cleanup()`
still shuts the state down in reverse order from `core`.

### Phase 3: resource state

Buffers/images/acceleration structures/micromaps/samplers/shaders/pipelines, resource and texture
groups, descriptor heaps/spaces, provenance sets, tier-2 resource views, and memory statistics state live
in `resource_facet`. Pipeline creation is a resource concern; there is no separate pipeline facet.

### Phase 4: command state

Command buffer allocation, mergeable command buffers, command submission, the submission registry,
completion/timeline state, and command/mergeable-buffer provenance live in `command_facet`.

### Phase 5: device state

Instance, physical/logical device, queues, surface/window, properties, extension entry points, VMA, and
the tier-2 `device_capabilities` object live in `device_facet`. `core` keeps construction/teardown
orchestration only. The facet definitions are ordered so `frame_device_capabilities` is complete before
the device member is declared.

## 4. Lifetime and initialization rules

* Facet constructors take `core&` and perform **no** Vulkan calls.
* `core` initializes in dependency order through its existing init steps: device, resource, command,
  frame. The facet members are constructed in declaration order (resource, command, frame, device);
  their constructors only store `core&` and make no Vulkan calls, so this order does not depend on
  device state.
* `core::do_cleanup()` remains the only cleanup entry point and shuts down in reverse order:
  frame, command, resource, device.
* Facet destructors contain only assertions, not Vulkan calls.
* Facets are direct members of `core`, not heap allocations. This keeps the existing pointer stability
  for views.
* `core` is still allocated and released exactly as today; no RHI ABI change is involved.

## 5. Facet entry points and the implementation boundary

The facets are the stable RHI face a host binds to and they implement the recording/creation logic in
their implementation units. `core` keeps thin delegating entry points for callers that still reach
the root directly, so the RHI contract and those internal call paths are unchanged. The split is
deliberate: the facets contain their data and entry points, while `core` remains the one object that
orchestrates device lifetime and cleanup.

`resource_facet : rhi::resource_factory`

* `memory_statistics()`
* `create_lazy_buffer(...)`, `create_lazy_image(...)`
* `create_buffer(...)`, `create_image(...)`
* `create_acceleration_structure(...)`, `create_micromap(...)`
* `create_sampler(...)`, `create_shader(...)`, `create_pipeline(...)`
* `create_resource_group(...)`, `assign_texture_group(...)`

`command_facet : rhi::command_context`

* `create_command_buffer(...)`
* `make_command_buffer(...)`
* `make_mergeable_command_buffer(...)`
* `submit(command_buffer&)`
* `submit(std::span<mergeable_command_buffer_ref const>)`

`frame_facet : rhi::frame_context`

* `begin_commands()`
* `frame_image()`, `frame_readback_buffer()`
* `frame_begin()`, `present()`
* `walk_frames()`, `profiler()`, `frame_swapchain()`
* `submit()`

The defaulted virtuals in the RHI interfaces stay defaulted on the facet and are not re-implemented
unless the current `core` already overrides them. `core` keeps its `api_core` overrides: `release()`,
`get_tag()` (from `object`), `abilities()`, `query_extension()`, `wait_idle()`, `api_version()`,
`main_context()`, `get_time_profiling()`, `record_stage_time()`, and `reset_time_profiling_window()`.

## 6. Files

* `source/backends/vulkan/core/declarations/core.declarations.cppm` - `core` composition root, the four facet
  definitions and the state each declares, and member order.
* `source/backends/vulkan/core/facet/core.resource_facet.cpp` - resource `resource_factory` entry points and
  their creation logic.
* `source/backends/vulkan/core/facet/core.command_facet.cpp` - command `command_context` entry points and
  their submission logic.
* `source/backends/vulkan/core/facet/core.frame_facet.cpp` - frame `frame_context` entry points and their
  acquire/present/recording logic.
* `source/backends/vulkan/core/core.constructor.cppm` - facade construction, view bindings, state members.
* `source/backends/vulkan/core/core.api_core.cpp` - global `api_core` methods and the thin delegating
  entry points for the facets.
* `source/backends/vulkan/core/core.cpp`, `core.entry.cpp`, `frame/core.frame_resources.cpp`,
  `frame/core.frame_checkpoint.cpp`, `resource/core.descriptor_space.cpp`,
  `resource/core.lazy_resources.cpp`, `resource/core.resource_groups.cpp`,
  `resource/core.texture_groups.cpp`, `render/core.render_operations.cpp`,
  `command/core.scoped_synchronization.cpp`, `command/core.submission_lifetimes.cpp`,
  `support/radix_sort.cpp` - qualified `device.`/`resource.`/`command.`/`frame.` member access.
* `source/backends/vulkan/core/support/core_support.cppm` - the standalone module that exports the
  `lazy_loading` and `push_range_validation` templates to the backend and to their unit tests without
  making the tests import `deren.vulkan.core`.
* `source/backends/vulkan/init_utils/init_utils.cpp` - VMA calls through the device facet.
* `source/backends/vulkan/main_context/main_context.cppm` - concrete `main_context_view` type.
* `source/promise/rhi/contract/rhi.interop.cppm` - opaque contract `main_context_view` type.
* `source/promise/rhi/rhi.api_core.cppm` and `source/promise/gui/gui.cppm` - updated return/field type.
* `source/backends/vulkan/gui/gui_dll.cpp` - updated cast target.
* `source/tests/bench_image_upload.cpp`, `spike_backend_boundary.cpp`, `test_buffer_transfer.cpp`,
  `test_resource_group.cpp`, `test_submission_completion.cpp`, `test_texture_group.cpp` - updated cast
  targets.
* The CMake module file set for `deren.vulkan.core` - the three facet implementation units under
  `source/backends/vulkan/core/facet/`.
* `docs/core_facets.md` and `Doxyfile` - this design record and its documentation input.

## 7. Acceptance

Phases 1-5 are complete when all of the following hold on a full Release build:

* `deren.vulkan.core` compiles with `core` deriving only from `api_core` and
  `enable_stack_destruct`.
* `api_core::resources`, `commands`, and `frames` point at the three facets, not at `core`.
* `api_core::main_context()` returns a valid `main_context_view` with the same tag and field values.
* Each facet's Vulkan state is declared inside that facet and its RHI entry points are implemented in the
  facet's implementation unit; `core` keeps only shared `api_core` state, orchestration data, and thin
  delegating entry points.
* The GUI plugin still accepts the real backend and still rejects a no-device core.
* The full CTest suite passes.
* Targeted tests pass: `test_dynamic_link`, `test_gui_plugin`, `test_runtime_dyn`,
  `spike_backend_boundary`, `test_buffer_transfer`, `test_resource_group`,
  `test_submission_completion`, `test_texture_group`.
* A validation-layer run of the normal sample reports no new VUID or ERROR lines.
* Pointer stability is preserved: a `main_context_view*` obtained before a frame remains valid after frame
  submission/presentation, and the facet addresses do not move.

## 8. Non-goals and constraints

* No RHI ABI bump. The contract type spelling changes to `main_context_view`; the opaque `object`
  base, the concrete layout, and the tag are unchanged.
* Backend and GUI plugin are rebuilt together; the plugin is not ABI-compatible across the rename.
* No new Vulkan calls are added in facet constructors or destructors.
* No separate pipeline, swapchain, or synchronization facets. Pipeline creation belongs to resources;
  swapchain and frame synchronization belong to frames; device-level synchronization belongs to device.
* `core` remains the only object responsible for the device lifetime and the only cleanup entry point.
* Repository files stay English-only.

## 9. Risks and mitigations

* **Call sites typed as an interface now see only the facet.** They must go through
  `api_core::resources/commands/frames`. Any direct `core`-as-interface assignment is a compile error,
  which is the intended check.
* **Removing `override` can silently drop virtual dispatch.** These methods are called through the RHI
  interface from the facet, so the facet call is the virtual entry point; `core` methods become ordinary
  implementations. No caller should depend on `core` itself being virtual for these methods.
* **`main_context_view` rename touches tests and the plugin.** A repo-wide search for
  `main_context`, `do_concrete<...main_context>`, and the old RHI spelling is part of the change, not an afterthought.
* **Duplicate frame/device facts.** `color_format` and `frames_in_flight` are authoritative on
  `frame_facet`; the device facet only snapshots them. Do not add a second writable copy.
* **Module and CMake drift.** New implementation units must be in the same module file set as the
  existing core units, or the build will silently omit them.
* **Early access from facet constructors.** The facets are constructed before the core body runs, so
  they must not read Vulkan state in their constructors. They only store `core&`.

## 10. Implementation checklist

- [x] Create branch `refactor/core-facets` from `origin/master`.
- [x] Rename `deren::promise::rhi::main_context` and `deren::vulkan::main_context` to
  `main_context_view` and update call sites.
- [x] Rename the RHI contract type used by `api_core::main_context()` and `gui::create_info`.
- [x] Add nested facet declarations and member instances.
- [x] Remove the three RHI base classes from `core`.
- [x] Add the three forwarding implementation units and CMake entries.
- [x] Bind views to facets in the constructor and route `core::main_context()` through `device_facet`.
- [x] Phase 2: move the frame state and the frame views into `frame_facet`.
- [x] Phase 3: move the resource state, descriptor heaps/spaces, samplers, texture groups, provenance
  sets and tier-2 resource views into `resource_facet`.
- [x] Phase 4: move the submission/completion state and command-buffer provenance into
  `command_facet`.
- [x] Phase 5: move the instance/device/queues/surface/window/VMA/entry points and the tier-2
  `device_capabilities` object into `device_facet`.
- [x] Move the resource, command, and frame RHI entry-point bodies into their facet implementation units;
  `core` keeps thin delegating entry points for internal callers.
- [x] Rebuild `deren_vulkan` and the full Release build.
- [x] Run the targeted tests, then the full CTest suite (85/85, 7 device-labelled tests skipped; CI runs 78).
- [x] Run a 60-frame validation-layer sample with no VUID or ERROR lines.
- [x] Commit and push through `scripts/commit.ps1` / `scripts/push.ps1`; opened PR #55
  (https://github.com/YzK0741/deren/pull/55).
