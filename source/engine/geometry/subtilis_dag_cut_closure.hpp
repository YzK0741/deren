#pragma once
namespace deren::engine::subtilis::dag_lod_detail {
    template <class Geometry, class Decisions>
    void close_coarsening(Geometry const& geometry, Decisions& decisions) {
        auto groups = geometry.groups();
        auto clusters = geometry.clusters();
        auto references = geometry.references();
        for (decltype(groups.size()) id = 0; id < groups.size(); ++id) {
            if (!decisions[id])
                continue;
            auto const& g = groups[id];
            for (auto c : references.subspan(g.fine.first, g.fine.count)) {
                auto producer = clusters[c].producer;
                if (producer != 0xffffffffu && !decisions[producer]) {
                    decisions[id] = false;
                    break;
                }
            }
        }
    }
} // namespace deren::engine::subtilis::dag_lod_detail
