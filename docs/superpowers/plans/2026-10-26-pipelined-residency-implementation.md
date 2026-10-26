# Pipelined texture residency - implementation plan (Phase 0 closure)

Date: 2026-10-26. Base: `af2a780a` (ABI 58 same-frame residency committed and pushed).
Source design: `docs/superpowers/specs/2026-10-26-pipelined-texture-residency-design.md`.
This plan starts only after the design is approved; it records the three decisions that design left
to the implementer so implementation does not start on unresolved assumptions.

## Scope of the first implementation

Prepared/display pipeline for **eligible static PBR scenes**, default off, keeping the existing eager
path and the ABI 58 same-frame mode as explicit alternatives. The first implementation does NOT add
virtual textures, mip/page streaming, geometry streaming, camera prediction, animation/deformation
snapshots, or an automatic residency manager.

Display always presents the latest **Complete** candidate output. A candidate whose preparation is
not complete stays invisible; the previous complete color is repeated. Repeated display must not
re-advance TAA/HZB history or duplicate scene rasterization.

## Decision D1 - queue policy (single graphics queue first, bounded uploads)

Current facts:

- `core.constructor.cppm` requests only the graphics and present queue families; the device has no
  dedicated transfer queue in the contract or current creation path.
- `submit_commands()` submits caller-owned and frame command buffers to `graphics_queue_handle`.
- `frame_checkpoint` (ABI 58) is synchronous and frame-bound; the pipeline must not use it in the
  main display loop.

Decision:

1. Phase 2-3 use the existing single graphics queue. Present submission happens before a frame's
   upload batch submission; the main thread never waits on an upload fence.
2. Upload work is submitted as a bounded batch (target: at most one batch in flight and a small GPU
   budget per display frame), not as one unbounded job covering the whole demand set.
3. This does **not** promise that `vkQueuePresentKHR` cannot be delayed by queued graphics work. The
   first performance gate measures present intervals with uploads enabled vs disabled; if p99 present
   delay exceeds the agreed baseline, Phase 4 adds a backend-internal optional transfer queue
   (no public RHI ABI change) or reduces the upload budget before any slot increase.
4. No whole-device `wait_idle` in the normal loop. Fence waits are allowed only in the upload worker
   for its own batch and at resize/exit drain.

## Decision D2 - descriptor banks need a new optional RHI extension (ABI 59)

Current facts:

- `descriptor_space` exposes one `fixed_slots()` range and one global physical table
  (`write_image`/`write_buffer`/`write_acceleration_structure` + `bind`). Runtime shader slot indices are
  stable globals, e.g. `render_layout::heap_slots::textures + i`.
- There is no bank selector in `descriptor_bind_info` or `descriptor_space`; republishing a slot while
  candidate A is shading would violate the design's "prepare B must not mutate what A reads".

Decision:

1. A candidate owns an immutable **descriptor bank**: one independent version of the same logical
   shader-slot table. Physical images may be shared; mutable descriptor state may not.
2. Add an **optional** `descriptor_bank` extension in ABI 59:
   - create/allocate a bank with the existing fixed-slot profile;
   - write logical slots into a bank;
   - bind a bank to a command buffer before recording the candidate's resolve/lighting/post.
   - The existing `descriptor_space` behavior and the eager path remain unchanged.
3. Fallback when the extension is absent: run the pipeline with a single bank and serialize
   visibility/shading handoffs; do not silently alias a bank between candidates.
4. ABI 59 is a separate change with its own compatibility tests and standard update. Phase 1-2 must
   not depend on it; Phase 3 is where it lands.

## Decision D3 - history policy

Current facts:

- TAA history is per-swapchain-image (`taa_pass::valid_history`); HZB history and frame-visible state are
  frame/image-bound. Repeated old color must not look like a new frame to them.

Decision:

1. Candidate output carries its request ID, scene generation and the camera/config that produced it.
2. On the first display of a candidate whose `(scene_generation, request_id)` differs from the last
   displayed candidate, reset TAA and HZB history and advance histories only for that candidate's
   shading result.
3. Repeated display of the same Complete color advances neither TAA/HZB history nor frame residency
   demand.
