/**
 * @file source/engine/render_graph/reflection_denoise.cppm
 * @defgroup engine_render_graph_reflection_denoise Reflection Denoise Graph
 * @brief the render-graph edges for AMD's Reproject -> Prefilter -> ResolveTemporal chain plus its history copy.
 *
 * Every image is reached through the frame heap, so the denoiser passes declare no bindings or targets. What the
 * graph still needs from us is the ORDER and the image transitions: each stage reads a different subset of the
 * G-buffer and of the previous frame's history, and writes a different subset of this frame's intermediates. The
 * copy stage then moves the current surfaces into the history images once the resolve has finished reading them.
 */
module;

#include <array>
#include <cstddef>
#include <optional>

export module deren.engine.render_graph.reflection_denoise;
export import deren.vstd;
export import deren.engine.render_graph;

export namespace deren::engine::render_graph::reflection_denoise {
    /// All images the four stages touch, handed in by the runtime after it imports them.
    struct inputs {
        texture_ref reflection;
        texture_ref depth;
        texture_ref normal;
        texture_ref velocity;
        texture_ref denoised;
        texture_ref denoised_history;
        texture_ref reprojected;
        texture_ref prefiltered;
        texture_ref average;
        texture_ref average_history;
        texture_ref moments;
        texture_ref moments_prefiltered;
        texture_ref moments_history;
        texture_ref surface_history;
        texture_ref surface_current;
        texture_ref depth_history;
        texture_ref depth_current;
    };

    struct outputs {
        texture_ref denoised;
        std::array<pass_ref, 4> passes;
    };

    /**
     * One stage's access list. Fixed-size optional slots let all four stages share a single parameter type, which
     * keeps the runtime's `bind_pass` call a single lambda for every stage instead of four distinct types.
     */
    struct stage_parameters {
        std::array<std::optional<texture_read>, 10> reads{};
        std::array<std::optional<texture_discard_write>, 5> writes{};

        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(reads, writes);
        }
    };

    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::compute_shader};
        constexpr access_scope storage{resource_usage::storage, pipeline_stage::compute_shader};

        stage_parameters reproject{};
        reproject.reads[0] = texture_read{in.reflection, {}, sampled};
        reproject.reads[1] = texture_read{in.depth, {}, sampled};
        reproject.reads[2] = texture_read{in.normal, {}, sampled};
        reproject.reads[3] = texture_read{in.velocity, {}, sampled};
        reproject.reads[4] = texture_read{in.denoised_history, {}, sampled};
        reproject.reads[5] = texture_read{in.moments_history, {}, sampled};
        reproject.reads[6] = texture_read{in.surface_history, {}, sampled};
        reproject.reads[7] = texture_read{in.depth_history, {}, sampled};
        reproject.reads[8] = texture_read{in.average_history, {}, sampled};
        reproject.writes[0] = texture_discard_write{in.reprojected, {}, storage};
        reproject.writes[1] = texture_discard_write{in.moments, {}, storage};
        reproject.writes[2] = texture_discard_write{in.average, {}, storage};

        stage_parameters prefilter{};
        prefilter.reads[0] = texture_read{in.reprojected, {}, sampled};
        prefilter.reads[1] = texture_read{in.normal, {}, sampled};
        prefilter.reads[2] = texture_read{in.depth, {}, sampled};
        prefilter.writes[0] = texture_discard_write{in.prefiltered, {}, storage};
        prefilter.writes[1] = texture_discard_write{in.moments_prefiltered, {}, storage};

        stage_parameters resolve{};
        resolve.reads[0] = texture_read{in.prefiltered, {}, sampled};
        resolve.reads[1] = texture_read{in.moments_prefiltered, {}, sampled};
        resolve.reads[2] = texture_read{in.depth, {}, sampled};
        resolve.reads[3] = texture_read{in.normal, {}, sampled};
        resolve.reads[4] = texture_read{in.denoised_history, {}, sampled};
        resolve.reads[5] = texture_read{in.moments_history, {}, sampled};
        resolve.reads[6] = texture_read{in.surface_history, {}, sampled};
        resolve.reads[7] = texture_read{in.depth_history, {}, sampled};
        resolve.reads[8] = texture_read{in.average_history, {}, sampled};
        resolve.writes[0] = texture_discard_write{in.denoised, {}, storage};
        resolve.writes[1] = texture_discard_write{in.moments, {}, storage};
        resolve.writes[2] = texture_discard_write{in.surface_current, {}, storage};
        resolve.writes[3] = texture_discard_write{in.depth_current, {}, storage};

        stage_parameters copy{};
        copy.reads[0] = texture_read{in.denoised, {}, sampled};
        copy.reads[1] = texture_read{in.moments, {}, sampled};
        copy.reads[2] = texture_read{in.surface_current, {}, sampled};
        copy.reads[3] = texture_read{in.depth_current, {}, sampled};
        copy.reads[4] = texture_read{in.average, {}, sampled};
        copy.writes[0] = texture_discard_write{in.denoised_history, {}, storage};
        copy.writes[1] = texture_discard_write{in.moments_history, {}, storage};
        copy.writes[2] = texture_discard_write{in.surface_history, {}, storage};
        copy.writes[3] = texture_discard_write{in.depth_history, {}, storage};
        copy.writes[4] = texture_discard_write{in.average_history, {}, storage};

        auto reproject_pass = graph.add_pass("reflection reproject", std::move(reproject));
        if (!reproject_pass) {
            return std::unexpected(reproject_pass.error());
        }
        auto prefilter_pass = graph.add_pass("reflection prefilter", std::move(prefilter));
        if (!prefilter_pass) {
            return std::unexpected(prefilter_pass.error());
        }
        auto resolve_pass = graph.add_pass("reflection resolve", std::move(resolve));
        if (!resolve_pass) {
            return std::unexpected(resolve_pass.error());
        }
        auto copy_pass = graph.add_pass("reflection history copy", std::move(copy));
        if (!copy_pass) {
            return std::unexpected(copy_pass.error());
        }

        for (auto const& [later, earlier] : {std::pair{*prefilter_pass, *reproject_pass},
                                             std::pair{*resolve_pass, *prefilter_pass},
                                             std::pair{*copy_pass, *resolve_pass}}) {
            auto ordered = graph.order_after(later, earlier);
            if (!ordered) {
                return std::unexpected(ordered.error());
            }
        }
        return outputs{in.denoised, {*reproject_pass, *prefilter_pass, *resolve_pass, *copy_pass}};
    }
} // namespace deren::engine::render_graph::reflection_denoise
