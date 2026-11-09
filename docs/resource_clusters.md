# Resource clusters

`deren.engine.resource_cluster` is an engine-only module that extends resource
lifetime across frames using the public RHI. It adds no backend implementation or RHI ABI entry. Existing registry,
generation snapshots and pass chains remain in use alongside the render graph.

An empty cluster is tied to a core and stores its debug name. It creates no GPU
resources and does not wait for the device:

```cpp
import deren.engine.resource_cluster;
// core_lifetime is the runtime's rhi::shared_object<rhi::api_core>, and the core stays valid while that copy exists.
auto resources = deren::engine::resource_cluster::create(core_lifetime, "MainView");
// Or, from a valid runtime:
auto view_resources = runtime.create_resource_cluster("MainView");
```

The factory takes the runtime's `rhi::shared_object<rhi::api_core>` (the core stays valid while that copy
exists) and returns nullptr for a null core. CPU storage, including the shared control blocks behind each `resource_access`, uses the utility allocator
and follows its fail-fast OOM policy.

Clusters currently create buffers, images, samplers and image views through the
existing RHI factories. Each returns `expected<resource_handle<T>, rhi::error>`;
a null RHI factory result becomes operation_failed. Failure adds no member and
does not discard earlier successful creations. Build a candidate cluster privately
and publish it only after checking every required result. Destroying a failed
candidate releases its resources; this is caller-controlled batch rollback.

```cpp
auto image = resources->create_image(image_desc);
if (!image) return image.error();
auto view = resources->create_image_view(*image, view_desc);
if (!view) return view.error();
```

`resource_handle<T>` is an engine handle that names one RHI object and is responsible for destroying it. The
object is the contract's factory product, `rhi::unique_object<T>` (destroyed exactly once, by the last
`resource_access` that goes away; one adapted with `from_shared` names a `rhi::shared_object<T>` instead).
Copies share that one object, and `get()` / `operator->` only name the RHI object without extending its lifetime. Do not call
`release()` on the pointer that `get()` returns, and do not wrap it in another `rhi::unique_object` - the handle is not a factory - or
in a second shared container. A `resource_access` can outlive the cluster; the last copy destroys the RHI object exactly
once, then releases the core dependency. Image views stay valid only while the parent image does.

`other_cluster->share(access)` registers the same resource in the target cluster, checks core identity
and is idempotent for that resource. An empty or foreign-device argument is rejected.
A shared resource's `resource_access` references the RHI object and the core, but not the cluster it came from, so
cross-cluster sharing cannot make two clusters depend on each other. The resource count is the number of
distinct resources the cluster registers, not physical allocations or descriptor entries.

Clusters are append-only and use a single writer. No concurrent structural access
is supported. There is no clear, destructive member replacement, seal or public
snapshot API in this first version. Construct a replacement cluster for resize or
batch replacement. Adding members does not invalidate a `resource_access` already handed out.

## GPU lifetime

A handle that names a resource says nothing about when the GPU has finished. For the
runtime's ordinary frame submission, explicitly hand every cluster whose recorded
resources are in use to the frame path:

```cpp
// After pace_and_acquire() returned proceed, before submit_and_present():
auto status = runtime.add_resource_cluster_for_frame(resources);
if (status != deren::promise::rhi::error::ok) return status;
```

The runtime checks the device and deduplicates cluster references. The active frame slot pins the
cluster until that slot has completed and is successfully acquired again. Other slots do not release it. Failed recordings/submissions keep their resources registered
conservatively; present failure cannot drop resources already submitted. Runtime shutdown
uses its existing device-idle wait before destroying the clusters the frame slots still name.

A call outside that frame window returns not_ready. Null or foreign clusters
inside the window return invalid_argument. Destroying an ordinary cluster or `resource_access` does
not call wait_idle. Custom/direct RHI submissions and asynchronous uploads must not destroy
their resources before completion; this API does not infer those uses.

The post graph adapts existing resource generations into `resource_access` handles and registers
a cluster for the acquired output view. This complements the existing resource
registry; clusters do not infer synchronization or pool GPU allocations.

An existing generation or shared handle can be adapted with
`resource_handle<T>::from_shared(core, resource)`, whose arguments are the
`rhi::shared_object<rhi::api_core>` (the core lives as long as that copy) and the RHI handle's shared form, `rhi::shared_object<T>` - not
a raw pointer and not a `std::shared_ptr` over the RHI type. An aliasing `rhi::shared_object<T>` may
name a separate image while also keeping an engine object (a generation, a subchain) valid. The `resource_access` references that
supplied handle and the core; it does not adopt another raw release path. Pass the actual
core and keep the supplied handle's lifetime correct. Executable render-graph preparation independently
validates backend provenance and shape.