4. Resize/scene reload/mode switch invalidates the candidate and resets history; the last complete
   color may be displayed scaled only if its format is compatible, otherwise use the splash.
5. First candidate has no history; TAA/HZB start fresh (no fabricated previous frame).

This is deliberately conservative: no attempt to replay history through repeated presentation or
merge histories across candidate IDs.

## Decision D4 - snapshot contents and first-increment eligibility

The first snapshot contains:

- request ID + scene generation;
- camera view/projection, viewport, render scale, clip/culling configuration;
- immutable draw-item -> material mapping, object transforms, material/texture version references;
- strong references to geometry, visibility ID/depth, feedback, color target and all resident inputs;
- command barriers/owners for the prepared prefix and suffix and a completion token.

Eligibility: static ordinary PBR opaque geometry with resident geometry, supported MASK/toon/overlay
handling and no animation/deformation/RT/transparent/virtual-texture dependency. Anything else falls
back to eager residency; never silently treat an unsupported scene as static.

## Phases

### Phase 1 - scheduler and snapshot model (no GPU behavior change)

- New engine-internal `candidate_scheduler` + `frame_snapshot` value model: mailbox, 3 slots, states
  Captured -> VisibilityInFlight -> FeedbackReady -> Uploading -> Renderable -> ShadingInFlight ->
  Complete -> Retiring; monotonic display IDs; bounded one upload batch; stale retirement.
- CPU tests for latest-request replacement, monotonic display, no loss of progress under churn,
  failed/stale candidate handling, and history-invalidation decisions.
- No runtime wiring yet; no RHI/ABI changes.

### Phase 2 - offscreen prepared/display split with one candidate

- Render one candidate's visibility/resolve/lighting/post into offscreen color on an independent
  command buffer/submission; display the latest Complete color through the existing present path.
- Keep one bank and serialize bank writes; no concurrent A/B yet.
- Tests: repeated display does not rasterize again or advance history; completed color survives the
  candidate's command buffer; resize/exit drain; baseline eager path unchanged when disabled.

### Phase 3 - ABI 59 descriptor banks and concurrent candidates

- Add the optional descriptor-bank extension and backend implementation; bank creation/write/bind
  tests, ownership/retention, absence fallback.
- Run visibility for B while A shades, with A/B banks immutable; shared physical textures deduplicated.
- Tests: publishing/retiring B cannot change A's reads; no half-published bank; ABI 58 behavior intact.

### Phase 4 - asynchronous upload worker and queue budget

- Candidate-level upload batch over owned sources with independent offscreen command buffers/tokens;
  worker waits only its own fence; main thread never waits.
- Bounded upload batch and per-frame GPU budget; queue policy D1 measured; add the optional transfer
  queue path only if the present-delay gate fails.
- Tests: upload > one display frame still presents; stale upload retirement; submit/wait failure keeps
  last color and reports a bounded error; no whole-device wait.

### Phase 5 - 3-slot pipeline, history, failure and acceptance

- Enable 3 slots/mailbox/stale retirement in the runtime; history policy D3; resize/reload/fallback;
  failure/OOM bounded retry; exit drain.
- Acceptance: correctness vs eager for frozen snapshots, first reveal, shared-texture dedup, no bank
  aliasing, validation clean, TAA/HZB history tests.
- Performance: report completed-candidate rate, present rate, repeat-present ratio, request->display
  latency, GPU/CPU/upload breakdown and process VRAM/CPU working set separately. Do not report repeated
  present FPS as rendering speed.

## Gates and non-goals

- No public ABI change before Phase 3; ABI 59 lands only with descriptor banks. Same-frame ABI 58
  remains available and default-off.
- No production default change until Phase 5 acceptance; pipeline is opt-in.
- No queue-parallel visibility promise; the design explicitly does not promise two cameras on GPU
  concurrently.
- No source mutation tracking, device-loss injection, virtual textures, geometry streaming or stable-FPS
  claim in the first acceptance.

## First next action

Implement Phase 1 only: scheduler/snapshot model and CPU tests. Do not start runtime wiring or ABI 59
until Phase 1 tests make the state machine and history invalidation rules explicit.
