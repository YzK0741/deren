// Headless unit tests: the RHI image-format vocabulary =========================
// The contract's `image_format` numbers cross the C ABI, so they are facts rather than an implementation
// detail: a value may be APPENDED but never renumbered (see the enum's own note in rhi.api_core.cppm).
// The host byte size is the second half of the same vocabulary - a caller that receives image content
// strides rows with it - and `r32_uint` is the appended value the visibility buffer is created with.
#include "vk_test.h"

#include <cstdint>

import deren.promise.rhi;

namespace {
    using deren::promise::rhi::image_format;

    /// the numbers as written in the contract; an existing value moving is an ABI break, not a cleanup
    static_assert(static_cast<std::uint32_t>(image_format::unknown) == 0u);
    static_assert(static_cast<std::uint32_t>(image_format::rgba8_unorm) == 1u);
    static_assert(static_cast<std::uint32_t>(image_format::rgba8_srgb) == 2u);
    static_assert(static_cast<std::uint32_t>(image_format::bgra8_unorm) == 3u);
    static_assert(static_cast<std::uint32_t>(image_format::bgra8_srgb) == 4u);
    static_assert(static_cast<std::uint32_t>(image_format::r16g16_sfloat) == 5u);
    static_assert(static_cast<std::uint32_t>(image_format::r16g16b16a16_sfloat) == 6u);
    static_assert(static_cast<std::uint32_t>(image_format::r32g32b32_sfloat) == 7u);
    static_assert(static_cast<std::uint32_t>(image_format::depth) == 0x7FFFFFFFu);
    // the two appends, in the order they were added: high-bit values AFTER the depth role
    static_assert(static_cast<std::uint32_t>(image_format::r16_sfloat) == 0x80000000u);
    static_assert(static_cast<std::uint32_t>(image_format::r32_uint) == 0x80000001u);

    void test_byte_sizes() {
        using deren::promise::rhi::bytes_per_pixel;
        CHECK(bytes_per_pixel(image_format::rgba8_unorm) == 4u);
        CHECK(bytes_per_pixel(image_format::rgba8_srgb) == 4u);
        CHECK(bytes_per_pixel(image_format::bgra8_unorm) == 4u);
        CHECK(bytes_per_pixel(image_format::bgra8_srgb) == 4u);
        CHECK(bytes_per_pixel(image_format::r16g16_sfloat) == 4u);
        CHECK(bytes_per_pixel(image_format::r16g16b16a16_sfloat) == 8u);
        CHECK(bytes_per_pixel(image_format::r32g32b32_sfloat) == 12u);
        CHECK(bytes_per_pixel(image_format::r16_sfloat) == 2u);
        // the visibility buffer's one channel: a packed 32-bit id, so four bytes and no ambiguity
        CHECK(bytes_per_pixel(image_format::r32_uint) == 4u);
        // the two values the contract answers "no host spelling" for
        CHECK(bytes_per_pixel(image_format::depth) == 0u);
        CHECK(bytes_per_pixel(image_format::unknown) == 0u);
    }
} // namespace

int main() {
    test_byte_sizes();
    return deren::vk_test::finish("test_image_formats");
}