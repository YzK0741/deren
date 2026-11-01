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
- **`wip/*` branches are outside this contract.** They are the backup channel for
  unfinished work: no local gate, no CI, never merged (see "The WIP channel").

## Hard rules

1. **Never push directly to `master`.** Work on a feature branch and open a pull
   request. The local pre-push hook rejects `master`, and the branch protection
   under "GitHub-side protection" rejects it on GitHub once applied. If the work is
   not reviewable yet, back it up on a `wip/` branch (below); that channel is never
   merged.
2. **Never merge locally into `master`.** The pre-merge-commit hook rejects merge
   commits; use a pull request (squash merge).
3. **Every commit must answer YES to all three questions:**
   - Is the feature complete (not a checkpoint, not a "fix later")?
   - Is the change ready for the local gate that runs after the commit
     (`scripts/commit.ps1` rolls the commit back if that gate fails)?
   - Is the commit honest (no hidden WIP, no untracked build dependencies, no
     claims that the code does not support)?
   A `wip/*` branch is the ONE exception: it is a backup channel, not a merge
   candidate, and the checklist does not apply there (see "The WIP channel").
4. **Do not push an unverified commit.** Run the local gate before pushing;
   `scripts/commit.ps1` runs it automatically against the new commit. If it fails,
   fix it and amend/squash locally; do not add a "fix CI" commit to the branch.
   The `wip/` channel below is the only place an unverified commit may be
   published, and nothing there is evidence.
5. **Do not hide history.** Do not delete/rewrite CI runs or rewrite `master` to
   make the Actions page look better. The fix is the process, not cosmetics.
6. **No untracked-file dependencies.** A build or test may only reference files
   that are committed. The CI failure mode we hit repeatedly was a target or
   `#include` pointing at an untracked scratch file. The local gate enforces it:
   the marker refuses to be written, and `scripts/check-ci-marker.ps1` refuses to
   validate it, while any untracked file outside generated output exists.
7. **One logical change per commit.** Keep refactors, asset moves, vendor updates
   and behaviour changes separate when they can be reviewed independently.
8. **Squash before review.** A feature branch may have many WIP commits locally;
   the PR should contain coherent commits (or one squash commit), not a trail of
   "try again" commits.
9. **Do not import outside patterns without precedent.** This tree has its own idioms
   and they are not suggestions: modules split into `.cppm`/`.cpp` with the
   `export import std` pattern, `vessel` containers instead of the C++ standard
   library, plain classes with direct members rather than pimpl
   (`source/backends/vulkan/graphical_user_interface/graphical_user_interface.cppm`
   states this explicitly), and private app headers included after the module
   imports (`source/app/environment_loader.hpp` shows the shape). Before introducing
   a technique, abstraction or "standard practice" from elsewhere, find where this
   repository already does it. If there is no precedent, ask instead of inventing
   one; if the repository has a precedent, follow it rather than adding a second way
   to do the same thing. The established design is the authority, not the newest
   pattern that happens to work.

## Required local gate

