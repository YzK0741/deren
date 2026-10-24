# Deferred render operations - acceptance (2026-10-24)

Status: **accepted with follow-ups** (0 Critical / 5 Important / 11 Minor; independent review in
`docs/reviews/2026-10-24-render-operations-review.md`). The mechanism buffers state/execution operations in a
command-owned stream, validates the batch, removes redundant state, and lowers to native commands in order.
ABI 53 adds `command_recorder::apply_operations()`; `end_recording()` applies automatically; immediate/native
boundaries flush first. Acceptance is for the mechanism and verified behavior; no frame-time/performance claim
is made.

## Mechanism

- `recording::operation_stream` (source/promise/rhi/render_operations.cppm): polymorphic operation values in
  pooled storage; `kind()`, `is_unique()`, value equality, copied push payloads; `simplify(final, optimize)`
  performs coarse duplicate filtering, last-writer liveness and push-overlap elimination while keeping every
  execution operation.
- Vulkan `frame_commands` (core.render_operations.cpp): validates the whole retained batch before native
  replay; generation identities for buffers/pipelines; deferred errors latch and make the recording
  non-executable; group/push state is compacted into `saved_state` and restored at interop boundaries; resource
  groups/texture groups retain their borrowed owners through pending and GPU use.
- Boundaries that flush before native work: explicit `apply_operations`, `end_recording`, `execute(span)`,
  native prepare/finish, scoped synchronization, radix sorting and frame begin/end.

## Measurement (what the deferral actually removes)

PBR (`deferred`) and Gaussian (`gaussian_splat`) were captured on one Release build with
`DEREN_RENDER_OPERATION_STATS=1`. "Unoptimized" (`DEREN_RENDER_OPERATIONS_UNOPTIMIZED=1`) is the
no-simplification control; both arms use the same deferred execution stream, so it measures the simplification
savings, not the cost of deferral itself.

| Scenario | arm | requests/frame | retained/frame | native setters/frame | work/frame | screenshot |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| deferred | optimized | 47 | 47 | **33** | 8 | SHA-256 `FAE60C0B...` |
| deferred | unoptimized | 47 | 47 | 41 | 8 | identical |
| gaussian_splat | optimized | 56 | 55 | **40** | 11 | SHA-256 `8555C829...` |
| gaussian_splat | unoptimized | 56 | 56 | 52 | 11 | identical |

- Steady-state savings: **8/41 (19.5%) native setters per frame** on PBR and **12/52 (23.1%)** on Gaussian;
  execution counts are identical (8 and 11 work operations), and the captured screenshots are byte-identical
  between the arms.
- The first frames show warmup (75/78 requests and larger setter counts) and are excluded; the repeated
  steady-state line is used.
- Build identity of both arms: `executable_sha256 b7e7c2b0...`, `deren_vulkan.dll 770d51fa...`, 59-shader bundle
  `fdedf14f...` (recorded in `results.json`).
- Raw per-frame stats, debug logs and screenshots: `docs/reviews/2026-10-24-render-operations/` with
  `results.json`.

## Test coverage

- `test_render_operations` (CPU, CTest): duplicate filtering, last-writer liveness (A/B/A with consumers),
  protected duplicate execution, push copying with full/partial overlap, terminal vs intermediate application,
  compute not consuming vertex/index state, and 256 deterministic random streams compared by consumer
  observations with simplification on/off.
- Device probes: `test_resource_group` covers explicit application, copied push bytes, stale borrowed
  buffer/pipeline identity rejection before native replay, recording-error latching and begin/reset recovery,
  pending group retention and the not-recording errors; `test_dynamic_link` covers the plain command-buffer
  lifecycle, apply_operations not_ready/invalid_argument/ok transitions and recording reset; mergeable batch
  execution and completion tokens are exercised by the mergeable/resource-group/submission probes.
