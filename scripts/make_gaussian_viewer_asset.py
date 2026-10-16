"""Write a small deterministic Gaussian-splat PLY for VIEWER verification.

WHY A GENERATOR AND NOT A CHECKED-IN ASSET: the viewer path has to be provable without a download, and the
acceptance asset the design names (Bouquet of Flowers) is large and lives outside the repository. This
produces a few thousand splats whose shape and colours are recognisable at a glance - a hue-graded shell
around a warm core - so a screenshot either shows our asset or shows nothing, and two runs produce byte
identical output, which is what makes "did the camera move?" comparable between frames.

The dialect is the one `source/tests/test_gaussian_loader.cpp` exercises and `deren.gaussian::load_ply`
accepts: an ASCII PLY whose vertex element carries x/y/z, the three LOG-SCALE axes, a quaternion, a LOGIT
opacity and the three SH DC coefficients. In that encoding exp(scale) is the splat's standard deviation and
sigmoid(opacity) its alpha; the colour a renderer shows is 0.5 + 0.28209479177387814 * f_dc (the SH DC term),
which is why the coefficients below are written as that inversion rather than as raw colours.
"""

from __future__ import annotations

import argparse
import math
import random

SH_DC = 0.28209479177387814


def logit(alpha: float) -> float:
    return math.log(alpha / (1.0 - alpha))


def dc_for(channel: float) -> float:
    return (channel - 0.5) / SH_DC


def write_ply(path: str, shell: int, core: int, seed: int) -> tuple[int, float]:
    rng = random.Random(seed)
    rows: list[str] = []

    # The shell: a hue-graded sphere so a screenshot shows orientation and colour, with per-splat jitter so the
    # cloud is a cloud rather than a mesh.
    for index in range(shell):
        # A Fibonacci sphere keeps the coverage even and deterministic.
        t = (index + 0.5) / shell
        inclination = math.acos(1.0 - 2.0 * t)
        azimuth = math.pi * (1.0 + 5.0**0.5) * index
        x = math.sin(inclination) * math.cos(azimuth)
        y = math.cos(inclination)
        z = math.sin(inclination) * math.sin(azimuth)
        x += rng.uniform(-0.05, 0.05)
        y += rng.uniform(-0.05, 0.05)
        z += rng.uniform(-0.05, 0.05)
        # Hue from the azimuth, value from the height: red at the bottom through green at the top.
        hue = (azimuth / (2.0 * math.pi)) % 1.0
        value = 0.35 + 0.5 * (y * 0.5 + 0.5)
        red, green, blue = hsv_to_rgb(hue, 0.75, value)
        sigma = rng.uniform(0.02, 0.045)
        rows.append(
            f"{x:.6f} {y:.6f} {z:.6f} "
            f"{math.log(sigma):.6f} {math.log(sigma):.6f} {math.log(sigma):.6f} "
            f"1.0 0.0 0.0 0.0 {logit(0.85):.6f} "
            f"{dc_for(red):.6f} {dc_for(green):.6f} {dc_for(blue):.6f}"
        )

    # The core: warmer, wider splats that read as a solid mass behind the shell.
    for _ in range(core):
        x = rng.gauss(0.0, 0.22)
        y = rng.gauss(0.0, 0.22)
        z = rng.gauss(0.0, 0.22)
        sigma = rng.uniform(0.06, 0.12)
        warm = rng.uniform(0.55, 0.95)
        rows.append(
            f"{x:.6f} {y:.6f} {z:.6f} "
            f"{math.log(sigma):.6f} {math.log(sigma):.6f} {math.log(sigma):.6f} "
            f"1.0 0.0 0.0 0.0 {logit(0.9):.6f} "
            f"{dc_for(warm):.6f} {dc_for(warm * 0.45):.6f} {dc_for(warm * 0.15):.6f}"
        )

    header = (
        "ply\n"
        "format ascii 1.0\n"
        f"element vertex {len(rows)}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float scale_0\n"
        "property float scale_1\n"
        "property float scale_2\n"
        "property float rot_0\n"
        "property float rot_1\n"
        "property float rot_2\n"
        "property float rot_3\n"
        "property float opacity\n"
        "property float f_dc_0\n"
        "property float f_dc_1\n"
        "property float f_dc_2\n"
        "end_header\n"
    )
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(header)
        handle.write("\n".join(rows))
        handle.write("\n")
    total = len(rows)
    return total, total / 1000.0


def hsv_to_rgb(hue: float, saturation: float, value: float) -> tuple[float, float, float]:
    i = int(hue * 6.0)
    f = hue * 6.0 - i
    p = value * (1.0 - saturation)
    q = value * (1.0 - f * saturation)
    t = value * (1.0 - (1.0 - f) * saturation)
    return [
        (value, t, p),
        (q, value, p),
        (p, value, t),
        (p, q, value),
        (t, p, value),
        (value, p, q),
    ][i % 6]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="PLY path to write")
    parser.add_argument("--shell", type=int, default=3000, help="splats on the hue-graded shell")
    parser.add_argument("--core", type=int, default=1000, help="splats in the warm core")
    parser.add_argument("--seed", type=int, default=20261, help="fixed seed (the output is deterministic)")
    args = parser.parse_args()
    count, _ = write_ply(args.output, args.shell, args.core, args.seed)
    print(f"wrote {args.output}: {count} splats")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
