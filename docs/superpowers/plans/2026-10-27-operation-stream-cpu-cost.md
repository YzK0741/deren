# Deferred operation stream CPU cost

User reports default-path regression and supplies preliminary apply_operations timing (~468 calls/frame,
~35us/call). Investigate the common ABI53 path, preserving execution order, validation-before-native,
lazy materialization ordering, error latching, resource lifetimes and interop restoration.

Use systematic debugging, then TDD for changes and inline execution in the current checkout.
Existing documentation and composed benchmark have been committed externally through 8849f822;
preserve that work and use its report as context. No commit/push or default feature-policy change requested.

- [x] Add opt-in staged diagnostics and capture the unchanged behavior; distinguish stream preparation,
  validation, lazy preparation, native replay and state saving.
- [x] Choose the smallest fix from measurements; add failing semantic/reuse tests before changing behavior.
- [x] Preserve ordinary apply behavior and restore-path correctness; avoid repeated payload copies and allocations
  when proven hot. Keep final/intermediate simplify and unoptimized developer control semantics.
- [x] Build, run CPU/full regressions and Vulkan probes; compare identical settings, frames and image output
  before/after in serial runs. Performance captures exclude profiling/resource samplers.
- [x] Record attribution, performance and remaining limitations; update composed benchmark conclusions only
  with new measurements, keeping historical evidence intact.

Results: ../../benchmarks/2026-10-27-operation-stream-cpu.md and accompanying CSV/gzip. Default CPU
post 5.397 -> 2.295ms; composed frame 25.625 -> 14.381ms. Default PBR images match; other paths have
small recorded image differences and an existing before-arm repeatability issue, so no universal
pixel-equivalence claim. Full CTest73/73, device probes and both optimization-control modes pass.
