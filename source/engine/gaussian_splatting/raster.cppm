module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
export module deren.engine.gaussian_splatting.raster;
export import deren.utility.shared;
export import deren.engine.gaussian_splatting;
export import deren.gaussian_loader;
import deren.promise.rhi;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.render_resource;
import deren.engine.render_resource.instances;
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
    /// Which half of the frustum decision a reference landed in. `outside` is the default and covers BOTH a support
    /// that the plane tests reject and a center the projection cannot place (clip.w <= 0, i.e. behind the camera):
    /// both mean "this reference draws nothing", and the frame's outside_count is the total of them.
    enum class projection_visibility { visible,
                                       outside,
                                       near_omitted,
    };
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

    class raster_asset;
    struct raster_instance {
        std::shared_ptr<raster_asset const> asset;
        glm::mat4 model{1};
        std::uint64_t stable_id = 0;
    };
    class raster_asset {
    public:
        raster_asset(raster_asset const&) = delete;
        raster_asset& operator=(raster_asset const&) = delete;
        raster_asset(raster_asset&&) = delete;
        raster_asset& operator=(raster_asset&&) = delete;
        [[nodiscard]] deren::gaussian::asset const& source() const noexcept {
            return *cpu_asset;
        }
        [[nodiscard]] std::span<geometry_record const> geometry_records() const noexcept {
            return geometry;
        }
        [[nodiscard]] std::shared_ptr<gpu_asset const> const& gpu() const noexcept {
            return gpu_owner;
        }

    private:
        raster_asset() = default;
        friend std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(deren::promise::rhi::api_core&, deren::gaussian::asset, foundation_limits const&, std::shared_ptr<deren::promise::rhi::api_core>);
        std::shared_ptr<deren::gaussian::asset const> cpu_asset;
        std::shared_ptr<gpu_asset const> gpu_owner;
        deren::utility::vessel::vector<geometry_record> geometry;
    };
    class raster_frame {
    public:
        raster_frame(raster_frame const&) = delete;
        raster_frame& operator=(raster_frame const&) = delete;
        raster_frame(raster_frame&&) = delete;
        raster_frame& operator=(raster_frame&&) = delete;
        [[nodiscard]] raster_push const& params() const noexcept {
            return push;
        }
        [[nodiscard]] std::span<draw_reference const> references() const noexcept {
            return reference_data;
        }
        [[nodiscard]] std::span<instance_record const> instances() const noexcept {
            return instance_data;
        }
        [[nodiscard]] std::uint64_t byte_size() const noexcept {
            return bytes;
        }
        [[nodiscard]] std::uint32_t total_count() const noexcept {
            return total;
        }
        [[nodiscard]] std::uint32_t near_omitted_count() const noexcept {
            return near_omitted;
        }
        [[nodiscard]] std::uint32_t outside_count() const noexcept {
            return outside;
        }
        [[nodiscard]] render_resource::instances::resource_snapshot const& resources() const noexcept {
            return storage_view;
        }

    private:
        raster_frame() = default;
        friend std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const&, raster_frame*, std::shared_ptr<deren::promise::rhi::api_core>);
        raster_push push;
        deren::utility::vessel::vector<draw_reference> reference_data;
        deren::utility::vessel::vector<draw_reference> staging_reference_data;
        deren::utility::vessel::vector<instance_record> instance_data;
        deren::promise::rhi::api_core* owner = nullptr;
        // These leases outlive the storage subchain's completion drain.
        std::shared_ptr<deren::promise::rhi::api_core> device_lease;
        deren::utility::vessel::vector<std::shared_ptr<raster_asset const>> owners;
        deren::utility::vessel::vector<std::shared_ptr<raster_asset const>> staging_owners;
        deren::engine::pass::pass_chain storage_chain;
        deren::engine::pass::buffer_storage_pass* storage_provider = nullptr;
        render_resource::instances::resource_snapshot storage_view;
        render_resource::instances::borrowed<deren::promise::rhi::buffer> reference_buffer, instance_buffer;
        std::uint64_t bytes = 0;
        std::uint32_t total = 0, near_omitted = 0, outside = 0;
    };
    [[nodiscard]] std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(deren::promise::rhi::api_core&, deren::gaussian::asset, foundation_limits const& = {}, std::shared_ptr<deren::promise::rhi::api_core> = {});
    /// @param reuse an optional frame to REFILL (the caller keeps it alive; the returned pointer is a non-owning
    ///        view of it when it is given), which is how a steady-state frame allocates nothing. @see raster_frame's
    ///        retained buffer capacities and the plan's Task 8.
    /// A reference-only caller retains the core. Production supplies an owning lease initially;
    /// an allocated legacy frame cannot gain an owning lease by reuse. Reuse must be a completed slot.
    [[nodiscard]] std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const& = {}, raster_frame* = nullptr, std::shared_ptr<deren::promise::rhi::api_core> = {});
} // namespace deren::engine::gaussian_splatting
