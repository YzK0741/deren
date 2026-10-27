# Screen mip residency review

Read-only independent review against the screen-mip design and plan on the current feature branch.

Findings fixed:

1. Preserve software-raster eligibility in the endpoint's third push word. Mip feedback now has its
   own fourth word at byte offset 12. The shader mirror checks all four fields; active software raster
   and depth merge execute with mip feedback during a clean Vulkan validation capture.
2. Full restoration must require mip zero and no pending upload. Intermediate residency alone no
   longer terminates restoration. Disable at frame 80 reaches full optional residency at update 313.
3. Memory measurement must fail on sampler failure or missing/insufficient counters. The runner now
   validates evidence, records explicit validity/error and refuses short captures before launching.
   Four unit cases pass after an observed failing run; both real successful memory captures pass the
   final validator. The unit cases are registered in CTest.

Final source review has no remaining actionable findings. It covered mip suffix bounds, generated
NPOT levels, actual byte sizes, pending uploads, retired frame ownership, low floor reuse, promotion
ordering, restoration and capture-source trimming. Source review did not claim measured performance.

Verification: final complete CTest 73/73 pass; six accepted validation captures; nine performance/
quality captures plus two separate successful WDDM memory captures. See the benchmark report for
comparisons, exclusions, the initial timeout/sampler failures and known limits. No commit/merge/push.
