#pragma once
namespace deren::asset::subtilis::detail {
    // Metric evaluation may round differently across dependencies. Close all
    // decisions before scanning clusters, even when the analytic metric is monotone.
    template <typename Payload, typename Decisions>
    void close_coarsening(Payload const& p, Decisions& decisions) {
        for (decltype(p.groups.size()) id = 0; id < p.groups.size(); ++id) {
            if (!decisions[id])
                continue;
            auto const& g = p.groups[id];
            for (decltype(g.fine_count) i = 0; i < g.fine_count; ++i) {
                auto producer = p.clusters[p.references[g.fine_first + i]].producer;
                if (producer != 0xffffffffu && !decisions[producer]) {
                    decisions[id] = false;
                    break;
                }
            }
        }
    }
} // namespace deren::asset::subtilis::detail
