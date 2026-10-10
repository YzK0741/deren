module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
export module deren.gaussian_loader;
export namespace deren::gaussian {
    enum class error_code { io,
                            unsupported_format,
                            invalid_header,
                            missing_property,
                            invalid_data,
                            truncated,
                            limit_exceeded };
    struct diagnostic {
        error_code code;
        std::size_t byte_offset = 0;
        std::string property;
        std::string message;
    };
    struct load_options {
        std::uint64_t max_particles = 4'000'000;
        std::uint64_t max_file_bytes = 1ull << 30;
        std::uint64_t max_output_bytes = 1ull << 30;
    };
    struct particle {
        std::array<float, 3> center{}, scale{};
        std::array<float, 4> rotation{}; // normalized wxyz
        float opacity = 0;
        std::array<float, 48> sh{}; // coefficient-major RGB, unused bands zero
    };
    struct asset {
        std::vector<particle> particles;
        std::uint32_t sh_degree = 0;
        std::array<float, 3> bounds_min{}, bounds_max{}; // 3 sigma support
    };
    using load_result = std::expected<asset, diagnostic>;
    [[nodiscard]] load_result parse_ply(std::span<std::byte const>, load_options const& = {});
    [[nodiscard]] load_result load_ply(std::string_view path, load_options const& = {});
} // namespace deren::gaussian
