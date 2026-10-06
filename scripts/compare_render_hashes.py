#!/usr/bin/env python3
"""compare_render_hashes.py - THE RENDER GATE'S REAL INSTRUMENT: the fourteen ACTUAL hashes.

WHY THIS EXISTS, AND WHY IT IS NOT `check_render.ps1`'s EXIT CODE
    `scripts/windows/check_render.ps1` renders a fixed set of scenarios and compares each one against a
    reference captured earlier ON THIS MACHINE. Its own verdict is therefore about those references -
    and this repository's local reference set is a 2026-10-01 set, so `CHANGED` is the NORMAL answer and
    the script exits 1 on a perfectly correct tree. The instrument that means something is the actual
    per-scenario hash it prints, compared against:

      * the OTHER TREE, while two build trees existed (the migration's development phase): the legacy
        tree was the CONTROL, and the dynamic tree had to reproduce its fourteen hashes byte for byte
        (matched 14 / mismatched 0 was the acceptance); or
      * the FROZEN list, once only one configuration is left (S5, the flip): the fourteen values in
        DYNAMIC_LINK_IMPLEMENTATION.md section 1, which every batch of the migration has reproduced.

    Keeping the comparison in a script - rather than in a human's eyes - is the point: "the hashes look
    right" is exactly the claim this repository does not accept.

USAGE
    # both trees still exist (the two-tree phase): compare two gate outputs
    python scripts/compare_render_hashes.py build-release-clang64/render-legacy.txt \\
                                              --against build-release-clang64/render-dyn.txt

    # one configuration (S5 onwards): compare one gate output against the frozen list
    python scripts/compare_render_hashes.py build-release-dyn-clang64/render-check/gate-output.txt --frozen

    # a standard gate run can ask check_render.ps1 to do this itself:
    pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare-frozen

    EXIT: 0 when all fourteen matched, 1 otherwise (a missing scenario counts as a mismatch: a scenario
    that did not run must never read as "unchanged").
"""
from __future__ import annotations

import argparse
import re
import sys

# THE FROZEN FOURTEEN (DYNAMIC_LINK_IMPLEMENTATION.md section 1, the reference set the whole migration
# was gated against). They are also what `check_render.ps1`'s own references were built from, so a tree
# that matches the frozen list matches the control tree the migration used.
FROZEN = {
    "deferred": "972A31EC5FF55C87",
    "deferred_taa_fxaa": "4021B16AFDB2F43E",
    "deferred_ssao_off": "BFE3A472FBAB0B5E",
    "shadow_single": "A92C5965316679F3",
    "unlit": "F3C2D7FEFDAD864F",
    "transparent_blend": "CC7F77F93487AA5E",
    "sponza": "50AF7E46CC1E2A92",
    "metal_rough_glossy": "A1AFBFB61DBFD104",
    "glossy_motion": "9F31E89BE38B771C",
    "deformation": "723569BA0D03640C",
    "laevatain_goo_toon": "CF5A34D8DF6B6FFC",
    "laevatain_goo_toon_body": "C3365CEEB8AD3723",
    "laevatain_no_sidecar": "E9A2983BEB57D5C5",
    "laevatain_old_chain": "190EB09D3E9FDCDA",
}

SCENARIO_RE = re.compile(r"^===\s+(\S+)\s+\(")
HASH_RE = re.compile(r"\b([0-9A-F]{16})\b")
# A scenario the harness did not render: recorded as a mismatch, never as "absent from both sides" (a
# scenario that stopped running must not read as unchanged).
FAILURE_PREFIXES = ("FAIL", "FLAKY", "NOT SEEDED", "ERROR")


def parse(path: str) -> dict[str, str]:
    """The (scenario -> actual hash) map of one gate round.

    TWO INPUT SHAPES, ONE PARSER: `check_render.ps1`'s stdout (a scenario header, then `ok (HASH)` /
    `CHANGED: HASH vs reference ...`), and the machine-readable `render_hashes.txt` the gate writes next
    to the screenshots (the same header, then `actual  (HASH)`). A FAIL / FLAKY line is kept as its text,
    so it compares unequal to any hash.
    """
    found: dict[str, str] = {}
    current: str | None = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = SCENARIO_RE.match(line)
            if match:
                current = match.group(1)
                continue
            if current is None:
                continue
            stripped = line.strip()
            if stripped.startswith(FAILURE_PREFIXES):
                found[current] = "FAIL: " + stripped
                current = None
                continue
            hash_match = HASH_RE.search(line)
            if hash_match:
                found[current] = hash_match.group(1)
                current = None
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="a check_render.ps1 output, captured from the tree being verified")
    parser.add_argument("--against", default="", metavar="OTHER",
                        help="the OTHER tree's gate output to compare against (the two-tree phase)")
    parser.add_argument("--frozen", action="store_true",
                        help="compare against the FROZEN fourteen (the one-configuration phase)")
    parser.add_argument("--quiet", action="store_true", help="only the verdict")
    parser.add_argument("--expect", type=int, default=14, metavar="N",
                        help="how many scenarios this round was SUPPOSED to render (default 14, the frozen "
                             "set). A round that ran fewer is not a pass: a scenario that silently stopped "
                             "running would otherwise read as 'no change'. `check_render.ps1 -Compare` passes "
                             "the number of scenarios its selection actually attempted.")
    args = parser.parse_args()

    if args.against and args.frozen:
        print("compare_render_hashes: --against and --frozen are alternatives", file=sys.stderr)
        return 2
    if not args.against and not args.frozen:
        # the default is the frozen list: it is the instrument the single tree can still be held to
        args.frozen = True

    actual = parse(args.input)
    reference = parse(args.against) if args.against else dict(FROZEN)
    label = f"against {args.against}" if args.against else "against the frozen fourteen"

    matched = mismatched = 0
    if not args.quiet:
        print(f"{'scenario':24} {'actual':18} {'reference':18} verdict")
    for name in sorted(set(reference) | set(actual)):
        want = reference.get(name, "(absent)")
        got = actual.get(name, "(absent)")
        ok = got == want
        matched += 1 if ok else 0
        mismatched += 0 if ok else 1
        if not args.quiet:
            print(f"{name:24} {got:18} {want:18} {'matched' if ok else 'MISMATCHED'}")

    print(f"\n{label}: matched {matched}, mismatched {mismatched}, total {matched + mismatched}")
    # `--expect` IS THE CONTRACT OF THE ROUND: a selection that rendered fewer scenarios than it should
    # have is not a pass, because a scenario that silently stopped running would otherwise read as "no
    # change" - the same failure mode `check_render.ps1` refuses an empty `-Only` selection for.
    if mismatched != 0 or matched != args.expect:
        print(f"compare_render_hashes: FAIL (expected {args.expect} matched scenarios, got {matched})", file=sys.stderr)
        return 1
    print(f"compare_render_hashes: PASS ({matched} of {args.expect})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
