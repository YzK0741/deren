/**
 * @file source/engine/pass/reflection_denoise.cppm
 * @defgroup engine_pass_reflection_denoise Reflection Denoise Pass
 * @brief the AMD FidelityFX reflection denoiser's three stages as heap-native compute passes.
 *
 * The denoiser is a three-stage temporal chain (Reproject -> Prefilter -> ResolveTemporal). Each stage is its
 * own pass node in the render graph so the graph owns the transitions between them; the shaders reach every
 * image through the frame's heap, so a pass declares no bindings and no targets. The only host-side constants
 * are the push block below, and the framework appends the two heap index lanes after them.
 */
module;

#include <cstdint>

export module deren.engine.pass.reflection_denoise;
export import deren.vstd;

export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    /// @brief which stage of the AMD chain this pass records
    enum class reflection_denoise_stage : std::uint8_t {
        reproject,
        prefilter,
        resolve,
        copy_history,
    };

    [[nodiscard]] constexpr std::string_view reflection_denoise_shader_name(reflection_denoise_stage stage) noexcept {
        switch (stage) {
        case reflection_denoise_stage::reproject:
            return "reflection_reproject.comp.spv";
        case reflection_denoise_stage::prefilter:
            return "reflection_prefilter.comp.spv";
        case reflection_denoise_stage::resolve:
            return "reflection_resolve.comp.spv";
        case reflection_denoise_stage::copy_history:
            return "reflection_copy_history.comp.spv";
        }
        return {};
    }

    [[nodiscard]] constexpr std::string_view reflection_denoise_pass_name(reflection_denoise_stage stage) noexcept {
        switch (stage) {
        case reflection_denoise_stage::reproject:
            return "reflection reproject";
        case reflection_denoise_stage::prefilter:
            return "reflection prefilter";
        case reflection_denoise_stage::resolve:
            return "reflection resolve";
        case reflection_denoise_stage::copy_history:
            return "reflection history copy";
        }
        return {};
    }

    /**
     * The push block the three shaders share. The host writes the first six lanes (24 bytes); the pass
     * framework appends `frame_slot` and `image_index`, for 32 bytes total - see `ReflectionDenoisePush` in
     * `source/shaders/lighting/reflection_denoise_common.slang`.
     */
    struct reflection_denoise_push {
        float temporal_stability_factor = 1.0f;
        std::uint32_t max_samples = 32u;
        std::uint32_t history_valid = 0u;
        std::uint32_t screen_width = 0u;
        std::uint32_t screen_height = 0u;
        float max_roughness = 0.2f;
    };
    static_assert(sizeof(reflection_denoise_push) == 24u);

    class reflection_denoise_pass final : public frame_pass {
    public:
        explicit reflection_denoise_pass(reflection_denoise_stage stage) noexcept
            : stage_(stage) {
            this->io_.name = reflection_denoise_pass_name(stage);
        }
        ~reflection_denoise_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // NO BINDINGS, NO TARGETS, NO DECLARED PUSH BLOCK: every image travels through the frame's heap, and
            // the graph adapter declares the read/write edges that order and transition them.
            return this->io_;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::compute,
                                                              .group_size_x = 8,
                                                              .group_size_y = 8,
                                                              .group_size_z = 1,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = false};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // gated by `rt_reflections` (default off) at the runtime/graph level
        }

        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->build_pipeline.has_value();
        }
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->build_pipeline.has_value() ? this->build_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // the pipeline depends on the device alone; every image arrives per frame through the heap
        }

        /// @brief set the history state the next recorded frame must use (false on resize / feature toggle)
        void set_history_valid(bool valid) noexcept {
            this->history_valid = valid;
        }
        [[nodiscard]] bool history_is_valid() const noexcept {
            return this->history_valid;
        }

        /// @brief the temporal accumulation cap (AMD's `max_samples`; 0 means the shader's own fallback)
        void set_max_samples(std::uint32_t samples) noexcept {
            this->max_samples = samples;
        }

        /// @brief the reproject stage's temporal stability factor (AMD's `temporal_stability_factor`)
        void set_temporal_stability_factor(float factor) noexcept {
            this->temporal_stability_factor = factor;
        }

        /// @brief the roughness below which the three stages filter; AMD's 0.2 by default
        void set_max_roughness(float roughness) noexcept {
            this->max_roughness = roughness;
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        reflection_denoise_stage stage_;
        render_resource::pass_io io_{};
        std::optional<pipelines::pipeline_handle> build_pipeline = std::nullopt;
        void* built_against = nullptr;
        bool history_valid = false;
        std::uint32_t max_samples = 32u;
        float temporal_stability_factor = 1.0f;
        float max_roughness = 0.2f;
    };
} // namespace deren::engine::pass
