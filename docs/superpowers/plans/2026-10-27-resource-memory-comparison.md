# Resource memory comparison plan

Execute inline in the current checkout, preserving prior work. User explicitly requests comparison
using the newly implemented resource/vessel statistics; no new streaming policy is being designed.

**Goal:** Compare eager texture preload, synchronous GPU texture feedback, and current progressive
screen-mip streaming on identical Bistro views with physical allocation counters.

**Method:** Release build, 1280x720, previous Bistro fixture/camera/render settings. Geometry
visibility/culling remains identical; only texture streaming mode changes. Fixed and 2deg/frame
sweep. Performance: 360 frames, omit first 60 intervals, two rounds in reversed mode order.
Separate memory captures: 360 frames, sample every 10 presented frames and after scene preparation,
use frame >=180 for steady usage; include allocator lifetime peaks. No concurrent GPU/build/test
workloads during performance capture. Report CPU vessel usable bytes separately from GPU device-local
allocations and VMA backing blocks. Compare final screenshots to eager reference and retain evidence.

- [x] Extend opt-in benchmark trace with separate resource/vessel CSV (no queries in timing runs).
- [x] Validate parser missing/unsupported data rejection and actual device trace/validation.
- [x] Complete build/regression before captures; run modes serially with hashes/manifests.
- [x] Compute timing/steady/peak/quality tables; document accounting limits and archive evidence.

Evidence: ../../benchmarks/2026-10-27-resource-memory-comparison.md and accompanying CSV/gzip.

Files: source/app/main.cpp (opt-in sampling), scripts/measure/measure_progressive_textures.py
(capture/validation/summary), source/tests/test_progressive_measure.py (evidence validation),
docs/benchmarks/2026-10-27-resource-memory-comparison.* (results/evidence).
