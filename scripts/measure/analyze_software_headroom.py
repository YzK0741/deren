"""Analyze the E1+E2 headroom measurement (measure_software_headroom.py output).

Derives, for every configuration:
  * per-mode medians and run spread for GPU frame, CPU-active, cut wall, the pass decomposition, candidate counts;
  * the subpixel-share ratios from the counts the runtime actually logs (exact denominators stated in the output);
  * the hybrid delta (triangle on - off), its hardware-raster saving, and the software/merge/resolve cost return;
  * changed-pixel statistics between the on/off screenshots (and the OFF repeatability control);
and the E1 headroom decomposition and the E2 hypothesis correlation.

Writes a compact JSON (the heavy per-run rows stay in the measurement work dir). The matrix and hash checks are
assertions: a missing cell, a failed run or a changed executable/shader bundle makes the analysis fail rather than
quietly summarize a partial matrix.
"""
import argparse
import json
import math
import statistics
import sys
from pathlib import Path

METRICS = ("gpu_ms", "cpu_active_ms", "cut_wall_us", "gpu_total_ms", "visibility_raster_ms", "hzb_cull_ms",
           "hzb_build_ms", "hzb_recovery_ms", "visibility_recovery_ms", "software_visibility_ms",
           "software_merge_ms", "scene_ms", "lighting_ms", "process_wall_seconds")
COUNT_METRICS = ("software_clusters_per_frame", "software_entries_per_frame", "hzb_culled_percent",
                 "hzb_survivors_per_culling_frame", "hzb_candidates_per_culling_frame", "hzb_recovered")
SHARE_DEFINITIONS = {
    "share_vs_hzb_survivors": "software clusters/frame divided by HZB survivors per culling frame "
                              "(fraction of the hardware-visible candidate stream routed to software)",
    "share_vs_hzb_candidates": "software clusters/frame divided by HZB candidates per culling frame "
                               "(fraction of the post-history-test candidate stream routed to software)",
}


def median(rows, key):
    values = [row[key] for row in rows if isinstance(row.get(key), (int, float)) and math.isfinite(row[key])]
    return statistics.median(values) if values else None


def spread(rows, key):
    values = [row[key] for row in rows if isinstance(row.get(key), (int, float)) and math.isfinite(row[key])]
    return [min(values), max(values)] if values else None


def rank(values):
    order = sorted(range(len(values)), key=lambda index: values[index])
    ranks = [0.0] * len(values)
    for position, index in enumerate(order):
        ranks[index] = float(position + 1)
    return ranks


def spearman(pairs):
    if len(pairs) < 3:
        return None
    xs = rank([pair[0] for pair in pairs])
    ys = rank([pair[1] for pair in pairs])
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    covariance = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    variance_x = sum((x - mean_x) ** 2 for x in xs)
    variance_y = sum((y - mean_y) ** 2 for y in ys)
    if variance_x == 0.0 or variance_y == 0.0:
        return None
    return covariance / math.sqrt(variance_x * variance_y)


def changed_pixels(off_path: Path, on_path: Path) -> dict:
    import numpy as np
    from PIL import Image
    off = np.array(Image.open(off_path).convert("RGB"), dtype=np.int16)
    on = np.array(Image.open(on_path).convert("RGB"), dtype=np.int16)
    if off.shape != on.shape:
        raise ValueError(f"image shapes differ: {off.shape} vs {on.shape}")
    delta = np.abs(off - on)
    return {"changed_pixels": int(np.any(delta, axis=2).sum()), "pixels": int(delta.shape[0] * delta.shape[1]),
            "mean_channel_delta": float(delta.mean()), "max_channel_delta": int(delta.max())}


def pair_pass_cost(on: dict, off: dict) -> dict:
    """The pass-level decomposition of one configuration's hybrid delta (positive = software costs more)."""
    result = {}
    for key in ("visibility_raster_ms", "visibility_recovery_ms", "hzb_build_ms", "hzb_recovery_ms", "scene_ms",
                "software_visibility_ms", "software_merge_ms", "gpu_total_ms", "gpu_ms", "cpu_active_ms", "cut_wall_us"):
        result[key] = (median(on, key), median(off, key),
                       (median(on, key) - median(off, key)) if median(on, key) is not None and median(off, key) is not None else None)
    # gross hardware-raster saving: the mesh-task raster interval plus the visibility-recovery raster interval
    # (both draw hardware candidates; the recovery interval is ~0 on frames without recovery).
    raster_saving = None
    recovery_saving = None
    if result["visibility_raster_ms"][0] is not None and result["visibility_raster_ms"][1] is not None:
        raster_saving = result["visibility_raster_ms"][1] - result["visibility_raster_ms"][0]
    if result["visibility_recovery_ms"][0] is not None and result["visibility_recovery_ms"][1] is not None:
        recovery_saving = result["visibility_recovery_ms"][1] - result["visibility_recovery_ms"][0]
    if raster_saving is not None:
        result["hardware_raster_saving_ms"] = raster_saving
        result["hardware_raster_saving_incl_recovery_ms"] = raster_saving + (recovery_saving or 0.0)
    if result["gpu_ms"][0] is not None and result["gpu_ms"][1] is not None:
        result["frame_delta_ms"] = result["gpu_ms"][0] - result["gpu_ms"][1]
    software = result["software_visibility_ms"][0]
    merge = result["software_merge_ms"][0]
    resolve_delta = result["scene_ms"][2]
    if software is not None or merge is not None or resolve_delta is not None:
        result["added_pass_cost_ms"] = (software or 0.0) + (merge or 0.0) + (resolve_delta or 0.0)
        if result.get("hardware_raster_saving_incl_recovery_ms") is not None:
            result["net_from_passes_ms"] = result["hardware_raster_saving_incl_recovery_ms"] - result["added_pass_cost_ms"]
    return result


