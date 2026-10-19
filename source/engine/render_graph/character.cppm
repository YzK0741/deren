module;
#include <expected>
#include <utility>
export module deren.engine.render_graph.character;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph {
    /// The character-forward pass: the character's own leaves, its outline group and its overlay masks, drawn
    /// INTO the shaded frame with LOAD on the scene target and the depth - the same shape the transparent pass
    /// has, so the target is a PRESERVE write and the depth is read through the depth test.
    namespace character_forward {
        struct inputs {
            texture_ref target;
            texture_ref depth;
        };
        struct parameters {
            texture_preserve_write target;
            texture_read depth;
            void visit_resources(parameter_visitor& visit) const noexcept {
                visit(target, depth);
            }
        };
        struct outputs {
            texture_ref target;
            pass_ref pass;
        };
        [[nodiscard]] inline std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
            if (in.target == in.depth) {
                return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "character forward", .resource_name = "character forward alias"});
            }
            parameters params{{in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                              {in.depth, {}, {resource_usage::depth_stencil_read, pipeline_stage::depth_stencil}}};
            auto added = graph.add_pass("character forward", std::move(params));
            if (!added) {
                return std::unexpected(added.error());
            }
            return outputs{in.target, *added};
        }
    } // namespace character_forward

    /// The toon screen rim: a fullscreen ADDITIVE effect over the shaded frame. Its sampled inputs (the depth and
    /// the albedo) come from per-image HEAP SLOTS the shader indexes itself - the reads are real, only the binding
    /// is out of the model, and declaring the access is what makes the graph transition and order them.
    namespace toon_screen_rim {
        struct inputs {
            texture_ref target;
            texture_ref depth;
            texture_ref albedo;
        };
        struct parameters {
            texture_preserve_write target;
            texture_read depth;
            texture_read albedo;
            void visit_resources(parameter_visitor& visit) const noexcept {
                visit(target, depth, albedo);
            }
        };
        struct outputs {
            texture_ref target;
            pass_ref pass;
        };
        [[nodiscard]] inline std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
            if (in.target == in.depth || in.target == in.albedo || in.depth == in.albedo) {
                return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "toon screen rim", .resource_name = "rim alias"});
            }
            parameters params{{in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                              {in.depth, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                              {in.albedo, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}}};
            auto added = graph.add_pass("toon screen rim", std::move(params));
            if (!added) {
                return std::unexpected(added.error());
            }
            return outputs{in.target, *added};
        }
    } // namespace toon_screen_rim

    /// The goo screen rim: the same fullscreen additive shape for the goo material's rim.
    namespace goo_rim {
        struct inputs {
            texture_ref target;
            texture_ref depth;
            texture_ref albedo;
        };
        struct parameters {
            texture_preserve_write target;
            texture_read depth;
            texture_read albedo;
            void visit_resources(parameter_visitor& visit) const noexcept {
                visit(target, depth, albedo);
            }
        };
        struct outputs {
            texture_ref target;
            pass_ref pass;
        };
        [[nodiscard]] inline std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
            if (in.target == in.depth || in.target == in.albedo || in.depth == in.albedo) {
                return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "goo rim", .resource_name = "rim alias"});
            }
            parameters params{{in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                              {in.depth, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                              {in.albedo, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}}};
            auto added = graph.add_pass("goo rim", std::move(params));
            if (!added) {
                return std::unexpected(added.error());
            }
            return outputs{in.target, *added};
        }
    } // namespace goo_rim
} // namespace deren::engine::render_graph
