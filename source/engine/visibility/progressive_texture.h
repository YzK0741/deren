#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace deren::engine::progressive_texture {
    struct fallback_image {
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint8_t> bytes;
    };
    inline std::optional<fallback_image> resample(std::span<std::uint8_t const> source,
        std::uint32_t width, std::uint32_t height, std::uint32_t target_width, std::uint32_t target_height, bool srgb) {
        if (!width || !height || !target_width || !target_height || target_width > width || target_height > height || std::uint64_t(width) * height > source.size() / 4)
            return {};
        fallback_image result{target_width, target_height, {}};
        if (target_width == width && target_height == height) {
            result.bytes.assign(source.begin(), source.begin() + std::size_t(width) * height * 4);
            return result;
        }
        result.bytes.resize(std::size_t(result.width) * result.height * 4);
        static auto const linear_table = [] {
            std::array<double, 256> table{};
            for (unsigned i = 0; i < table.size(); ++i) {
                double value = double(i) / 255.0;
                table[i] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            }
            return table;
        }();
        auto decode = [srgb](std::uint8_t byte, unsigned channel) {
            return srgb && channel < 3 ? linear_table[byte] : double(byte) / 255.0;
        };
        for (std::uint32_t y = 0; y < result.height; ++y)
            for (std::uint32_t x = 0; x < result.width; ++x) {
                auto x0 = std::uint32_t(std::uint64_t(x) * width / result.width);
                auto x1 = std::uint32_t(std::uint64_t(x + 1) * width / result.width);
                auto y0 = std::uint32_t(std::uint64_t(y) * height / result.height);
                auto y1 = std::uint32_t(std::uint64_t(y + 1) * height / result.height);
                for (unsigned c = 0; c < 4; ++c) {
                    double sum = 0;
                    for (auto sy = y0; sy < y1; ++sy)
                        for (auto sx = x0; sx < x1; ++sx)
                            sum += decode(source[(std::size_t(sy) * width + sx) * 4 + c], c);
                    double value = sum / (double(x1 - x0) * (y1 - y0));
                    if (srgb && c < 3)
                        value = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    result.bytes[(std::size_t(y) * result.width + x) * 4 + c] = std::uint8_t(std::clamp(std::lround(value * 255), 0l, 255l));
                }
            }
        return result;
    }
    inline std::optional<fallback_image> fallback(std::span<std::uint8_t const> source,
        std::uint32_t width, std::uint32_t height, bool srgb, std::uint32_t limit = 128) {
        if (!width || !height || !limit) return {};
        auto longest = std::max(width, height);
        return resample(source, width, height,
            std::max(1u, std::uint32_t(std::uint64_t(width) * std::min(limit, longest) / longest)),
            std::max(1u, std::uint32_t(std::uint64_t(height) * std::min(limit, longest) / longest)), srgb);
    }
    struct mip_slice {
        std::uint32_t width = 0, height = 0, levels = 0;
        std::uint64_t offset = 0, bytes = 0;
    };
    inline std::optional<mip_slice> mip_range(std::uint32_t width, std::uint32_t height,
        std::uint32_t levels, std::uint32_t first) {
        if (!width || !height || !levels || levels > std::uint32_t(std::bit_width(std::max(width, height))) || first >= levels) return {};
        mip_slice result{std::max(1u, width >> first), std::max(1u, height >> first), levels - first, 0, 0};
        for (std::uint32_t mip = 0; mip < levels; ++mip) {
            std::uint64_t pixels = std::uint64_t(std::max(1u, width >> mip)) * std::max(1u, height >> mip);
            if (pixels > (UINT64_MAX - result.offset - result.bytes) / 4) return {};
            if (mip < first) result.offset += pixels * 4;
            else result.bytes += pixels * 4;
        }
        return result;
    }
    inline std::uint32_t target_mip(std::uint32_t width, std::uint32_t height, std::uint32_t extent) {
        if (!extent) return 0;
        std::uint32_t mip = 0, longest = std::max(width, height);
        while (mip < 31 && (longest >> (mip + 1)) >= extent) ++mip;
        return mip;
    }
    inline std::uint32_t floor_mip(std::uint32_t width, std::uint32_t height) {
        std::uint32_t mip = 0;
        while (mip < 31 && (std::max(width, height) >> mip) > 128) ++mip;
        return mip;
    }
    inline std::uint64_t tier_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t mip) {
        if (!width || !height || mip >= std::uint32_t(std::bit_width(std::max(width, height)))) return 0;
        if (mip < levels) {
            auto slice = mip_range(width, height, levels, mip);
            return slice ? slice->bytes : 0;
        }
        return std::uint64_t(std::max(1u, width >> mip)) * std::max(1u, height >> mip) * 4;
    }
    struct mip_candidate {
        std::uint32_t width = 0, height = 0, levels = 0, floor = 0, target = 0, resident = 0;
        std::uint64_t last_used = 0;
        bool wanted = false;
    };
    inline std::vector<std::uint32_t> select_mips(std::span<mip_candidate const> entries, std::uint64_t now,
        std::uint64_t budget, std::uint64_t retention) {
        std::vector<std::uint32_t> result, targets;
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto const& e = entries[i];
            result.push_back(e.floor);
            targets.push_back(e.wanted ? std::min(e.target, e.resident) : e.resident);
            if (e.width && e.height && e.floor <= 31 && (e.wanted || (e.resident < e.floor && now >= e.last_used && now - e.last_used <= retention))) order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            auto const& x = entries[a]; auto const& y = entries[b];
            if (x.wanted != y.wanted) return x.wanted > y.wanted;
            if ((x.resident < x.floor) != (y.resident < y.floor)) return x.resident < x.floor;
            return x.last_used > y.last_used;
        });
        for (unsigned round = 0; round < 32; ++round) {
            bool improved = false;
            for (auto i : order) {
                auto const& e = entries[i];
                if (result[i] <= targets[i]) continue;
                auto before = result[i] == e.floor ? 0 : tier_bytes(e.width, e.height, e.levels, result[i]);
                auto after = tier_bytes(e.width, e.height, e.levels, result[i] - 1);
                if (after >= before && after - before <= budget) {
                    budget -= after - before; --result[i]; improved = true;
                }
            }
            if (!improved) break;
        }
        return result;
    }
    inline std::vector<std::size_t> promotion_order(std::span<mip_candidate const> entries, std::span<std::uint32_t const> selected) {
        std::vector<std::size_t> order;
        if (entries.size() != selected.size()) return order;
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].resident > selected[i]) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            auto const& x = entries[a]; auto const& y = entries[b];
            if (x.wanted != y.wanted) return x.wanted > y.wanted;
            auto x_steps = x.floor - x.resident, y_steps = y.floor - y.resident;
            if (x_steps != y_steps) return x_steps < y_steps;
            return x.last_used > y.last_used;
        });
        return order;
    }
    struct candidate {
        std::uint64_t bytes = 0, last_used = 0;
        bool resident = false, wanted = false;
    };
    inline std::vector<std::uint8_t> select(std::span<candidate const> entries, std::uint64_t now,
        std::uint64_t budget, std::uint64_t retention) {
        std::vector<std::size_t> order;
        std::vector<std::uint8_t> result(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].wanted || (entries[i].resident && now >= entries[i].last_used && now - entries[i].last_used <= retention))
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            auto const& x = entries[a]; auto const& y = entries[b];
            if (x.wanted != y.wanted) return x.wanted > y.wanted;
            if (x.resident != y.resident) return x.resident > y.resident;
            return x.last_used > y.last_used;
        });
        for (auto index : order)
            if (entries[index].bytes && entries[index].bytes <= budget) {
                result[index] = 1;
                budget -= entries[index].bytes;
            }
        return result;
    }
    inline bool can_start(std::uint64_t bytes, std::uint64_t pending, std::size_t jobs,
        std::uint64_t upload_budget, std::size_t max_jobs) {
        return jobs < max_jobs && pending <= upload_budget && bytes <= upload_budget - pending;
    }
}
