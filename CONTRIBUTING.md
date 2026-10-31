# Contributing and commit rules

These rules exist because CI history is public evidence. A red `master` is not an
acceptable intermediate state, and a green final commit does not excuse the red ones
that came before it.

## Merge contract

- CI runs on **pull requests** and on manual `workflow_dispatch`, not on pushes to
  `master`.
- **PR CI is the merge authority.** A pull request is merged only when every job in
  `.github/workflows/ci.yml` is green.
- **Squash merge only.** The commit that lands on `master` is exactly the PR tree
  the run validated, so `master` is not rebuilt after the merge and no post-merge
  push build is needed or wanted.
- To re-check a branch (including `master`) by hand, use `workflow_dispatch` on
  that ref.

## Hard rules

1. **Never push directly to `master`.** Work on a feature branch and open a pull
   request. The local pre-push hook rejects `master`, and branch protection rejects
   it on GitHub too.
2. **Never merge locally into `master`.** The pre-merge-commit hook rejects merge
   commits; use a pull request (squash merge).
3. **Every commit must answer YES to all three questions:**
   - Is the feature complete (not a checkpoint, not a "fix later")?
   - Is the change ready for the local gate that runs after the commit
     (`scripts/commit.ps1` rolls the commit back if that gate fails)?
   - Is the commit honest (no hidden WIP, no untracked build dependencies, no
     claims that the code does not support)?
4. **Do not push an unverified commit.** Run the local gate before pushing;
   `scripts/commit.ps1` runs it automatically against the new commit. If it fails,
   fix it and amend/squash locally; do not add a "fix CI" commit to the branch.
5. **Do not hide history.** Do not delete/rewrite CI runs or rewrite `master` to
   make the Actions page look better. The fix is the process, not cosmetics.
6. **No untracked-file dependencies.** A build or test may only reference files
   that are committed. The CI failure mode we hit repeatedly was a target or
   `#include` pointing at an untracked scratch file. This is enforced: the local
   gate marker refuses to be written, and `scripts/check-ci-marker.ps1` refuses to
   validate it, while any untracked file outside generated output exists.
7. **One logical change per commit.** Keep refactors, asset moves, vendor updates
   and behaviour changes separate when they can be reviewed independently.
8. **Squash before review.** A feature branch may have many WIP commits locally;
   the PR should contain coherent commits (or one squash commit), not a trail of
   "try again" commits.

## Required local gate

Run from the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-local-ci.ps1 -Mode Full
```

This is a **local gate, not a CI pass**. It is a strict subset of the remote
workflow that can run on a Windows checkout:

| Runs locally (`-Mode Full`) | Remote CI only |
| --- | --- |
| `git diff --cached --check` | Debug build (assert-enabled compile gate) |
| Release build | ASan + UBSan over the headless tests |
| backend-boundary ratchet (`check-backend-boundary`) | POSIX stacktrace probe (Ubuntu job) |
| full CTest with a workspace-local `TEMP`/`TMP` | |
| `clang-format-check` | |
| the CI `TIDY_FILES` list through `scripts/tools/run_clang_tidy.py` | |

`-Mode Fast` runs the same build, backend-boundary, CTest and clang-format steps
but skips clang-tidy, and it writes no marker. Only `-Mode Full` produces
`tmp/local-gate-pass.json`.

The marker attests to the local subset above, not to remote CI. PR CI remains the
merge authority. Before opening a PR, also run the sanitizer build if the change
touches engine or shader code. The exact CI commands are in
`.github/workflows/ci.yml`; when CI gains a gate that is cheap to run locally, add
it to the script rather than widening the claim in this document.

## Committing

Use the checklist script, not `git commit` directly:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\commit.ps1 -Message "toon: ..." -Paths source/shaders/toon/game_toon
```

It stages the given paths, prints the staged diff, asks the three questions, creates
the commit, then runs the local gate **against that commit**. On success the gate
writes `tmp/local-gate-pass.json` with the commit SHA, tree SHA, commit time, mode,
the gates that actually ran, the gates that did not, and a timestamp. If the gate
fails, the commit is rolled back with `git reset --soft` and the staged changes stay
in place. The `pre-commit` hook requires the checklist marker, so a bypass is
visible in the shell history rather than silent.

## Pushing and merging

```powershell
# push a feature branch and optionally open a PR
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\push.ps1 -Branch feature/my-change -CreatePr
```

`push.ps1` refuses `master`/`main`, validates that `tmp/local-gate-pass.json` names
the current HEAD and was generated after that commit, asks `Is the push complete?
[y/N]`, and only then pushes. `.githooks/pre-push` runs the same
`check-ci-marker.ps1` validation and checks every ref on stdin: any ref whose local
SHA is not the verified HEAD is rejected, so a stale branch tip cannot be pushed
along with a verified one. Deleting a non-`master` branch publishes no tree and is
allowed.

The marker is local by design: CI cannot write a repository file without creating a
second, unverified commit, and a committed marker would only assert itself. GitHub
cannot see `tmp/local-gate-pass.json`; the server-side equivalent is the PR-only
branch protection + required status checks described below. Never commit
`tmp/local-gate-pass.json` (`tmp/` is ignored).

Enable the hooks once per clone:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-git-hooks.ps1
```

The hook files are tracked with the executable bit set in the index, so a normal
checkout can run them. If a clone still skips them, restore the mode with:

```powershell
git update-index --chmod=+x .githooks/pre-commit .githooks/pre-merge-commit .githooks/pre-push
```

## GitHub-side protection

Local hooks are advisory; a collaborator can bypass them. Configure branch
protection on `master` (repository Settings → Branches, or the helper script
`scripts/setup-branch-protection.ps1`) with:

- require a pull request before merging;
- require the checks produced by the PR workflow:
  `posix-stacktrace`, `Release build + ctest + Debug gate + clang-tidy`, and
  `ASan + UBSan (headless tests)`;
- require branches to be up to date before merging;
- require linear history (the squash merge is the only landing path);
- disallow force pushes and deletions;
- apply the rule to administrators as well.

**Required approvals:** the helper script's default is `-Approvals 0`, which is the
explicit solo-maintainer exception. GitHub does not let a PR author approve their
own PR, so a required approval would make every PR unmergeable while there is one
maintainer. This exception is printed by the script and is the only reason reviews
are not enforced server-side. As soon as a second maintainer can review, re-run the
script with `-Approvals 1`.

The helper script is deliberately `-Apply`-gated and prints the exact API call it
would make. Do not run it against a repository where another maintainer owns the
branch policy without asking.
