module;
#include <array>
#include <memory>
#include <string_view>
export module deren.engine.pass.gaussian_splatting;
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.gaussian_splatting.raster;
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
        void record(resolved_io const& /*io*/) override;
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
            frame = std::move(snapshot);
        }
        [[nodiscard]] deren::promise::rhi::error last_error() const noexcept {
            return record_error;
        }

    private:
        deren::promise::rhi::image_format color_format;
        deren::promise::rhi::object_manager<deren::promise::rhi::pipeline> pipeline;
        std::shared_ptr<gaussian_splatting::raster_frame const> frame;
        deren::promise::rhi::error record_error = deren::promise::rhi::error::ok;
    };
} // namespace deren::engine::pass
