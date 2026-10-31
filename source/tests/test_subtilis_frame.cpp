#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>
#include <memory>
#include <span>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.runtime;
import deren.engine.primitive;
import deren.engine.scene_tree;
import deren.engine.meshlet;
import deren.engine.pass.character_forward;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.dag_import;
import deren.asset.subtilis.dag;
import deren.engine.visibility;
import deren.engine.visibility.software;
import deren.engine.render_layout;
import deren.engine.render_resource;
import deren.engine.pass;
import deren.engine.pass.chain;
import deren.engine.pass.output_targets;
import deren.engine.pass.scene;
import deren.engine.pass.shadow;
import deren.engine.pass.post;
import deren.engine.pass.visibility_raster;
import deren.engine.pass.visibility_resolve;
import deren.engine.pass.hzb_build;
import deren.engine.pass.hzb_cull;
#include "probe_backend.hpp"
#include "subtilis_dag_fixtures.hpp"
namespace sub = deren::engine::subtilis;
namespace rhi = deren::promise::rhi;
namespace pass = deren::engine::pass;
namespace rr = deren::engine::render_resource;
namespace vis = deren::engine::visibility;
namespace layout = deren::engine::render_layout;
namespace {
    struct pipeline final : rhi::pipeline {
        pipeline()
            : rhi::pipeline("subtilis frame probe") {
        }
        void release() noexcept override {
        }
    };
    struct screen_pass final : pass::frame_pass {
        bool composite;
        pipeline state;
        explicit screen_pass(bool value)
            : composite(value) {
        }
        rr::pass_io const& io() const noexcept override {
            return composite ? rr::post_composite_io : rr::deferred_io;
        }
        pass::behaviour const& behaviour() const noexcept override {
            static pass::behaviour value{};
            return value;
        }
        std::string_view feature() const noexcept override {
            return {};
        }
        void create(pass::pass_context const&) override {
        }
        void on_swapchain_recreated(pass::pass_host const&) override {
        }
        rhi::error record_content(pass::resolved_io const&) noexcept override {
            return rhi::error::ok;
        }
        rhi::pipeline* pipeline_handle() const noexcept override {
            return const_cast<pipeline*>(&state);
        }
        pass::owned_pipeline named_pipeline(std::string_view name) const noexcept override {
            return composite && name == "post_hdr" ? pass::owned_pipeline{.contract = const_cast<pipeline*>(&state)} : pass::owned_pipeline{};
        }
    };
    std::shared_ptr<sub::built_geometry const> source() {
        deren::utility::vessel::vector<std::array<float, 20>> vertices;
        deren::utility::vessel::vector<std::uint32_t> indices;
        for (unsigned y = 0; y < 17; ++y) {
            for (unsigned x = 0; x < 17; ++x) {
                vertices.push_back({(float(x) - 8) * .1f, (float(y) - 8) * .1f, 0, 0, 0, 1, float(x) / 16, float(y) / 16});
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
        auto v = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(vertices.data()), vertices.size() * 80};
        auto i = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(indices.data()), indices.size() * 4};
        auto built = sub::build({{v, 80, 289, i, 4, 0, static_cast<unsigned>(indices.size()), 0}});
        return built ? std::make_shared<sub::built_geometry const>(std::move(*built)) : nullptr;
    }
} // namespace
int exercise(bool adopted) {
    auto cpu = source();
    if (adopted) {
        auto input = subtilis_fixture::cross_dag();
        deren::utility::vessel::vector<std::array<float, 20>> vertices(input.source.positions.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            std::memcpy(vertices[i].data(), input.source.positions[i].data(), 12);
            std::memcpy(vertices[i].data() + 3, input.source.normals[i].data(), 12);
            std::memcpy(vertices[i].data() + 6, input.source.uv0[i].data(), 8);
        }
        auto payload = deren::asset::subtilis::encode(input.data, {});
        CHECK(payload);
        if (payload) {
            auto bytes = [](auto const& values) { return std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(values.data()), values.size() * sizeof(values[0])); };
            auto asset = sub::import_payload({bytes(*payload), 1, "shared-vertices", true, true, bytes(vertices), 80, static_cast<uint32_t>(vertices.size()), bytes(input.source.indices), 4, static_cast<uint32_t>(input.source.indices.size())});
            CHECK(asset);
            if (asset) {
                auto built = sub::built_geometry::adopt_imported(std::make_shared<sub::imported_asset const>(std::move(*asset)));
                CHECK(built);
                if (built)
                    cpu = std::make_shared<sub::built_geometry const>(std::move(*built));
            }
        }
    }
    CHECK(cpu && (adopted ? cpu->imported_payload() : cpu->statistics().depth > 0));
    if (!cpu) {
        return deren::vk_test::finish("test_subtilis_frame");
    }
    rhi::create_info options{};
    options.window_width = options.window_height = 32;
    auto core = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
    deren::vk_test::probe_graphics_enabled(*core, true);
    deren::engine::scene_tree::scene scene;
    pass::pass_chain chain;
    chain.emplace<pass::output_targets_pass>();
    auto& scene_pass = chain.emplace<pass::scene_pass>();
    auto& character = chain.emplace<pass::character_forward_pass>();
    chain.emplace<pass::visibility_raster_pass>();
    chain.emplace<pass::visibility_resolve_pass>();
    chain.emplace<pass::hzb_build_pass>();
    chain.emplace<pass::hzb_cull_pass>();
    chain.emplace<screen_pass>(false);
    chain.emplace<screen_pass>(true);
    for (unsigned i = 0; i < 4; ++i) {
        chain.emplace<pass::post_bloom_pass>(i);
    }
    deren::engine::runtime runtime{core, options};
    runtime.set_scene(scene);
    runtime.camera.distance = adopted ? 2000 : 20;
    struct context {
        pass::scene_pass* scene;
        pass::character_forward_pass* character;
    } context{&scene_pass, &character};
    deren::engine::runtime::chain_wiring wiring{};
    wiring.owner = &context;
    wiring.feature_active = [](void*, auto const& facts, std::string_view name) { return (name == "scene" && facts.gbuffer_pipeline) || (name == "character_forward" && facts.character_forward_pending); };
    wiring.prepare = [](void* owner, auto const& services, std::string_view name) {
        if (name == "scene") {
            static_cast<struct context*>(owner)->scene->set_frame(services.make_scene_frame(services.owner));
        } else if (name == "character_forward") {
            static_cast<struct context*>(owner)->character->set_frame(services.make_character_forward_frame(services.owner));
        }
    };
    runtime.set_pass_chain(chain, wiring);
    std::array<std::uint8_t, 4> shader{3, 2, 35, 7};
    runtime.register_shader("visibility.mesh.spv", shader);
    runtime.register_shader("visibility.frag.spv", shader);
    runtime.register_shader("visibility_resolve.comp.spv", shader);
    runtime.register_shader("hzb_build.comp.spv", shader);
    runtime.register_shader("hzb_cull.comp.spv", shader);
    runtime.create_passes();
    CHECK(runtime.make_pipeline("pbr", shader, shader, shader));
    CHECK(runtime.make_gbuffer_pipeline(shader, shader, shader));
    CHECK(runtime.visibility_available());
    CHECK(runtime.make_character_forward_pipeline(deren::engine::runtime::character_forward_pipeline_name, shader, shader, shader));
    deren::engine::primitive_create_info info;
    info.subtilis_geometry = cpu;
    auto* leaf = runtime.make_primitive("pbr", info);
    CHECK(leaf && leaf->meshlet_count == cpu->lod0_clusters().size());
    if (!leaf) {
        return deren::vk_test::finish("test_subtilis_frame");
    }
    runtime.set_visibility_buffer(true);
    runtime.set_subtilis(true);
    auto row_at = [&](vis::push_block const& block, unsigned item) {
        auto bytes = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::visibility_items));
        auto offset = (std::size_t(block.frame_slot) * vis::id_item_capacity + item) * sizeof(vis::draw_item);
        vis::draw_item row{};
        CHECK(offset + sizeof(row) <= bytes.size());
        if (offset + sizeof(row) <= bytes.size()) {
            std::memcpy(&row, bytes.data() + offset, sizeof(row));
        }
        return row;
    };
    unsigned hzb_observations = 0;
    auto record = [&] {
        CHECK(runtime.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(runtime.begin_recording() == deren::engine::frame_status::proceed);
        runtime.record_main_drawcalls();
        CHECK(runtime.end_recording() == deren::engine::frame_status::proceed);
        auto pushed = deren::vk_test::probe_last_push(*core, sizeof(vis::push_block));
        CHECK(pushed.size() == sizeof(vis::push_block));
        vis::push_block block{};
        if (pushed.size() == sizeof(block)) {
            std::memcpy(&block, pushed.data(), sizeof(block));
        }
        auto row = row_at(block, block.item_index);
        auto items = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::hzb_items));
        auto item_offset = (size_t(block.frame_slot) * pass::hzb_cull_item_capacity + block.item_index) * sizeof(pass::hzb_item);
        if (adopted && row.meshlet_table == 2 && item_offset + sizeof(pass::hzb_item) <= items.size()) {
            pass::hzb_item item{};
            std::memcpy(&item, items.data() + item_offset, sizeof(item));
            ++hzb_observations;
            CHECK(item.candidate_count == block.geometry.index_count && item.candidate_count > 0);
        }
        CHECK(row.meshlet_table == unsigned(block.geometry.base_vertex));
        CHECK(row.meshlet_base == block.geometry.first_index && row.index_width == block.geometry.index_width);
        auto commands = deren::vk_test::probe_last_indirect(*core);
        CHECK(commands.groups_x == block.geometry.index_count && commands.groups_y == 1 && commands.groups_z == 1);
        CHECK(runtime.submit_and_present() == deren::engine::frame_status::proceed);
        return block;
    };
    auto coarse = record();
    CHECK(coarse.geometry.index_count > 0 && coarse.geometry.index_count < leaf->meshlet_count);
    CHECK(row_at(coarse, 0).meshlet_table == 1);
    CHECK(leaf->draw_index_count == cpu->lod0_index_count()); // shadow/BLAS always see the prefix
    auto selected_bytes = [&](vis::push_block const& block) {
        auto bytes = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::meshlet_culled));
        auto offset = (std::size_t(block.frame_slot) * deren::engine::meshlet_capacity + leaf->meshlet_base) * sizeof(deren::engine::meshlet);
        auto size = std::size_t(block.geometry.index_count) * sizeof(deren::engine::meshlet);
        CHECK(offset + size <= bytes.size());
        return offset + size <= bytes.size() ? bytes.subspan(offset, size) : std::span<std::byte>{};
    };
    auto first = selected_bytes(coarse);
    deren::utility::vessel::vector<std::byte> saved(first.begin(), first.end());
    bool appended = false;
    for (unsigned i = 0; i < coarse.geometry.index_count; ++i) {
        deren::engine::meshlet meshlet{};
        std::memcpy(&meshlet, first.data() + i * sizeof(meshlet), sizeof(meshlet));
        appended |= meshlet.first_index >= cpu->lod0_index_count();
    }
    CHECK(appended);
    auto material = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::materials));
    auto* flags = material.data() + std::size_t(leaf->push.material_index.value) * sizeof(deren::engine::material_record) + offsetof(deren::engine::material_record, flags);
    unsigned mask = 16;
    std::memcpy(flags, &mask, sizeof(mask));
    auto masked = record();
    CHECK(masked.frame_slot != coarse.frame_slot);
    CHECK(masked.geometry.index_count == leaf->meshlet_count);
    CHECK(row_at(masked, 0).meshlet_table == 1);
    auto old = selected_bytes(coarse);
    CHECK(old.size() == saved.size() && std::memcmp(old.data(), saved.data(), saved.size()) == 0);
    // MASK geometry must remain entirely hardware even when the software threshold accepts every sphere.
    runtime.register_shader("software_visibility.comp.spv", shader);
    runtime.register_shader("software_merge.vert.spv", shader);
    runtime.register_shader("software_merge.frag.spv", shader);
    runtime.set_software_raster_threshold_px(4096.0f);
    runtime.set_software_raster(true);
    CHECK(runtime.pace_and_acquire() == deren::engine::frame_status::proceed);
    CHECK(runtime.begin_recording() == deren::engine::frame_status::proceed);
    runtime.record_main_drawcalls();
    CHECK(runtime.end_recording() == deren::engine::frame_status::proceed);
    auto tiles = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::software_tiles));
    auto const software_lane = (1u - masked.frame_slot) * deren::engine::visibility::software::software_tiles_bytes(32u, 32u);
    CHECK(tiles.size() >= software_lane + 24u);
    if (tiles.size() >= software_lane + 24u) {
        std::uint32_t clusters = UINT32_MAX;
        std::memcpy(&clusters, tiles.data() + software_lane + 16u, 4u);
        CHECK(clusters == 0u);
        std::array<std::uint32_t, 2> grid{};
        std::memcpy(grid.data(), tiles.data() + software_lane, 8u);
        CHECK(grid[0] == 2u && grid[1] == 2u); // active empty software frame, not a silent whole-frame refusal
    }
    CHECK(runtime.submit_and_present() == deren::engine::frame_status::proceed);
    runtime.set_software_raster(false);

    mask = 0;
    std::memcpy(flags, &mask, sizeof(mask));
    runtime.set_character_forward(true);
    auto exact = record(); // ordinary PBR is re-shaded too: no toon-family exemption
    CHECK(exact.geometry.index_count == leaf->meshlet_count);
    runtime.set_character_forward(false);
    leaf->environment = true;
    auto background = record();
    CHECK(background.geometry.index_count > 0 && background.geometry.index_count < leaf->meshlet_count);
    leaf->environment = false;
    runtime.set_subtilis(false);
    auto disabled = record();
    CHECK(disabled.geometry.index_count == leaf->meshlet_count && row_at(disabled, 0).meshlet_table == 0);
    runtime.set_subtilis(true);
    auto generation = leaf->subtilis_geometry;
    leaf->subtilis_geometry.reset();
    auto legacy = record();
    CHECK(legacy.geometry.index_count == leaf->meshlet_count && row_at(legacy, 0).meshlet_table == (adopted ? 2u : 1u));
    leaf->subtilis_geometry = generation;
    --leaf->draw_index_count;
    auto mismatch = record();
    CHECK(mismatch.geometry.index_count == leaf->meshlet_count && row_at(mismatch, 0).meshlet_table == 0);
    ++leaf->draw_index_count;
    if (adopted) {
        runtime.set_subtilis_error_px(.000001f);
        CHECK(record().geometry.index_count == leaf->meshlet_count);
        runtime.set_subtilis_error_px(1.f);
        CHECK(record().geometry.index_count == coarse.geometry.index_count);
        runtime.set_subtilis_hysteresis(true);
        CHECK(record().geometry.index_count == coarse.geometry.index_count);
        // Warm just below the coarsen boundary, then enter the hysteresis band
        // without crossing the camera-cut gate. Reuse the exact object address.
        runtime.camera.yaw = runtime.camera.pitch = 0;
        runtime.camera.target = {1, 1, 0};
        runtime.camera.distance = 2000;
        auto camera = deren::engine::make_orbit_camera_ubo(0, 0, 2000, {1, 1, 0}, 100, 1);
        float target = .2f * camera.proj[0][0] * 16 / (2000 - 4) / .8f * 1.000001f;
        runtime.set_subtilis_error_px(target);
        CHECK(record().geometry.index_count == 2);
        runtime.camera.distance -= .01f;
        CHECK(record().geometry.index_count == 2);
        auto base = leaf->meshlet_base;
        auto material_id = leaf->push.material_index;
        auto lo = leaf->local_aabb_min, hi = leaf->local_aabb_max;
        auto* storage = static_cast<deren::engine::normal_draw_primitive*>(leaf);
        std::destroy_at(storage);
        std::construct_at(storage);
        CHECK(storage->bind_subtilis_geometry(generation)); // generation remains alive elsewhere
        storage->meshlets.assign(cpu->lod0_clusters().begin(), cpu->lod0_clusters().end());
        storage->meshlet_base = base;
        storage->meshlet_count = static_cast<uint32_t>(storage->meshlets.size());
        storage->push.material_index = material_id;
        storage->has_bounds = true;
        storage->local_aabb_min = lo;
        storage->local_aabb_max = hi;
        CHECK(record().geometry.index_count == 4); // replacement starts with fresh decisions
        runtime.scene_changed();
        CHECK(record().geometry.index_count == 4);
        runtime.set_subtilis_hysteresis(false);
        runtime.set_subtilis_error_px(1.f);
    }
    auto* alias = runtime.make_primitive("pbr", info);
    CHECK(alias != nullptr);
    if (alias) {
        auto lo = alias->local_aabb_min, hi = alias->local_aabb_max;
        auto count = alias->meshlet_count;
        alias->local_aabb_min = glm::vec3(1e8f);
        alias->local_aabb_max = glm::vec3(1e8f + 10);
        alias->meshlet_count = vis::id_meshlet_capacity + 1;
        auto culled = record(); // invalid run would reject the frame if it reached planning/admission
        CHECK(culled.item_index == 0 && row_at(culled, 0).meshlet_table == (adopted ? 2u : 1u));
        alias->local_aabb_min = lo;
        alias->local_aabb_max = hi;
        alias->meshlet_count = count;
        runtime.scene_changed();
        alias->meshlet_base = leaf->meshlet_base;
        auto conflict = record();
        CHECK(conflict.item_index == 1 && conflict.geometry.index_count == leaf->meshlet_count);
        CHECK(row_at(conflict, 0).meshlet_table == 1 && row_at(conflict, 1).meshlet_table == 0);
    }
    deren::utility::wait_log_all();
    if (adopted)
        CHECK(hzb_observations > 0);
    if (adopted) {
        // Use the real upload cursor, not the pure admission helper. The large
        // filler is offscreen; only the overflowing object must reject the frame.
        uint32_t used = 0;
        for (auto const& root : scene.roots)
            used += static_cast<deren::engine::primitive*>(root.primitive_leaf.get())->meshlet_count;
        std::array<std::array<float, 20>, 3> vertices{};
        vertices[1][0] = vertices[2][1] = 1;
        deren::utility::vessel::vector<uint32_t> indices(size_t(deren::engine::meshlet_capacity - used - 1) * deren::engine::meshlet_max_indices);
        for (size_t i = 0; i < indices.size(); ++i)
            indices[i] = static_cast<uint32_t>(i % 3);
        deren::engine::primitive_create_info raw{};
        raw.vertex_data = {reinterpret_cast<uint8_t const*>(vertices.data()), sizeof(vertices)};
        raw.vertex_stride = 80;
        raw.vertex_count = 3;
        raw.index_data = {reinterpret_cast<uint8_t const*>(indices.data()), indices.size() * 4};
        raw.index_count = static_cast<uint32_t>(indices.size());
        auto* filler = runtime.make_primitive("pbr", raw);
        CHECK(filler && filler->meshlet_count == deren::engine::meshlet_capacity - used - 1);
        filler->local_aabb_min = glm::vec3(1e8f);
        filler->local_aabb_max = glm::vec3(1e8f + 10);
        raw.index_data = raw.index_data.first(258 * 4);
        raw.index_count = 258; // two legacy records, only one table slot remains
        auto* overflow = runtime.make_primitive("pbr", raw);
        CHECK(overflow && overflow->meshlet_count == 0 && overflow->meshlets.empty());
        CHECK(overflow && overflow->draw_index_count == 258 && overflow->index_buffer->size() == 258 * 4);
        CHECK(!runtime.make_shadow_frame().allow_meshlets);
        auto visibility_table = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::visibility_items));
        std::fill(visibility_table.begin(), visibility_table.end(), std::byte{0xA5});
        CHECK(runtime.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(runtime.begin_recording() == deren::engine::frame_status::proceed);
        runtime.record_main_drawcalls();
        CHECK(runtime.end_recording() == deren::engine::frame_status::proceed);
        CHECK(std::all_of(visibility_table.begin(), visibility_table.end(), [](auto b) { return b == std::byte{0xA5}; }));
        // The small chain stubs lighting, so inspect the actual fallback draw
        // through the runtime's production environment on its segment buffer.
        auto fallback_frame = runtime.make_scene_frame();
        CHECK(!fallback_frame.segments.empty());
        static deren::engine::mesh_geometry_lanes fallback_lanes{};
        fallback_lanes = {};
        if (!fallback_frame.segments.empty()) {
            auto env = runtime.make_scene_environment(&runtime, deren::engine::recording_target{fallback_frame.segments.front().buffer}, true);
            env.push_at = [](void*, deren::engine::recording_target const&, uint32_t offset, std::span<std::byte const> bytes) {
                if (offset == deren::engine::mesh_geometry_push_offset_scene && bytes.size() == deren::engine::mesh_stage_block_size - offset)
                    std::memcpy(&fallback_lanes, bytes.data() + deren::engine::mesh_geometry_lanes_offset - offset, sizeof(fallback_lanes));
                return true;
            };
            overflow->draw(env);
            CHECK(env.mesh_stage && !env.meshlets);
        }
        CHECK(fallback_lanes.first_index == 0 && fallback_lanes.index_count == 258);
        CHECK(runtime.submit_and_present() == deren::engine::frame_status::proceed);
        raw.index_data = raw.index_data.first(3 * 4);
        raw.index_count = 3;
        auto* exact = runtime.make_primitive("pbr", raw);
        CHECK(exact && exact->meshlet_count == 1 && exact->meshlet_base == deren::engine::meshlet_capacity - 1);
        auto* zero = runtime.make_primitive("pbr", raw);
        CHECK(zero && zero->meshlet_count == 0 && zero->draw_index_count == 3);
        // Shared geometry keeps this a small fixture: only CPU leaves exceed the item limit.
        while (scene.roots.size() <= vis::id_item_capacity) {
            auto extra = std::make_unique<deren::engine::normal_draw_primitive>();
            CHECK(extra->bind_subtilis_geometry(generation));
            extra->meshlets = leaf->meshlets;
            extra->meshlet_count = leaf->meshlet_count;
            extra->meshlet_base = leaf->meshlet_base;
            extra->push.material_index = leaf->push.material_index;
            scene.add_root().primitive_leaf = std::move(extra);
        }
        runtime.scene_changed();
        auto table = deren::vk_test::probe_heap_buffer(*core, layout::heap_slot_offset(layout::heap_slots::visibility_items));
        CHECK(!table.empty());
        std::fill(table.begin(), table.end(), std::byte{0xA5});
        CHECK(runtime.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(runtime.begin_recording() == deren::engine::frame_status::proceed);
        runtime.record_main_drawcalls();
        CHECK(runtime.end_recording() == deren::engine::frame_status::proceed);
        CHECK(std::all_of(table.begin(), table.end(), [](auto b) { return b == std::byte{0xA5}; }));
        CHECK(runtime.submit_and_present() == deren::engine::frame_status::proceed);
    }
    return deren::vk_test::finish("test_subtilis_frame");
}
int main() {
#ifdef _WIN32
    _putenv_s("DEREN_HZB_BUILD", "");
    _putenv_s("DEREN_HZB_CULL", "");
    _putenv_s("DEREN_HZB_RECOVERY", "");
#else
    unsetenv("DEREN_HZB_BUILD");
    unsetenv("DEREN_HZB_CULL");
    unsetenv("DEREN_HZB_RECOVERY");
#endif
    auto forest = exercise(false);
#ifdef _WIN32
    _putenv_s("DEREN_HZB_BUILD", "1");
    _putenv_s("DEREN_HZB_CULL", "1");
#else
    setenv("DEREN_HZB_BUILD", "1", 1);
    setenv("DEREN_HZB_CULL", "1", 1);
#endif
    auto dag = exercise(true);
    return forest || dag;
}
