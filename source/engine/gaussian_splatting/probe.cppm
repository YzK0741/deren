module;
#include <array>
#include <cstdint>
#include <expected>
#include <span>
export module deren.engine.gaussian_splatting.probe;
export import deren.utility.shared;
export import deren.engine.gaussian_splatting.raster;
import deren.promise.rhi;
export namespace deren::engine::gaussian_splatting {
    struct raster_probe_report {
        std::uint32_t cases = 0;
        std::uint32_t pixels = 0;
        float maximum_error = 0;
        std::uint32_t maximum_depth_ulp = 0, depth_order_differences = 0;
    };
    [[nodiscard]] std::expected<std::uint32_t, foundation_error> run_gpu_sort_probe(deren::promise::rhi::api_core& core);
    // Isolated diagnostic: waits for its own submissions before HDR host readback.
    [[nodiscard]] std::expected<raster_probe_report, foundation_error> run_raster_probe(
        deren::promise::rhi::api_core& core, std::span<std::uint8_t const> vertex,
        std::span<std::uint8_t const> fragment,
        // NOLINTNEXTLINE(readability-trailing-comma): Clang 23 mislocates commas around empty module default arguments.
        deren::utility::vessel::array<std::span<std::uint8_t const>, 3> compute = {}, bool gpu = false);
} // namespace deren::engine::gaussian_splatting
