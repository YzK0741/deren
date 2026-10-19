#include "vk_test.h"

#include <array>
#include <limits>
#include <span>
import deren.engine.subtilis;
import deren.engine.meshlet;
import deren.engine.subtilis.asset;
import deren.engine.subtilis.lod;
import deren.utility.shared;
import deren.engine.visibility;
namespace sub = deren::engine::subtilis;
using deren::engine::meshlet;
int main() {
    std::array<meshlet, 3> clusters{};
    for (unsigned i = 0; i < 3; ++i) {
        clusters[i].first_index = i * 3;
        clusters[i].index_count = 3;
        clusters[i].center_x = i == 0 ? -.2f : (i == 1 ? .2f : 0.f);
        clusters[i].center_z = -5;
        clusters[i].radius = .1f;
    }
    std::array<sub::group_record, 3> groups{{{{0, 1}, {}, 0}, {{1, 1}, {}, 0}, {{2, 1}, {0, 2}, 1}}};
    std::array<unsigned, 1> roots{2};
    auto built = sub::asset::make({clusters, groups, roots, 6, 9});
    CHECK(built);
    if (!built) {
        return deren::vk_test::finish("test_subtilis_lod");
    }
    auto const& asset = *built;
    sub::view_parameters view{};
    view.kind = sub::projection_kind::perspective;
    view.width = view.height = 1000;
    view.projection = {};
    view.projection[0] = view.projection[5] = 1;
    view.projection[10] = -1;
    view.projection[11] = -1;
    view.projection[14] = -.1f;
    deren::utility::vessel::vector<meshlet> selected;
    auto cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2);
    CHECK(selected.size() == 2 && selected[0].first_index == 0 && selected[1].first_index == 3);
    view.target_error_px = 300;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 1 && selected[0].first_index == 6);
    view.force_finest = true;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2);
    view.force_finest = false;
    view.target_error_px = 1;
    view.view[14] = -1000;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 1 && selected[0].first_index == 6);
    view.view = sub::identity_matrix();
    cut = sub::select_cut(asset, view, 1, selected);
    CHECK(!cut && cut.error() == sub::cut_error::capacity && selected.empty());
    view.target_error_px = 300;
    view.model[0] = 4;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2);
    view.model = sub::identity_matrix();
    view.model[4] = 4;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2);
    view.model = sub::identity_matrix();
    view.target_error_px = 10000;
    view.model[14] = 4.8f;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2); // near-plane intersection forces refinement
    view.model = sub::identity_matrix();
    view.model[12] = 10000;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.empty());
    view.model[0] = std::numeric_limits<float>::quiet_NaN();
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && cut->forced_finest && selected.size() == 2);
    view.model = sub::identity_matrix();
    view.width = 0;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && cut->forced_finest && selected.size() == 2);
    view.width = 1000;
    view.kind = sub::projection_kind::orthographic;
    view.projection = sub::identity_matrix();
    view.projection[10] = -.0001f;
    view.target_error_px = 1000;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 1);
    view.view[14] = -1000;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 1); // independent of distance
    view.target_error_px = 1;
    cut = sub::select_cut(asset, view, 2, selected);
    CHECK(cut && selected.size() == 2);
    view.view = sub::identity_matrix();
    unsigned calls = 0;
    auto publish = [&](unsigned base, std::span<meshlet const> records) {++calls;CHECK(base==4 && records.size()==2);return true; };
    auto run = sub::prepare_hierarchy(asset, {4, 2}, view, selected, {}, publish);
    CHECK(run.source == sub::run_source::visible && run.base == 4 && run.count == 2 && calls == 1);
    deren::engine::visibility::draw_item row{};
    deren::engine::visibility::push_block block{};
    deren::engine::visibility::apply_meshlet_run(row, block, run);
    CHECK(row.meshlet_table == 1 && block.geometry.index_count == 2 && (block.geometry.base_vertex & 1));
    run = sub::prepare_hierarchy(asset, {4, 2}, view, selected, {}, [](unsigned, auto) { return false; });
    CHECK(run.source == sub::run_source::shared && run.count == 2 && run.reason == sub::fallback_reason::publication_failed);
    std::array<sub::reservation, 1> published{{{5, 1}}};
    run = sub::prepare_hierarchy(asset, {4, 2}, view, selected, published, publish);
    CHECK(run.reason == sub::fallback_reason::overlap && calls == 1);
    run = sub::prepare_hierarchy(asset, {4, 1}, view, selected, {}, publish);
    CHECK(run.reason == sub::fallback_reason::source_mismatch && calls == 1);
    run = sub::prepare_hierarchy(asset, {65535, 2}, view, selected, {}, publish);
    CHECK(run.reason == sub::fallback_reason::capacity && calls == 1);
    view.model[12] = 10000;
    run = sub::prepare_hierarchy(asset, {4, 2}, view, selected, {}, publish);
    CHECK(run.source == sub::run_source::visible && run.count == 0 && calls == 1);
    auto empty = sub::asset::make({});
    CHECK(empty);
    if (empty) {
        cut = sub::select_cut(*empty, view, 0, selected);
        CHECK(cut && selected.empty());
    }
    // A forest emits complete roots, in their explicit order.
    std::array<sub::group_record, 2> leaves{groups[0], groups[1]};
    std::array<unsigned, 2> forest_roots{1, 0};
    auto forest = sub::asset::make({std::span(clusters).first(2), leaves, forest_roots, 6, 6});
    CHECK(forest);
    view.model = sub::identity_matrix();
    if (forest) {
        cut = sub::select_cut(*forest, view, 2, selected);
        CHECK(cut && selected.size() == 2 && selected[0].first_index == 3 && selected[1].first_index == 0);
    }
    // A near-crossing ancestor forces finest through descendants that are safely in front.
    std::array<meshlet, 4> near_clusters{clusters[0], clusters[1], clusters[2], clusters[2]};
    near_clusters[3].first_index = 9;
    near_clusters[3].center_z = -.15f;
    std::array<sub::group_record, 4> near_groups{groups[0], groups[1], groups[2], sub::group_record{{3, 1}, {2, 1}, 5}};
    roots[0] = 3;
    auto near_asset = sub::asset::make({near_clusters, near_groups, roots, 6, 12});
    CHECK(near_asset);
    view.kind = sub::projection_kind::perspective;
    view.projection = {};
    view.projection[0] = view.projection[5] = 1;
    view.projection[10] = -1;
    view.projection[11] = -1;
    view.projection[14] = -.1f;
    view.target_error_px = 10000;
    if (near_asset) {
        cut = sub::select_cut(*near_asset, view, 2, selected);
        CHECK(cut && selected.size() == 2 && selected[0].first_index == 0);
        view.kind = sub::projection_kind::orthographic;
        view.projection = sub::identity_matrix();
        view.projection[10] = -.0001f;
        cut = sub::select_cut(*near_asset, view, 2, selected);
        CHECK(cut && cut->forced_finest && selected.size() == 2 && selected[0].first_index == 0);
    }
    // Cut traversal also stays iterative; force_finest must reach the bottom of a deep chain.
    constexpr unsigned depth = 20000;
    deren::utility::vessel::vector<meshlet> chain_clusters(depth, clusters[0]);
    deren::utility::vessel::vector<sub::group_record> chain(depth);
    for (unsigned i = 0; i < depth; ++i) {
        chain_clusters[i].first_index = i * 3;
        chain[i] = {{i, 1}, i ? sub::cluster_range{i - 1, 1} : sub::cluster_range{}, float(i)};
    }
    roots[0] = depth - 1;
    auto deep = sub::asset::make({chain_clusters, chain, roots, 3, depth * 3});
    CHECK(deep);
    if (deep) {
        view.force_finest = true;
        cut = sub::select_cut(*deep, view, 1, selected);
        CHECK(cut && cut->groups_visited == depth && selected.size() == 1 && selected[0].first_index == 0);
    }
    return deren::vk_test::finish("test_subtilis_lod");
}
