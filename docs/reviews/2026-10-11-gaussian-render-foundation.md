# Gaussian rendering foundation review — 2026-10-11

Scope: the approved foundation plan, based on `ef42e608`, covering GPU record packing, deterministic global CPU ordering and transactional RHI upload. Raster shaders, projection/SH evaluation, frame snapshots and viewer integration are the next batch.

## Delivered

- `1ae7656b`: validated geometry/SH packing, fixed layouts and conservative bounds.
- `fe71b876`: instance validation and deterministic scene-wide draw references.
- `c3a3efe5`: immutable shared GPU assets with transactional buffer ownership and upload.

## Independent review and corrections

An independent reviewer found two important validation defects. Both were reproduced with failing tests and corrected in one fix pass.

1. The covariance check previously allowed a separate tolerance on normalized principal minors and determinant. This could accept a materially negative eigenvalue in an anisotropic matrix. Validation now requires the normalized covariance plus `1e-6 I` to be positive semidefinite, bounding the tolerated negative eigenvalue directly. Regression coverage includes the reported counterexample, rank 0/1/2 PSD matrices, rotated anisotropy and the tolerance boundary.
2. Sorting previously allocated scene-sized scratch before validating public particles and transformed centers. Full particle/center validation now precedes the ID/key arrays. Allocation instrumentation verifies invalid scale, quaternion, opacity and SH inputs are rejected without scene-sized allocation.

The regression suite was observed failing (393 checks, 8 failures) before the fix and passing (393 checks, 0 failures) afterward. The reviewer confirmed both fixes and reported no remaining findings within this foundation's scope.

## Verification

- Full local Release build: passed.
- CTest: 22/22 passed.
- Gaussian foundation checks: 393/393 passed.
- clang-format-check: passed.
- Backend boundary: zero engine/application backend imports and zero leaked boundary symbols.
- Strict clang-tidy for all 19 CI source entries: passed on local LLVM 22.1.8.
- Remote LLVM 23.1.3 CI: passed. Release CTest 22/22 and ASan/UBSan CTest 21/21 passed; Debug build, strict analysis and runtime packaging passed. [CI run 38100311365](https://github.com/YzK0741/deren/actions/runs/38100311365) validated code commit `c913f6a8`.

The local tree also contains an unrelated Vulkan surface-cleanup change and the preexisting root probe DLL deletion. They are preserved and excluded from this batch's commits. Remote CI will verify the committed tree independently.

## Implementation decisions

- Use native Windows tooling for the ledger, retaining the task/test record format. Incorrect equivalence would affect bookkeeping, not the compiled implementation.
- Execute directly on master and push under the user's existing authorization. The renewed stop condition is 5% remaining quota.
- Expose model validation for subsequent instance preparation. This modestly expands the engine API; changing its contract later would require updating callers.
- Count packed geometry/SH or returned draw-reference bytes against the configured byte limit, documenting sorting scratch separately. Scratch and host allocation exhaustion follow the existing no-exceptions policy; the byte limit is not a total process-memory guarantee.
- Let upload accept the same optional foundation limits, and recompute GPU bounds from actual packed covariance. Bounds may differ slightly from CPU bounds, while supplied narrowed metadata cannot shrink published support.
- Shared GPU assets retain RHI buffers under the existing device/backend-root lifetime contract. Submission retention, real GPU layout/readback probes and raster visibility are required by the next batch; this foundation claims no rendered output.

No minor findings were deferred.

## Completion

The foundation and review corrections were committed and pushed to master through `c913f6a8`. All gates are green. The final completion update changes only this report and the plan, and uses `[skip ci]` because the validated source tree is unchanged. No source changes followed the CI run. Incorrectly classifying a code change as documentation would invalidate this shortcut; the final staged file list is checked before committing.

Next batch: CPU projection/SH reference, Slang raster shaders and layout/readback probes, the Gaussian pass, frame-slot retention and viewer integration. Actual Gaussian rendering is not yet implemented.
