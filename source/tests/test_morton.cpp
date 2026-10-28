#include "../utility/BVH/morton_encoding.hpp"
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace {
    using bytes = std::array<std::uint8_t, 12>;
    using point = std::array<std::uint32_t, 3>;

    constexpr bytes reference(point const p) {
        bytes result{};
        for (unsigned bit = 0; bit < 32; ++bit) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                unsigned const output = 3 * bit + axis;
                result[output / 8] |= std::uint8_t(((p[axis] >> bit) & 1u) << (output % 8));
            }
        }
        return result;
    }

    constexpr auto encode(point const p) {
        return deren::utility::bvh_detail::encode_morton_bytes(p[0], p[1], p[2]);
    }

    static_assert(encode({1, 0, 0})[0] == 1);
    static_assert(encode({0, 1, 0})[0] == 2);
    static_assert(encode({0, 0, 1})[0] == 4);
    static_assert(encode({0xffffffffu, 0xffffffffu, 0xffffffffu}) == reference({0xffffffffu, 0xffffffffu, 0xffffffffu}));

    void compare(point const p) {
        CHECK(encode(p) == reference(p));
    }
} // namespace

int main() {
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            point p{};
            p[axis] = std::uint32_t{1} << bit;
            compare(p);
        }
    }
    for (std::uint32_t x : {0u, 1u, 255u, 256u, (1u << 21) - 1, 1u << 21, 0xffffffffu}) {
        for (std::uint32_t y : {0u, 1u, 0xffffffffu}) {
            for (std::uint32_t z : {0u, 1u, 0xffffffffu})
                compare({x, y, z});
        }
    }
    std::mt19937 random(0x4d6f7274);
    std::vector<bytes> expected, actual;
    for (unsigned i = 0; i < 100000; ++i) {
        point p{static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random())};
        if ((i & 1u) != 0)
            for (auto& axis : p)
                axis &= (1u << 21) - 1;
        compare(p);
        if (i < 10000) {
            expected.push_back(reference(p));
            actual.push_back(encode(p));
        }
    }
    // The public BVH stores little-endian bytes and sorts them lexicographically, not numerically.
    std::ranges::sort(expected);
    std::ranges::sort(actual);
    CHECK(actual == expected);
    CHECK(encode({1, 0, 0}) > encode({0, 0, 4})); // low byte is compared before high bytes
    return deren::vk_test::finish("test_morton");
}
