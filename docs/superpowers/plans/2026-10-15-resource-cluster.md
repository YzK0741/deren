# Engine resource cluster: first implementation

User authorized incremental render-graph work, beginning with an engine resource
cluster. Preserve the existing registry/passchain and all prior workspace edits.
No backend-specific implementation or RHI ABI change. Execute inline using TDD.

## Agreed scope and concrete interface

- New independent module `deren.engine.resource_cluster` under
  `source/engine/resource_cluster/`, linked through deren_engine.
- `resource_cluster::create(shared_ptr<rhi::api_core>, string_view)` creates an
  empty named cluster; null device returns nullptr. Metadata uses vessel storage.
- `runtime::create_resource_cluster(name)` delegates to the same engine factory.
- Cluster creates buffers, images, samplers and image views via existing RHI
  factories; returns `expected<resource_lease<T>, rhi::error>`.
- A lease is an owning engine reference, with get/operator-> for borrowed RHI use.
  Typed holders contain the RHI object_manager and core owner. Image views retain
  their image holder. No raw-pointer adoption in this first batch.
- `share(lease)` retains the same holder in another cluster, rejects foreign
  devices and empty leases, and does not duplicate a holder already present.
- Clusters are append-only, single-writer objects. No clear, member replacement,
  mandatory seal or public snapshot. Resources cannot be freed by adding members.
- `runtime::retain_resource_cluster_for_frame(cluster)` is explicit: allowed only
  after successful pace/acquire and before submit, checks device identity, keeps
  the cluster in the current frame slot until successful completion/acquire of
  that slot or runtime shutdown after wait_idle. Failed frames retain conservatively.
  Direct/custom RHI submissions remain the caller's lifetime responsibility.
- Ordinary cluster/lease destruction releases references; it does not wait_idle.
  Escaped/shared individual leases intentionally extend their resource lifetime.

## Tasks

- [x] Add headless tests first: empty/null/name/device, creation and failure,
  sharing/device rejection, view-before-image destruction and device lifetime.
  Watch the missing module fail compilation before implementing it.
- [x] Implement typed lease/holder and cluster with existing RHI factories; wire
  CMake, test target and CI. Verify resource release exactly once with probe types.
- [x] Add runtime empty factory and frame-slot retention. Test with portable probe
  runtime: closed window rejection, current/foreign core, caller reset, next-slot
  isolation and completed-slot release. Keep old resource snapshots unchanged.
- [x] Run appropriate focused/full tests and boundary checks, review code, document
  usage and limits. Do not migrate production pass resources or commit/push.
