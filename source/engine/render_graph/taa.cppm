module;
#include <array>
#include <cstddef>
#include <expected>
#include <utility>
export module deren.engine.render_graph.taa;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::taa {
    /// The temporal resolve's declarations, split into the TWO graph passes the model forces.
    ///
    /// One pass cannot hold both halves: the resolve writes the HDR target as a colour attachment and the
    /// history copy then reads that same image as a transfer source, which is two accesses to one resource
    /// with different scopes - the graph refuses that as `conflicting_access`. The pass object still records
    /// both halves (`taa_pass::record_content` and `taa_pass::record_copy`); the graph just sees two passes.
    struct resolve_inputs {
        texture_ref scene_color; // the shaded image the lighting pass wrote (sampled)
        texture_ref velocity;    // motion vectors (sampled)
        texture_ref depth;       // G-buffer depth, for the disocclusion guard (sampled)
        texture_ref history;     // the PREVIOUS resolve's history (sampled; contents are never discarded)
        texture_ref target;      // the HDR image this resolve writes
    };
    struct resolve_parameters {
        std::array<texture_read, 4> inputs;
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(inputs, target);
        }
    };
    struct resolve_outputs {
        texture_ref target;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<resolve_outputs, graph_error> append_resolve(builder& graph, resolve_inputs const& in) noexcept {
        std::array refs{in.scene_color, in.velocity, in.depth, in.history, in.target};
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (refs[i] == refs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "TAA resolve", .resource_name = "resolve alias"});
                }
            }
        }
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::fragment_shader};
        resolve_parameters params{{{{in.scene_color, {}, sampled}, {in.velocity, {}, sampled}, {in.depth, {}, sampled}, {in.history, {}, sampled}}},
                                  {in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}}};
        auto added = graph.add_pass("TAA resolve", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return resolve_outputs{in.target, *added};
    }
    /// The history copy: the HDR target out through a transfer read, the history image written through a
    /// transfer write. Its own pass because the resolve already owns another access to the target.
    struct copy_inputs {
        texture_ref source;  // the HDR target the resolve wrote
        texture_ref history; // the history image; written wholesale by the copy
    };
    struct copy_parameters {
        texture_read source;
        texture_discard_write history;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(source, history);
        }
    };
    struct copy_outputs {
        texture_ref history;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<copy_outputs, graph_error> append_history_copy(builder& builder_graph, copy_inputs const& in) noexcept {
        if (in.source == in.history) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "TAA history copy", .resource_name = "copy alias"});
        }
        constexpr access_scope read{resource_usage::transfer_source, pipeline_stage::transfer};
        constexpr access_scope write{resource_usage::transfer_destination, pipeline_stage::transfer};
        copy_parameters params{{in.source, {}, read}, {in.history, {}, write}};
        auto added = builder_graph.add_pass("TAA history copy", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return copy_outputs{in.history, *added};
    }
} // namespace deren::engine::render_graph::taa
