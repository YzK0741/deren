module;
#include <array>
#include <memory>
#include <string_view>
export module deren.engine.pass.gaussian_splatting;
export import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.gaussian_splatting.raster;
import deren.engine.gaussian_splatting.gpu_frame;
export namespace deren::engine::pass {
    class gaussian_splatting_pass final : public frame_pass {
    public:
        explicit gaussian_splatting_pass(deren::promise::rhi::image_format color = deren::promise::rhi::image_format::r16g16b16a16_sfloat)
            : color_format(color) {
        }
        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override {
            return "gaussian";
        }
        void create(pass_context const& /*context*/) override;
        void on_swapchain_recreated(pass_host const& /*host*/) override {
        }

        /// \brief the raster alone - the pass's three image hand-offs (depth to an attachment and back, the target's
        ///        instance dependency) were the removed legacy record's, and a graph frame declares the two accesses instead.
        ///
        /// The GPU compute chain INSIDE this entry is the snapshot's own: its three dispatches write and read the
        /// frame snapshot's private buffers (no other pass touches them) and it emits their barriers itself, so the
        /// graph's boundary for this pass is exactly the raster's target write and depth read.
        void record_owned(resolved_io const& io);
        deren::promise::rhi::error record_content(resolved_io const& io) noexcept override;
        /// \brief the one body both entries run: own_barriers is the only difference between them
        deren::promise::rhi::error record_impl(resolved_io const& io, bool own_barriers) noexcept;
        [[nodiscard]] bool ready() const noexcept override {
            return static_cast<bool>(pipeline);
        }
        [[nodiscard]] deren::promise::rhi::pipeline* pipeline_handle() const noexcept override {
            return pipeline.get();
        }
        [[nodiscard]] owned_pipeline named_pipeline(std::string_view name) const noexcept override {
            return {name == "gaussian" ? pipeline.get() : nullptr};
        }
        void set_frame(std::shared_ptr<gaussian_splatting::raster_frame const> snapshot) noexcept {
            gpu_snapshot.reset();
            frame = std::move(snapshot);
        }
        void set_gpu_frame(std::shared_ptr<gaussian_splatting::gpu_frame const> snapshot) noexcept {
            frame.reset();
            gpu_snapshot = std::move(snapshot);
        }
        [[nodiscard]] bool gpu_ready() const noexcept {
            return ready() && compute[0] && compute[1] && compute[2];
        }
        void set_timing(gaussian_splatting::gpu_frame_timing points) noexcept {
            timing = points;
        }
        [[nodiscard]] deren::promise::rhi::error last_error() const noexcept {
            return record_error;
        }

    private:
        deren::promise::rhi::image_format color_format;
        deren::promise::rhi::object_ptr<deren::promise::rhi::pipeline> pipeline;
        std::shared_ptr<gaussian_splatting::raster_frame const> frame;
        std::shared_ptr<gaussian_splatting::gpu_frame const> gpu_snapshot;
        deren::utility::vessel::array<deren::promise::rhi::object_ptr<deren::promise::rhi::pipeline>, 3> compute;
        gaussian_splatting::gpu_frame_timing timing;
        deren::promise::rhi::error record_error = deren::promise::rhi::error::ok;
    };
} // namespace deren::engine::pass
