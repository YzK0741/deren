#pragma once
#include "document.hpp"
namespace deren::tools::subtilis::package_detail {
    // Count-only entry point: exercise the GLB u32 boundary without allocating GBs.
    inline result<std::uint32_t> glb_length(std::uint64_t json, std::uint64_t bin, asset::limits const& limits) {
        if (json > UINT32_MAX - 3u || bin > UINT32_MAX - 3u)
            return std::unexpected(asset::error{asset::error_code::capacity, "GLB chunk length overflow", {}, {}});
        auto j = (json + 3) & ~std::uint64_t(3), b = (bin + 3) & ~std::uint64_t(3);
        auto total = 20 + j + (b ? 8 + b : 0);
        if (total > UINT32_MAX || total > limits.max_input_bytes)
            return std::unexpected(asset::error{asset::error_code::capacity, "GLB total byte limit", {}, {}});
        return static_cast<std::uint32_t>(total);
    }
} // namespace deren::tools::subtilis::package_detail
