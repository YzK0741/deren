# Contributing and commit rules

These rules exist because CI history is public evidence. A red `master` is not an
acceptable intermediate state, and a green final commit does not excuse the red ones
that came before it.

## Hard rules

1. **Never push directly to `master`.** Work on a feature branch and open a pull
   request. The local pre-push hook rejects `master`, and branch protection should
   reject it on GitHub too.
2. **Never merge locally into `master`.** The pre-merge-commit hook rejects merge
   commits; use a pull request (squash merge).
3. **Every commit must answer YES to all three questions:**
   - Is the feature complete (not a checkpoint, not a "fix later")?
   - Has the local CI gate passed (`scripts/run-local-ci.ps1`)?
   - Is the commit honest (no hidden WIP, no untracked build dependencies, no
     claims that the code does not support)?
4. **Do not push an unverified commit.** Run the local gate first. If it fails,
   fix it and amend/squash locally; do not add a "fix CI" commit to the branch.
5. **Do not hide history.** Do not delete/rewrite CI runs or rewrite `master` to
   make the Actions page look better. The fix is the process, not cosmetics.
6. **No untracked-file dependencies.** A build or test may only reference files
   that are committed. The CI failure mode we hit repeatedly was a target or
   `#include` pointing at an untracked scratch file.
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

This runs:

- `git diff --cached --check`
- Release build
- full CTest with a workspace-local `TEMP`/`TMP`
- `clang-format-check`
- the CI `TIDY_FILES` list through `scripts/tools/run_clang_tidy.py`

Before opening a PR, also run the sanitizer build if the change touches engine or
shader code. The exact CI commands are in `.github/workflows/ci.yml`; the local
script is deliberately kept in sync with them.

## Committing

Use the checklist script, not `git commit` directly:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\commit.ps1 -Message "toon: ..." -Paths source/shaders/toon/game_toon
```

It stages the given paths, prints the staged diff, asks the three questions, creates
the commit, then runs the local gate **against that commit**. On success the gate
writes `tmp/ci-pass.json` with the commit SHA, tree SHA, commit time and a timestamp.
If the gate fails, the commit is rolled back with `git reset --soft` and the staged
changes stay in place. The `pre-commit` hook requires the checklist marker, so a
bypass is visible in the shell history rather than silent.

## Pushing and merging

```powershell
# push a feature branch and optionally open a PR
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\push.ps1 -Branch feature/my-change -CreatePr
```

`push.ps1` refuses `master`/`main`, validates that `tmp/ci-pass.json` names the
current HEAD and was generated after that commit, asks `push 完备? [y/N]`, and only
then pushes. `.githooks/pre-push` runs the same `check-ci-marker.ps1` validation, so
a direct feature-branch push with a stale or missing marker is rejected.

The marker is local by design: CI cannot write a repository file without creating a
second, unverified commit, and a committed marker would only assert itself. GitHub
cannot see `tmp/ci-pass.json`; the server-side equivalent is the PR-only branch
protection + required status checks described below. Never commit `tmp/ci-pass.json`.

Enable the hooks once per clone:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-git-hooks.ps1
```

## GitHub-side protection

Local hooks are advisory; a collaborator can bypass them. Configure branch
protection on `master` (repository Settings → Branches, or the helper script
`scripts/setup-branch-protection.ps1`) with:

- require a pull request before merging;
- require the `ci` workflow checks to pass;
- require branches to be up to date before merging;
- disallow force pushes and deletions;
- apply the rule to administrators as well.

The helper script is deliberately `-Apply`-gated and prints the exact API call it
would make. Do not run it against a repository where another maintainer owns the
branch policy without asking.
