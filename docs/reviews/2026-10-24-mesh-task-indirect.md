# Mesh-task indirect stride validation fix (2026-10-24)

## Symptom

The visibility render gate (`check_render.ps1 -Visibility -Compare
docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`) failed with `FAIL: no screenshot produced`. The
debug log showed:

```
mesh indirect: the backend refused the indirect record (9) - the meshlet dispatches go through the DIRECT call
post graph: lane 19 refused recording (9)
```

Lane 19 is the visibility raster pass, so the failure was a real render-path failure, not a reference-image
mismatch.

## Root cause

`command_buffer::draw_mesh_tasks_indirect` validated its argument range with
`rhi::validate_draw_indirect_range`. That validator requires a stride of at least 16 B because the draw-indirect
record is 16 B, but the mesh-task record (`rhi::mesh_task_command`) is **12 B** and
`rhi::mesh_task_command_size` is 12. Every mesh-task indirect dispatch was therefore rejected with
`invalid_argument`; the visibility item recorder returned `false`, the visibility raster pass returned
`invalid_argument`, and no frame was captured. Normal frames mask the bug because the meshlet path falls back to
the direct dispatch, which is also why this is a pre-existing defect rather than a regression from the
2026-10-24 RHI checkpoint.

## Fix

- Added `rhi::validate_mesh_task_indirect_range(size, offset, count, stride)`: the same alignment, offset and
  overflow contract as the draw-indirect validator, with a 12 B minimum stride and 12 B record tail.
- `command_buffer::draw_mesh_tasks_indirect` now uses that validator.
- Added compile-time cases to `test_dynamic_link.cpp` (12/24/36-byte ranges valid; 8-byte or misaligned stride
  rejected; two records beyond a 12-byte buffer rejected).

## Verification

- Release CTest: **69/69 PASS** (includes the new static assertions).
- `check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`:
  **matched** `AEE07350B796AC07` - the tracked visibility reference is unchanged and is met again.
- `check_render.ps1 -Full`: **15/15 PASS** on the local per-driver references.
- `test_docs` and static gates re-run on the final tree.

## Files

- `source/promise/rhi/rhi.api_core.cppm` (new validator)
- `source/backends/vulkan/core/core.api_core.cpp` (call site)
- `source/tests/test_dynamic_link.cpp` (compile-time coverage)
