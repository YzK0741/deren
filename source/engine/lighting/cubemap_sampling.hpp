#pragma once
#include <cmath>
#include <cstdint>

// Private scalar math shared by the CPU bake and the optional CUDA experiment.
// Consumers: source/tests/test_math.cpp and tools/ibl_cuda_spike/filter.cu.
// This is not a test-only header; keep it under engine/lighting so the CUDA tool
// does not depend on source/tests.
// No CUDA types, allocations, RHI dependencies or public module exports.
// Preconditions: size > 0, finite direction, and fetch(face,x,y) reads an in-range texel.
// Border helpers receive only the one-texel bilinear border [-1,size].
#ifdef __CUDACC__
#define DEREN_CUBE_HD __host__ __device__
#else
#define DEREN_CUBE_HD
#endif

namespace deren::engine::environment_detail {
    struct cube_vector {
        float x, y, z;
    };
    struct cube_location {
        int32_t face;
        float u, v;
    };
    DEREN_CUBE_HD inline int32_t cube_clamp_index(int32_t value, int32_t size) {
        if (value < 0) {
            return 0;
        }
        if (value >= size) {
            return size - 1;
        }
        return value;
    }
    DEREN_CUBE_HD inline cube_vector cube_mix(cube_vector a, cube_vector b, float t) {
        return {a.x * (1 - t) + b.x * t, a.y * (1 - t) + b.y * t, a.z * (1 - t) + b.z * t};
    }
    // Unnormalized direction: only ratios are used by the inverse mapping.
    DEREN_CUBE_HD inline cube_vector cube_direction(int32_t face, float u, float v) {
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
        float const ax = ::fabsf(d.x);
        float const ay = ::fabsf(d.y);
        float const az = ::fabsf(d.z);
        float major;
        cube_location p;
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
    DEREN_CUBE_HD cube_vector cube_edge_texel(Fetch const& fetch, int32_t size, int32_t face, int32_t x, int32_t y) {
        auto [cube_face, u, v] = cube_lookup(cube_direction(face, (static_cast<float>(x) + .5f) / static_cast<float>(size) * 2 - 1, (static_cast<float>(y) + .5f) / static_cast<float>(size) * 2 - 1));
        int32_t nx = cube_clamp_index(static_cast<int32_t>((u * .5f + .5f) * static_cast<float>(size)), size);
        int32_t ny = cube_clamp_index(static_cast<int32_t>((v * .5f + .5f) * static_cast<float>(size)), size);
        return fetch(cube_face, nx, ny);
    }
    template <class Fetch>
    DEREN_CUBE_HD cube_vector cube_border_texel(Fetch const& fetch, int32_t size, int32_t face, int32_t x, int32_t y) {
        bool outside_x = x < 0 || x >= size;
        bool outside_y = y < 0 || y >= size;
        if (!outside_x && !outside_y) {
            return fetch(face, x, y);
        }
        if (outside_x && outside_y) {
            // No unique diagonal neighbor at a cube corner. Average the three incident corner texels.
            // https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-cubemapedge
            int32_t cx = cube_clamp_index(x, size);
            int32_t cy = cube_clamp_index(y, size);
            auto a = fetch(face, cx, cy);
            auto b = cube_edge_texel(fetch, size, face, x, cy);
            auto c = cube_edge_texel(fetch, size, face, cx, y);
            return {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
        }
        return cube_edge_texel(fetch, size, face, x, y);
    }
    template <class Fetch>
    DEREN_CUBE_HD cube_vector cube_sample_level(Fetch const& fetch, int32_t size, cube_vector direction) {
        auto p = cube_lookup(direction);
        float fx = (p.u * .5f + .5f) * static_cast<float>(size) - .5f;
        float fy = (p.v * .5f + .5f) * static_cast<float>(size) - .5f;
        int32_t x0 = static_cast<int32_t>(::floorf(fx));
        int32_t y0 = static_cast<int32_t>(::floorf(fy));
        float const dx = fx - static_cast<float>(x0);
        float const dy = fy - static_cast<float>(y0);
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < size && y0 + 1 < size) {
            return cube_mix(cube_mix(fetch(p.face, x0, y0), fetch(p.face, x0 + 1, y0), dx),
                            cube_mix(fetch(p.face, x0, y0 + 1), fetch(p.face, x0 + 1, y0 + 1), dx), dy);
        }
        return cube_mix(cube_mix(cube_border_texel(fetch, size, p.face, x0, y0), cube_border_texel(fetch, size, p.face, x0 + 1, y0), dx),
                        cube_mix(cube_border_texel(fetch, size, p.face, x0, y0 + 1), cube_border_texel(fetch, size, p.face, x0 + 1, y0 + 1), dx), dy);
    }
} // namespace deren::engine::environment_detail
#undef DEREN_CUBE_HD