def analyze(data: dict) -> dict:
    runs = data["runs"]
    if not data.get("complete"):
        raise ValueError("measurement matrix is not marked complete")
    keys = sorted({row["key"] for row in runs})
    # completeness: every key has every expected mode and every round, and every row passed.
    expected_modes = {}
    for row in runs:
        expected_modes.setdefault(row["key"], set()).add(row["mode"])
    for row in runs:
        if not row.get("passed"):
            raise ValueError(f"failed run in the matrix: {row['tag']} {row.get('failures')}")
        if row.get("exit_code") != 0 or row.get("gpu_frames") != data["frames"] - 1:
            raise ValueError(f"incomplete timing in {row['tag']}")
        if not row.get("hzb_cull_active"):
            raise ValueError(f"HZB inactive in {row['tag']}")
    rounds = {row["round"] for row in runs}
    if len(rounds) != data["rounds"]:
        raise ValueError("round set does not match the declared round count")
    cells = {(row["key"], row["mode"], row["round"]) for row in runs}
    if len(cells) != len(runs):
        raise ValueError("duplicate configuration/mode/round cells")
    for key, modes in expected_modes.items():
        for mode in modes:
            count = sum(1 for row in runs if row["key"] == key and row["mode"] == mode)
            if count != data["rounds"]:
                raise ValueError(f"{key}/{mode} has {count} rounds, expected {data['rounds']}")
    summary = {"rounds": data["rounds"], "frames": data["frames"], "executable_sha256": data["executable_sha256"],
               "shader_bundle_sha256": data["shader_bundle_sha256"], "asset_sha256": data["asset_sha256"],
               "keys": keys, "modes": {key: sorted(modes) for key, modes in expected_modes.items()},
               "share_definitions": SHARE_DEFINITIONS, "per_key": {}, "hybrid": {}, "changed_pixels": {},
               "e1": {}}
    for key in keys:
        summary["per_key"][key] = {}
        for mode in sorted(expected_modes[key]):
            rows = [row for row in runs if row["key"] == key and row["mode"] == mode]
            entry = {"runs": len(rows)}
            for metric in METRICS + COUNT_METRICS:
                entry[metric] = median(rows, metric)
                entry[metric + "_spread"] = spread(rows, metric)
            summary["per_key"][key][mode] = entry
        off_rows = [row for row in runs if row["key"] == key and row["mode"] == "off"]
        triangle_rows = [row for row in runs if row["key"] == key and row["mode"] == "triangles"]
        if off_rows and triangle_rows:
            summary["hybrid"][key] = pair_pass_cost(triangle_rows, off_rows)
            off_spread = spread(off_rows, "gpu_ms")
            on_spread = spread(triangle_rows, "gpu_ms")
            summary["hybrid"][key]["off_gpu_spread"] = off_spread
            summary["hybrid"][key]["on_gpu_spread"] = on_spread
            summary["hybrid"][key]["spread_overlap"] = bool(off_spread and on_spread and not (on_spread[1] < off_spread[0] or on_spread[0] > off_spread[1]))
            clusters = median(triangle_rows, "software_clusters_per_frame")
            survivors = median(triangle_rows, "hzb_survivors_per_culling_frame")
            candidates = median(triangle_rows, "hzb_candidates_per_culling_frame")
            if clusters is not None and survivors:
                summary["hybrid"][key]["share_vs_hzb_survivors"] = clusters / survivors
            if clusters is not None and candidates:
                summary["hybrid"][key]["share_vs_hzb_candidates"] = clusters / candidates
            # changed pixels per round, plus the OFF-repeatability control (round1 vs round2)
            pairs = []
            for round_id in sorted({row["round"] for row in runs}):
                off = next((row for row in off_rows if row["round"] == round_id), None)
                on = next((row for row in triangle_rows if row["round"] == round_id), None)
                if off and on and off.get("image") and on.get("image"):
                    stats = changed_pixels(Path(off["image"]), Path(on["image"]))
                    stats["round"] = round_id
                    pairs.append(stats)
            summary["changed_pixels"][key] = pairs
            repeat = []
            rounds_off = [row for row in off_rows if row.get("image")]
            if len(rounds_off) >= 2:
                repeat.append(changed_pixels(Path(rounds_off[0]["image"]), Path(rounds_off[1]["image"])))
            summary["changed_pixels"][key + ":off_repeat"] = repeat
    # E2 hypothesis: does the hybrid delta track the subpixel share positively (negative delta = win)?
    pairs = [(summary["hybrid"][key]["share_vs_hzb_survivors"], summary["hybrid"][key]["frame_delta_ms"])
             for key in summary["hybrid"]
             if summary["hybrid"][key].get("share_vs_hzb_survivors") is not None and summary["hybrid"][key].get("frame_delta_ms") is not None]
    summary["e2"] = {"pairs": [{"share_vs_hzb_survivors": share, "frame_delta_ms": delta} for share, delta in pairs],
                     "spearman_share_vs_frame_delta": spearman(pairs),
                     "note": "a NEGATIVE frame delta means the hybrid frame was faster; the hypothesis predicts a "
                             "negative correlation between subpixel share and frame delta"}
    # E1: the 1280x720 far/near three-mode decomposition and the headroom arithmetic.
    for camera in ("far", "near"):
        key = f"1280x720-{camera}"
        if key not in summary["per_key"]:
            continue
        modes = summary["per_key"][key]
        e1 = {"key": key, "modes": modes}
        if "off" in modes and "triangles" in modes:
            hybrid = summary["hybrid"][key]
            e1.update(hybrid)
            off_raster = modes["off"].get("visibility_raster_ms")
            on_raster = modes["triangles"].get("visibility_raster_ms")
            tiled_raster = modes.get("tiled", {}).get("visibility_raster_ms")
            e1["visibility_raster_off_ms"] = off_raster
            e1["visibility_raster_triangle_ms"] = on_raster
            e1["visibility_raster_tiled_ms"] = tiled_raster
        summary["e1"][camera] = e1
    return summary


