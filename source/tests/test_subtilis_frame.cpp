#include "vk_test.h"

#include <array>
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
import deren.engine.visibility;
import deren.engine.render_layout;
import deren.engine.render_resource;
import deren.engine.pass;
import deren.engine.pass.chain;
import deren.engine.pass.output_targets;
import deren.engine.pass.scene;
import deren.engine.pass.post;
import deren.engine.pass.visibility_raster;
import deren.engine.pass.visibility_resolve;
#include "probe_backend.hpp"
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
        deren::utility::vessel::vector<std::array<float, 16>> vertices;
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
        auto v = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(vertices.data()), vertices.size() * 64};
        auto i = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(indices.data()), indices.size() * 4};
        auto built = sub::build({{v, 64, 289, i, 4, 0, static_cast<unsigned>(indices.size()), 0}});
        return built ? std::make_shared<sub::built_geometry const>(std::move(*built)) : nullptr;
    }
} // namespace
int main() {
    auto cpu = source();
    CHECK(cpu && cpu->statistics().depth > 0);
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
    chain.emplace<screen_pass>(false);
    chain.emplace<screen_pass>(true);
    for (unsigned i = 0; i < 4; ++i) {
        chain.emplace<pass::post_bloom_pass>(i);
    }
    deren::engine::runtime runtime{core, options};
    runtime.set_scene(scene);
    runtime.camera.distance = 20;
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
    runtime.create_passes();
    CHECK(runtime.make_pipeline("pbr", shader, shader, shader));
    CHECK(runtime.make_gbuffer_pipeline(shader, shader, shader));
    CHECK(runtime.visibility_available());
    CHECK(runtime.make_character_forward_pipeline(deren::engine::runtime::character_forward_pipeline_name, shader, shader, shader));
    deren::engine::primitive_create_info info;
    info.subtilis_geometry = cpu;
    auto* leaf = runtime.make_primitive("pbr", info);
    CHECK(leaf && leaf->meshlet_count == cpu->hierarchy().lod0_count());
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
        CHECK(row.meshlet_table == (unsigned(block.geometry.base_vertex) & 1u));
        CHECK(row.meshlet_base == block.geometry.first_index && row.index_width == block.geometry.index_width);
        auto commands = deren::vk_test::probe_last_indirect(*core);
        CHECK(commands.groups_x == block.geometry.index_count && commands.groups_y == 1 && commands.groups_z == 1);
        CHECK(runtime.submit_and_present() == deren::engine::frame_status::proceed);
        return block;
    };
    auto coarse = record();
    CHECK(coarse.geometry.index_count > 0 && coarse.geometry.index_count < leaf->meshlet_count);
    CHECK(row_at(coarse, 0).meshlet_table == 1);
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
    CHECK(legacy.geometry.index_count == leaf->meshlet_count && row_at(legacy, 0).meshlet_table == 1);
    leaf->subtilis_geometry = generation;
    --leaf->draw_index_count;
    auto mismatch = record();
    CHECK(mismatch.geometry.index_count == leaf->meshlet_count && row_at(mismatch, 0).meshlet_table == 0);
    ++leaf->draw_index_count;
    auto* alias = runtime.make_primitive("pbr", info);
    CHECK(alias != nullptr);
    if (alias) {
        alias->meshlet_base = leaf->meshlet_base;
        auto conflict = record();
        CHECK(conflict.item_index == 1 && conflict.geometry.index_count == leaf->meshlet_count);
        CHECK(row_at(conflict, 0).meshlet_table == 1 && row_at(conflict, 1).meshlet_table == 0);
    }
    deren::utility::wait_log_all();
    return deren::vk_test::finish("test_subtilis_frame");
}
