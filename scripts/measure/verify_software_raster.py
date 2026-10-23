#!/usr/bin/env python3
"""Validate software-raster GPU probe dumps against the CPU reference.

Each capture directory contains manifest.json and little-endian uint32 image files.
Depth files store IEEE-754 float bits. The manifest schema is intentionally strict:
incomplete captures must fail instead of producing partial PASS results.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile

MAGIC = "DEREN_SOFTWARE_RASTER_PROBE"
ID_VALID = 1 << 31
ID_TRIANGLE_MASK = 0x7F
ID_MESHLET_MASK = 0xFFF
ID_ITEM_MASK = 0xFFF


def fail(message: str) -> None:
    raise ValueError(message)


def read_words(path: Path, expected: int | None = None) -> list[int]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        fail(f"{path}: cannot read ({exc})")
    if len(data) % 4:
        fail(f"{path}: byte length is not a multiple of four")
    if expected is not None and len(data) != expected * 4:
        fail(f"{path}: expected {expected} words, got {len(data) // 4}")
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def write_words(path: Path, words: list[int]) -> None:
    path.write_bytes(struct.pack(f"<{len(words)}I", *words))


def load_capture(root: Path) -> tuple[dict, dict[str, list[int]]]:
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"{root}: missing or invalid manifest.json ({exc})")
    if not isinstance(manifest, dict) or manifest.get("format") != MAGIC or manifest.get("version") != 1:
        fail(f"{root}: unsupported probe format/version")
    required = {"extent", "csr", "images", "pass_order"}
    if not required.issubset(manifest):
        fail(f"{root}: manifest missing {sorted(required - manifest.keys())}")
    extent = manifest["extent"]
    if not isinstance(extent, dict) or not all(type(extent.get(k)) is int and extent[k] > 0 for k in ("width", "height")):
        fail(f"{root}: invalid extent")
    pixel_count = extent["width"] * extent["height"]
    csr = manifest["csr"]
    if not isinstance(csr, dict):
        fail(f"{root}: invalid csr section")
    tiles_x, tiles_y = csr.get("tiles_x"), csr.get("tiles_y")
    if type(tiles_x) is not int or type(tiles_y) is not int or tiles_x < 1 or tiles_y < 1:
        fail(f"{root}: invalid CSR tile dimensions")
    tile_count = tiles_x * tiles_y
    budget = csr.get("entry_budget")
    entry_count = csr.get("entry_count")
    if type(budget) is not int or type(entry_count) is not int or budget < 0 or not 0 <= entry_count <= budget:
        fail(f"{root}: invalid CSR entry budget/count")
    files = manifest["images"]
    names = ("cpu_software_depth", "cpu_software_id", "gpu_software_depth", "gpu_software_id",
             "hardware_depth", "hardware_id", "resolved_depth", "resolved_id", "merged_depth")
    if not isinstance(files, dict) or any(not isinstance(files.get(name), str) for name in names):
        fail(f"{root}: missing image file references")
    words = {name: read_words(root / files[name], pixel_count) for name in names}

    counts = read_words(root / csr.get("counts", ""), tile_count)
    offsets = read_words(root / csr.get("offsets", ""), tile_count + 1)
    entries = read_words(root / csr.get("entries", ""), entry_count * 2)
    if offsets[0] != 0 or offsets[-1] != entry_count or any(a > b for a, b in zip(offsets, offsets[1:])):
        fail(f"{root}: CSR offsets are not monotonic or do not span entries")
    if any(offsets[i + 1] - offsets[i] != counts[i] for i in range(tile_count)):
        fail(f"{root}: CSR counts disagree with offsets")
    if any(value > entry_count for value in offsets):
        fail(f"{root}: CSR offset exceeds entry_count")
    order = manifest["pass_order"]
    expected_order = ["visibility raster", "software visibility", "visibility resolve", "software merge"]
    if order != expected_order:
        fail(f"{root}: pass order must be {expected_order}")
    return manifest, words


def depth(word: int, label: str) -> float:
    value = struct.unpack("<f", struct.pack("<I", word))[0]
    if not math.isfinite(value) or not 0.0 <= value <= 1.0:
        fail(f"{label}: depth is not finite normalized [0,1]")
    return value


def validate(root: Path) -> dict[str, int | str]:
    manifest, images = load_capture(root)
    if images["cpu_software_depth"] != images["gpu_software_depth"]:
        fail(f"{root}: CPU/GPU software depth differs")
    if images["cpu_software_id"] != images["gpu_software_id"]:
        fail(f"{root}: CPU/GPU software ID differs")
    wins = 0
    for i, (hw_d, hw_id, sw_d, sw_id, _resolved_depth, _resolved_id, merged) in enumerate(zip(
        images["hardware_depth"], images["hardware_id"], images["gpu_software_depth"],
        images["gpu_software_id"], images["resolved_depth"], images["resolved_id"], images["merged_depth"],
    )):
        hw_valid, sw_valid = bool(hw_id & ID_VALID), bool(sw_id & ID_VALID)
        h = depth(hw_d, f"pixel {i} hardware")
        # Empty software pixels use the shader's uint sentinel, not float depth.
        if not sw_valid and sw_d not in (0xFFFFFFFF, 0x3F800000):
            fail(f"{root}: pixel {i} invalid software empty depth")
        s = depth(sw_d, f"pixel {i} software") if sw_valid else 1.0
        rd = depth(images["resolved_depth"][i], f"pixel {i} resolved")
        m = depth(merged, f"pixel {i} merged")
        sw_wins_resolve = sw_valid and (not hw_valid or s < h)
        expected_id = sw_id if sw_wins_resolve else hw_id
        expected_resolved_depth = s if sw_wins_resolve else h
        if images["resolved_id"][i] != expected_id or rd != expected_resolved_depth:
            fail(f"{root}: pixel {i} resolve did not select the nearest valid candidate (ties favor hardware)")
        if sw_valid:
            # Decode each packed field using the same masks as mesh_fetch.slang. The
            # verifier has no table lengths, so range validation belongs to the probe.
            _triangle = (sw_id >> 0) & ID_TRIANGLE_MASK
            _meshlet = (sw_id >> 7) & ID_MESHLET_MASK
            _item = (sw_id >> 19) & ID_ITEM_MASK
            wins += int(sw_wins_resolve)
        expected = min(h, s) if sw_valid else h
        if m != expected:
            fail(f"{root}: pixel {i} merged depth is not min(hardware, software candidate)")
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file() and p.name != "manifest.json"):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(path.read_bytes())
    return {"pixels": len(images["gpu_software_id"]), "software_winners": wins, "sha256": digest.hexdigest()}


def self_test() -> None:
    def make_capture(root: Path, *, truncate: bool = False, bad_order: bool = False) -> None:
        root.mkdir()
        fbits = lambda v: struct.unpack("<I", struct.pack("<f", v))[0]
        depth_values = [fbits(0.7), fbits(1.0)]
        ids = [ID_VALID | (1 << 19) | (1 << 7), 0]
        write_words(root / "cpu_sw_depth.u32", [fbits(0.4), fbits(1.0)])
        write_words(root / "cpu_sw_id.u32", [ids[0], 0])
        write_words(root / "gpu_sw_depth.u32", [fbits(0.4), fbits(1.0)])
        write_words(root / "gpu_sw_id.u32", [ids[0], 0])
        write_words(root / "hw_depth.u32", depth_values)
        write_words(root / "hw_id.u32", [ID_VALID | (2 << 19), 0])
        write_words(root / "resolved_depth.u32", [fbits(0.4), fbits(1.0)])
        write_words(root / "resolved_id.u32", [ids[0], 0])
        write_words(root / "merged_depth.u32", [fbits(0.4), fbits(1.0)])
        if truncate:
            (root / "gpu_sw_id.u32").write_bytes(b"\0")
        manifest = {"format": MAGIC, "version": 1, "extent": {"width": 2, "height": 1},
                    "csr": {"tiles_x": 2, "tiles_y": 1, "entry_budget": 4, "entry_count": 1,
                            "counts": "counts.u32", "offsets": "offsets.u32", "entries": "entries.u32"},
                    "images": {"cpu_software_depth": "cpu_sw_depth.u32", "cpu_software_id": "cpu_sw_id.u32",
                               "gpu_software_depth": "gpu_sw_depth.u32", "gpu_software_id": "gpu_sw_id.u32",
                               "hardware_depth": "hw_depth.u32", "hardware_id": "hw_id.u32",
                               "resolved_depth": "resolved_depth.u32", "resolved_id": "resolved_id.u32",
                               "merged_depth": "merged_depth.u32"},
                    "pass_order": ["visibility raster", "software visibility", "visibility resolve", "software merge"]}
        if bad_order:
            manifest["pass_order"] = list(reversed(manifest["pass_order"]))
        (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        write_words(root / "counts.u32", [1, 0])
        write_words(root / "offsets.u32", [0, 1, 1])
        write_words(root / "entries.u32", [0, 0])

    scratch = Path(__file__).resolve().parents[2] / ".superpowers/sdd/2026-10-22-software-raster"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="verify-", dir=scratch) as temporary:
        base = Path(temporary)
        first, second = base / "first", base / "second"
        make_capture(first)
        make_capture(second)
        a, b = validate(first), validate(second)
        assert a["software_winners"] == 1 and a["sha256"] == b["sha256"]
        for name, kwargs in (("truncated", {"truncate": True}), ("wrong-order", {"bad_order": True})):
            path = base / name
            make_capture(path, **kwargs)
            try:
                validate(path)
            except ValueError:
                pass
            else:
                raise AssertionError(f"self-test accepted {name} capture")
        # Corrupt one stage at a time. A verifier that silently ignores any of
        # these buffers must not qualify a real GPU capture as accepted.
        mutations = {
            "gpu-depth": ("gpu_sw_depth.u32", [0, 0x3F800000]),
            "gpu-id": ("gpu_sw_id.u32", [0, 0]),
            "resolve-id": ("resolved_id.u32", [0, 0]),
            "resolve-depth": ("resolved_depth.u32", [0, 0x3F800000]),
            "merge-depth": ("merged_depth.u32", [0, 0x3F800000]),
            "nan-hardware-depth": ("hw_depth.u32", [0x7FC00000, 0x3F800000]),
            "csr-count": ("counts.u32", [0, 0]),
            "csr-offset": ("offsets.u32", [0, 2, 1]),
        }
        for name, (filename, values) in mutations.items():
            path = base / name
            make_capture(path)
            write_words(path / filename, values)
            try:
                validate(path)
            except ValueError:
                pass
            else:
                raise AssertionError(f"self-test accepted {name} corruption")
    print("software raster verifier self-test: PASS (valid, deterministic, truncated, pass-order, eight stage corruptions)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path)
    parser.add_argument("--compare", type=Path, help="second capture; hashes must match")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
            return 0
        if args.capture is None:
            parser.error("capture directory is required unless --self-test is used")
        result = validate(args.capture)
        if args.compare:
            other = validate(args.compare)
            if result["sha256"] != other["sha256"]:
                fail("capture hashes differ; run is nondeterministic")
        print(f"software raster capture: PASS pixels={result['pixels']} software_winners={result['software_winners']} sha256={result['sha256']}")
        return 0
    except (OSError, ValueError, AssertionError) as exc:
        print(f"software raster capture: FAIL {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
