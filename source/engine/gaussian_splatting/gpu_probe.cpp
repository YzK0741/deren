module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <numeric>
#include <span>
#include <string>
#include <vector>
module deren.engine.gaussian_splatting.probe;
import deren.utility.shared;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    std::expected<std::uint32_t, foundation_error> run_gpu_sort_probe(rhi::api_core& core) {
        auto fail = [](deren::utility::vessel::string message) { return std::unexpected(foundation_error{error_code::invalid_argument, std::move(message)}); };
        auto* ability = rhi::query_extension<rhi::gpu_sorting>(core);
        if (!ability) {
            return fail("GPU sorting capability unavailable");
        }
        rhi::object_ptr<rhi::radix_sorter> sorter{ability->create_radix_sorter()};
        if (!sorter) {
            return fail("GPU sorter creation failed");
        }
        if (sorter->requirements(UINT32_MAX).scratch_bytes != 0u) {
            return fail("GPU sorter did not reject an unrepresentable maximum count");
        }
        std::uint32_t cases = 0;
        for (std::uint32_t const count : {0u, 1u, 255u, 256u, 257u, 511u, 512u, 513u, 4095u, 4096u, 4097u, 65537u}) {
            for (std::uint32_t mode = 0; mode < 4; ++mode) {
                deren::utility::vessel::vector<std::uint32_t> keys(count);
                deren::utility::vessel::vector<std::uint32_t> values(count);
                deren::utility::vessel::vector<std::uint32_t> expected(count);
                std::iota(values.begin(), values.end(), 0u);
                std::iota(expected.begin(), expected.end(), 0u);
                for (std::uint32_t i = 0; i < count; ++i) {
                    switch (mode) {
                    case 0u:
                        keys[i] = (i * 1664525u + 1013904223u) % 37u;
                        break;
                    case 1u:
                        keys[i] = UINT32_MAX;
                        break;
                    case 2u:
                        keys[i] = count - i;
                        break;
                    default:
                        keys[i] = i;
                        break;
                    }
                }
                std::stable_sort(expected.begin(), expected.end(), [&](auto a, auto b) { return keys[a] < keys[b]; });
                auto make = [&](std::span<std::uint32_t const> data, std::uint64_t size, rhi::buffer_flags flags) {
                    rhi::buffer_desc desc;
                    desc.size = std::max<std::uint64_t>(size, 4u);
                    desc.usage = rhi::buffer_usage::storage_coherent;
                    desc.flags = flags;
                    desc.initial_bytes = std::as_bytes(data);
                    return rhi::object_ptr<rhi::buffer>{core.create_buffer(desc)};
                };
                auto requirements = sorter->requirements(count);
                auto kb = make(keys, static_cast<std::uint64_t>(count) * 4u, rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::indirect));
                auto vb = make(values, static_cast<std::uint64_t>(count) * 4u, rhi::to_bits(rhi::buffer_flag::storage));
                auto scratch = make({}, requirements.scratch_bytes, requirements.scratch_flags);
                auto commands = core.make_command_buffer({.kind = rhi::command_buffer_kind::primary});
                if (!kb || !vb || !scratch || !commands) {
                    return fail("GPU sort probe allocation failed");
                }
                if (sorter->record(*commands, {}) != rhi::error::not_ready || commands->draw_indirect(*kb, 0u, 0u, 16u) != rhi::error::not_ready) {
                    return fail("GPU recording verbs accepted an unopened command buffer");
                }
                if (commands->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) != rhi::error::ok) {
                    return fail("GPU sort probe begin failed");
                }
                rhi::radix_sort_info const info{.element_count = count, .keys = kb.get(), .values = vb.get(), .scratch = scratch.get()};
                if (count != 0u) {
                    auto invalid = info;
                    invalid.keys_offset = 1u;
                    if (sorter->record(*commands, invalid) != rhi::error::invalid_argument) {
                        return fail("GPU sorter accepted misaligned keys");
                    }
                    invalid = info;
                    invalid.values = invalid.keys;
                    if (sorter->record(*commands, invalid) != rhi::error::invalid_argument) {
                        return fail("GPU sorter accepted overlapping values");
                    }
                    invalid = info;
                    invalid.scratch = nullptr;
                    if (sorter->record(*commands, invalid) != rhi::error::invalid_argument) {
                        return fail("GPU sorter accepted missing scratch");
                    }
                }
                if (sorter->record(*commands, info) != rhi::error::ok || commands->end_recording() != rhi::error::ok || core.submit(*commands) != rhi::error::ok) {
                    return fail("GPU sort probe recording/submission failed");
                }
                core.wait_idle(); // Diagnostic only: wait before reading host-coherent output.
                auto km = kb->mapped();
                auto vm = vb->mapped();
                if (km.size() < count * 4ull || vm.size() < count * 4ull) {
                    return fail("GPU sort probe output not mapped");
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t key = 0;
                    std::uint32_t value = 0;
                    std::memcpy(&key, km.data() + i * 4ull, 4);
                    std::memcpy(&value, vm.data() + i * 4ull, 4);
                    if (value != expected[i] || key != keys[expected[i]]) {
                        return fail("GPU stable sort mismatch at count " + deren::utility::vessel::to_string(count) + ", mode " + deren::utility::vessel::to_string(mode) + ", index " + deren::utility::vessel::to_string(i));
                    }
                }
                ++cases;
            }
        }
        return cases;
    }
} // namespace deren::engine::gaussian_splatting
