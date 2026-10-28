# amnis phase closeout

**Status: phase closed, 2026-10-28.** Texture residency, progressive preparation, source ownership
and asynchronous feedback readback form the accepted opt-in baseline. Further feature expansion
is deferred; maintenance and reproducible regressions remain in scope. This checkpoint describes
the bounded results below, rather than a general production qualification on every GPU or scene.

## Accepted scope

| Capability | Stage result |
|---|---|
| GPU visibility demand | Visible-material feedback selects requested textures. Original whole-image same-frame streaming remains available as a comparison mode. |
| Low-resolution fallback residency | Two-tier streaming keeps low-resolution coverage while full textures upload asynchronously. |
| Progressive mip residency | Screen-footprint demand selects intermediate mips under existing residency/upload budgets and job limits. |
| Safe publication/restoration | Completed uploads publish through frame texture banks; disabling streaming restores full-resolution sources asynchronously. |
| CPU source ownership | Immutable whole-vector sources are shared; mutable, borrowed and partial inputs retain copy semantics. Import-only pixels/references are released. |
| Consuming material import | Referenced RGBA pixels move into mip0; unused decoded pixels are released. Rectangular chains stop at the declared mip count. |
| Asynchronous feedback readback | GPU storage is copied to independent coherent CPU-readable storage per existing frame slot, without another frame of feedback delay or a new same-frame wait. |

The RHI resource/vessel counters, buffer transfers, synchronization and operation-stream CPU fixes
are shared supporting infrastructure, not exclusive amnis policy. Geometry DAG cuts and HZB remain
[subtilis](../subtilis.md) responsibilities. Progressive mip images are physical mip suffixes;
this stage does not implement a virtual-texture page table or sparse physical page pool.

## Configuration boundary

- Full residency, original visibility streaming, two-tier streaming and progressive mip streaming
  remain separately named comparison modes.
- Two-tier streaming requires configured visibility streaming and `DEREN_PROGRESSIVE_TEXTURES=1`.
  Progressive mips additionally require `DEREN_PROGRESSIVE_MIPS=1`.
- The progressive paths remain opt-in. Stage closure changes no defaults, public ABI or shader policy.
- CPU occlusion and dual-camera preparation are experiments, not requirements of the accepted path.
- Software raster remains optional/default-off under subtilis; it is not an amnis acceptance condition.

## Evidence checkpoint

The latest controlled comparison is textured-DAG Bistro at 1280 x 720, DAG + HZB + progressive mips,
with software raster and optional post effects off. Two reversed-order timing rounds per view use
360 requested frames each, discarding the first 60 presentation intervals (602 per cell).

| View | Direct mapped feedback, mean frame | Copy readback, mean frame | Reduction |
|---|---:|---:|---:|
| Near | 11.919 ms | 9.959 ms | 16.4% |
| Far | 12.050 ms | 10.112 ms | 16.1% |
| Moving | 12.862 ms | 10.645 ms | 17.2% |

CPU post falls by 1.89-2.12 ms. Moving GPU time rises by 0.465 ms, while the full frame still improves
by 2.217 ms. This measured tradeoff is accepted; no GPU-equivalence claim is made.

The latest separate moving memory capture observes about 1.154 GiB of device-local resource
allocation, 2.570 GiB of current project CPU allocation and 2.971 GiB of lifetime project CPU peak.
Feedback copies add 64 KiB of live device-local allocation and approximately 196 KiB of total live
resource allocation; VMA host block reservation grows separately by 32 MiB in that run.

Historical same-asset source-ownership captures reduced retained project CPU allocations by about
5.069 GiB (raw Bistro progressive case 7.568 to 2.499 GiB). Import revisions and assets differ from
the latest textured-DAG capture; these figures must not be combined into one cumulative benchmark.
Allocator counters, reserved block capacity, OS working/private bytes and WDDM residency are
different measures. Frozen manifests and source patches, rather than a Git HEAD alone, identify
the tested uncommitted implementation.

Validation records include:

- Full restored-source CTest: 73/73; final targeted regression: 5/5.
- GPU buffer-transfer regression: 101 checks, zero failures, validation enabled.
- Loader regression: 286 checks, including exact rectangular mip payloads and both ownership paths.
- Final 21 captures: seven validation, two memory, twelve timing, all activity/exit/screenshot gates pass.
- Restoration: wanted=full=456, jobs=failed=0; final image matches the current eager reference exactly.
- Independent source and evidence review: no remaining confirmed material findings.

## Known limits and deferred work

Near/far final images match exactly. Moving captures produce the same two final images in both
arms, differing by 14,442 pixels (RGB MAE 0.008528/255); cross-round comparisons match exactly.
The underlying renderer/capture nondeterminism remains unresolved. Final screenshots and short
validation runs are not exhaustive full-trajectory or long-duration streaming qualification.

Only one machine, asset and resolution are measured. Hardware memory preferences, transfer
scheduling, rapid camera motion and asynchronous upload readiness can change the outcome. The
retained full mip sources still occupy CPU memory; toon-sidecar input conservatively retains raw
pixels. OOM/retry behavior was not exhaustively fault-injected in the full renderer.

Deferred directions are true virtual-texture paging, disk/compressed CPU source streaming, more
aggressive request compression, CPU HZB occlusion and dual-camera preparation. Static-camera
special cases are not a continuing optimization target. subtilis GPU cut selection and geometry
paging remain separate deferred work, not unfinished amnis milestones.

Reopen amnis for a reproducible correctness defect, a representative scene exceeding an agreed
memory/latency budget, or new measured evidence that a deferred direction is worthwhile. Additional
features are not appended merely because the subsystem now exists.

## Next stage

Use the current implementation as a fixed baseline. The recommended next investigation is common
CPU frame preparation/command recording: near GPU time is about 4.345 ms while whole-frame time is
about 9.959 ms. These overlapping measurements cannot simply be subtracted to identify a hotspot.
Any new work should profile the common path and task waits before changing graph preparation,
operation validation/replay or geometry cut scheduling. Treat that as a separate renderer task,
not an extension of this closed texture-residency stage.

## Report index

- [amnis scope and configuration](../amnis.md)
- [Source ownership and import memory](../benchmarks/2026-10-27-texture-source-memory.md)
- [Shared operation-stream CPU cost](../benchmarks/2026-10-27-operation-stream-cpu.md)
- [Earlier eager/progressive isolation](../benchmarks/2026-10-27-amnis-isolation.md)
- [Rejected direct host-cached GPU writes](../benchmarks/2026-10-28-feedback-readback-probe.md)
- [Accepted feedback copies, CSV and compressed evidence](../benchmarks/2026-10-28-feedback-copy-readback.md)
