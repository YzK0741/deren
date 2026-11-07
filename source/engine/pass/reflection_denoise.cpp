/**
 * @file source/engine/pass/reflection_denoise.cpp
 * @brief the reflection denoiser passes' pipelines and their one 8x8 full-screen dispatch each.
 */
module;

#include <cstddef>
#include <cstdint>
module deren.engine.pass.reflection_denoise;
import deren.vstd;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;

namespace deren::engine::pass {
    void reflection_denoise_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->build_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->build_pipeline.has_value()) {
            return;
        }
        std::string_view const shader_name = reflection_denoise_shader_name(this->stage_);
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("reflection denoise disabled: the owner has no {}", shader_name);
            return;
        }
        auto built = pipelines::build_reflection_denoise(*context.face, spirv, reflection_denoise_pass_name(this->stage_).data());
        if (!built) {
            deren::utility::log("reflection denoise ({}) disabled: {}", reflection_denoise_pass_name(this->stage_), built.error());
            this->build_pipeline.reset();
            return;
        }
        this->build_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: reflection denoise pipeline created ({})", reflection_denoise_pass_name(this->stage_));
    }

    rhi::error reflection_denoise_pass::record_content(resolved_io const& io) noexcept {
        if (!this->build_pipeline.has_value() || io.list == nullptr || io.extent.width == 0u || io.extent.height == 0u) {
            return rhi::error::invalid_argument;
        }
        reflection_denoise_push const push{.temporal_stability_factor = this->temporal_stability_factor,
                                           .max_samples = this->max_samples,
                                           .history_valid = this->history_valid ? 1u : 0u,
                                           .screen_width = io.extent.width,
                                           .screen_height = io.extent.height};
        if (!io.push_block(*io.list, std::as_bytes(std::span<reflection_denoise_push const>(&push, 1u)))) {
            return rhi::error::invalid_argument;
        }
        constexpr std::uint32_t group_size = 8u; // the shaders' [numthreads(8, 8, 1)]
        io.list->dispatch((io.extent.width + group_size - 1u) / group_size, (io.extent.height + group_size - 1u) / group_size, 1u);
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
