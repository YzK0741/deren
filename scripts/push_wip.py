#!/usr/bin/env python3
"""Publish an unfinished branch over the WIP channel: no local gate, no CI, never merged.

THE WIP CHANNEL. Branches under ``wip/`` are the backup channel for work that is
not reviewable yet:

  * no three-question checklist and no local gate marker - those belong to the
    release path (scripts/commit.ps1 and scripts/push.ps1), which is unchanged;
  * no pull request and no CI - ci.yml and subtilis-converter.yml skip every job
    on a ``wip/*`` ref, so nothing published here produces or claims a green run;
  * never mergeable - the landing path is still a feature branch whose commits
    passed the local gate, pushed with scripts/push.ps1 and squash-merged by PR CI.

Cross-platform on purpose: the release path needs PowerShell and a Windows build
tree, but backing up unfinished work should not. Standard library only, and it
stays Python 3.8 compatible like the other scripts (see scripts/build_helpers).

Exit codes: 0 published (or deleted), 1 refused or failed.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

WIP_PREFIX = "wip/"
PROTECTED_BRANCHES = ("master", "main")
DEFAULT_REMOTE = "origin"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class WipPushError(Exception):
    """A refusal or failure this script can explain better than git's own message."""


class NothingToDelete(WipPushError):
    """The remote wip branch is already gone; cleanup is idempotent."""


