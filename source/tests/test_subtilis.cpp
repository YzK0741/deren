#include "vk_test.h"

#include <array>
#include <limits>
#include <span>
import deren.engine.subtilis;
import deren.engine.meshlet;
import deren.engine.visibility;
import deren.utility.shared;
namespace sub = deren::engine::subtilis;
using deren::engine::meshlet;
int main() {
    std::array<float, 16> clip{};
    clip[0] = clip[5] = clip[10] = clip[15] = 1.0f;
    std::array<meshlet, 3> rows{};
    rows[0].center_x = 0;
    rows[0].center_z = .5f;
    rows[0].first_index = 3;
    rows[1].center_x = 4;
    rows[1].center_z = .5f;
    rows[1].first_index = 6;
    rows[2].center_x = .5f;
    rows[2].center_z = .5f;
    rows[2].first_index = 9;
    deren::utility::vessel::vector<meshlet> scratch, uploaded;
    unsigned writes = 0;
    auto writer = [&](unsigned base, std::span<meshlet const> records) {
        CHECK(base == 7);
        ++writes;
        uploaded.assign(records.begin(), records.end());
        return true;
    };
    sub::reservation space{7, 3};
    auto run = sub::prepare_lod0(rows, space, clip, scratch, {}, writer);
    CHECK(run.source == sub::run_source::visible && run.base == 7 && run.count == 2);
    CHECK(run.reason == sub::fallback_reason::none && writes == 1);
    CHECK(uploaded.size() == 2 && uploaded[0].first_index == 3 && uploaded[1].first_index == 9);
    auto again = sub::prepare_lod0(rows, space, clip, scratch, {}, writer);
    CHECK(again.count == run.count && uploaded[1].first_index == 9);
    run = sub::prepare_lod0(rows, space, clip, scratch, {}, [](unsigned, auto) { return false; });
    CHECK(run.source == sub::run_source::shared && run.count == 3 && run.base == 7);
    CHECK(run.reason == sub::fallback_reason::publication_failed);
    auto no_write = [&](unsigned, auto) { CHECK(false); return true; };
    run = sub::prepare_lod0(rows, sub::reservation{7, 2}, clip, scratch, {}, no_write);
    CHECK(run.source == sub::run_source::shared && run.count == 2 && run.reason == sub::fallback_reason::source_mismatch);
    run = sub::prepare_lod0(rows, sub::reservation{65535, 3}, clip, scratch, {}, no_write);
    CHECK(run.source == sub::run_source::shared && run.reason == sub::fallback_reason::capacity);
    std::array<sub::reservation, 1> published{{{8, 1}}};
    run = sub::prepare_lod0(rows, space, clip, scratch, published, no_write);
    CHECK(run.source == sub::run_source::shared && run.reason == sub::fallback_reason::overlap);
    published[0] = {10, 3}; // exactly adjacent, valid
    run = sub::prepare_lod0(rows, space, clip, scratch, published, writer);
    CHECK(run.source == sub::run_source::visible);
    deren::utility::vessel::vector<meshlet> large(4097);
    run = sub::prepare_lod0(large, sub::reservation{0, 4097}, clip, scratch, {}, no_write);
    CHECK(run.reason == sub::fallback_reason::capacity);
    large.pop_back();
    run = sub::prepare_lod0(large, sub::reservation{61440, 4096}, clip, scratch, {}, [](unsigned, auto) { return true; });
    CHECK(run.source == sub::run_source::visible && run.count == 4096);
    for (auto& row : rows)
        row.center_x = 4;
    auto before = writes;
    run = sub::prepare_lod0(rows, space, clip, scratch, {}, writer);
    CHECK(run.source == sub::run_source::visible && run.count == 0 && run.reason == sub::fallback_reason::none);
    CHECK(writes == before && scratch.empty());
    // Sphere intersects the clip plane after nonuniform scale and shear; keep it.
    meshlet sphere{};
    sphere.center_x = 1.1f;
    sphere.center_z = .5f;
    sphere.radius = .2f;
    CHECK(sub::sphere_visible(sphere, clip));
    clip[4] = 10; // shear: x' = x + 10*y
    sphere.center_x = 2;
    sphere.radius = .11f;
    CHECK(sub::sphere_visible(sphere, clip));
    sphere.radius = 0;
    CHECK(!sub::sphere_visible(sphere, clip));
    clip[0] = 2;
    clip[4] = 0;
    sphere.center_x = .55f;
    sphere.radius = .1f;
    CHECK(sub::sphere_visible(sphere, clip));
    sphere.center_x = 0;
    sphere.center_z = -.1f;
    sphere.radius = .2f;
    CHECK(sub::sphere_visible(sphere, clip)); // near-plane crossing
    sphere.radius = -1;
    CHECK(sub::sphere_visible(sphere, clip));
    sphere.radius = std::numeric_limits<float>::quiet_NaN();
    CHECK(sub::sphere_visible(sphere, clip));
    clip[0] = std::numeric_limits<float>::infinity();
    CHECK(sub::sphere_visible(rows[0], clip));
    deren::engine::visibility::draw_item item{};
    deren::engine::visibility::push_block block{};
    deren::engine::visibility::apply_meshlet_run(item, block, {7, 2, sub::run_source::visible});
    CHECK(item.meshlet_table == 1 && item.meshlet_base == 7);
    CHECK(block.geometry.first_index == 7 && block.geometry.index_count == 2 && (block.geometry.base_vertex & 1) == 1);
    deren::engine::visibility::apply_meshlet_run(item, block, sub::shared_run({9, 3}));
    CHECK(item.meshlet_table == 0 && item.meshlet_base == 9);
    CHECK(block.geometry.first_index == 9 && block.geometry.index_count == 3 && (block.geometry.base_vertex & 1) == 0);
    return deren::vk_test::finish("test_subtilis");
}