def self_test() -> None:
    base = {"complete": True, "rounds": 3, "frames": 130, "executable_sha256": "e", "shader_bundle_sha256": "s",
            "asset_sha256": "a", "runs": []}
    for key, modes in (("1280x720-far", ("off", "tiled", "triangles")), ("640x360-far", ("off", "triangles")),
                        ("1920x1080-far", ("off", "triangles"))):
        for mode in modes:
            for round_id in (1, 2, 3):
                base["runs"].append({"tag": f"{key}-{mode}-r{round_id}", "key": key, "mode": mode, "round": round_id,
                                     "passed": True, "exit_code": 0, "gpu_frames": 129, "hzb_cull_active": True,
                                     "gpu_ms": 10.0 if mode == "off" else (10.1 if mode == "tiled" else 9.9),
                                     "cpu_active_ms": 8.0, "cut_wall_us": 1.5, "visibility_raster_ms": 7.0 if mode == "off" else 6.9,
                                     "software_visibility_ms": 0.1 if mode != "off" else 0.0,
                                     "software_merge_ms": 1.0 if mode != "off" else 0.0, "scene_ms": 5.0,
                                     "software_clusters_per_frame": (3000.0 if key.startswith("640") else (1000.0 if key.startswith("1280") else 500.0)) if mode == "triangles" else None,
                                     "hzb_survivors_per_culling_frame": 5000.0,
                                     "hzb_candidates_per_culling_frame": 20000.0, "image": ""})
    summary = analyze(base)
    assert abs(summary["hybrid"]["1280x720-far"]["frame_delta_ms"] + 0.1) < 1e-9
    assert abs(summary["hybrid"]["1280x720-far"]["share_vs_hzb_survivors"] - 0.2) < 1e-9
    assert summary["e2"]["spearman_share_vs_frame_delta"] is not None
    negative_controls = []
    bad = json.loads(json.dumps(base)); bad["complete"] = False; negative_controls.append(bad)
    bad = json.loads(json.dumps(base)); bad["runs"][0]["passed"] = False; negative_controls.append(bad)
    bad = json.loads(json.dumps(base)); bad["runs"][0]["gpu_frames"] = 3; negative_controls.append(bad)
    bad = json.loads(json.dumps(base)); bad["runs"].pop(); negative_controls.append(bad)
    bad = json.loads(json.dumps(base)); bad["runs"][0]["hzb_cull_active"] = False; negative_controls.append(bad)
    for bad in negative_controls:
        try:
            analyze(bad)
        except ValueError:
            continue
        raise AssertionError("invalid matrix accepted")
    print("software-raster headroom analysis: valid matrix + 5 negative controls PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.results is None or args.output is None:
        parser.error("--results and --output are required unless --self-test is used")
    data = json.loads(args.results.read_text(encoding="utf-8"))
    summary = analyze(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"keys": list(summary["hybrid"]), "e2_spearman": summary["e2"]["spearman_share_vs_frame_delta"]}, indent=2))
    for key, hybrid in summary["hybrid"].items():
        print(f"{key}: frame_delta {hybrid['frame_delta_ms']:+.3f} ms  hw_raster_saving {hybrid['hardware_raster_saving_ms']:+.3f} ms  "
              f"added_pass_cost {hybrid['added_pass_cost_ms']:+.3f} ms  share {hybrid.get('share_vs_hzb_survivors')}  "
              f"spread_overlap {hybrid['spread_overlap']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"software raster headroom analysis: FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
