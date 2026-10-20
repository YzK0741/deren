#include "vk_test.h"

#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <span>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.primitive;
import deren.engine.meshlet;
import deren.engine.subtilis;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
import deren.engine.subtilis.lod;
import deren.engine.subtilis.selection;
#include "probe_backend.hpp"
namespace sub = deren::engine::subtilis;
namespace rhi = deren::promise::rhi;
namespace {
    template <class T>
    auto bytes(std::span<T> value) {
        return std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(value.data()), value.size_bytes()};
    }
    std::shared_ptr<sub::built_geometry const> source(std::uint32_t stride = 64, std::uint32_t normal = 12) {
        deren::utility::vessel::vector<std::array<float, 16>> vertices;
        deren::utility::vessel::vector<std::uint32_t> indices;
        for (unsigned y = 0; y < 17; ++y) {
            for (unsigned x = 0; x < 17; ++x) {
                vertices.push_back({float(x), float(y), 0, 0, 0, 1, float(x) / 16, float(y) / 16});
            }
        }
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                auto a = y * 17 + x;
                for (auto id : {a, a + 1, a + 17, a + 1, a + 18, a + 17}) {
                    indices.push_back(id);
                }
            }
        }
        // A nonstandard stride is a distinct CPU-only fixture, not valid raster geometry.
        auto built = sub::build({{bytes(std::span(vertices)), stride, 289, bytes(std::span(indices)), 4, 0, static_cast<unsigned>(indices.size()), 0}, normal, 24});
        return built ? std::make_shared<sub::built_geometry const>(std::move(*built)) : nullptr;
    }
} // namespace
int main() {
    auto device = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto cpu = source();
    CHECK(cpu && cpu->statistics().normal_offset == 12 && cpu->statistics().uv_offset == 24);
    if (!cpu) {
        return deren::vk_test::finish("test_subtilis_selection");
    }
    auto uploaded = sub::upload_geometry(device, cpu);
    CHECK(uploaded);
    if (!uploaded) {
        return deren::vk_test::finish("test_subtilis_selection");
    }
    deren::engine::normal_draw_primitive leaf;
    CHECK(leaf.bind_subtilis_geometry(*uploaded));
    leaf.meshlets = deren::engine::build_meshlets({cpu->vertices(), 64, 289, bytes(cpu->indices()), 4, 0, cpu->lod0_index_count(), 0});
    leaf.meshlet_count = static_cast<unsigned>(leaf.meshlets.size());
    leaf.meshlet_base = 4;
    CHECK(sub::matches_generation(leaf));
    auto projection = glm::orthoRH_ZO(-40.f, 40.f, -40.f, 40.f, .1f, 1000.f);
    auto camera = glm::mat4(1);
    camera[3][2] = -100;
    auto view = sub::make_item_view(glm::mat4(1), camera, projection, 800, 600);
    CHECK(view.selection.kind == sub::projection_kind::orthographic && std::abs(view.selection.near_plane - .1f) < 1e-5f);
    deren::utility::vessel::vector<deren::engine::meshlet> selected;
    unsigned writes = 0;
    auto publish = [&](unsigned base, auto records) { ++writes; CHECK(base == 4 && !records.empty()); return true; };
    auto run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
    CHECK(run.source == sub::run_source::visible && run.count > 0 && run.count < leaf.meshlet_count && writes == 1);
    auto saved = leaf.draw_index_count;
    --leaf.draw_index_count;
    CHECK(!sub::matches_generation(leaf));
    run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
    CHECK(run.source == sub::run_source::shared && run.reason == sub::fallback_reason::source_mismatch && writes == 1);
    leaf.draw_index_count = saved;
    sub::item_policy exact;
    exact.exact_depth_redraw = true;
    run = sub::prepare_item(leaf, exact, view, selected, {}, publish);
    CHECK(run.count == leaf.meshlet_count);
    for (unsigned flags : {16u, 32u}) {
        run = sub::prepare_item(leaf, {flags, false}, view, selected, {}, publish);
        CHECK(run.count == leaf.meshlet_count);
    }
    auto require_mismatch = [&] {
        auto before = writes;
        auto result = sub::prepare_item(leaf, {}, view, selected, {}, publish);
        CHECK(result.source == sub::run_source::shared && result.reason == sub::fallback_reason::source_mismatch && writes == before && selected.empty());
    };
    leaf.index_type = rhi::index_type::uint16;
    require_mismatch();
    leaf.index_type = rhi::index_type::uint32;
    --leaf.vertex_count;
    require_mismatch();
    ++leaf.vertex_count;
    leaf.vertex_stride = 32;
    require_mismatch();
    leaf.vertex_stride = 64;
    --leaf.meshlet_count;
    require_mismatch();
    ++leaf.meshlet_count;
    ++leaf.meshlets.back().first_index;
    require_mismatch();
    --leaf.meshlets.back().first_index;
    auto owner = leaf.vertex_buffer.lease();
    auto* buffer = leaf.vertex_buffer.get();
    leaf.vertex_buffer.bind(buffer, std::shared_ptr<void const>{uploaded->get(), [](void const*) {}});
    require_mismatch();
    leaf.vertex_buffer.bind(buffer, owner);
    auto other = sub::upload_geometry(device, cpu);
    CHECK(other);
    if (other) {
        leaf.vertex_buffer.bind((*other)->vertices(), *other);
        require_mismatch();
        leaf.vertex_buffer.bind(buffer, owner);
    }
    CHECK(sub::matches_generation(leaf));
    for (unsigned mode = 0; mode < 5; ++mode) {
        leaf.transparent = mode == 0;
        leaf.overlay_kind = mode == 1;
        leaf.push.flags = mode == 2 ? 1u : 0u;
        leaf.push.skin_base = mode == 3 ? 1u : 0u;
        leaf.push.morph_targets = mode == 4 ? 1u : 0u;
        run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
        CHECK(run.count == leaf.meshlet_count);
    }
    leaf.transparent = false;
    leaf.overlay_kind = leaf.push.flags = leaf.push.skin_base = leaf.push.morph_targets = 0;
    leaf.pipeline_name = "custom";
    run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
    CHECK(run.count == leaf.meshlet_count);
    leaf.pipeline_name = {};
    auto perspective = sub::make_item_view(glm::mat4(1), camera, glm::perspectiveRH_ZO(1.f, 1.f, 2.f, 1000.f), 800, 600);
    CHECK(perspective.selection.kind == sub::projection_kind::perspective && perspective.selection.near_plane >= 2.f && perspective.selection.near_plane < 2.00001f);
    auto invalid = sub::make_item_view(glm::mat4(1), camera, glm::mat4(0), 800, 600);
    run = sub::prepare_item(leaf, {}, invalid, selected, {}, publish);
    CHECK(run.count == leaf.meshlet_count);
    invalid = view;
    invalid.selection.width = 0;
    run = sub::prepare_item(leaf, {}, invalid, selected, {}, publish);
    CHECK(run.count == leaf.meshlet_count);
    auto hidden_camera = camera;
    hidden_camera[3][0] = -10000;
    auto hidden = sub::make_item_view(glm::mat4(1), hidden_camera, projection, 800, 600);
    auto before = writes;
    run = sub::prepare_item(leaf, {}, hidden, selected, {}, publish);
    CHECK(run.source == sub::run_source::visible && run.count == 0 && writes == before);
    std::array<sub::reservation, 1> overlaps{{{5, 1}}};
    run = sub::prepare_item(leaf, {}, view, selected, overlaps, publish);
    CHECK(run.reason == sub::fallback_reason::overlap && writes == before);
    run = sub::prepare_item(leaf, {}, view, selected, {}, [](unsigned, auto) { return false; });
    CHECK(run.source == sub::run_source::shared && run.reason == sub::fallback_reason::publication_failed && run.count == leaf.meshlet_count);
    auto alt = source(64, 0);
    CHECK(alt && alt->statistics().normal_offset == 0);
    if (alt) {
        auto alternate = sub::upload_geometry(device, alt);
        CHECK(alternate);
        if (alternate) {
            leaf.subtilis_geometry = *alternate;
            leaf.vertex_buffer.bind((*alternate)->vertices(), *alternate);
            leaf.index_buffer.bind((*alternate)->indices(), *alternate);
            run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
            CHECK(run.count == leaf.meshlet_count);
        }
    }
    leaf.subtilis_geometry.reset();
    run = sub::prepare_item(leaf, {}, view, selected, {}, publish);
    CHECK(run.source == sub::run_source::visible && run.count == leaf.meshlet_count);
    return deren::vk_test::finish("test_subtilis_selection");
}
