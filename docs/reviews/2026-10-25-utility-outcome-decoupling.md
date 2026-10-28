# Utility outcome decoupling

Date: 2026-10-25. Base: fe0280c29857404b89411d700c36c73aa2c38d69.
Scope: the first utility audit follow-up; thread pool and file I/O are deferred.

## Result

RHI-specific result/verdict classification now belongs to `deren.engine.outcome` in
`source/engine/outcome/outcome.cppm`, built by `engine_outcome_core`. The exported names are
`deren::engine::outcome::{result, verdict, pass_success, pass_failure, fatal, propagate, enforce}`.
Runtime's frame-acquire classifier explicitly imports this module. Generic `ensure`, panic,
logging and allocation remain in utility. No compatibility re-export restores the old dependency.

The utility module's documented source interface version is 2.0.0: users of its former outcome
names must migrate imports and qualifications. The RHI contract remains ABI 55. There is no change
to error rewriting, fatal handling, or process-wide sink/allocator ownership.

## Verification

- Regression RED: both new boundary checks failed before extraction (RHI import and direct link).
- Regression GREEN: both pass after extraction and are included by existing CTest Python discovery.
- Generated CMake Graphviz dependency traversal: static_utility reaches only shared_utility, vstd_lib,
  xxhash, mimalloc and platform libraries; no promise, rhi_sync_contract, engine_outcome_core or deren_engine.
- Full Release build: exit 0 with 14 build workers.
- Headless CTest: 71/71 passed. New test_engine_outcome: 19 checks, zero failures, including void and
  move-only values. The original utility-only test keeps ensure coverage and has no RHI import.
- Device lane: 7/7 passed (resource group 221; texture group 116; submission completion 75;
  render graph frame 12; Gaussian GPU frame 65; buffer transfer 65; subtilis transfer 83).
- Dynamic runtime with device: ABI 55, 1174 checks, zero failures. Its debug log contains no
  `VUID` or `Validation Error` matches.
- Independent read-only review: no actionable correctness findings.
- Exact CI tidy gate (39 sources, now including the outcome module): exit 0.

## Evidence and limits

Local logs are in `tmp/utility-outcome-{full-build,ctest,device,runtime-device,dependency-check}.log`;
tidy uses `tmp/ci-fix-tidy-primary-all.log`. These are local verification artifacts, not committed docs.

The durable Python boundary regression checks utility source imports and static_utility's direct
links. The current transitive boundary was separately verified from the generated build graph;
the regex check is not a generic transitive CMake dependency validator.

Fatal and dishonest-classifier execution paths retain their existing implementations; no new
subprocess termination tests were added. No remote CI run or local sanitizer rebuild is claimed;
the sanitizer workflow's explicit target list now includes the extracted test. No commit or push
was requested for this change.
