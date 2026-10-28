# Memory acceptance and amnis CPU isolation

User approved completing texture-source memory acceptance, then comparing the same current
binary with DAG + HZB and either eager texture residency or progressive mip streaming.
Execute inline with executing-plans and verification-before-completion. Preserve other local work.

- [x] Build all targets and run regression checks after immutable-owner boundary correction.
- [x] Freeze one binary/DLL/shader bundle. Validate moving progressive streaming and restoration.
- [x] Capture eager/progressive near, far and 2-degree/frame sweep, two reversed-order rounds,
  360 frames each. Keep software raster off and identical geometry/camera/render settings.
- [x] Capture memory separately; capture CPU operation diagnostics separately if attribution needs it.
- [x] Compare final images and inspect the previously observed tiny differences; distinguish
  reproducible differences from unproven explanations. Preserve raw traces and manifests.
- [x] Document memory ownership, peaks, accounting limits and acceptance status. Replace stale
  pending descriptions. Document amnis CPU costs and select next work from measured evidence.
- [x] Review the final changes and verify documents/checks.

Completed 2026-10-27: [amnis isolation report](../../benchmarks/2026-10-27-amnis-isolation.md).
Progressive mips cost 1.7–2.4 ms mean wall time in these views, almost all in CPU post; GPU session time
increases by about 0.35–0.39 ms in the fixed views and is nearly unchanged in the sweep. Progressive
lowers GPU local allocation from about 2.966 to 1.10–1.15 GiB while retaining
about 2.25 GiB of CPU source. Near/far final frames are exact; the moving pair has a reproducible
within-binary round difference. Full CTest 73/73 and test_docs 499/0 pass on the current tree.

2026-10-28 evidence follow-up: [feedback copy probe](../../benchmarks/2026-10-28-feedback-readback-probe.md)
isolates approximately 1.9 ms copying the 64 KiB mip feedback table. Direct cached-host GPU writes
reduce CPU read time but regress GPU time substantially, so that experiment was restored rather
than adopted. Separate GPU storage and asynchronous CPU-readable transfer remain a proposal.

Ruling: Do not change production behavior during the comparison. A measured hotspot will guide
the next change; this increment establishes attribution and completes documentation.
Ruling: Do not commit or push; user has not requested integration.
