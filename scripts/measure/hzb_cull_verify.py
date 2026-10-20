#!/usr/bin/env python3
"""hzb_cull_verify.py - independent consistency checks over the HZB cull probe's dumps (M3 phase B1).

WHAT THIS CHECKS, AND WHAT IT DELIBERATELY DOES NOT. The design's §6 DECISION comparison (shader vs the committed
CPU reference in source/engine/visibility/hzb.cppm) is done IN THE ENGINE by the cull lane's probe: it re-runs
`hzb_test_sphere` over the dumped history data and logs "hzb cull verify: ...". That comparison is not repeated
here - a Python reimplementation of the conservative math would be a THIRD spelling of the contract, and a
disagreement between it and the two committed ones would say nothing about the GPU.

What is left for this script is the arithmetic that must hold whatever the decisions are, because a violation is a
bug no picture can show:
  - the flags/counts/survivor tables are SELF-CONSISTENT: survivors == candidates - culled, the per-item counts sum
    to the survivor total, the survivor list of an item is the item's kept candidates in ASCENDING candidate order
    and each survivor record is byte-identical to the candidate record it names;
  - the stats block matches the tables (candidates/culled/survivors, and candidates == culled + survivors);
  - the metadata says WHICH history lane and camera the dump describes (the frame slot's PREVIOUS use, 2 frames
    old) and the history pyramid/header files agree with it;
  - the candidate records are plausible meshlet records and the draw-item model matrices are finite.

USAGE
    python scripts/measure/hzb_cull_verify.py build-release-dyn-clang64/bistro-probe/hzbcull-far
    python scripts/measure/hzb_cull_verify.py <prefix> --quiet
The prefix is the value DEREN_HZB_CULL_PROBE_CAPTURE was set to; the files are <prefix>.*.bin + <prefix>.json.
Exit code 0 = every check passed, 1 = a check failed (the failing lines name the item/candidate), 2 = a file/shape
problem (a wrong path or a truncated dump - not a culling bug).
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# ---- the host-side layout constants this script mirrors (whose single source is deren.engine.pass.hzb_cull and
#      deren.engine.visibility; they are MIRRORED here rather than imported because the script has no C++ compiler
#      in the loop - a drift is exactly what the shape checks below are for) -------------------------------------
ITEM_CAPACITY = 4096          # hzb_cull_item_capacity: one hzb_item per possible item per frame lane
CANDIDATE_CAPACITY = 65536    # hzb_cull_candidate_capacity: the meshlet table's budget, one flags word per candidate
ITEM_BYTES = 16               # hzb_item: {meshlet_base, candidate_count, source, pad}
MESHLET_BYTES = 48            # deren::engine::meshlet: window + sphere + normal cone
DRAW_ITEM_BYTES = 112         # deren::engine::visibility::draw_item (model at byte 48)
FLAGS_OFFSET = 0              # the per-frame word offsets of heap_slots_hzb_results
PASS1_OFFSET = CANDIDATE_CAPACITY
PASS2_OFFSET = PASS1_OFFSET + ITEM_CAPACITY
STATS_OFFSET = PASS2_OFFSET + ITEM_CAPACITY
STATS_CAPACITY = 16
RESULTS_WORDS = STATS_OFFSET + STATS_CAPACITY
STAT_CANDIDATES = 0
STAT_CULLED = 1
STAT_SURVIVORS = 2
STAT_OVERFLOW_ITEMS = 3
SOURCE_UNCULLED = 0
SOURCE_CULLED = 1
HZB_HEADER_MAGIC = 0x485A4201  # "HZB", version 1 - common/hzb.slang's hzb_magic
HZB_CULL_PYRAMID_MAGIC = 0x485A4342  # "HZCB" - the cull probe's HISTORY pyramid file magic
HEADER_BYTES = 256
HEADER_WORD_VIEW = 4
HEADER_WORD_PROJ = 20
HEADER_WORD_CAMERA_POS = 36


class DumpError(RuntimeError):
    """A file is missing/short/shaped wrong: not a culling bug, a dump problem."""


class Checker:
    def __init__(self, quiet: bool) -> None:
        self.quiet = quiet
        self.failures: list[str] = []
        self.checks = 0

    def check(self, condition: bool, message: str) -> bool:
        self.checks += 1
        if not condition:
            self.failures.append(message)
            print(f"FAIL {message}")
        elif not self.quiet:
            print(f"ok   {message}")
        return condition

    def finish(self, what: str) -> int:
        if self.failures:
            print(f"\n{what}: {len(self.failures)} check(s) FAILED")
            return 1
        print(f"\n{what}: PASS ({self.checks} check(s))")
        return 0


def read_bytes(prefix: Path, suffix: str) -> bytes:
    path = Path(f"{prefix}{suffix}")
    if not path.is_file():
        raise DumpError(f"missing dump file: {path}")
    return path.read_bytes()


def parse_items(data: bytes) -> list[tuple[int, int, int, int]]:
    """The dumped items file is ONE frame lane (the probe copies the slot's lane), so entry 0 is the file's start."""
    end = ITEM_CAPACITY * ITEM_BYTES
    if len(data) < end:
        raise DumpError(f"items file is {len(data)} B, expected at least {end}")
    entries = []
    for index in range(ITEM_CAPACITY):
        entries.append(struct.unpack_from("<IIII", data, index * ITEM_BYTES))
    return entries


def parse_u32s(data: bytes, count: int, offset_bytes: int = 0) -> list[int]:
    need = offset_bytes + count * 4
    if len(data) < need:
        raise DumpError(f"table is {len(data)} B, expected at least {need}")
    return list(struct.unpack_from(f"<{count}I", data, offset_bytes))


def pyramid_words(width: int, height: int) -> tuple[int, int]:
    """The level count and total word count, from deren.engine.visibility.hzb's formula (mirrored, not imported)."""
    if width == 0 or height == 0:
        return 0, 0
    total = 0
    levels = 0
    w, h = width, height
    while True:
        total += w * h
        levels += 1
        if w == 1 and h == 1:
            break
        w = (w + 1) // 2
        h = (h + 1) // 2
    return levels, total


def parse_header(words: list[int]) -> dict[str, object]:
    def floats(base: int, count: int) -> list[float]:
        return list(struct.unpack_from(f"<{count}f", struct.pack(f"<{count}I", *words[base:base + count])))

    return {
        "magic": words[0],
        "width": words[1],
        "height": words[2],
        "levels": words[3],
        "view": floats(HEADER_WORD_VIEW, 16),
        "proj": floats(HEADER_WORD_PROJ, 16),
        "position": floats(HEADER_WORD_CAMERA_POS, 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix", help="the DEREN_HZB_CULL_PROBE_CAPTURE prefix (the dump files are <prefix>.*)")
    parser.add_argument("--quiet", action="store_true", help="print only failures and the final verdict")
    args = parser.parse_args()
    prefix = Path(args.prefix)
    checker = Checker(args.quiet)

    try:
        metadata = json.loads(read_bytes(prefix, ".json").decode("utf-8"))
    except DumpError as error:
        print(f"error: {error}")
        return 2
    except json.JSONDecodeError as error:
        print(f"error: {prefix}.json is not valid JSON: {error}")
        return 2

    # ---- 1. THE METADATA NAMES THE HISTORY LANE AND THE SHAPES ----
    checker.check(metadata.get("kind") == "hzb-cull-probe", "the dump is a cull probe (metadata kind)")
    extent = metadata.get("extent", {})
    width, height = int(extent.get("width", 0)), int(extent.get("height", 0))
    levels, words = pyramid_words(width, height)
    checker.check(width > 0 and height > 0, f"extent is non-degenerate ({width}x{height})")
    checker.check(int(metadata.get("level_count", -1)) == levels, f"metadata level count matches the extent ({levels})")
    checker.check(int(metadata.get("words_per_lane", -1)) == words, f"metadata words per lane matches the extent ({words})")
    frame_slot = int(metadata.get("frame_slot", -1))
    item_count = int(metadata.get("item_count", -1))
    candidate_total = int(metadata.get("candidate_total", -1))
    checker.check(0 <= frame_slot < 2, f"frame slot is a valid ring slot ({frame_slot})")
    checker.check(0 < item_count <= ITEM_CAPACITY, f"item count is a dispatch the pass can record ({item_count})")
    checker.check(0 <= candidate_total <= CANDIDATE_CAPACITY, f"candidate total fits the flags table ({candidate_total})")
    history = metadata.get("history", {})
    checker.check("HISTORY" in str(metadata.get("note", "")) or "history" in str(metadata.get("note", "")),
                  "the metadata says the dump describes the HISTORY lane (the previous use of the frame slot)")
    checker.check(int(history.get("frame_slot", -1)) == frame_slot, "the history lane is the same frame slot")
    checker.check(isinstance(history.get("header_matches_saved_snapshot"), bool),
                  "the metadata records whether the history header matched the gate's saved snapshot")

    # ---- 2. THE HISTORY PYRAMID AND HEADER FILES ----
    try:
        pyramid_raw = read_bytes(prefix, ".history.pyramid.bin")
        header_raw = read_bytes(prefix, ".history.header.bin")
        items_raw = read_bytes(prefix, ".items.bin")
        flags_raw = read_bytes(prefix, ".flags.bin")
        counts_raw = read_bytes(prefix, ".counts.bin")
        stats_raw = read_bytes(prefix, ".stats.bin")
        survivors_raw = read_bytes(prefix, ".survivors.bin")
        candidates_raw = read_bytes(prefix, ".candidates.bin")
        draw_items_raw = read_bytes(prefix, ".draw_items.bin")
    except DumpError as error:
        print(f"error: {error}")
        return 2
    checker.check(len(pyramid_raw) == 16 + words * 4, f"history pyramid file holds the 4-word header + {words} word(s)")
    if len(pyramid_raw) >= 16:
        magic, pyramid_width, pyramid_height, pyramid_levels = struct.unpack_from("<4I", pyramid_raw, 0)
        checker.check(magic == HZB_CULL_PYRAMID_MAGIC, "history pyramid file magic")
        checker.check((pyramid_width, pyramid_height, pyramid_levels) == (width, height, levels),
                      "history pyramid file header matches the extent/levels")
    checker.check(len(header_raw) == HEADER_BYTES, f"history header file is {HEADER_BYTES} B")
    header_words = parse_u32s(header_raw, HEADER_BYTES // 4)
    header = parse_header(header_words)
    checker.check(header["magic"] == HZB_HEADER_MAGIC, "history header MAGIC (the GPU table's own magic)")
    checker.check((header["width"], header["height"], header["levels"]) == (width, height, levels),
                  "history header extent/level count match the metadata")
    checker.check(int(history.get("header_magic", -1)) == header["magic"], "metadata header magic matches the dumped header")
    checker.check(all(math.isfinite(v) for v in header["view"]), "history camera view is finite")
    checker.check(all(math.isfinite(v) for v in header["proj"]), "history camera projection is finite")
    checker.check(all(math.isfinite(v) for v in header["position"]), "history camera position is finite")

    # ---- 3. THE TABLES ----
    checker.check(len(items_raw) == ITEM_CAPACITY * ITEM_BYTES, f"items file is {ITEM_CAPACITY} x {ITEM_BYTES} B")
    checker.check(len(flags_raw) == CANDIDATE_CAPACITY * 4, f"flags file is {CANDIDATE_CAPACITY} u32")
    checker.check(len(counts_raw) == ITEM_CAPACITY * 4, f"counts file is {ITEM_CAPACITY} u32")
    checker.check(len(stats_raw) == STATS_CAPACITY * 4, f"stats file is {STATS_CAPACITY} u32")
    checker.check(len(survivors_raw) == CANDIDATE_CAPACITY * MESHLET_BYTES, f"survivors file is {CANDIDATE_CAPACITY} x {MESHLET_BYTES} B")
    checker.check(len(candidates_raw) == CANDIDATE_CAPACITY * MESHLET_BYTES, f"candidates file is {CANDIDATE_CAPACITY} x {MESHLET_BYTES} B")
    checker.check(len(draw_items_raw) == ITEM_CAPACITY * DRAW_ITEM_BYTES, f"draw-items file is {ITEM_CAPACITY} x {DRAW_ITEM_BYTES} B")

    items = parse_items(items_raw)
    flags = parse_u32s(flags_raw, CANDIDATE_CAPACITY)
    counts = parse_u32s(counts_raw, ITEM_CAPACITY)
    stats = parse_u32s(stats_raw, STATS_CAPACITY)

    # ---- 4. THE ITEM TABLE DESCRIBES THE PLAN THE METADATA NAMES ----
    planned = 0
    pad_bad = 0
    for index, (meshlet_base, count, source, pad) in enumerate(items):
        if source not in (SOURCE_UNCULLED, SOURCE_CULLED):
            checker.check(False, f"item {index}: source {source} is not one of the two sources")
            break
        if pad != 0:
            pad_bad += 1
        if source == SOURCE_UNCULLED:
            checker.check(count == 0, f"item {index}: an unculled-source item offers no candidates")
        else:
            planned += count
            checker.check(count <= ITEM_CAPACITY and meshlet_base <= CANDIDATE_CAPACITY and count <= CANDIDATE_CAPACITY - meshlet_base,
                          f"item {index}: candidate run [{meshlet_base}, +{count}) is addressable")
    checker.check(pad_bad == 0, f"every item entry's pad word is zero ({pad_bad} bad entry(ies))")
    checker.check(planned == candidate_total, f"the plan's candidate total matches the metadata ({planned} == {candidate_total})")

    # ---- 5. FLAGS, COUNTS AND SURVIVORS ARE SELF-CONSISTENT ----
    # The flags lane holds one word per CANDIDATE SLOT of the meshlet index space; the slots no run of this frame
    # names are never written (the table is host-zeroed at creation), so the only flags a check may read are the
    # ones inside an item's run. `culled_total` therefore counts flags==1, and the candidate survivors are the
    # plan's total minus that.
    culled_total = sum(1 for flag in flags if flag != 0)
    counts_total = 0
    order_problems = 0
    item_problems = 0
    # the dumped survivors file is ONE frame lane (the probe copies the slot's lane) - like the items file
    survivor_lane = 0
    for index, (meshlet_base, count, source, _) in enumerate(items):
        if source != SOURCE_CULLED or count == 0:
            if counts[index] != 0:
                item_problems += 1
            continue
        kept = [meshlet_base + offset for offset in range(count) if flags[meshlet_base + offset] == 0]
        counts_total += counts[index]
        if counts[index] != len(kept):
            item_problems += 1
            if item_problems <= 8:
                print(f"FAIL item {index}: pass1_count {counts[index]} != the item's kept candidates {len(kept)}")
        for order, candidate in enumerate(kept):
            survivor_bytes = survivors_raw[(survivor_lane + meshlet_base + order) * MESHLET_BYTES:
                                           (survivor_lane + meshlet_base + order + 1) * MESHLET_BYTES]
            candidate_bytes = candidates_raw[candidate * MESHLET_BYTES:(candidate + 1) * MESHLET_BYTES]
            if survivor_bytes != candidate_bytes:
                order_problems += 1
                if order_problems <= 8:
                    print(f"FAIL item {index}: survivor {order} is not candidate {candidate} (the ascending compaction contract)")
    checker.check(counts_total == planned - culled_total,
                  f"the per-item counts sum to the candidates minus the culled ({counts_total} == {planned} - {culled_total})")
    checker.check(item_problems == 0, f"every per-item count equals its item's kept candidates ({item_problems} mismatch(es))")
    checker.check(order_problems == 0, f"the survivor list is the ascending compaction of the kept candidates ({order_problems} bad record(s))")

    # ---- 6. THE STATS BLOCK MATCHES THE TABLES (spec §4.3/§10.3) ----
    checker.check(stats[STAT_CANDIDATES] == planned, f"stats candidates == the plan's total ({stats[STAT_CANDIDATES]})")
    checker.check(stats[STAT_CULLED] == culled_total, f"stats culled == the flags' culled count ({culled_total})")
    expected_survivors = sum(
        len([offset for offset in range(count) if flags[meshlet_base + offset] == 0])
        for meshlet_base, count, source, _ in items if source == SOURCE_CULLED and count != 0)
    checker.check(stats[STAT_SURVIVORS] == expected_survivors, f"stats survivors == the survivors of the candidate runs ({expected_survivors})")
    checker.check(stats[STAT_CANDIDATES] == stats[STAT_CULLED] + stats[STAT_SURVIVORS],
                  "stats candidates == culled + survivors")
    checker.check(stats[STAT_OVERFLOW_ITEMS] == 0, f"no item overflowed its run ({stats[STAT_OVERFLOW_ITEMS]} overflow item(s))")

    # ---- 7. THE CANDIDATE RECORDS AND THE MODEL MATRICES ARE PLAUSIBLE ----
    # The 48-byte meshlet record's layout (deren::engine::meshlet): first_index@0, index_count@4, base_vertex@8,
    # _pad@12, center@16..27, radius@28, axis@32..43, cone_cos@44. A run of all-zero records is what an
    # unpopulated lane looks like, so it is checked against as a finding rather than accepted as geometry.
    bad_records = 0
    bad_models = 0
    for meshlet_base, count, source, _ in items:
        if source != SOURCE_CULLED or count == 0:
            continue
        for offset in range(count):
            index = meshlet_base + offset
            record_base = index * MESHLET_BYTES
            first_index, index_count, base_vertex, _pad = struct.unpack_from("<III I".replace(" ", ""), candidates_raw, record_base)
            radius = struct.unpack_from("<f", candidates_raw, record_base + 28)[0]
            if (first_index, index_count, base_vertex) == (0, 0, 0):
                bad_records += 1
            elif index_count == 0 or index_count % 3 != 0 or not math.isfinite(radius) or radius < 0.0:
                bad_records += 1
    checker.check(bad_records == 0, f"every candidate record is a plausible meshlet ({bad_records} bad record(s))")
    for index in range(item_count):
        model = struct.unpack_from("<16f", draw_items_raw, index * DRAW_ITEM_BYTES + 48)
        if not all(math.isfinite(value) for value in model):
            bad_models += 1
    checker.check(bad_models == 0, f"every item's model matrix is finite ({bad_models} bad item(s))")

    # ---- 8. THE IN-ENGINE VERIFICATION'S OWN RESULT, CROSS-CHECKED ----
    verify = metadata.get("verify", {})
    decisions = int(verify.get("decisions", -1))
    agree = int(verify.get("agree", -1))
    order_mismatches = int(verify.get("survivor_order_mismatches", -1))
    count_mismatches = int(verify.get("count_mismatches", -1))
    checker.check(decisions == planned, f"the in-engine verify tested every candidate ({decisions} == {planned})")
    checker.check(agree == decisions, f"the in-engine CPU-reference verify agreed on every decision ({agree}/{decisions})")
    checker.check(order_mismatches == 0, f"the in-engine verify found no survivor-order mismatch ({order_mismatches})")
    checker.check(count_mismatches == 0, f"the in-engine verify found no per-item count mismatch ({count_mismatches})")

    return checker.finish(f"hzb_cull_verify {prefix}")


if __name__ == "__main__":
    sys.exit(main())
