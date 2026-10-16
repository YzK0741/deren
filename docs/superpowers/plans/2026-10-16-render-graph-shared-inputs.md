# Shared read-only scene inputs implementation plan

Goal: extend the approved main graph with accurate task/mesh stage vocabulary and generation-retained shared read-only buffer inputs. Execute inline in the existing authorized workspace; no commit/push.

Design: add task_shader=256 and mesh_shader=512 through graph, scope contract, bridge, diagnostics and native mapping. Backend advertises each only when enabled on a graphics queue. Unknown bits remain errors. Image layout-transition coverage includes new stages only when this resource uses them, avoiding unsupported stage masks in ordinary post graphs.

Geometry gets an optional named shared-input bundle: camera (uniform mesh+fragment), materials (storage fragment), instances, previous transforms, current/previous skin matrices and morph data (storage mesh). Lighting gets an optional camera/light uniform fragment bundle. Existing append callers may omit bundles for attachment-only CPU graphs; runtime main path must supply complete bundles. Import camera once at combined mesh+fragment read state and reuse its canonical ref. Other host-published inputs enter with defined contents and their consumer read scopes; the frame-slot wait/write/flush/submission contract remains external, not a fake GPU upload pass. Buffer leases alias the owning generation and validate device, selected instance and actual borrowed handle.

Out of scope: mesh vertex/index addresses, material textures/IBL, meshlet/indirect/statistics, cluster GPU producer/consumer, shadow and temporal resources. In particular meshlet statistics are atomic writes and must not be mislabeled read-only. New bundles describe only their named inputs, not all accesses of the pass.

- [x] RED: task/mesh scope compilation, lowering and diagnostic names; unknown bits and unsupported device stage rejection.
- [x] Implement stage support including queue/feature gates, preserve prior enum bits and stage coverage behavior.
- [x] RED: geometry/lighting shared typed parameters from defined buffers, undefined input rejection and cross-pass resource reuse.
- [x] Implement optional named input bundles and runtime generation imports for camera/light/material/instance/deformation resources; require complete runtime bundles.
- [x] Build -j12, relevant/full CTest and ASan, GPU equivalence with timings and synchronization validation, independent review and documentation.

Review focus: camera imported consistently across both consumers; per-slot buffers select slot rather than swapchain index; uniform/storage usage matches shader declarations; no barriers assume host writes were GPU writes; unsupported mesh/task scopes rejected before recording; no new stages injected into graphs that never use them.

Acceptance: Release 44/44, focused ASan 17/17, expanded backend boundary zero; five Helmet GPU pairs and one animated-skin pair byte-identical with timings and synchronization validation. Independent review found no confirmed P1/P2 findings. Details and limitations: docs/reviews/2026-10-16-render-graph-shared-inputs.md.
