"""Supplementary tight-alternation A/B for the E2 regimes where the main matrix's GPU clock drift dominates.

The main E1/E2 matrix is round-major: each configuration's OFF and TRIANGLE runs are separated by the other
configurations of that round (~2-4 minutes apart), and the measured GPU raster time drifted by up to ~25% over the
session. This supplementary pass runs each key as three ADJACENT OFF/TRIANGLE pairs with the pair order alternating
(off-first, on-first, off-first), so the pair delta is measured within ~30-60 s. It is evidence about measurement
stability, not a replacement for the three-round matrix.

usage: python scripts/measure/measure_software_headroom_abba.py --work <fresh> --asset <package>.gltf
"""
import argparse
import json
from pathlib import Path

from measure_software_headroom import make_config, environment, run_one, sha256, shader_bundle_sha256

# The two extremes of the measured subpixel share plus the reference configuration.
KEYS = (("640x360-far", "640x360", "far"),
        ("1280x720-far", "1280x720", "far"),
        ("1920x1080-near", "1920x1080", "near"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--exe", type=Path, default=Path("build-release-dyn-clang64/deren.exe"))
    parser.add_argument("--frames", type=int, default=130)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert [(pair, key) for pair in (1, 2, 3) for key, _, _ in KEYS][:3] == [(1, "640x360-far"), (1, "1280x720-far"), (1, "1920x1080-near")]
        print("headroom tight-alternation plan self-test PASS")
        return 0
    work, asset, exe = args.work.resolve(), args.asset.resolve(), args.exe.resolve()
    for name, path in (("asset", asset), ("executable", exe)):
        if not path.is_file():
            parser.error(f"{name} does not exist: {path}")
    if args.frames < 130:
        parser.error("frames must be at least 130")
    work.mkdir(parents=True, exist_ok=False)
    result = {"complete": False, "frames": args.frames, "executable_sha256": sha256(exe),
              "asset_sha256": sha256(asset), "shader_bundle_sha256": shader_bundle_sha256(exe.parent / "shaders"),
              "pairs": [], "runs": []}
    output = work / "results.json"
    for pair in (1, 2, 3):
        for key, resolution, camera in KEYS:
            order = ("off", "triangles") if pair != 2 else ("triangles", "off")
            pair_row = {"pair": pair, "key": key, "order": list(order)}
            for mode in order:
                row = run_one(exe, asset, work, key, resolution, camera, mode, pair, args.frames)
                result["runs"].append(row)
                pair_row[mode] = row
            off = pair_row["off"]["gpu_ms"]
            on = pair_row["triangles"]["gpu_ms"]
            pair_row["gpu_delta_ms"] = on - off
            pair_row["visibility_raster_delta_ms"] = (pair_row["triangles"]["visibility_raster_ms"]
                                                      - pair_row["off"]["visibility_raster_ms"])
            pair_row["software_pass_ms"] = pair_row["triangles"]["software_visibility_ms"]
            pair_row["software_merge_ms"] = pair_row["triangles"]["software_merge_ms"]
            pair_row["share_vs_hzb_survivors"] = (pair_row["triangles"]["software_clusters_per_frame"]
                                                  / pair_row["triangles"]["hzb_survivors_per_culling_frame"])
            result["pairs"].append(pair_row)
            output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if sha256(exe) != result["executable_sha256"] or shader_bundle_sha256(exe.parent / "shaders") != result["shader_bundle_sha256"]:
        raise SystemExit("executable or shader bundle changed during the tight-alternation pass")
    failures = [row["tag"] for row in result["runs"] if not row["passed"]]
    if failures:
        raise SystemExit(f"failed runs: {failures}")
    result["complete"] = True
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for pair_row in result["pairs"]:
        print(f"pair {pair_row['pair']} {pair_row['key']} order={pair_row['order']} "
              f"gpu_delta={pair_row['gpu_delta_ms']:+.3f} raster_delta={pair_row['visibility_raster_delta_ms']:+.3f} "
              f"share={pair_row['share_vs_hzb_survivors']:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
