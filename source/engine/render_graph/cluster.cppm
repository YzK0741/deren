module;
#include <expected>
#include <utility>
export module deren.engine.render_graph.cluster;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::cluster {
    /// The clustered-light sort's two per-slot outputs.
    ///
    /// `counts` is the bin count table: the dispatch ATOMICALLY increments it, and its zeroes come from the
    /// frame-slot reset (a host write on the coherent mapped buffer) rather than from this pass - so the
    /// declaration preserves the contents rather than discarding them. `indices` is the light-list table,
    /// written wholesale, so its contents start undefined.
    struct inputs {
        buffer_ref counts, indices;
    };
    struct parameters {
        buffer_read_write counts;
        buffer_discard_write indices;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(counts, indices);
        }
    };
    struct outputs {
        pass_ref pass;
    };
    /// The producer's half of the cluster migration: declared here, the graph orders the sort's writes
    /// against every consumer that declares the same buffers as reads (the deferred lighting stage does).
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        constexpr access_scope compute{resource_usage::storage, pipeline_stage::compute_shader};
        parameters params{{in.counts, {}, compute}, {in.indices, {}, compute}};
        auto added = graph.add_pass("clustered light sort", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{*added};
    }
} // namespace deren::engine::render_graph::cluster