Run from the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-local-ci.ps1 -Mode Full
```

The build tree is auto-detected the same way the build helper names it
(`build-release-clang64` on Windows, `build-release` on POSIX), falling back to this
checkout's hand-made `build-release-dyn-clang64`; pass `-BuildDir <path>` to override.

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
the gates that actually ran, the gates that did not, a timestamp, and the SHA-256 of
`tmp/local-gate.log` (the transcript every step's output was tee'd into). If the gate
fails, the commit is rolled back with `git reset --soft` and the staged changes stay
in place.

The `pre-commit` hook refuses `git commit` unless `commit.ps1` set its checklist
variable. That is a guard against forgetting the checklist, not a lock: `git commit
--no-verify` skips it, and the variable can be set by hand. What `push.ps1` and the
`pre-push` hook actually validate is the marker: it must name HEAD, its tree must
match, and its gate log must exist with the recorded SHA-256 and carry every gate's
header. That makes a forgotten or hand-written marker fail loudly - it is evidence a
run happened, tamper-evident against accident rather than against a maintainer who
deliberately writes both files, which is why PR CI remains the merge authority.

The `wip/` branches are the exception: `.githooks/pre-commit` passes on any `wip/*`
branch without the checklist, because that branch is a backup channel and cannot be
merged (see "The WIP channel"). Do not use it to publish work you intend to land.

## Pushing and merging

There are two push channels, and each tool refuses the other's branches: the
**release channel** (`scripts/push.ps1`: feature branch, local gate marker, PR) and
the **WIP channel** (`scripts/push_wip.py`: `wip/*`, no marker, no PR, no CI - see
below).

```powershell
# push a feature branch and optionally open a PR
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\push.ps1 -Branch feature/my-change -CreatePr
```

`push.ps1` refuses `master`/`main` and `wip/*`, validates that `tmp/local-gate-pass.json`
names the current HEAD and was generated after that commit, asks `Is the push
complete? [y/N]`, and only then pushes. `.githooks/pre-push` runs the same
`check-ci-marker.ps1` validation and checks every release ref on stdin: any ref whose
local SHA is not the verified HEAD is rejected, so a stale branch tip cannot be pushed
along with a verified one (`wip/*` refs are exempt; see "The WIP channel"). Deleting a
non-`master` branch publishes no tree and is allowed.

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

## The WIP channel (`wip/*`)

Unfinished work has a channel of its own, so that "back this up" never means
"publish an unverified commit as if it were reviewable":

- **Commit freely on a `wip/` branch.** `.githooks/pre-commit` skips the
  three-question checklist for `wip/*`: the channel exists for half-done work, and
  nothing on it is a merge candidate. Local merges into the branch are allowed too
  (`.githooks/pre-merge-commit` follows the same branch rule); merging master into
  a wip branch is local upkeep and can never land.
- **Publish with `python scripts/push_wip.py`.** It is cross-platform on purpose -
  no `pwsh`, no Windows build tree, no local gate marker - because backing up
  unfinished work should not depend on the release toolchain. Plain `git push
  origin wip/<name>` also works: `.githooks/pre-push` exempts `wip/*` refs from the
  marker. `scripts/push.ps1` refuses `wip/*` and `push_wip.py` refuses every other
  branch, so the two channels cannot be confused.
- **No CI, locally or remotely.** `ci.yml` and `subtilis-converter.yml` skip every
  job on a `wip/*` ref, and this channel never opens a pull request. The guard is
  checked, not trusted: `scripts/checks/check_wip_ci_guard.py` runs in the Release
  job and fails a release PR when a job loses it. A `wip/` branch that somehow gets
  a PR produces no required checks, so the branch protection below (once applied)
  blocks the merge.
- **Never merge, never cite.** A `wip/` branch is not evidence of anything. When
  the work becomes reviewable, squash it onto a feature branch, run
  `scripts/commit.ps1`, push with `scripts/push.ps1`, and let PR CI be the merge
  authority.
- **Rewrite and delete freely.** `push_wip.py -f` force-pushes a rewritten wip
  branch (expected on a backup branch) and `push_wip.py --delete -b wip/<name>`
  removes it. Delete a `wip/` branch once its work has landed or been abandoned;
  it must not accumulate as a second history.

The channel is defined by the branch name, not by a flag, so a release branch
cannot reach it by accident.

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

These required checks belong to the release path only: a `wip/` branch never
produces them, so once this protection is applied a wip PR cannot merge. That
protection is repository configuration, not code - it is only in force after the
helper has been run with `-Apply`, and `gh api
repos/<owner>/<repo>/branches/master/protection` answers 404 while it is not.

**Required approvals:** the helper script's default is `-Approvals 0`, which is the
explicit solo-maintainer exception. GitHub does not let a PR author approve their
own PR, so a required approval would make every PR unmergeable while there is one
maintainer. This exception is printed by the script and is the only reason reviews
are not enforced server-side. As soon as a second maintainer can review, re-run the
script with `-Approvals 1`.

The helper script is deliberately `-Apply`-gated and prints the exact API call it
would make. Do not run it against a repository where another maintainer owns the
branch policy without asking.