- Plan-required cases not directly covered: mid-window state beyond the resource-group apply-then-bind sequence;
  secondary/mergeable ordering under a failing implicit flush; native-interop `prepare_native`/`finish_native`
  restore with a consumer afterwards; `execute(segment)` with pending state and a later consumer; post-ABI53 GUI
  explicit-heap-bind smoke; address-recycling identities beyond the stale-buffer/pipeline cases; push/group token
  interactions across pipeline switches. These are recorded as residual gaps, code-inspected only where noted,
  not claimed as proven.

## Limitations

- The control is simplified-vs-unoptimized on the same deferred stream, not a pre-deferral build; the plan's
  original "before deferral" baseline could not be reconstructed because the feature builds on already-authorized
  uncommitted RHI changes. This is sufficient for "simplification does not change observable work" but it is not
  a deferral-vs-immediate performance comparison.
- The counts are totals across a 10-frame capture divided into per-frame steady state; they are not a benchmark
  and no frame-time improvement is claimed (fewer setters alone do not establish one).
- The GPU-side state cache still compares/emits per apply; no command sorting, draw fusion, automatic
  synchronization or render-graph inference is added.
- The full stage verifier / native lowering equivalence for every family is not implemented; validation is on
  the operation contracts and device probes above.
- CPU recording cost and allocation counts required by the spec are not measured (I2). The count table is not a
  benchmark; fewer native setters do not establish a frame-time improvement.
- `DEREN_RENDER_OPERATION_STATS` and `DEREN_RENDER_OPERATIONS_UNOPTIMIZED` are developer-only controls
  (documented in `results.json`), not product options.
- The CPU probe backend is an ABI-surface double (no saved-state/restore/identity equivalence), so device probes
  are the contract evidence. Device probes remain outside headless CTest by design.

## Verification

- Release CTest **69/69**, `test_docs` **419/0**, clang-format-check + check-backend-boundary PASS.
- Frozen render gate (`-Full`) **15/15**; visibility gate matched its tracked reference after the separate
  mesh-task indirect stride fix.
- `test_render_operations` **271 checks, 0 failed**.

## Independent review and disposition

Review: `docs/reviews/2026-10-24-render-operations-review.md` (base HEAD `7f38b720`; verdict **accepted with
follow-ups**, 0 Critical / 5 Important / 11 Minor). Findings disposition:

| Finding | Disposition |
| --- | --- |
| I1 optimized-vs-unoptimized control is not a pre-deferral baseline | **Narrowed claim**: only "simplification removes native setters on the same deferred implementation without changing work or output" is claimed. The plan's baseline checkbox is marked not reconstructed with the reason; no deferral performance claim is made. |
| I2 CPU recording cost / allocation count not measured | **Follow-up**, explicitly out of scope for this checkpoint; the spec requirement is listed as open. |
| I3 native interop / execute-restore / post-ABI53 GUI path not device-probed | **Follow-up**; recorded as residual gaps (native `prepare_native`/`finish_native` restore with consumer, execute with pending state, GUI smoke on the ABI 53 build). Code-inspected, not claimed as proven. |
| I4 `resource_groups.md` contradicted ABI 53 | **Fixed** in this checkpoint: the public resource-group doc now states queued logical state, same-pipeline preservation, switch retention/republish and apply/end error surfacing; `dynamic_runtime.md` ABI 53 section and ABI 52 push wording updated. |
| I5 coverage incomplete / understated | **Partially addressed**: added independent cull/depth-write state-kind coverage (275 checks) and expanded the residual gap list above; remaining device/native/GUI gaps stay follow-ups. |

Minor dispositions: build identity is now recorded in `results.json`; the two environment controls are marked
developer-only above; CPU-probe equivalence (M1) and the remaining minor items are follow-ups, not acceptance
claims.

## Verdict

**Accepted with follow-ups.** The deferred recording mechanism is functionally correct on the exercised CPU
and device paths, produces identical captured output, and removes 8/41 (PBR) and 12/52 (Gaussian) native setters
per steady-state frame without changing work counts. It is accepted as the mechanism; the plan's pre-deferral
baseline, CPU/allocation cost, native-interop/execute-restore and post-ABI53 GUI evidence remain open follow-ups
and must not be reported as done.
