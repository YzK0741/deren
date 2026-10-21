#include "vk_test.h"

#include <array>
#include <limits>
import deren.engine.subtilis.dag_asset;
namespace sub = deren::engine::subtilis;
int main() {
    std::array<sub::dag_cluster, 10> clusters{};
    uint32_t offset = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        auto& c = clusters[i];
        c.record.first_index = offset;
        c.record.index_count = i < 4 ? 18 : i < 8 ? 12
                                                  : 18;
        offset += c.record.index_count;
        c.record.center_x = 1;
        c.record.center_y = 1;
        c.record.radius = 2;
        c.producer = i < 4 ? sub::dag_none : i < 8 ? (i - 4) / 2
                                                   : i - 6;
        c.consumer = i < 4 ? i / 2 : i < 8 ? 2 + i % 2
                                           : sub::dag_none;
    }
    std::array<sub::dag_group, 4> groups{{{{0, 2}, {2, 2}, {1, 1, 0, 4}, .1f}, {{4, 2}, {6, 2}, {1, 1, 0, 4}, .1f}, {{8, 2}, {10, 1}, {1, 1, 0, 4}, .2f}, {{11, 2}, {13, 1}, {1, 1, 0, 4}, .2f}}};
    std::array<uint32_t, 14> references{0, 1, 4, 5, 2, 3, 6, 7, 4, 6, 8, 5, 7, 9};
    std::array<uint32_t, 4> leaves{0, 1, 2, 3};
    std::array<uint32_t, 2> roots{8, 9};
    auto make = [&]() { return sub::dag_asset::make(clusters, groups, references, leaves, roots, 72, offset); };
    auto asset = make();
    CHECK(asset);
    if (asset) {
        CHECK(asset->clusters().size() == 10 && asset->groups().size() == 4);
        CHECK(asset->lod0_clusters().size() == 4 && asset->lod0_index_count() == 72);
        CHECK(asset->maximum_selected_bound() >= 4);
        CHECK(asset->clusters()[4].consumer == 2 && asset->clusters()[5].consumer == 3);
        clusters[0].record.radius = 8;
        CHECK(asset->clusters()[0].record.radius == 2);
        clusters[0].record.radius = 2;
        auto again = make();
        CHECK(again && asset->groups()[0] == again->groups()[0] && asset->clusters()[0] == again->clusters()[0]);
    }
    auto rejects = [&]() { CHECK(!make()); };
    clusters[4].producer = 2;
    rejects();
    clusters[4].producer = 0;
    clusters[0].consumer = 4;
    rejects();
    clusters[0].consumer = 0;
    references[1] = 0;
    rejects();
    references[1] = 1;
    references[13] = UINT32_MAX;
    rejects();
    references[13] = 9;
    groups[3].coarse = {14, 1};
    rejects();
    groups[3].coarse = {13, 1};
    groups[0].coarse = {1, 2};
    rejects();
    groups[0].coarse = {2, 2};
    leaves[3] = 2;
    rejects();
    leaves[3] = 3;
    roots[1] = 8;
    rejects();
    roots[1] = 9;
    clusters[1].record.first_index = 0;
    rejects();
    clusters[1].record.first_index = 18;
    clusters[9].record.first_index = offset;
    rejects();
    clusters[9].record.first_index = offset - 18;
    clusters[0].record.index_count = 17;
    rejects();
    clusters[0].record.index_count = 18;
    clusters[0].record.base_vertex = -1;
    rejects();
    clusters[0].record.base_vertex = 0;
    clusters[0].record.radius = -1;
    rejects();
    clusters[0].record.radius = 2;
    groups[0].lod.radius = -1;
    rejects();
    groups[0].lod.radius = 4;
    groups[0].lod.x = std::numeric_limits<float>::infinity();
    rejects();
    groups[0].lod.x = 1;
    groups[2].error = -1;
    rejects();
    groups[2].error = .2f;
    groups[2].error = std::numeric_limits<float>::quiet_NaN();
    rejects();
    groups[2].error = .2f;
    groups[2].error = .05f;
    rejects();
    groups[2].error = .2f;
    groups[2].lod.radius = 3;
    rejects();
    groups[2].lod.radius = 4;
    clusters[0].record.center_x = std::numeric_limits<float>::infinity();
    rejects();
    clusters[0].record.center_x = 1;
    clusters[0].record.first_index = UINT32_MAX - 1;
    rejects();
    auto overflow = make();
    CHECK(!overflow && overflow.error() == sub::dag_error::capacity);
    clusters[0].record.first_index = 0;
    clusters[8].record.index_count = 24;
    rejects();
    clusters[8].record.index_count = 18;
    groups[2].error = .1f;
    CHECK(make());
    groups[2].error = .2f;
    // Cluster geometry spheres may be looser than the LOD sphere; full offline
    // validation covers actual vertices, so a larger overestimate stays valid.
    clusters[0].record.radius = 5;
    CHECK(make());
    clusters[0].record.radius = 2;
    clusters[0].record.center_x = 100;
    clusters[0].record.radius = 101;
    CHECK(make()); // both conservative bounds can contain the same vertices
    clusters[0].record.center_x = 1;
    clusters[0].record.radius = 2;
    std::swap(leaves[0], leaves[3]);
    auto reordered = make();
    CHECK(reordered);
    if (reordered)
        CHECK(reordered->lod0_clusters()[0].first_index == 0);
    // The asset layer does not impose the packed item's 4096-record admission
    // limit. Runtime import makes that separate fallback decision later.
    deren::utility::vessel::vector<sub::dag_cluster> many(4097);
    deren::utility::vessel::vector<uint32_t> ids;
    for (uint32_t i = 0; i < many.size(); ++i) {
        many[i].record.first_index = i * 3;
        many[i].record.index_count = 3;
        ids.push_back(i);
    }
    auto large = sub::dag_asset::make(many, {}, {}, ids, ids, 4097 * 3, 4097 * 3);
    CHECK(large && large->lod0_clusters().size() == 4097 && large->maximum_selected_bound() == 4097);
    return deren::vk_test::finish("subtilis_dag_asset");
}
