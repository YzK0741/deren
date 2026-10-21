#include "../asset/subtilis/cut_closure.hpp"
#include "vk_test.h"

#include <cstdint>
#include <limits>
import deren.asset.subtilis.dag;
#include "subtilis_dag_fixtures.hpp"
namespace sub = deren::asset::subtilis;
using subtilis_fixture::cross_dag;

int main() {
    auto input = cross_dag();
    // Deliberately inconsistent raw metric decisions, as if equality rounded
    // differently: both consumers depend on both lower groups and must close.
    sub::vector<bool> decisions{false, true, true, true};
    sub::detail::close_coarsening(input.data, decisions);
    CHECK(decisions == sub::vector<bool>({false, true, false, false}));
    sub::detail::close_coarsening(input.data, decisions);
    CHECK(decisions == sub::vector<bool>({false, true, false, false}));
    CHECK(sub::validate_structure(input.data, {}));
    auto asset = sub::make_asset(input.source, input.data, {});
    CHECK(asset);
    if (!asset)
        return deren::vk_test::finish("subtilis_dag");
    sub::cut_parameters p;
    auto want = [&](sub::vector<uint32_t> expected) { auto cut=sub::select_cut(*asset,p); CHECK(cut && *cut==expected); };
    p.threshold = 0;
    want({0, 1, 2, 3});
    p.threshold = std::nextafter(.1f, 0.f);
    want({0, 1, 2, 3});
    p.threshold = .1f;
    want({4, 5, 6, 7});
    p.threshold = std::nextafter(.2f, 0.f);
    want({4, 5, 6, 7});
    p.threshold = .2f;
    want({8, 9});
    p.max_clusters = 2;
    want({8, 9});
    p.threshold = 0;
    CHECK(!sub::select_cut(*asset, p));
    auto reject = [&](auto mutate) { auto bad=cross_dag(); mutate(bad); CHECK(!sub::make_asset(std::move(bad.source),std::move(bad.data),{})); };
    reject([](auto& x) { x.data.clusters[4].consumer = 3; });
    reject([](auto& x) { x.data.references[7] = 4; });
    reject([](auto& x) { x.data.roots.pop_back(); });
    reject([](auto& x) { x.data.clusters[0].producer = 2; });
    reject([](auto& x) { x.data.groups[2].error = .05f; });
    reject([](auto& x) { x.data.groups[2].lod.radius = 3.99f; });
    reject([](auto& x) { x.data.clusters[0].geometry.radius = 1.4f; });
    reject([](auto& x) { std::swap(x.data.indices[0], x.data.indices[1]); });                                                     // leaf winding
    reject([](auto& x) { x.data.indices[x.data.clusters[8].first_index] = x.data.indices[x.data.clusters[8].first_index + 1]; }); // broken boundary/degenerate face
    reject([](auto& x) { x.source.positions[0][0] = .01f; });                                                                     // stale source binding
    reject([](auto& x) { x.data.groups[0].fine_count = UINT32_MAX; });
    reject([](auto& x) { x.data.clusters[0].index_count = UINT32_MAX; });
    reject([](auto& x) { x.data.leaves[1] = 0; });
    reject([](auto& x) { x.data.references.push_back(UINT32_MAX); }); // even unused reference cells carry cluster IDs
    reject([](auto& x) { x.data.groups[0].error = std::numeric_limits<float>::quiet_NaN(); });
    reject([](auto& x) { for(auto& c:x.data.clusters) c.geometry.radius=std::sqrt(2.f); }); // f32 sqrt rounds inward
    auto outward = cross_dag();
    for (auto& c : outward.data.clusters)
        c.geometry.radius = std::nextafter(std::sqrt(2.f), std::numeric_limits<float>::infinity());
    CHECK(sub::make_asset(std::move(outward.source), std::move(outward.data), {}));

    // Break symmetric errors to make an independently derived mixed cut.
    auto mixed = cross_dag();
    mixed.data.groups[3].error = .3f;
    auto mixed_asset = sub::make_asset(std::move(mixed.source), std::move(mixed.data), {});
    CHECK(mixed_asset);
    if (mixed_asset) {
        p = {};
        p.threshold = .2f;
        auto cut = sub::select_cut(*mixed_asset, p);
        CHECK(cut && *cut == sub::vector<uint32_t>({5, 7, 8}));
    }

    p = {};
    p.mode = sub::cut_mode::screen_error;
    p.extent = {1000, 1000};
    p.near_plane = .1f;
    p.model[14] = -10;
    p.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.001001f, -1, 0, 0, -.1001001f, 0};
    p.threshold = 9;
    want({4, 5, 6, 7}); // .1*500/(10-4)=8.333.., .2→16.666..
    p.threshold = 17;
    want({8, 9});
    p.model[0] = 2;
    p.threshold = 17;
    want({0, 1, 2, 3}); // conservative distance 10-8, .1*2*500/2=50
    p.model[0] = 1;
    p.model[14] = -4;
    p.threshold = 10000;
    want({0, 1, 2, 3}); // near-plane crossing
    p.model[14] = -10;
    p.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -.1001001f, 0, 0, 0, -.01001001f, 1};
    p.threshold = 51;
    want({4, 5, 6, 7});
    p.threshold = 101;
    want({8, 9});
    p.view[0] = std::numeric_limits<float>::quiet_NaN();
    want({0, 1, 2, 3});
    p.max_clusters = 3;
    CHECK(!sub::select_cut(*asset, p));
    p = {};
    p.mode = sub::cut_mode::screen_error;
    p.extent = {1000, 1000};
    p.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.001001f, -1, 0, 0, -.1001001f, 0};
    p.threshold = 5;
    uint32_t camera_seed = 12345;
    for (unsigned i = 0; i < 128; ++i) {
        camera_seed = 1664525u * camera_seed + 1013904223u;
        unsigned distance = camera_seed % 80;
        p.model[14] = -float(distance);
        // Hand-derived integer-distance intervals; f32 .1/.2 sit above the exact decimals.
        want(distance >= 25 ? sub::vector<uint32_t>{8, 9} : distance >= 15 ? sub::vector<uint32_t>{4, 5, 6, 7}
                                                                           : sub::vector<uint32_t>{0, 1, 2, 3});
    }
    p.projection[11] = 1;
    want({0, 1, 2, 3}); // unsupported handedness
    p = {};
    p.threshold = std::numeric_limits<float>::infinity();
    CHECK(!sub::select_cut(*asset, p));
    // Owned lifetime: destroy caller copies, then select using the immutable asset.
    input = {};
    p = {};
    p.threshold = .2f;
    want({8, 9});
    uint32_t seed = 12345;
    for (unsigned i = 0; i < 128; ++i) {
        seed = 1664525u * seed + 1013904223u;
        p.threshold = float(seed % 400) / 1000;
        want(p.threshold < .1f ? sub::vector<uint32_t>{0, 1, 2, 3} : p.threshold < .2f ? sub::vector<uint32_t>{4, 5, 6, 7}
                                                                                       : sub::vector<uint32_t>{8, 9});
    }
    return deren::vk_test::finish("subtilis_dag");
}
