"""A/B statistics for the visibility buffer's ON/OFF arms (visibility-buffer v0.6).

Usage: python visibility_ab.py <off_dir> <on_dir>

Both directories hold the capture gate's per-scenario artefacts:
`<scenario>.last.png` (the frame the run kept) and `<scenario>.deren-debug.log`. Produce them with two -Full rounds
of scripts/windows/check_render.ps1, one plain (OFF) and one with `-ExtraConfig 'visibility_buffer = true'`
(ON); this instrument is what turns those two rounds into the numbers the review records.

Per scenario it prints: ADMITTED (with the item count the raster lane recorded) or the fallback reason the
runtime named, the count of validation findings in the ON log, and the ON-vs-OFF image difference - how many
pixels differ, the frame-level mean of the per-pixel maximum channel difference, the mean over the pixels that
did change, and the worst pixel. A scenario that FELL BACK should read 0 pixels changed (the whole frame
records the old lane); anything else means the arms took different paths by accident, which is the failure
mode this instrument is here to catch before the numbers are quoted.

The counts are computed on the per-pixel MAXIMUM channel difference (unlike scripts/measure/diff.py, whose
counts are green-only and documented as such); a multi-megabyte capture's pixels are all compared, so the
3-channel max is both the strictest and the cheapest single-channel answer to "did this pixel move".
"""

import os
import re
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the measured-machine check
    print("Pillow is required: python -c \"import PIL\" must work")
    raise SystemExit(2)

import numpy as np


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int32)


def admission(log_path):
    """(admitted, detail) read out of the ON run's own log lines.

    A session can carry BOTH lines: the switch's inputs are mirrored into the runtime every frame (the app's
    GUI mirror), so a frame before that mirror lands can record one lane and every later frame the other. The
    LAST occurrence is the lane the captured frame (frame 40) recorded through, which is what the picture
    comparison measures - so that is the answer this returns, with the earlier line named in the detail.
    """
    if not os.path.exists(log_path):
        return None, "no log"
    text = open(log_path, encoding="utf-8", errors="replace").read()
    recorded = text.rfind("visibility raster: recorded")
    fallback_at = text.rfind("visibility buffer: frame NOT admitted")
    recorded_at = -1 if recorded < 0 else recorded
    if fallback_at >= 0 and fallback_at > recorded_at:
        match = re.search(r"visibility buffer: frame NOT admitted \((.*?)\), so the geometry lane", text[fallback_at:], re.DOTALL)
        detail = match.group(1) if match else "frame refused"
        return False, detail + (" [after an earlier admitted frame]" if recorded_at >= 0 else "")
    if recorded_at >= 0:
        match = re.search(r"visibility raster: recorded (\d+) item", text[recorded_at:])
        return True, f"{match.group(1)} item(s)" if match else "items"
    return None, "neither lane logged"


def validation_findings(log_path):
    if not os.path.exists(log_path):
        return "?"
    text = open(log_path, encoding="utf-8", errors="replace").read()
    return text.count("VUID-") + len(re.findall(r"\[ERROR\]", text))


def diff_stats(off_path, on_path):
    a = load(off_path)
    b = load(on_path)
    if a.shape != b.shape:
        return f"SHAPE {a.shape} vs {b.shape}"
    d = np.abs(a - b).max(axis=2)
    changed = d > 0
    total = d.size
    mean_changed = float(d[changed].mean()) if changed.any() else 0.0
    return {
        "changed": int(changed.sum()),
        "changed_pct": 100.0 * changed.sum() / total,
        "mean": float(d.mean()),
        "mean_changed": mean_changed,
        "gt4_pct": 100.0 * (d > 4).sum() / total,
        "max": int(d.max()),
    }


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    off_dir, on_dir = argv[1], argv[2]
    names = sorted(
        os.path.splitext(f)[0][: -len(".last")] for f in os.listdir(off_dir) if f.endswith(".last.png")
    )
    rows = []
    for name in names:
        off_png = os.path.join(off_dir, f"{name}.last.png")
        on_png = os.path.join(on_dir, f"{name}.last.png")
        if not os.path.exists(on_png):
            rows.append((name, "MISSING ON CAPTURE", "?", None))
            continue
        admitted, detail = admission(os.path.join(on_dir, f"{name}.deren-debug.log"))
        findings = validation_findings(os.path.join(on_dir, f"{name}.deren-debug.log"))
        stats = diff_stats(off_png, on_png)
        rows.append((name, admitted, detail, findings, stats))

    print(f"{'scenario':26} {'lane':10} {'validation':10} {'pixels changed':>15} {'frame mean':>11} {'mean changed':>13} {'|d|>4':>7} {'max':>4}  note")
    for row in rows:
        name, admitted, detail, findings, stats = row
        if stats is None or isinstance(stats, str):
            print(f"{name:26} {'?':10} {str(findings):10} {str(stats or 'MISSING'):>15}")
            continue
        lane = "visibility" if admitted else ("geometry" if admitted is False else "?")
        print(
            f"{name:26} {lane:10} {str(findings):10} {stats['changed_pct']:14.2f}% {stats['mean']:11.3f} "
            f"{stats['mean_changed']:13.3f} {stats['gt4_pct']:6.2f}% {stats['max']:4d}  {detail}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
