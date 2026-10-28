# CI pause checkpoint — 2026-10-19

Paused at the user's remaining-5% limit: 300-minute window used 95%, remaining 5%. Do not resume work without user continuation; do not redeem reset credits automatically.

## Submitted work

- Branch: master
- Commit: c5df0913fa838d494585da8ea54121b3bf1a6a0d
- Title: feat: add subtilis LOD0 publication and fix render graph handoffs
- Pushed to origin/master successfully. The existing 97 local ancestor commits were uploaded with it, without rewriting history.
- Commit contains 46 files: previous ownership/GUI/RT fixes and subtilis M1a, tests and evidence.
- Precommit Release CTest: 51/51 passed. Earlier relevant ASan and device checks are recorded in 2026-10-19-subtilis-m1a.md.

## CI at pause

Run: https://github.com/YzK0741/deren/actions/runs/39240772119

- posix-stacktrace: SUCCESS (27 seconds).
- Release build + ctest + Debug gate + clang-tidy: IN PROGRESS, Build all targets (incl. tests + deren.exe).
- ASan + UBSan (headless tests): IN PROGRESS, Build the headless tests.
- No CI job failure reported at the last direct query. Final result unknown.

The watch command stopped because the GitHub API returned HTTP 502; a subsequent direct query succeeded and confirmed the jobs are still building. Treat this as a watcher/API interruption, not a CI failure.

## Resume

1. Query this run first, matching head SHA to the commit above:
   gh run view 39240772119 --repo YzK0741/deren
2. If failed, inspect the failing job logs and fix the verified cause; retest locally, commit and push the follow-up.
3. If successful, report success with the run URL; do not repeat M1a.
4. Continue honoring the remaining-5% quota pause policy.

This checkpoint was written locally after the pushed commit. It is intentionally not included in that commit; the code worktree was clean immediately after pushing.
