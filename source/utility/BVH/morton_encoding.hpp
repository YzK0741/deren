#pragma once

// Private to BVH and its encoder test. No Boost type is exported by deren.utility.
#include <boost/int128/int128.hpp>
#include <array>
#include <cstdint>

namespace deren::utility::bvh_detail {
    namespace morton_detail {
        using wide = boost::int128::uint128;

        template <unsigned Block>
        constexpr wide spread_mask() noexcept {
            static_assert(Block == 1 || Block == 2 || Block == 4 || Block == 8 || Block == 16);
            wide mask = 0;
            for (unsigned group = 0; group < 32 / Block; ++group) {
                mask |= ((wide{1} << Block) - wide{1}) << (3 * Block * group);
            }
            return mask;
        }

        constexpr wide spread(std::uint32_t const input) noexcept {
            wide value = input;
            value = (value | (value << 32)) & spread_mask<16>();
            value = (value | (value << 16)) & spread_mask<8>();
            value = (value | (value << 8)) & spread_mask<4>();
            value = (value | (value << 4)) & spread_mask<2>();
            value = (value | (value << 2)) & spread_mask<1>();
            return value;
        }
    }

    // Preserve all 32 bits per axis and the existing little-endian 96-bit byte representation.
    constexpr std::array<std::uint8_t, 12> encode_morton_bytes(std::uint32_t const x, std::uint32_t const y,
                                                            std::uint32_t const z) noexcept {
        auto const value = morton_detail::spread(x) | (morton_detail::spread(y) << 1) | (morton_detail::spread(z) << 2);
        std::array<std::uint8_t, 12> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
        return bytes;
    }
}
