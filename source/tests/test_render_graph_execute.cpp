#include "vk_test.h"

#include <array>
#include <memory>
import deren.engine.render_graph.execute;
import deren.promise.rhi;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace execute = deren::engine::render_graph::execution;
namespace rhi = deren::promise::rhi;
int main() {
    auto core = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto cluster = deren::engine::resource_cluster::create(core, "graph");
    {
        rhi::buffer_desc desc{};
        desc.size = 64;
        desc.usage = rhi::buffer_usage::storage_coherent;
        auto buffer = *cluster->create_buffer(desc);
        rg::builder graph;
        auto ref = *graph.import_buffer(execute::identify(buffer), rg::initial_contents::defined, "mesh input", {64},
                                        rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read});
        rg::resource_access accesses[] = {{ref, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::mesh_shader}}};
        CHECK(graph.add_pass("mesh input", accesses, {true}));
        auto plan = graph.compile({true});
        CHECK(plan);
        std::array<execute::resource_binding, 1> bindings{buffer};
        if (plan) {
            CHECK(!execute::prepared_graph::prepare(core, *plan, bindings));
            deren::vk_test::probe_graphics_enabled(*core, true);
            CHECK(execute::prepared_graph::prepare(core, *plan, bindings));
            auto task_plan = *plan;
            task_plan.passes[0].accesses[0].scope.stages = rg::pipeline_stage::task_shader;
            CHECK(!execute::prepared_graph::prepare(core, std::move(task_plan), bindings));
            deren::vk_test::probe_graphics_enabled(*core, false);
        }
    }
    rhi::image_desc desc{};
    desc.extent = {4, 4, 1};
    desc.format = rhi::image_format::rgba8_unorm;
    desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::color_attachment);
    auto image = *cluster->create_image(desc);
    auto alias = *cluster->create_image(desc);
    deren::vk_test::probe_scoped_alias_image(*core, *alias.get(), *image.get());
    CHECK(image.get() != alias.get());
    CHECK(execute::identify(image) == execute::identify(alias));
    rg::builder graph;
    auto ref = *graph.import_texture(execute::identify(image), rg::initial_contents::undefined, "image", {}, rg::resource_state{});
    auto alias_ref = *graph.import_texture(execute::identify(alias), rg::initial_contents::undefined, "alias", {}, rg::resource_state{});
    CHECK(ref == alias_ref);
    rg::resource_access write[] = {{ref, rg::content_access::discard_write, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}};
    rg::resource_access read[] = {{ref, rg::content_access::read, {}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}};
    CHECK(graph.add_pass("write", write, {true}));
    CHECK(graph.add_pass("read", read, {true}));
    auto plan = *graph.compile({true});
    std::array<execute::resource_binding, 1> bindings{image};
    auto missing = execute::prepared_graph::prepare(core, plan, {});
    CHECK(!missing && missing.error().code == rhi::error::invalid_argument);
    std::array<execute::resource_binding, 2> duplicate{image, image};
    CHECK(!execute::prepared_graph::prepare(core, plan, duplicate));
    auto plain = plan;
    plain.synchronization_planned = false;
    CHECK(!execute::prepared_graph::prepare(core, std::move(plain), bindings));
    auto ready = execute::prepared_graph::prepare(core, plan, bindings);
    CHECK(ready);
    if (!ready)
        return deren::vk_test::finish("test_render_graph_execute");
    auto frame = core->frame_begin();
    (void)frame;
    unsigned calls = 0;
    auto result = ready->record(*core->begin_commands(), [&](execute::record_context const& context) noexcept {
        CHECK(context.pass_index() == calls);
        CHECK(context.pass().name == (calls == 0 ? "write" : "read"));
        CHECK(context.image(ref.index()) == image.get());
        CHECK(context.image(999) == nullptr && context.buffer(ref.index()) == nullptr);
        ++calls;
        return rhi::error::ok;
    });
    CHECK(result == rhi::error::ok && calls == 2);
    CHECK(deren::vk_test::probe_scoped_record_count(*core) >= 2);
    auto foreign = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto foreign_frame = foreign->frame_begin();
    (void)foreign_frame;
    calls = 0;
    result = ready->record(*foreign->begin_commands(), [&](execute::record_context const&) noexcept { ++calls; return rhi::error::ok; });
    CHECK(result != rhi::error::ok && calls == 0);
    deren::vk_test::probe_scoped_enabled(*core, false);
    CHECK(!execute::prepared_graph::prepare(core, plan, bindings));
    deren::vk_test::probe_scoped_enabled(*core, true);
    auto bad_plan = plan;
    bad_plan.resources[0].shape = rg::texture_shape{2, 1};
    CHECK(!execute::prepared_graph::prepare(core, std::move(bad_plan), bindings));
    auto before = deren::vk_test::probe_scoped_record_count(*core);
    deren::vk_test::probe_scoped_validation_failure(*core, 1);
    CHECK(!execute::prepared_graph::prepare(core, plan, bindings));
    CHECK(deren::vk_test::probe_scoped_record_count(*core) == before);
    deren::vk_test::probe_scoped_validation_failure(*core, -1);
    {
        rg::builder empty;
        auto prepared = execute::prepared_graph::prepare(core, *empty.compile({true}), {});
        CHECK(prepared);
        unsigned empty_calls = 0;
        auto callback = [&](execute::record_context const&) noexcept { ++empty_calls; return rhi::error::ok; };
        if (prepared) {
            CHECK(prepared->record(*foreign->begin_commands(), callback) != rhi::error::ok);
            CHECK(prepared->record(*core->begin_commands(), callback) == rhi::error::ok);
        }
        CHECK(empty_calls == 0);
    }
    {
        rg::builder exported;
        auto output = *exported.import_texture(execute::identify(image), rg::initial_contents::defined, "export only", {}, rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        CHECK(exported.export_resource(output));
        auto export_plan = *exported.compile({true});
        CHECK(!execute::prepared_graph::prepare(core, export_plan, {}));
        CHECK(execute::prepared_graph::prepare(core, std::move(export_plan), bindings));
    }
    calls = 0;
    result = ready->record(*core->begin_commands(), [&](execute::record_context const&) noexcept { ++calls; return rhi::error::operation_failed; });
    CHECK(result == rhi::error::operation_failed && calls == 1);
    {
        rhi::buffer_desc buffer_desc{};
        buffer_desc.size = 128;
        auto buffer = *cluster->create_buffer(buffer_desc);
        rg::builder buffer_graph;
        auto buffer_ref = *buffer_graph.import_buffer(execute::identify(buffer), rg::initial_contents::defined, "buffer", {128}, rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        rg::resource_access access[] = {{buffer_ref, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        CHECK(buffer_graph.add_pass("read buffer", access, {true}));
        auto buffer_plan = *buffer_graph.compile({true});
        CHECK(buffer_plan.barriers.empty());
        std::array<execute::resource_binding, 1> buffer_bindings{buffer};
        CHECK(!execute::prepared_graph::prepare(core, buffer_plan, {}));
        auto prepared = execute::prepared_graph::prepare(core, buffer_plan, buffer_bindings);
        CHECK(prepared);
        if (prepared) {
            CHECK(prepared->record(*core->begin_commands(), [&](execute::record_context const& context) noexcept {
                CHECK(context.buffer(buffer_ref.index()) == buffer.get());
                CHECK(context.image(buffer_ref.index()) == nullptr);
                return rhi::error::ok;
            }) == rhi::error::ok);
        }
        buffer_plan.resources[0].shape = rg::buffer_shape{64};
        CHECK(!execute::prepared_graph::prepare(core, std::move(buffer_plan), buffer_bindings));
    }
    auto token = ready->retention();
    rhi::shared_object<rhi::api_core>::weak weak = core;
    ready = std::unexpected(execute::execution_error{rhi::error::operation_failed});
    bindings[0] = deren::engine::resource_lease<rhi::image>{};
    duplicate = {};
    image = {};
    alias = {};
    cluster.reset();
    core.reset();
    CHECK(!weak.expired());
    token.reset();
    CHECK(weak.expired());
    return deren::vk_test::finish("test_render_graph_execute");
}
