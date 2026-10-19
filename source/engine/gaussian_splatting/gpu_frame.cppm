module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
export module deren.engine.gaussian_splatting.gpu_frame;
export import deren.utility.shared;
export import deren.engine.gaussian_splatting.raster;
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.render_resource;
export namespace deren::engine::gaussian_splatting {
    struct gpu_prepare_pipelines {
        deren::promise::rhi::pipeline* reset = nullptr;
        deren::promise::rhi::pipeline* prepare = nullptr;
        deren::promise::rhi::pipeline* finalize = nullptr;
        [[nodiscard]] bool ready() const noexcept {
            return reset && prepare && finalize;
        }
    };
    struct gpu_frame_stats {
        std::uint32_t visible = 0, outside = 0, near_omitted = 0, errors = 0;
        std::uint64_t frame_id = 0;
    };
    struct gpu_frame_timing {
        void* owner = nullptr;
        void (*mark)(void*, std::uint32_t phase) = nullptr;
    };
    struct gpu_instance_range {
        std::uint32_t begin = 0, end = 0, instance_index = 0, padding = 0;
    };
    struct alignas(16) gpu_projected_splat {
        glm::vec4 center{}, basis{}, color_opacity{};
    };
    static_assert(sizeof(gpu_projected_splat) == 48);
    struct alignas(16) gpu_prepare_parameters {
        raster_push raster;
        glm::vec4 forward{0};
        std::uint64_t ranges_address = 0, keys_address = 0, values_address = 0;
        std::uint64_t status_address = 0, draw_address = 0;
        std::uint32_t candidate_count = 0, range_count = 0;
    };
    static_assert(sizeof(gpu_instance_range) == 16);
    static_assert(sizeof(gpu_prepare_parameters) == 176 && offsetof(gpu_prepare_parameters, forward) == 112 &&
                  offsetof(gpu_prepare_parameters, ranges_address) == 128 && offsetof(gpu_prepare_parameters, candidate_count) == 168);
    class gpu_frame : public std::enable_shared_from_this<gpu_frame> {
    public:
        gpu_frame(gpu_frame const&) = delete;
        gpu_frame& operator=(gpu_frame const&) = delete;
        [[nodiscard]] std::uint32_t candidate_count() const noexcept {
            return push.visible_count;
        }
        [[nodiscard]] raster_push const& params() const noexcept {
            return push;
        }
        [[nodiscard]] std::uint64_t byte_size() const noexcept {
            return bytes;
        }
        [[nodiscard]] render_resource::instances::resource_snapshot const& resources() const noexcept {
            return storage_view;
        }
        [[nodiscard]] deren::promise::rhi::buffer* draw_buffer() const noexcept {
            return buffers[draw].get();
        }
        // Diagnostic readback sources; no CPU visibility result is fabricated from them.
        [[nodiscard]] deren::promise::rhi::buffer* references_buffer() const noexcept {
            return buffers[references].get();
        }
        [[nodiscard]] deren::promise::rhi::buffer* keys_buffer() const noexcept {
            return buffers[keys].get();
        }
        [[nodiscard]] deren::promise::rhi::buffer* values_buffer() const noexcept {
            return buffers[values].get();
        }
        [[nodiscard]] deren::promise::rhi::error record_preparation(deren::promise::rhi::command_buffer&, gpu_prepare_pipelines const&, deren::engine::pass::resolved_io::push_endpoint const&, gpu_frame_timing const& = {}) const;
        /// Call only after the submitted frame using this slot has completed.
        [[nodiscard]] gpu_frame_stats completed_stats(std::uint64_t frame_id) const noexcept;

    private:
        gpu_frame() = default;
        friend std::expected<std::shared_ptr<gpu_frame>, foundation_error> prepare_gpu_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const&, gpu_frame*, deren::promise::rhi::shared_object<deren::promise::rhi::api_core>);
        enum buffer_index : std::size_t { instances,
                                          ranges,
                                          keys,
                                          values,
                                          references,
                                          scratch,
                                          status,
                                          draw,
                                          parameters,
                                          projected,
                                          buffer_count,
        };
        deren::promise::rhi::api_core* owner = nullptr;
        raster_push push;
        std::uint64_t parameter_address = 0, bytes = 0;
        // The device and asset leases outlive the subchain's drain and all owned allocations.
        deren::promise::rhi::shared_object<deren::promise::rhi::api_core> device_lease;
        deren::utility::vessel::vector<std::shared_ptr<raster_asset const>> owners;
        deren::promise::rhi::unique_object<deren::promise::rhi::radix_sorter> sorter;
        deren::engine::pass::pass_chain storage_chain;
        deren::engine::pass::buffer_storage_pass* storage_provider = nullptr;
        render_resource::instances::resource_snapshot storage_view;
        deren::utility::vessel::array<render_resource::instances::borrowed<deren::promise::rhi::buffer>, buffer_count> buffers;
    };
    /// reuse must be a completed slot obtained from this factory on the same core.
    /// A reference-only caller keeps the core alive; production passes provide an owning device lease.
    /// An allocated reference-only frame cannot be promoted to owning reuse; create a fresh frame instead.
    // NOLINTNEXTLINE(readability-trailing-comma): Clang 23 mislocates commas around empty module default arguments.
    [[nodiscard]] std::expected<std::shared_ptr<gpu_frame>, foundation_error> prepare_gpu_frame(deren::promise::rhi::api_core&, std::span<raster_instance const>, raster_camera const&, foundation_limits const& = {}, gpu_frame* = nullptr, deren::promise::rhi::shared_object<deren::promise::rhi::api_core> = {});
} // namespace deren::engine::gaussian_splatting
