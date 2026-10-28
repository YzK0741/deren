# Common CPU frame attribution

User approved a separate renderer investigation after closing amnis. Keep the current amnis path
as a fixed baseline. Scope: measure common CPU frame preparation, graph construction/recording,
deferred-operation execution and task batching/waits before choosing the next optimization.
Use systematic-debugging and verification-before-completion; do not extend amnis or alter policy.

- [ ] Preserve current source/binary identities and prepare a temporary diagnostic build.
- [ ] Measure begin subphases; post graph setup/prepare/record; per-pass access/barrier/callback costs;
  visibility planning/publication/recording; heavy deferred batches and task-group elapsed time.
- [ ] Capture near/far/moving DAG+HZB with eager residency and current progressive mips. Run two
  reversed-order diagnostic rounds; exclude all diagnostic frame times from performance claims.
- [ ] Restore exact production sources, rebuild/verify and run a clean control if needed to bound
  instrumentation effects. Preserve manifests, source patches and useful raw attribution evidence.
- [ ] Identify the dominant wall-time component and recommend one next change, with uncertainty
  and parallel-worker versus caller-wait distinctions. Document and independently review findings.

Existing accepted frame baseline is the feedback-copy report. Per-pass barriers can flush commands
recorded by an earlier pass; label them accordingly. CPU/GPU durations overlap and must not be
subtracted as independent costs. Diagnostic clocks/logging perturb execution. Preserve all current
uncommitted work and do not commit/push.

Ruling: start with per-lane behaviour/content clocks plus total graph-record wall time. Their residual
contains graph access validation/barriers and custom callbacks. Only instrument each graph barrier
if that residual is significant; avoid rebuilding the exported graph execution module prematurely.
Heavy deferred batches are grouped by enclosing post-frame markers, excluding startup batches and
avoiding the old cumulative/thread-local profile's startup/per-frame ambiguity. Planning-group wall
time includes worker execution and scheduling, not necessarily idle caller time.

Ruling: discard initial tmp/common-cpu captures. The workspace binary equalled the old feedback-copy
control, while sources had been restored. Copy-Item preserved an older source timestamp, so incremental
builds omitted the restored texture-residency object. Diagnostic recompilation then mixed new mip
publication with old allocation code and failed first-frame validation. The accepted frozen amnis
final bundle is intact. Force timestamps/rebuild all four profiled units plus texture_residency;
verify actual GPU-only/readback allocations before new snapshots. Restore using write_bytes (fresh
mtime), not timestamp-preserving copies. Final evidence moves to tmp/common-cpu-v2.
