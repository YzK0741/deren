module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <string>
#include <vector>
export module deren.engine.gaussian_splatting;
import deren.gaussian_loader;
export namespace deren::engine::gaussian_splatting {
    enum class error_code { invalid_argument,
                            limit_exceeded,
                            unsupported,
                            allocation_failure,
                            invalid_address,
    };
    struct foundation_error {
        error_code code;
        std::string message;
    };
    struct foundation_limits {
        std::uint64_t max_references = 4'000'000;
        std::uint64_t max_packed_bytes = 1ull << 30;
    };
    struct geometry_record {
        std::array<float, 4> center_opacity{}, covariance_x{}, covariance_yz{};
    };
    struct sh_record {
        std::array<float, 48> coefficients{};
    };
    struct instance_record {
        std::uint64_t geometry_address = 0, sh_address = 0;
        glm::mat4 model{1.0f}, inverse_model{1.0f};
        std::uint32_t sh_degree = 0;
        std::array<std::uint32_t, 3> padding{};
    };
    struct draw_reference {
        std::uint32_t instance_index = 0, local_gaussian_index = 0;
    };
    static_assert(sizeof(geometry_record) == 48 && offsetof(geometry_record, covariance_x) == 16 && offsetof(geometry_record, covariance_yz) == 32);
    static_assert(sizeof(sh_record) == 192);
    static_assert(sizeof(instance_record) == 160 && offsetof(instance_record, model) == 16 && offsetof(instance_record, inverse_model) == 80 && offsetof(instance_record, sh_degree) == 144 && offsetof(instance_record, padding) == 148);
    static_assert(sizeof(draw_reference) == 8 && offsetof(draw_reference, local_gaussian_index) == 4);
    struct packed_asset {
        std::vector<geometry_record> geometry;
        std::vector<sh_record> sh;
        std::uint32_t sh_degree = 0;
        std::array<float, 3> bounds_min{}, bounds_max{};
    };
    [[nodiscard]] std::expected<std::uint64_t, foundation_error> packed_byte_size(std::uint64_t count, foundation_limits const& limits = {});
    [[nodiscard]] std::expected<packed_asset, foundation_error> pack_asset(deren::gaussian::asset const& input, foundation_limits const& limits = {});
} // namespace deren::engine::gaussian_splatting
