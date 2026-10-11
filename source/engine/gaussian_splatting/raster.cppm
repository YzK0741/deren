module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
export module deren.engine.gaussian_splatting.raster;
import deren.engine.gaussian_splatting;
import deren.promise.rhi;
export namespace deren::engine::gaussian_splatting {
    struct raster_camera {
        glm::mat4 view_projection{1};
        glm::vec3 position{0};
        deren::promise::rhi::image_extent extent{0, 0};
    };
    struct alignas(16) raster_push {
        glm::mat4 view_projection{1};
        glm::vec4 camera_position{0};
        std::uint64_t references_address = 0, instances_address = 0;
        float viewport_width = 0, viewport_height = 0;
        std::uint32_t visible_count = 0, flags = 0;
    };
    static_assert(sizeof(raster_push) == 112 && offsetof(raster_push, camera_position) == 64 && offsetof(raster_push, references_address) == 80 && offsetof(raster_push, instances_address) == 88 && offsetof(raster_push, viewport_width) == 96 && offsetof(raster_push, visible_count) == 104 && offsetof(raster_push, flags) == 108);
    enum class projection_visibility { visible,
                                       outside,
                                       near_omitted };
    struct projected_splat {
        projection_visibility visibility = projection_visibility::outside;
        glm::vec2 center_pixel{0}, basis_x{0}, basis_y{0};
        glm::vec3 covariance{0};
        float depth = 0;
    };
    [[nodiscard]] std::expected<sort_camera, foundation_error> validate_raster_camera(raster_camera const&);
    [[nodiscard]] std::expected<projected_splat, foundation_error> project_splat(geometry_record const&, glm::mat4 const&, raster_camera const&);
    [[nodiscard]] std::expected<glm::vec3, foundation_error> evaluate_sh(sh_record const&, std::uint32_t, glm::vec3 const&);
    [[nodiscard]] float fragment_alpha(float, glm::vec2 const&) noexcept;
} // namespace deren::engine::gaussian_splatting