def git(*arguments, check=True):
    """Run git in the repository root and return the completed process."""
    try:
        process = subprocess.run(
            ["git", "--no-pager", *arguments],
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except FileNotFoundError:
        raise WipPushError("git was not found on PATH; install git or run this from a normal checkout")
    if check and process.returncode != 0:
        output = (process.stdout or "").strip()
        raise WipPushError(output or "git {} failed".format(" ".join(arguments)))
    return process


def git_output(*arguments):
    return (git(*arguments).stdout or "").strip()


def repository_check():
    process = git("rev-parse", "--is-inside-work-tree", check=False)
    if process.returncode != 0 or (process.stdout or "").strip() != "true":
        raise WipPushError("not inside a git work tree: {}".format(REPO_ROOT))


def current_branch():
    process = git("symbolic-ref", "--short", "-q", "HEAD", check=False)
    branch = (process.stdout or "").strip()
    if process.returncode != 0 or not branch:
        raise WipPushError("HEAD is detached; check out the wip branch you want to publish")
    return branch


def target_branch(requested, deleting):
    if deleting:
        if not requested:
            raise WipPushError("--delete needs -b/--branch: name the remote wip branch to remove")
        return requested
    # An explicit -b publishes that branch from any checkout (git push does the same); without
    # one, "the branch I am on" is the target and a detached HEAD is an error.
    return requested or current_branch()


def validate_channel(branch):
    if branch in PROTECTED_BRANCHES:
        raise WipPushError(
            "'{}' is protected; the WIP channel never publishes a protected branch".format(branch)
        )
    if not branch.startswith(WIP_PREFIX):
        raise WipPushError(
            "'{}' is not on the WIP channel: wip branches are named '{}<name>'.\n"
            "  - for unfinished work:   git switch -c wip/<name>\n"
            "  - for reviewable work:   scripts/commit.ps1 + scripts/push.ps1 (the local gate marker is required)".format(
                branch, WIP_PREFIX
            )
        )


def remote_check(remote):
    process = git("remote", "get-url", remote, check=False)
    url = (process.stdout or "").strip()
    if process.returncode != 0 or not url:
        raise WipPushError("no remote named '{}'; pass -r/--remote <name>".format(remote))
    return url


def uncommitted_files():
    output = git("status", "--porcelain", "--untracked-files=normal", check=False).stdout or ""
    return [line for line in output.splitlines() if line.strip()]


def upstream_of(branch):
    process = git(
        "rev-parse", "--abbrev-ref", "--symbolic-full-name", branch + "@{upstream}", check=False
    )
    name = (process.stdout or "").strip()
    return name if process.returncode == 0 and name and name != branch else None


def commits_to_send(branch, upstream):
    if upstream is None:
        return "no upstream configured"
    count = git_output("rev-list", "--count", branch, "^" + upstream)
    return "{} commit(s) beyond {}".format(count, upstream)


def describe(branch, remote):
    print("WIP channel: {} -> {}".format(branch, remote))
    print("  tip:  {}".format(git_output("log", "-1", "--format=%h %s", branch)))
    print("  send: {}".format(commits_to_send(branch, upstream_of(branch))))
    dirty = uncommitted_files()
    if dirty:
        print("  note: {} uncommitted path(s) stay local; only commits are pushed:".format(len(dirty)))
        for line in dirty[:10]:
            print("        {}".format(line))
        if len(dirty) > 10:
            print("        ... and {} more".format(len(dirty) - 10))


def run_push(command):
    print("")
    print("$ git " + " ".join(command))
    process = git(*command, check=False)
    output = (process.stdout or "").rstrip()
    if output:
        print(output)
    if process.returncode != 0:
        if "remote ref does not exist" in output:
            raise NothingToDelete("the remote branch does not exist")
        if "rejected" in output or "non-fast-forward" in output or "fetch first" in output:
            raise WipPushError(
                "push rejected: the remote branch has commits you do not have.\n"
                "  A wip branch may be rewritten: rerun with -f/--force."
            )
        raise WipPushError("git push failed (exit {})".format(process.returncode))
    return output


def publish(branch, remote, force, dry_run):
    tip = git("rev-parse", "--verify", "--quiet", branch + "^{commit}", check=False)
    if tip.returncode != 0:
        raise WipPushError(
            "branch '{}' has no commit yet; commit the unfinished work first - the checklist is "
            "skipped on wip branches".format(branch)
        )
    describe(branch, remote)
    command = ["push"]
    if force:
        # --force-with-lease, not --force: rewriting your own backup branch keeps the ergonomics
        # while still refusing to clobber a concurrent update someone else pushed there.
        command.append("--force-with-lease")
    if dry_run:
        command.append("--dry-run")
    command.extend(["-u", remote, branch])
    output = run_push(command)
    if dry_run:
        print("")
        print("Dry run: nothing was published.")
        return
    if "Everything up-to-date" in output:
        print("")
        print("Already up to date: the remote branch has this commit; nothing was published.")
        return
    print("")
    print("Published {}. WIP channel rules: no local gate marker, no CI job, no pull request,".format(branch))
    print("and nothing on this branch can be merged as-is. When the work becomes reviewable,")
    print("squash it onto a feature branch, run scripts/commit.ps1, then scripts/push.ps1 and open a PR.")


def delete_remote(branch, remote, dry_run):
    print("WIP channel: deleting {} from {}".format(branch, remote))
    command = ["push", "--delete"]
    if dry_run:
        command.append("--dry-run")
    command.extend([remote, branch])
    try:
        run_push(command)
    except NothingToDelete:
        print("")
        print("No remote branch {} on {}; nothing to delete.".format(branch, remote))
        return
    if dry_run:
        print("")
        print("Dry run: nothing was deleted.")
        return
    print("")
    print("Deleted the remote branch {}. The wip channel keeps only what is still in use.".format(branch))


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="push_wip.py",
        description="Publish an unfinished branch over the WIP channel (no local gate, no CI, never merged).",
        epilog=(
            "examples:\n"
            "  git switch -c wip/toon-experiment\n"
            "  git commit -am 'wip: try the second rim term'   # the checklist is skipped on wip/*\n"
            "  python scripts/push_wip.py\n"
            "  python scripts/push_wip.py -f                   # rewrite the remote wip branch\n"
            "  python scripts/push_wip.py --delete -b wip/toon-experiment\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("-b", "--branch", help="branch to publish (default: the current branch)")
    parser.add_argument("-r", "--remote", default=DEFAULT_REMOTE, metavar="NAME",
                        help="remote to push to (default: %(default)s)")
    parser.add_argument("-f", "--force", action="store_true",
                        help="rewrite the remote wip branch (git push --force-with-lease)")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="run git push --dry-run and publish nothing")
    parser.add_argument("--delete", action="store_true",
                        help="delete the remote wip branch instead of pushing")
    args = parser.parse_args(argv)

    try:
        repository_check()
        branch = target_branch(args.branch, args.delete)
        validate_channel(branch)
        remote_check(args.remote)
        if args.delete:
            delete_remote(branch, args.remote, args.dry_run)
        else:
            publish(branch, args.remote, args.force, args.dry_run)
    except WipPushError as error:
        print("push_wip: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
