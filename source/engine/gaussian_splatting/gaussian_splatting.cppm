module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>
export module deren.engine.gaussian_splatting;
import deren.gaussian_loader;
import deren.promise.rhi;
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
    struct alignas(16) geometry_record {
        std::array<float, 4> center_opacity{}, covariance_x{}, covariance_yz{};
    };
    struct sh_record {
        std::array<float, 48> coefficients{};
    };
    struct alignas(16) instance_record {
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
    struct sort_instance {
        deren::gaussian::asset const* asset = nullptr;
        glm::mat4 model{1.0f};
        std::uint64_t stable_id = 0;
    };
    struct sort_camera {
        glm::vec3 position{0};
        glm::vec3 forward{0, 0, -1};
    };
    [[nodiscard]] std::expected<glm::mat4, foundation_error> validate_model(glm::mat4 const& model);
    [[nodiscard]] std::expected<std::vector<draw_reference>, foundation_error> sort_draw_references(std::span<sort_instance const> instances, sort_camera const& camera, foundation_limits const& limits = {});
    [[nodiscard]] std::expected<std::uint64_t, foundation_error> packed_byte_size(std::uint64_t count, foundation_limits const& limits = {});
    [[nodiscard]] std::expected<packed_asset, foundation_error> pack_asset(deren::gaussian::asset const& input, foundation_limits const& limits = {});
    class gpu_asset {
    public:
        [[nodiscard]] std::uint32_t particle_count() const noexcept {
            return count;
        }
        [[nodiscard]] std::uint32_t sh_degree() const noexcept {
            return degree;
        }
        [[nodiscard]] std::uint64_t byte_size() const noexcept {
            return bytes;
        }
        [[nodiscard]] std::uint64_t geometry_address() const noexcept {
            return geometry_gpu_address;
        }
        [[nodiscard]] std::uint64_t sh_address() const noexcept {
            return sh_gpu_address;
        }
        [[nodiscard]] std::array<float, 3> const& bounds_min() const noexcept {
            return minimum;
        }
        [[nodiscard]] std::array<float, 3> const& bounds_max() const noexcept {
            return maximum;
        }

    private:
        gpu_asset() = default;
        friend std::expected<std::shared_ptr<gpu_asset const>, foundation_error> upload_asset(deren::promise::rhi::api_core&, packed_asset const&, foundation_limits const&);
        deren::promise::rhi::object_manager<deren::promise::rhi::buffer> geometry_buffer, sh_buffer;
        std::uint32_t count = 0, degree = 0;
        std::uint64_t bytes = 0, geometry_gpu_address = 0, sh_gpu_address = 0;
        std::array<float, 3> minimum{}, maximum{};
    };
    [[nodiscard]] std::expected<std::shared_ptr<gpu_asset const>, foundation_error> upload_asset(deren::promise::rhi::api_core& core, packed_asset const& asset, foundation_limits const& limits = {});

} // namespace deren::engine::gaussian_splatting
