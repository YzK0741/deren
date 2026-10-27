# Resource memory statistics review

Independent read-only review found no actionable correctness issue in VMA create/destroy wrapper
coverage, error cleanup, staging-cache teardown, shared image/view ownership, lazy allocation,
coherent category/combined peaks, shared CPU counters, or ABI 59 and its fixture updates.

Verification:

- CPU tests first failed for missing statistics APIs; implemented tests then passed. Final tests
  cover usable bytes, capacity retained by clear, cross-thread and cross-DLL frees, failures/null
  frees, current usage returning to baseline, and retained peaks.
- Pure GPU tracker tests first failed for the missing tracker; final tests cover disjoint peaks,
  overlapping categories, device-local classification, coherent snapshots, and four concurrent
  allocations/frees.
- Full Release dynamic build passed. Complete CTest passed 73/73. After the final test additions,
  targeted vessel/frame/docs regression passed 3/3.
- Actual Vulkan frame test passed 191 checks with validation enabled, including buffer/image
  creation/release, content deduplication, views retaining allocations, lazy materialization and
  unload, VMA allocation-total agreement, and peak retention. No VUID/validation error was found
  in the device log.
- Initial device test used the unsupported shader_read -> shader_read barrier recipe and failed
  before lazy materialization. The test now uses the supported shader_read -> transfer_destination
  recipe in a discarded recording. No backend behavior change was needed.

The device run's teardown log recorded zero current resource bytes, buffer peak 4,329,488 B,
image peak 16,384 B and combined peak 4,341,760 B; VMA still retained 100,663,296 B of backing blocks.
This illustrates why allocated resource bytes and reserved block bytes are exposed separately;
it is a contract test workload, not a scene VRAM benchmark.

Local evidence: tmp/resource-memory-build-all.log, tmp/resource-memory-build-final.log,
tmp/resource-memory-ctest.log, tmp/resource-memory-device.log,
tmp/resource-memory-final-tests.log. No commit, merge or push.
