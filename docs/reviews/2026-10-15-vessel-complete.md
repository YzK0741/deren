# Complete project container migration - 2026-10-15

Completed the user's request to migrate the remaining project-owned standard
containers to deren::utility::vessel. Backend storage, RHI readback ownership,
GUI interfaces/callback strings, application adapters and test fixtures now use
the shared mimalloc allocator. Fixed-size array uses an identical std::array
alias; string streams now own strings with the project allocator. The test
printing harness writes formatted output through a FILE iterator, avoiding its
previous owned default-allocator string and avoiding module imports in headers.

RHI ABI33（当时值；当前 RHI ABI 45 / GUI ABI 5） and GUI ABI3 replace ABI32/ABI2 because owned concrete C++ types in
the DLL interfaces changed. Consumers and plugins were rebuilt together.
Version checks reject incompatible plugins before returning typed objects.
File/stream boundaries use filesystem paths, views or explicit string conversion.
No raw allocator or shader algorithm changed in this batch.

The source census scans 196 project C++ source/header files: zero old std
container declarations or mentions remain outside the underlying alias
implementation. Third-party sources, standard-library re-exports in vstd and
reference sources are excluded. Borrowed span/string_view and initializer_list
retain standard types. Ordinary new, shared_ptr control blocks, function and
hidden library allocations are outside the container migration.

CMake 4.4.4's synthesized BMIs exposed conflicting paths when promise began
depending on shared_utility. Target-specific warning/sanitizer exceptions now
apply as target-conditioned source options; target module options stay canonical.
Generated commands preserve all-sanitizer suppression for promise/backend/DLL
fixtures, leave the static probe instrumented, preserve app_config's specific
float-cast-overflow suppression and assets' unused-parameter warning suppression.
Independent review found no actionable remaining issue in this handling or in
ABI/ownership adapters. MSVC rules were not generated or tested locally.

Validation (clang64 23.1.3, Windows):

- Full Release build succeeds with -j14; complete CTest26/26 passes in21.72s.
  Hardlink tests run outside the filesystem sandbox, as in the preceding batch.
  Initial failure of a whitespace-specific source check was repaired by comparing
  the same neutral values after removing whitespace; the values were unchanged.
- Debug ASan/UBSan build of test_vessel, test_app_config and
  test_vulkan_capabilities succeeds; all3 tests pass with halt_on_error enabled.
  This is targeted sanitizer validation, not the full sanitizer suite.
- test_vessel retains77 checks, including deterministic OOM and concurrency tests.
  GUI plugin storage/acquisition/refusal checks pass35/35.
- Real Vulkan AS build/refit/micromap/native-heap traversal passes498/498 with
  validation enabled. RHI reports ABI33.
- Fifteen40-frame render sweeps have clean synchronization validation and final
  RGBA pixels identical to the pre-vessel ownership-migration archive.
- Flower CPU/GPU each render40frames; each mode's final RGBA and HDR bytes match
  its own archived pre-vessel capture exactly. Existing small CPU/GPU differences
  remain unchanged.
- VR_BUILD_TESTS=OFF shared_utility builds, with no utility_memory_test_* symbols.
- git diff --check passes. Prior dirty resource-ownership work remains preserved;
  no commit or push was made.

Evidence: [logs, census, baseline patch, comparisons and scripts](2026-10-14-vessel-complete/).
Live captures are under build-release-dyn-clang64/shared-assets-check/vessel-complete-*
and build-release-dyn-clang64/vessel-complete-gaussian-check.
Usage and current scope: [vessel guide](../vessel.md).
