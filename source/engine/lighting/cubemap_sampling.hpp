#pragma once
#include <cmath>

// Private scalar math shared by the CPU bake and the optional CUDA experiment.
// No CUDA types, allocations, RHI dependencies or public module exports.
// Preconditions: size > 0, finite direction, and fetch(face,x,y) reads an in-range texel.
// Border helpers receive only the one-texel bilinear border [-1,size].
#if defined(__CUDACC__)
#define DEREN_CUBE_HD __host__ __device__
#else
#define DEREN_CUBE_HD
#endif

namespace deren::engine::environment_detail {
    struct cube_vector {
        float x, y, z;
    };
    struct cube_location {
        int face;
        float u, v;
    };
    DEREN_CUBE_HD inline int cube_clamp_index(int value, int size) {
        return value < 0 ? 0 : value >= size ? size - 1
                                             : value;
    }
    DEREN_CUBE_HD inline cube_vector cube_mix(cube_vector a, cube_vector b, float t) {
        return {a.x * (1 - t) + b.x * t, a.y * (1 - t) + b.y * t, a.z * (1 - t) + b.z * t};
    }
    // Unnormalized direction: only ratios are used by the inverse mapping.
    DEREN_CUBE_HD inline cube_vector cube_direction(int face, float u, float v) {
        switch (face) {
        case 0:
            return {1, -v, -u};
        case 1:
            return {-1, -v, u};
        case 2:
            return {u, 1, v};
        case 3:
            return {u, -1, -v};
        case 4:
            return {u, -v, 1};
        default:
            return {-u, -v, -1};
        }
    }
    DEREN_CUBE_HD inline cube_location cube_lookup(cube_vector d) {
        float ax = ::fabsf(d.x), ay = ::fabsf(d.y), az = ::fabsf(d.z), major;
        cube_location p{};
        if (ax >= ay && ax >= az) {
            p.face = d.x >= 0 ? 0 : 1;
            p.u = p.face == 0 ? -d.z : d.z;
            p.v = -d.y;
            major = ax;
        } else if (ay >= ax && ay >= az) {
            p.face = d.y >= 0 ? 2 : 3;
            p.u = d.x;
            p.v = p.face == 2 ? d.z : -d.z;
            major = ay;
        } else {
            p.face = d.z >= 0 ? 4 : 5;
            p.u = p.face == 4 ? d.x : -d.x;
            p.v = -d.y;
            major = az;
        }
        if (major > 0) {
            p.u /= major;
            p.v /= major;
        }
        return p;
    }
    template <class Fetch>
    DEREN_CUBE_HD cube_vector cube_edge_texel(Fetch const& fetch, int size, int face, int x, int y) {
        auto p = cube_lookup(cube_direction(face, (float(x) + .5f) / size * 2 - 1, (float(y) + .5f) / size * 2 - 1));
        int nx = cube_clamp_index(int((p.u * .5f + .5f) * size), size);
        int ny = cube_clamp_index(int((p.v * .5f + .5f) * size), size);
        return fetch(p.face, nx, ny);
    }
    template <class Fetch>
    DEREN_CUBE_HD cube_vector cube_border_texel(Fetch const& fetch, int size, int face, int x, int y) {
        bool outside_x = x < 0 || x >= size, outside_y = y < 0 || y >= size;
        if (!outside_x && !outside_y)
            return fetch(face, x, y);
        if (outside_x && outside_y) {
            // No unique diagonal neighbor at a cube corner. Average the three incident corner texels.
            // https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-cubemapedge
            int cx = cube_clamp_index(x, size), cy = cube_clamp_index(y, size);
            auto a = fetch(face, cx, cy), b = cube_edge_texel(fetch, size, face, x, cy), c = cube_edge_texel(fetch, size, face, cx, y);
            return {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
        }
        return cube_edge_texel(fetch, size, face, x, y);
    }
    template <class Fetch>
    DEREN_CUBE_HD cube_vector cube_sample_level(Fetch const& fetch, int size, cube_vector direction) {
        auto p = cube_lookup(direction);
        float fx = (p.u * .5f + .5f) * size - .5f, fy = (p.v * .5f + .5f) * size - .5f;
        int x0 = int(::floorf(fx)), y0 = int(::floorf(fy));
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < size && y0 + 1 < size) {
            return cube_mix(cube_mix(fetch(p.face, x0, y0), fetch(p.face, x0 + 1, y0), fx - x0),
                            cube_mix(fetch(p.face, x0, y0 + 1), fetch(p.face, x0 + 1, y0 + 1), fx - x0), fy - y0);
        }
        return cube_mix(cube_mix(cube_border_texel(fetch, size, p.face, x0, y0), cube_border_texel(fetch, size, p.face, x0 + 1, y0), fx - x0),
                        cube_mix(cube_border_texel(fetch, size, p.face, x0, y0 + 1), cube_border_texel(fetch, size, p.face, x0 + 1, y0 + 1), fx - x0), fy - y0);
    }
} // namespace deren::engine::environment_detail
#undef DEREN_CUBE_HD
