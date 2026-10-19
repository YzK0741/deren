#include "vk_test.h"

#include <array>
#include <limits>
#include <span>
#include <utility>
import deren.engine.subtilis.asset;
import deren.engine.meshlet;
import deren.utility.shared;
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
    sub::asset_description description{clusters, groups, roots, 6, 9};
    auto asset = sub::asset::make(description);
    CHECK(asset);
    if (asset) {
        CHECK(asset->lod0_count() == 2 && asset->maximum_cut() == 2);
        CHECK(asset->groups().size() == 3 && asset->roots()[0] == 2);
        auto const& sphere = asset->groups()[2].bounds;
        CHECK(sphere.radius >= .3 && sphere.center[2] == -5);
        CHECK(asset->groups()[2].worst_cut == 2);
        clusters[0].radius = 5;
        CHECK(asset->clusters()[0].radius == .1f); // owns input, no borrowed lifetime
        clusters[0].radius = .1f;
    }
    auto rejects = [&](sub::asset_error error) {auto bad=sub::asset::make(description);CHECK(!bad && bad.error()==error); };
    groups[2].children = {1, 2};
    rejects(sub::asset_error::invalid_hierarchy);
    groups[2].children = {0, 2};
    groups[1].children = {0, 1};
    rejects(sub::asset_error::invalid_hierarchy);
    groups[1].children = {};
    roots[0] = 0;
    rejects(sub::asset_error::invalid_hierarchy);
    roots[0] = 2;
    groups[1].coarse = {0, 1};
    rejects(sub::asset_error::invalid_cluster_range);
    groups[1].coarse = {1, 1};
    groups[1].coarse = {3, 1};
    rejects(sub::asset_error::invalid_cluster_range);
    groups[1].coarse = {1, 1};
    groups[2].error = -1;
    rejects(sub::asset_error::invalid_error);
    groups[2].error = 1;
    groups[0].error = .1f;
    rejects(sub::asset_error::invalid_error);
    groups[0].error = 0;
    groups[2].error = std::numeric_limits<float>::infinity();
    rejects(sub::asset_error::invalid_error);
    groups[2].error = 1;
    clusters[0].radius = -1;
    rejects(sub::asset_error::invalid_cluster);
    clusters[0].radius = .1f;
    clusters[0].center_x = std::numeric_limits<float>::quiet_NaN();
    rejects(sub::asset_error::invalid_cluster);
    clusters[0].center_x = -.2f;
    clusters[0].first_index = std::numeric_limits<unsigned>::max();
    rejects(sub::asset_error::invalid_cluster);
    clusters[0].first_index = 0;
    clusters[0].index_count = 256;
    rejects(sub::asset_error::invalid_cluster);
    clusters[0].index_count = 3;
    clusters[0].base_vertex = 1;
    rejects(sub::asset_error::invalid_cluster);
    clusters[0].base_vertex = 0;
    clusters[1].first_index = 0;
    rejects(sub::asset_error::invalid_lod0_coverage);
    clusters[1].first_index = 3;
    description.lod0_index_count = 9;
    rejects(sub::asset_error::invalid_lod0_coverage);
    description.lod0_index_count = 6;
    std::array<unsigned, 2> duplicate_roots{2, 2};
    description.roots = duplicate_roots;
    rejects(sub::asset_error::invalid_hierarchy);
    description.roots = roots;
    description.roots = {};
    rejects(sub::asset_error::invalid_hierarchy);
    description.roots = roots;
    description.groups = std::span(groups).first(2);
    rejects(sub::asset_error::invalid_cluster_range); // third cluster has no owner
    description.groups = groups;
    // A parent with more records than its complete finest representation cannot use M1's reservation.
    std::array<meshlet, 5> over{};
    for (auto& c : over) {
        c.index_count = 3;
        c.radius = 0;
    }
    over[1].first_index = 3;
    groups[2].coarse = {2, 3};
    description.clusters = over;
    rejects(sub::asset_error::capacity);
    // Three levels; child error must not decrease at its parent.
    std::array<sub::group_record, 4> three_levels{groups[0], groups[1], sub::group_record{{2, 1}, {0, 2}, 1}, sub::group_record{{3, 1}, {2, 1}, .5f}};
    std::array<unsigned, 1> top{3};
    description = {std::span(over).first(4), three_levels, top, 6, 9};
    rejects(sub::asset_error::invalid_error);
    // Restore the small valid fixture after malformed-input tests above.
    groups[2].coarse = {2, 1};
    auto movable = sub::asset::make({clusters, groups, roots, 6, 9});
    CHECK(movable);
    if (movable) {
        auto moved = std::move(*movable);
        CHECK(moved.lod0_count() == 2 && movable->lod0_count() == 0 && movable->maximum_cut() == 0);
    }
    auto empty = sub::asset::make({});
    CHECK(empty && empty->groups().empty() && empty->maximum_cut() == 0);
    // Bottom-up validation is iterative, even for a deep no-progress hierarchy.
    constexpr unsigned depth = 20000;
    deren::utility::vessel::vector<meshlet> chain_clusters(depth);
    deren::utility::vessel::vector<sub::group_record> chain(depth);
    for (unsigned i = 0; i < depth; ++i) {
        chain_clusters[i].index_count = 3;
        chain[i] = {{i, 1}, i ? sub::cluster_range{i - 1, 1} : sub::cluster_range{}, float(i)};
    }
    top[0] = depth - 1;
    auto deep = sub::asset::make({chain_clusters, chain, top, 3, 3});
    CHECK(deep && deep->maximum_cut() == 1);
    return deren::vk_test::finish("test_subtilis_asset");
}
