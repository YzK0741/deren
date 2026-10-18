# Resource clusters

`deren.engine.resource_cluster` is an engine-only ownership module using the
public RHI. It adds no backend implementation or RHI ABI entry. Existing registry,
generation snapshots and pass chains remain in use alongside the render graph.

An empty cluster is tied to a core and owns its debug name. It creates no GPU
resources and does not wait for the device:

```cpp
import deren.engine.resource_cluster;
auto resources = deren::engine::resource_cluster::create(core_owner, "MainView");
// Or, from a valid runtime:
auto view_resources = runtime.create_resource_cluster("MainView");
```

The factory returns nullptr for a null core. CPU storage, including shared control
blocks, uses the utility allocator and follows its fail-fast OOM policy.

Clusters currently create buffers, images, samplers and image views through the
existing RHI factories. Each returns `expected<resource_lease<T>, rhi::error>`;
a null RHI factory result becomes operation_failed. Failure adds no member and
does not discard earlier successful creations. Build a candidate cluster privately
and publish it only after checking every required result. Releasing a failed
candidate drops its resources; this is caller-controlled batch rollback.

```cpp
auto image = resources->create_image(image_desc);
if (!image) return image.error();
auto view = resources->create_image_view(*image, view_desc);
if (!view) return view.error();
```

`resource_lease<T>` is an owning reference. Copies share a single RHI factory
reference; `get()` and `operator->` borrow that object. Do not call release on
the borrowed pointer or wrap it in another object_manager. An escaped lease can
outlive the cluster; the final owning reference releases the RHI object exactly
once, before releasing its core dependency. Image views retain the parent image.

`other_cluster->share(lease)` retains the same resource holder, checks core identity
and is idempotent for that holder. An empty or foreign-device lease is rejected.
Holders retain resources and the core, not other clusters, so cross-cluster sharing
does not create cluster ownership cycles. The resource count is the number of
distinct retained holders, not physical allocations or descriptor entries.

Clusters are append-only and use a single writer. No concurrent structural access
is supported. There is no clear, destructive member replacement, seal or public
snapshot API in this first version. Construct a replacement cluster for resize or
batch replacement. Adding members does not invalidate existing leases.

## GPU lifetime

Owning references alone do not establish when the GPU has finished. For the
runtime's ordinary frame submission, explicitly retain every cluster whose borrowed
resources are recorded:

```cpp
// After pace_and_acquire() returned proceed, before submit_and_present():
auto status = runtime.retain_resource_cluster_for_frame(resources);
if (status != deren::promise::rhi::error::ok) return status;
```

The runtime checks the device and deduplicates cluster references. It retains the
cluster in the active frame slot until that slot has completed and is successfully
acquired again. Other slots do not release it. Failed recordings/submissions retain
conservatively; present failure cannot release submitted resources. Runtime shutdown
uses its existing device-idle wait before its retained clusters are destroyed.

Retention outside that frame window returns not_ready. Null or foreign clusters
inside the window return invalid_argument. Ordinary cluster/lease destruction does
not call wait_idle. Custom/direct RHI submissions and asynchronous uploads need
their own retention until completion; this API does not infer those uses.

The post graph adapts existing resource generations into leases and retains
a cluster for the acquired output view. This complements the existing resource
registry; clusters do not infer synchronization or pool GPU allocations.

Existing generation/shared owners can be adapted with
`resource_lease<T>::from_shared(core, shared_owner)`. An aliasing shared_ptr may
pin a generation while naming one of its images. The lease retains that supplied
owner and the core; it does not adopt another raw release reference. Pass the
actual owning core and keep the supplied owner's lifetime correct. Executable
render-graph preparation independently validates backend provenance and shape.
