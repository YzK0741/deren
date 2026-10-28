# subtilis workstream - final closeout (2026-10-23)

Status: **CLOSED.** The subtilis large-scene workstream is finished for this phase. The shipping path is
Visibility buffer + Cluster DAG LOD + HZB occlusion/recovery (DAG+HZB); the optional software-raster lane is
kept as an experimental, default-off feature with **no further optimization planned**. No component of this
closeout changes runtime behavior or the default configuration.

## 1. Accepted shipping scope (evidence-backed)

| Component | Status | Key evidence |
| --- | --- | --- |
| M1a visibility-lane LOD0 bridge | accepted 2026-10-19 | `docs/reviews/2026-10-19-subtilis-m1a.md` |
| M1b hierarchy/upload/CPU cut | accepted 2026-10-20 | M1b reports + CTest |
| M1c runtime import of offline cluster DAGs (object LOD) | accepted 2026-10-22 | [object-LOD acceptance](2026-10-21-object-lod.md), [final review](2026-10-22-object-lod-final-review.md) |
| M3 HZB occlusion culling/recovery | accepted 2026-10-21 | [HZB recovery](2026-10-21-hzb-recovery.md), frozen gates |
| Offline glTF cluster-DAG converter (`tools/subtilis`) | accepted 2026-10-21 | [converter report](2026-10-21-subtilis-converter.md) + final review |
| Textured Bistro acceptance | accepted 2026-10-23 (corrected) | DAG+HZB **10.44 / 5.64 ms** far/near (visibility baseline 25.52/22.98, DAG 21.18/19.84); no pixel-identity claim |

DAG+HZB is the accepted large-scene gain. The earlier 5.22 ms / about 26x software-enabled headline is
withdrawn (it predated three software-raster wiring fixes); the historical legacy -> DAG+HZB gain in the
2026-10-22 matrix is 15.5x far / 22.4x near.

## 2. Optional / experimental (not shipping)

**M4 software raster** (tiled + opt-in triangle producer + opt-in GPU survivor routing) remains
default-off/opt-in and is **not accepted**. Final fresh review:
[software-routing review](2026-10-23-software-routing-review.md) (0 Critical / 4 Important / 12 Minor;
Important findings dispositioned in the [M4 decision](2026-10-23-m4-decision.md)).

Headroom / subpixel-share sweep ([report](2026-10-23-software-raster-headroom.md)):

- T1's GO upper bound (1.54 ms / 7.66% on the **no-HZB** M1c path) overestimates the recoverable budget in the
  shipping configuration by about **3-5x**: HZB already culls ~70.6% far, software routing removes 2,399 of
  12,332 survivors (19.5%), work-fraction estimate ~0.43 ms, paired median raster saving only 0.33 ms, with
  software pass 0.10 ms, merge 0.04 ms and CPU routing +0.66 ms cut wall / +1.5 ms CPU-active.
- Raising subpixel share from 12% to 49% (software clusters 400 -> 4,968/frame) produced no configuration win
  outside its run spread (Spearman -0.39 on 7 configs, 5/7 paired deltas sign-flip, all on/off spreads
  overlap); the software pass stayed 0.04-0.20 ms. **Verdict: unresolved - "the scene lacks tiny triangles"
  is not supported as the explanation; GPU clock drift prevents a definitive refutation.**
- GPU survivor routing removes ~0.63/0.47 ms CPU routing cost but has no stable whole-frame win.
- E3 (Jinx static field) was **stopped per user decision**: source acquisition + license/attribution and a
  4x4 smoke scene exist under `D:/deren-assets/2026-10-23-software-raster/jinx-stress/`; no cooked
  DEREN_subtilis package and no measurement were produced.

## 3. Not started / deferred

- M2 (GPU cut selection/finalize), M5 (streaming/paging), automatic import-time asset pipeline/on-disk cache.
- Optional follow-ups (explicitly not planned for this phase): remove/rename the T1 developer-only workload
  probe, one M3-only HZB re-baseline capture, full-stage verifier (manifest/CSR/hardware/resolve/merge
  readbacks), far-recovery 1-pixel oracle investigation, fixed-GPU-clock measurements.

## 4. Evidence and archives

- Object LOD / converter: `D:/deren-assets/2026-10-21-object-lod/`, `D:/deren-assets/2026-10-21-subtilis-converter/`.
- M4 experiments: `D:/deren-assets/2026-10-23-software-raster/` (earlier 1,801 files / 2,391,269,261 B),
  `.../gpu-routing-acceptance/` (728 files / 1,055,832,427 B), `.../headroom/` (251 files / 482,293,598 B),
  `.../jinx-stress/` (source + attribution + 4x4 smoke only). All have per-file SHA-256 manifests; the Lead
  re-hashed the final manifests with 0 missing / 0 mismatched.
- Review/decision: [M4 decision](2026-10-23-m4-decision.md), [routing review](2026-10-23-software-routing-review.md),
  [routing acceptance](2026-10-23-software-routing.md), [headroom report](2026-10-23-software-raster-headroom.md).
- Manuals: [docs/subtilis.md](../subtilis.md), [docs/visibility_buffer.md](../visibility_buffer.md).

## 5. Final gates on the closing tree

Release build no-work exit 0; full CTest **68/68**; CI-shaped ASan/UBSan **67/67** (recorded in the routing
acceptance); frozen Full **15/15** + Visibility **1/1** byte-identical (default OFF; same executable as the
recorded run); `test_docs` **407/0**; converter standalone **6/6**; clang-format-check and
check-backend-boundary PASS. Local commits only, no push.

## 6. Decision

The subtilis phase is closed: **DAG+HZB ships; software raster stays optional/default-off and will not be
optimized further.** Work can move to other engine areas (e.g., M2/M5 or unrelated rendering work) without
reopening this workstream.
