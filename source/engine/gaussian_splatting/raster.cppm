module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
export module deren.engine.gaussian_splatting.raster;
export import deren.engine.gaussian_splatting;
export import deren.gaussian_loader;
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
        friend std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(deren::promise::rhi::api_core&, deren::gaussian::asset, foundation_limits const&);
        std::shared_ptr<deren::gaussian::asset const> cpu_asset;
        std::shared_ptr<gpu_asset const> gpu_owner;
        std::vector<geometry_record> geometry;
    };
    class raster_frame {
    public:
        raster_frame(raster_frame const&) = delete;
        raster_frame& operator=(raster_frame const&) = delete;
        raster_frame(raster_frame&&) = delete;
        raster_frame& operator=(raster_frame&&) = delete;
        /// RECORD THIS FRAME'S UPLOAD INTO THE FRAME (plan Task 9, NVIDIA's tryConsumeAndUploadCpuSortingResult).
        ///
        /// THE SHAPE OF THE FIX, and why the previous one corrupted: this frame's two arrays used to BE the
        /// shader's buffers - host-visible, written through `mapped()`, read through their device addresses. That
        /// write has NO ordering relationship to any frame's execution: the CPU writes whenever it prepares, and
        /// the GPU may already be reading the buffer for the frame before. The corruption that produced was
        /// visible only in the cloud-only configuration, and only on the FIRST swapchain resize, which is the
        /// frame whose preparation the rebuild moves relative to the previous frame's execution.
        ///
        /// NOW: the CPU writes a HOST-VISIBLE STAGING buffer, and the DEVICE buffer the shader reads is filled by
        /// a copy RECORDED IN THIS FRAME - so the write is ordered by the frame itself, exactly as the driver
        /// orders any other recorded work. The device buffers carry `storage` + `device_address` (the shader reads
        /// them as an SSBO through the address in the push block) and are not host-visible at all.
        ///
        /// @return `error::ok`, or the contract's refusal for a copy that cannot be recorded (the caller logs it;
        ///         a frame with no visible splat records nothing).
        [[nodiscard]] deren::promise::rhi::error record_uploads(deren::promise::rhi::command_buffer& commands) const noexcept;
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

    private:
        raster_frame() = default;
        friend std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const&, raster_frame*);
        raster_push push;
        std::vector<draw_reference> reference_data;
        std::vector<instance_record> instance_data;
        std::vector<std::shared_ptr<raster_asset const>> owners;
        /// THE DEVICE BUFFERS THE SHADER READS: `uniform_gpu_only` + `storage` + `device_address`, filled by the
        /// copy `record_uploads` records - never written by the host (see that method's note).
        deren::promise::rhi::object_manager<deren::promise::rhi::buffer> reference_buffer, instance_buffer;
        /// THE HOST-VISIBLE STAGING BUFFERS THE CPU WRITES (a `staging_source` intent): the CPU's bytes land here
        /// through `mapped()`, and the frame's recorded copy moves them to the device buffers above.
        deren::promise::rhi::object_manager<deren::promise::rhi::buffer> reference_staging, instance_staging;
        /// WHAT THIS FRAME MUST COPY, in bytes: set by `prepare_raster_frame` to the sizes it wrote into the
        /// staging buffers, and read by `record_uploads`.
        std::uint64_t instance_upload_bytes = 0, reference_upload_bytes = 0;
        /// WHAT EACH PAIR WAS CREATED WITH, so a later frame can write into it in place instead of creating new
        /// buffers: the staging half is host-visible (`mapped()`), which is what the contract's `mapped()` exists
        /// for, and re-creating 4.5 MB every frame is both the measured cost and the per-frame creation a
        /// swapchain rebuild may trip over (the plan's Task 8).
        std::uint64_t reference_capacity = 0;
        std::uint64_t instance_capacity = 0;
        std::uint64_t bytes = 0;
        std::uint32_t total = 0, near_omitted = 0, outside = 0;
    };
    [[nodiscard]] std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(deren::promise::rhi::api_core&, deren::gaussian::asset, foundation_limits const& = {});
    /// @param reuse an optional frame to REFILL (the caller keeps it alive; the returned pointer is a non-owning
    ///        view of it when it is given), which is how a steady-state frame allocates nothing. @see raster_frame's
    ///        capacity members and the plan's Task 8.
    [[nodiscard]] std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const& = {}, raster_frame* = nullptr);
} // namespace deren::engine::gaussian_splatting
