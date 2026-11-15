"""Execute production Slang sampling math in the CPU math tests.

Like generate_vulkan_query_fixture.py, extract the implementation verbatim so
the regression does not test a second copy of the algorithm.
"""
import argparse
from pathlib import Path

from generate_vulkan_query_fixture import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    shader = (args.root / "source/shaders/lighting/rt_reflection.slang").read_text(encoding="utf-8")
    sampling = extract(shader, "float3 reflection_ggx_direction(")
    header = """#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
namespace render_math_fixture {
using float2 = glm::vec2;
using float3 = glm::vec3;
using glm::normalize;
using glm::cross;
using glm::dot;
using glm::reflect;
using std::sqrt;
using std::cos;
using std::sin;
using std::abs;
inline float max(float a, float b) { return std::max(a, b); }
""" + sampling + """}
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf-8")


if __name__ == "__main__":
    main()
