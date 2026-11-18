#include "vk_test.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

import deren.engine.visibility.progressive_texture;
import deren.engine.visibility.texture_cache;

int main() {
    namespace p = deren::engine::progressive_texture;
    std::array<std::uint8_t, 8> black_white{0, 0, 0, 0, 255, 255, 255, 255};
    auto linear = p::fallback(black_white, 2, 1, false, 1);
    auto srgb = p::fallback(black_white, 2, 1, true, 1);
    CHECK(linear && linear->width == 1 && linear->height == 1);
    CHECK(linear && linear->bytes[0] == 128 && linear->bytes[3] == 128);
    CHECK(srgb && srgb->bytes[0] == 188 && srgb->bytes[3] == 128);
    std::array<std::uint8_t, 60> npot{};
    npot[56] = 255;
    auto odd = p::fallback(npot, 5, 3, false, 2);
    CHECK(odd && odd->width == 2 && odd->height == 1);
    CHECK(odd && odd->bytes[4] > 0); // right/bottom source edge must contribute
    CHECK(!p::fallback(black_white, 3, 1, false, 1));
    CHECK(!p::fallback(black_white, 0, 1, false, 1));
    CHECK(!p::fallback(black_white, 2, 1, false, 0));
    auto unchanged = p::fallback(black_white, 2, 1, true, 128);
    CHECK(unchanged && unchanged->bytes == std::vector<std::uint8_t>(black_white.begin(), black_white.end()));

    // Desired textures precede recent ones; within a class prefer already resident content.
    std::array<p::candidate, 4> candidates{
        {
            {.bytes = 60, .last_used = 99, .resident = true},
            {.bytes = 60, .last_used = 100, .wanted = true},
            {.bytes = 200, .last_used = 100, .wanted = true},
            {.bytes = 10, .last_used = 1, .resident = true},
        },
    };
    auto selected = p::select(candidates, 100, 100, 30);
    CHECK(!selected[0] && selected[1] && !selected[2] && !selected[3]);
    candidates[0].wanted = true;
    selected = p::select(candidates, 100, 100, 30);
    CHECK(selected[0] && !selected[1]);
    CHECK(!p::select(candidates, 100, 0, 30)[0]);
    CHECK(p::can_start(40, 60, 1, 100, 2));
    CHECK(!p::can_start(41, 60, 1, 100, 2));
    CHECK(!p::can_start(40, 60, 2, 100, 2));
    CHECK(!p::can_start(std::numeric_limits<std::uint64_t>::max(), 1, 0, 100, 2));
    // Mip suffixes must upload existing bytes without re-filtering or losing odd source edges.
    namespace rhi = deren::promise::rhi;
    CHECK(rhi::bytes_per_pixel(rhi::image_format::bc7_srgb) == 0);
    CHECK(rhi::image_mip_bytes(rhi::image_format::bc7_srgb, 5, 7) == 64);
    CHECK(rhi::image_mip_bytes(rhi::image_format::bc7_unorm, 1, 1) == 16);
    CHECK(rhi::image_mip_bytes(rhi::image_format::unknown, 4, 4) == 0);
    CHECK(rhi::image_mip_bytes(rhi::image_format::rgba8_unorm, UINT32_MAX, UINT32_MAX) == 0);
    auto bc7 = p::mip_range(9, 8, 4, 1, rhi::image_format::bc7_srgb);
    CHECK(bc7 && bc7->offset == 96 && bc7->bytes == 48 && bc7->width == 4);
    CHECK(p::tier_bytes(9, 8, 4, 0, rhi::image_format::bc7_srgb) == 144);
    CHECK(!p::tier_bytes(9, 8, 1, 1, rhi::image_format::bc7_srgb));
    namespace cache = deren::engine::texture_cache;
    CHECK(rhi::image_mip_bytes(rhi::image_format::bc5_unorm, 5, 7) == 64);
    CHECK(cache::normal_bc5_compatible(std::array<std::uint8_t, 4>{128, 128, 255, 255}));
    CHECK(!cache::normal_bc5_compatible(std::array<std::uint8_t, 4>{128, 128, 127, 255}));
    CHECK(!cache::normal_bc5_compatible({}));
    std::vector<std::uint8_t> dds(148 + 16);
    auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            dds[at + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    };
    put(0, 0x20534444u);
    put(4, 124);
    put(12, 1);
    put(16, 1);
    put(28, 1);
    put(76, 32);
    put(80, 4);
    put(84, 0x30315844u);
    put(128, 99);
    put(132, 3);
    put(140, 1);
    CHECK(cache::valid_dds(dds, 1, 1, 1, true));
    CHECK(!cache::valid_dds(dds, 1, 1, 1, false));
    put(128, 83);
    CHECK(cache::valid_dds(dds, 1, 1, 1, false, rhi::image_format::bc5_unorm));
    CHECK(!cache::valid_dds(dds, 1, 1, 1, false));
    put(128, 99);
    put(140, 2);
    CHECK(!cache::valid_dds(dds, 1, 1, 1, true));
    put(140, 1);
    dds.pop_back();
    CHECK(!cache::valid_dds(dds, 1, 1, 1, true));
    CHECK(!cache::valid_dds({}, 1, 1, 1, true));
    std::array<p::mip_candidate, 1> compressed_budget{{{.width = 8, .height = 8, .levels = 4, .floor = 3, .target = 0, .resident = 3, .last_used = 1, .wanted = true, .format = rhi::image_format::bc7_srgb}}};
    CHECK(p::select_mips(compressed_budget, 1, 112, 0)[0] == 0);
    auto suffix = p::mip_range(9, 8, 4, 1);
    CHECK(suffix && suffix->width == 4 && suffix->height == 4 && suffix->levels == 3);
    CHECK(suffix && suffix->offset == 288 && suffix->bytes == 84);
    CHECK(!p::mip_range(0, 8, 4, 1));
    CHECK(!p::mip_range(9, 8, 4, 4));
    CHECK(p::target_mip(1024, 1024, 300) == 1);
    CHECK(p::target_mip(1024, 1024, 256) == 2);
    CHECK(p::target_mip(1024, 1024, 0) == 0); // missing footprint is conservative
    CHECK(p::floor_mip(1024, 1024) == 3);
    CHECK(p::tier_bytes(1024, 1024, 11, 2) == 349524);
    CHECK(p::tier_bytes(1024, 1024, 1, 2) == 262144);
    auto generated = p::resample(black_white, 2, 1, 1, 1, true);
    CHECK(generated && generated->bytes[0] == 188);
    CHECK(!p::resample(black_white, 2, 1, 3, 1, false));

    // With one full image's budget, improve both textures before giving all bytes to one.
    std::array<p::mip_candidate, 2> tiers{
        {
            {.width = 512, .height = 512, .levels = 1, .floor = 2, .target = 0, .resident = 2, .last_used = 10, .wanted = true},
            {.width = 512, .height = 512, .levels = 1, .floor = 2, .target = 0, .resident = 2, .last_used = 10, .wanted = true},
        },
    };
    auto levels = p::select_mips(tiers, 10, 1024 * 1024, 30);
    CHECK(levels[0] == 1 && levels[1] == 1);
    levels = p::select_mips(tiers, 10, 0, 30);
    CHECK(levels[0] == 2 && levels[1] == 2);
    tiers[0].wanted = false;
    tiers[0].last_used = 1;
    tiers[0].resident = 1;
    levels = p::select_mips(tiers, 100, 262144, 30);
    CHECK(levels[0] == 2 && levels[1] == 1);
    tiers[0].wanted = true;
    tiers[0].resident = 1;
    tiers[1].resident = 2;
    std::array<std::uint32_t, 2> selected_tiers{0, 0};
    auto order = p::promotion_order(tiers, selected_tiers);
    CHECK(order.size() == 2 && order[0] == 1 && order[1] == 0);
    return deren::vk_test::finish("test_progressive_texture");
}
