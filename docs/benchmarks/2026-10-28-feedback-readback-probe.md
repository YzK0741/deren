# Feedback readback CPU diagnostic

Reviewed 2026-10-28; captures were made 2026-10-27. Follow-up to the
[same-binary amnis comparison](2026-10-27-amnis-isolation.md). This is a diagnostic experiment,
not an accepted production optimization or a replacement for the formal timing matrix.

## Isolated cost

`prepare_texture_feedback` copies a 2 KiB material bitset and a 64 KiB mip-demand table from
mapped coherent GPU buffers, then clears them for reuse. `update_progressive_textures` starts
its own timer after these copies; its 0.075–0.139 ms measurements therefore omit this cost.

Temporary CPU timers separate the material copy, mip copy and clear. Two 60-frame windows after
the first window of a 180-frame run give these means, in milliseconds:

| View / allocation | Material read | Mip read | Clear | GPU session total |
|---|---:|---:|---:|---:|
| Near, existing storage_coherent | 0.0513 | 1.9162 | 0.0048 | 5.19 |
| Near, host-cached preference + storage capability | 0.0011 | 0.0104 | 0.0007 | 19.57 |
| Sweep, existing storage_coherent | 0.0514 | 1.8707 | 0.0054 | 5.95 |
| Sweep, host-cached preference + storage capability | 0.0013 | 0.0098 | 0.0008 | 17.95 |

The existing allocation uses CPU_TO_GPU/sequential host-write intent. The experimental allocation
uses the existing uniform_cached intent (CPU_ONLY, HOST_CACHED preferred, coherent host visibility
required) plus the storage and device-address flags, preserving GPU storage access. The code does
not expose the chosen physical memory type here, so the actual HOST_CACHED bit and bus path were
not independently read back. The measured copy reduction is consistent with the changed host-read
allocation preference, and accounts for much of the formal comparison's 1.7–2.4 ms mode delta.

## Rejected direct-write variant

The GPU still writes/atomically updates the same feedback buffers directly in the cached experiment.
Although CPU reads become fast, GPU session time becomes much worse. The observed end-to-end
diagnostic frame means also increase (near 12.20 to 26.23 ms, sweep 17.93 to 23.41 ms).
These single diagnostic pairs include operation-profile overhead and transient uploads; they are
not formal performance estimates. Their size is sufficient to reject adopting this variant now.
The experiment does not prove which individual GPU pass or atomic access causes the regression.

All four probes exit zero with no reported backend errors and valid DAG/HZB activation, but Vulkan
validation was not enabled in these probes. Temporary profiling and allocation changes were restored;
neither runtime source file retains this experiment. Frozen diagnostic binaries are under
`tmp/amnis-feedback-before-bin/` and `tmp/amnis-feedback-cached-bin/`.

## Next experiment

Keep GPU feedback writes in a GPU-suitable storage buffer. At the end of feedback production,
copy the small feedback payload to a separate coherent buffer suitable for CPU reads. Reuse the
existing frame-slot completion/feedback delay to consume a completed copy without a same-frame
wait. Measure the GPU copy, CPU read and full frame separately, and validate transfer/storage flags,
barriers, slot lifetimes and restoration before considering adoption. This remains a proposal.

An earlier coarse timer-only near run suffered a 22–28 ms feedback-region anomaly, while its moving
run was near 2 ms. Those numbers are not used as formal estimates; the finer copy timers above
reproduced the approximately 1.9 ms mip-copy cost. No general GPU-equivalence claim is made.

## Evidence

- [Diagnostic CSV](2026-10-28-feedback-readback-probe.csv)
- [Manifests, frame/residency traces, summaries, diagnostic windows and instrumentation patches](2026-10-28-feedback-readback-probe-evidence.json.gz)
- Raw logs: `tmp/amnis-feedback-probe/`; analyzer: `tmp/analyze_feedback_probe.py`.
- Earlier coarse logs: `tmp/amnis-post-attribution/`; these are excluded from the copy-cost table.

Both diagnostic arms use the same shaders and prior ownership/recorder work. They predate the
later consuming-import and other source changes; no claim is made that they identify the current
workspace binary. Source ownership acceptance and the final statistical report remain separate.
