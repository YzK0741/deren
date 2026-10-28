# Composed rendering benchmark plan

**Goal:** Measure subtilis DAG + HZB + amnis progressive mip streaming, with an experimental
software-raster comparison. User confirmed this scope; postprocessing/shadow settings stay unchanged.

Documentation boundary cleanup is completed and test_docs passes before benchmark work starts.
subtilis controls geometry, amnis controls textures, and resource/synchronization/statistics services
are shared. Historical acceptance results retain their original conditions.

**Method:** Existing Release executable/shaders, archived textured Bistro DAG asset (464 payload-bearing
primitives), 1280x720, same near/far cameras and near 2deg/frame sweep. Three profiles: amnis only;
subtilis DAG + HZB build/cull/recovery + amnis; same plus experimental triangle software raster with
GPU survivor routing. Error threshold 1px, hysteresis off. All profiles use progressive mip streaming.
Two rounds in reversed profile order, 360 requested frames, discard first 60 timing intervals. Separate
resource captures for each profile/scenario. Validation smokes before timing. No concurrent build/test/GPU
loads during captures. Require logged DAG adoption/cut, HZB activity/recovery and requested software route;
configuration alone is not activation evidence. Small software deltas do not establish a stable win.

- [x] Clarify documentation boundaries and check manual constraints.
- [x] Extend measurement CLI with independent subtilis/HZB/software gates, manifests and activity checks.
- [x] Test missing activity rejection, validate actual composed paths, and complete regression.
- [x] Serial timing and separate memory captures; same-binary/shader identities across profiles.
- [x] Archive traces and summarize composed performance, memory, activation and limits.

Completed 2026-10-27: [composed rendering report](../../benchmarks/2026-10-27-composed-rendering.md),
[timing CSV](../../benchmarks/2026-10-27-composed-rendering.csv),
[resource CSV](../../benchmarks/2026-10-27-composed-resources.csv) and compressed evidence. The matrix
did not establish a composed speedup: DAG+HZB added 3.5–6.8 ms over amnis in the captured views and
the software profile was not consistently separated from DAG+HZB. Activation gates passed and all
runs used one binary/shader identity. A follow-up matrix with only subtilis enabled (no HZB, no
software) measured 45.90/44.24/46.28 FPS near/far/sweep versus amnis 44.43/40.66/44.84, with GPU local
steady allocation 3–6% lower; the HZB addition is what makes the composed `dag_hzb` timing regress.

Files: docs/{subtilis,amnis,managed_resources,resource_memory_statistics,visibility_buffer}.md;
scripts/measure/measure_progressive_textures.py; source/tests/test_progressive_measure.py;
docs/benchmarks/2026-10-27-composed-rendering.*. No policy/renderer implementation changes planned.
