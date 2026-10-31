# Resource groups

`resource_group` is an immutable, shared composition of shader resources. Descriptor heaps, sets,
pools and physical allocations stay inside the backend. An engine `resource_cluster` remains a
separate ownership/lifetime mechanism; a resource group describes shader bindings.

Members can be an existing `texture_group`, sampled/storage image, uniform/storage buffer, sampler,
or top-level acceleration structure. A texture group retains its indexed texture-array behavior as
one member of the general group. Version 1 has no recursive resource groups or multiple loaded group
slots. Existing texture-group entry points remain available for incremental migration.

## Creation and loading

```cpp
namespace rhi = deren::promise::rhi;

// Transfer the factory owner into shared ownership; never manufacture a second owner from its raw pointer.
rhi::shared_object<rhi::buffer> data(core.resources->create_buffer({
    .size = 256,
    .usage = rhi::buffer_usage::storage_coherent,
    .flags = rhi::to_bits(rhi::buffer_flag::device_address),
}));

std::array entries{
    rhi::resource_group_entry{.binding = 0, .kind = rhi::resource_kind::storage_buffer,
                             .resource = data, .offset = 0, .size = 256},
    rhi::resource_group_entry{.binding = 1, .kind = rhi::resource_kind::texture_group,
                             .resource = textures}, // existing texture_group_ref
};
auto group = core.resources->create_resource_group({.entries = entries});
if (!group) return group.error();

std::array layout{
    rhi::resource_group_binding{0, rhi::resource_kind::storage_buffer, 0, 4},
    rhi::resource_group_binding{1, rhi::resource_kind::texture_group, 4, 4},
};
pipeline_desc.resource_group = layout; // before creating the pipeline
// With the resulting pipeline, while recording:
if (auto result = commands.bind_pipeline(pipeline); result != rhi::error::ok) return result;
if (auto result = commands.load_resource_group(*group); result != rhi::error::ok) return result;
commands.dispatch(1, 1, 1);
```

`binding` is a logical member number; entry order is irrelevant. Group and pipeline must have exactly
the same binding numbers and resource kinds. The layout's last two fields name a shader parameter's
byte offset and consuming stages (vertex=1, fragment=2, compute=4, mesh=8), never descriptor memory.
The current shader interface uses one uint32 handle per member, filled by the backend. Application
push data cannot overwrite these fields; place ordinary parameters elsewhere. A group has at most
32 members and must fit the device's parameter budget. Empty groups, duplicate bindings/parameter
locations, null resources and invalid device/type/range inputs are rejected.

Creation returns `expected<resource_group_ref,error>`; an unsupported backend returns `unsupported`.
Descriptor and layout spans are copied before the creation call returns. Image `view` selects its
sampled/storage role and mip/layer range. Buffer `offset`/`size` are bytes; size=0 means the remaining
range. Buffers need the appropriate usage and device_address capability. Vulkan's uniform-to-storage
translation is internal and requires no additional frontend flags.

## Ownership and execution

Members must carry lifetime ownership. `shared_object::borrow()` does not retain a resource and is
rejected during creation/loading. Lifetime aliases are accepted, provided their owner actually keeps
the target alive. Never wrap an already-owned raw pointer with an independent control block. The
originating core/backend must outlive every group and member reference.

Composition fixes member references, kinds, ranges and views. It does not freeze resource contents or
nested texture-group writes through the existing `descriptor_space::write` interface; those retain
their own mutation/freeze and synchronization rules. Loading inserts no barriers, builds no TLAS,
and does not automatically retain all external scene dependencies of an acceleration structure.

`load_resource_group` requires recording and a compatible bound pipeline. It validates the complete
group before recording bindings. Recorded commands retain groups; pending submissions preserve them
through GPU use. Primary, secondary composition, batch and frame paths share this rule. Rerecording
or final submission retirement releases the corresponding owners; private descriptor slots return
only after the group's last owner dies. Loading a later group cannot release resources used by earlier
recorded draws.

Under ABI 53, `load_resource_group` (like push parameters) queues logical recorder state; a
compatible group loaded before a pipeline switch is retained across that switch and the backend
validates/republishes it at the next consuming operation. Re-binding the same pipeline preserves a
loaded compatible group. Callers must still load the group before the first draw/dispatch/trace
that consumes it; validation and deferred errors surface at `apply_operations()`/`end_recording()`.
Complete draw-state tracking and changes to void draw/dispatch error reporting remain outside this
batch. Shader parameters use `command_recorder::push_constants` (ABI 52; queued under ABI 53).

## Backend and version

Vulkan uses private reusable resource/sampler slots and fills shader handle fields from the pipeline
layout. Current Slang heap ConstantBuffer access lowers to a StorageBuffer pointer, so native uniform
allocations/descriptors also support that representation while the frontend keeps uniform semantics.
General layouts support compute, vertex/fragment, mesh/fragment and vertex-only depth pipelines.
Ray-tracing pipeline layouts currently return unsupported; TLAS members can serve compute ray queries.

The old texture-group range overlapped the scene grid. Its private table/pool now begins at
32768/32769, reserved during core construction before dynamic allocations, with matching shader
constants. The resource heap working budget increased from 1280 KiB to 4 MiB for disjoint fixed ranges
and reusable group descriptors. Rebuild shaders and backend together.

Resource groups originated in RHI ABI **51**. Unified recorder push_constants and deferred recording require ABI
**53 or newer**; application, backend and plugins need matching rebuilt binaries. The validation evidence and
the ABI boundary record were maintainer material and are not distributed with the repository; the boundaries
stated here are the contract.
