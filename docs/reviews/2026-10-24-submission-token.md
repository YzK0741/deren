# Submission completion token verification

Implemented the approved non-owning value token through optional `command_submission`.
The old RHI submit interfaces and frame/presentation path remain intact (ABI 49).

## Evidence

- Red: ownership contract test failed on missing submission_token / command_submission.
- Full Release build completed with 14 jobs.
- Headless CTest: 68/68 passed.
- New standalone Vulkan device probe: 63 checks passed, validation enabled, no VUID/error logged.
  Covers actual GPU buffer copy/readback; pending query and finite/zero timeout; rejected empty,
  unrecorded, already-pending, foreign and future inputs; successful-only sequence publication;
  concurrent waiter and submitter; primary and batch retention; repeated query after pending
  reclamation; completed token while a later queue submission remains blocked; distinct IDs for
  live cores and for destroyed/recreated cores.
- Gaussian windowed 8-frame smoke: exit 0, screenshot produced, zero validation errors.
  `DEREN_GPU_SORT_PROBE=1` ran the migrated token-based readback consumer: PASS 48 cases.
- PBR DamagedHelmet windowed 8-frame smoke: exit 0, screenshot produced, zero validation errors.
- Normal interactive startup with splash produced repeated frame-phase reports and closed through
  the window's normal close message with exit 0. The first fixed-12-second sampling attempt ended
  before timing reports appeared; the successful check waited for actual rendering progress instead.
  This does not claim to diagnose or fix the earlier intermittent startup report.
- Fresh independent review reported no actionable findings. Device recreation coverage was expanded
  after review identified that gap in the initial test.

## Limits and rulings

- Device loss and native vkQueueSubmit allocation/failure results are propagated, but not fault-injected
  in these tests. Tests reject invalid host inputs without submitting GPU work.
- Tokens prove completion, not general resource ownership or correct synchronization barriers.
- Frame submissions, multiple queues and wholesale replacement of wait_idle are outside this change.
- Existing device-suite failures documented in `2026-10-24-rhi-command-recorder.md` are unchanged;
  they are not counted as passing here. The new completion probe avoids that legacy texture-group path.
- Initial test-fixture validation exposed a missing transfer_source flag; the fixture was corrected
  and the final 63-check run was validation-clean.
- No commit, push or publication requested/performed.

Raw local evidence: `tmp/submission-full-build.log`, `tmp/submission-ctest.log`,
`tmp/submission-device-final.log`, `tmp/submission-device-validation.log`, and
`tmp/submission-smoke/results.json` with separate scene logs/captures.
