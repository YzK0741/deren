#include "vk_test.h"

#include <array>
#include <expected>
#include <memory>
#include <type_traits>
import deren.engine.render_graph.compose;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace ex = rg::execution;
namespace rhi = deren::promise::rhi;
struct parameters {
    int tag;
    void visit_resources(rg::parameter_visitor&) const noexcept {
    }
};
struct wrong_parameters {
    void visit_resources(rg::parameter_visitor&) const noexcept {
    }
};
struct writer {
    rg::texture_discard_write target;
    void visit_resources(rg::parameter_visitor& visit) const noexcept {
        visit(target);
    }
};
struct buffer_writer {
    rg::buffer_discard_write target;
    void visit_resources(rg::parameter_visitor& visit) const noexcept {
        visit(target);
    }
};
struct recorder {
    rhi::error operator()(parameters const&, rhi::command_buffer&, ex::record_context const&) const noexcept {
        return rhi::error::ok;
    }
};
struct mutable_recorder {
    rhi::error operator()(parameters const&, rhi::command_buffer&, ex::record_context const&) noexcept {
        return rhi::error::ok;
    }
};
struct move_only {
    std::unique_ptr<int> value;
    rhi::error operator()(parameters const&, rhi::command_buffer&, ex::record_context const&) const noexcept {
        return rhi::error::ok;
    }
};
static_assert(ex::ownable<parameters> && !ex::ownable<parameters&> && !ex::ownable<parameters const>);
static_assert(ex::pass_recorder<recorder, parameters> && ex::pass_recorder<move_only, parameters>);
static_assert(!ex::pass_recorder<mutable_recorder, parameters>);
static_assert(ex::graph_appender<decltype([](rg::builder&) noexcept -> std::expected<void, rg::graph_error> { return {}; })>);
static_assert(!ex::graph_appender<decltype([](rg::builder&) -> std::expected<void, rg::graph_error> { return {}; })>);
static_assert(!ex::graph_appender<decltype([](rg::builder&) noexcept { return true; })>);
using referenced_result = std::expected<int, rg::graph_error>;
struct reference_appender {
    referenced_result& operator()(rg::builder&) const noexcept;
};
struct boolean_recorder {
    bool operator()(parameters const&, rhi::command_buffer&, ex::record_context const&) const noexcept;
};
static_assert(!ex::graph_appender<reference_appender> && !ex::pass_recorder<boolean_recorder, parameters>);
template <class F>
concept bindable = requires(ex::builder& frame, rg::pass_ref pass, F&& fn) {
    frame.bind_pass<parameters>(pass, std::forward<F>(fn));
};
static_assert(bindable<move_only> && !bindable<move_only&>);
int main() {
    auto core = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    (void)core->frames->frame_begin();
    auto cluster = deren::engine::resource_cluster::create(core);
    rhi::image_desc desc{};
    desc.extent = {4, 4, 1};
    desc.format = rhi::image_format::rgba8_unorm;
    desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::color_attachment);
    auto image = *cluster->create_image(desc);
    {
        ex::builder frame(core);
        auto a = frame.import_texture(image, {}, "image");
        auto b = frame.import_texture(image, {}, "alias");
        CHECK(a && b && *a == *b);
        CHECK(!frame.import_texture(image, {rg::initial_contents::defined, {rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read}}, "conflict"));
        CHECK(!frame.prepare());
    }
    unsigned calls = 0;
    std::weak_ptr<int> weak;
    auto prepared = [&] {
        ex::builder frame(core);
        auto payload = std::make_shared<int>(7);
        weak = payload;
        auto passes = frame.append([](rg::builder& graph) noexcept -> std::expected<std::array<rg::pass_ref, 2>, rg::graph_error> {
            auto a = graph.add_pass("first", parameters{1}, {true});
            auto b = graph.add_pass("second", parameters{2}, {true});
            if (!a)
                return std::unexpected(a.error());
            if (!b)
                return std::unexpected(b.error());
            auto order = graph.order_after(*a, *b);
            if (!order)
                return std::unexpected(order.error());
            auto dead = graph.add_pass("dead", parameters{3});
            if (!dead)
                return std::unexpected(dead.error());
            return std::array{*a, *b};
        });
        CHECK(passes);
        for (auto ref : *passes)
            CHECK(frame.bind_pass<parameters>(ref, [payload, &calls](parameters const& p, rhi::command_buffer&, ex::record_context const&) noexcept {
                CHECK(*payload == 7 && p.tag == (calls == 0 ? 2 : 1));
                ++calls;
                return rhi::error::ok;
            }));
        auto result = frame.prepare();
        CHECK(!frame.prepare());
        return result;
    }();
    CHECK(prepared && !weak.expired());
    if (prepared) {
        CHECK(prepared->record(*core->frames->begin_commands(), [](ex::record_context const&) noexcept { return rhi::error::ok; }) == rhi::error::invalid_argument);
        CHECK(prepared->record(*core->frames->begin_commands()) == rhi::error::ok && calls == 2);
        auto retention = prepared->retention();
        prepared = std::unexpected(ex::build_error{});
        CHECK(!weak.expired());
        retention.reset();
        CHECK(weak.expired());
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        ex::builder frame(core);
        auto pass = frame.append([](rg::builder& graph) noexcept { return graph.add_pass("live", parameters{1}, {true}); });
        CHECK(pass);
        if (failure == 1) {
            CHECK(frame.bind_pass<parameters>(*pass, recorder{}));
            CHECK(!frame.bind_pass<parameters>(*pass, recorder{}));
        } else if (failure == 2) {
            rg::builder other;
            auto foreign = other.add_pass("foreign", parameters{1}, {true});
            CHECK(!frame.bind_pass<parameters>(*foreign, recorder{}));
        } else if (failure == 3) {
            CHECK(frame.bind_pass<wrong_parameters>(*pass, [](wrong_parameters const&, rhi::command_buffer&, ex::record_context const&) noexcept { return rhi::error::ok; }));
        } else if (failure == 4) {
            auto broken = frame.append([](rg::builder& graph) noexcept -> std::expected<void, rg::graph_error> {
                auto orphan = graph.add_pass("orphan", parameters{9}, {true});
                if (!orphan)
                    return std::unexpected(orphan.error());
                return std::unexpected(rg::graph_error{.code = rg::error_code::invalid_ref, .pass_name = {}, .resource_name = {}});
            });
            CHECK(!broken);
        }
        auto before = deren::vk_test::probe_scoped_record_count(*core);
        CHECK(!frame.prepare());
        CHECK(deren::vk_test::probe_scoped_record_count(*core) == before);
    }
    {
        ex::builder frame(core);
        auto void_append = frame.append([](rg::builder&) noexcept -> std::expected<void, rg::graph_error> { return {}; });
        CHECK(void_append);
        auto pass = frame.append([](rg::builder& graph) noexcept { return graph.add_pass("boundary", std::span<rg::resource_access const>{}, {true}); });
        CHECK(frame.bind_barrier_only(*pass));
        auto owner = std::make_shared<int>(1);
        std::weak_ptr<int> weak_owner = owner;
        CHECK(frame.retain(owner));
        owner.reset();
        auto ready = frame.prepare();
        CHECK(ready && ready->record(*core->frames->begin_commands()) == rhi::error::ok && !weak_owner.expired());
    }
    {
        ex::builder frame(core);
        unsigned recorded = 0;
        for (unsigned i = 0; i < 2; ++i) {
            auto pass = frame.append([&](rg::builder& graph) noexcept { return graph.add_pass("failure", parameters{static_cast<int>(i)}, {true}); });
            CHECK(frame.bind_pass<parameters>(*pass, [&recorded](parameters const&, rhi::command_buffer&, ex::record_context const&) noexcept { ++recorded; return rhi::error::unsupported; }));
        }
        auto ready = frame.prepare();
        CHECK(ready && ready->record(*core->frames->begin_commands()) == rhi::error::unsupported && recorded == 1);
    }
    {
        auto alias = *cluster->create_image(desc);
        deren::vk_test::probe_scoped_alias_image(*core, *alias.get(), *image.get());
        ex::builder frame(core);
        auto first = frame.import_texture(image, {}, "first");
        auto second = frame.import_texture(alias, {}, "second");
        CHECK(first && second && *first == *second);
        auto foreign = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
        ex::builder foreign_frame(foreign);
        CHECK(!foreign_frame.import_texture(image, {}, "foreign") && !foreign_frame.prepare());
    }
    {
        ex::builder frame(core);
        auto ref = frame.import_texture(image, {}, "target");
        auto pass = frame.append([&](rg::builder& graph) noexcept { return graph.add_pass("write", writer{{*ref, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}}); });
        CHECK(frame.bind_pass<writer>(*pass, [](writer const&, rhi::command_buffer&, ex::record_context const&) noexcept { return rhi::error::ok; }));
        CHECK(frame.export_resource(*ref));
        auto ready = frame.prepare();
        CHECK(ready && ready->record(*core->frames->begin_commands()) == rhi::error::ok);
    }
    {
        rhi::buffer_desc description{};
        description.size = 128;
        auto buffer = *cluster->create_buffer(description);
        ex::builder frame(core);
        auto ref = frame.import_buffer(buffer, {}, "buffer");
        CHECK(ref);
        auto pass = frame.append([&](rg::builder& graph) noexcept { return graph.add_pass("buffer write", buffer_writer{{*ref, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}}); });
        CHECK(frame.bind_pass<buffer_writer>(*pass, [](buffer_writer const&, rhi::command_buffer&, ex::record_context const&) noexcept { return rhi::error::ok; }));
        CHECK(frame.export_resource(*ref));
        auto ready = frame.prepare();
        CHECK(ready && ready->record(*core->frames->begin_commands()) == rhi::error::ok);
    }
    {
        auto frames = rhi::query_extension<rhi::frame_resources>(*core);
        auto snapshot = deren::engine::resource_lease<rhi::image>::from_shared(core, frames->retain_image());
        (void)core->frames->frame_begin();
        ex::builder frame(core);
        CHECK(!frame.import_texture(snapshot, {}, "stale") && !frame.prepare());
        deren::vk_test::probe_scoped_enabled(*core, false);
        ex::builder unsupported(core);
        CHECK(!unsupported.import_texture(image, {}, "unsupported"));
        deren::vk_test::probe_scoped_enabled(*core, true);
    }
    {
        auto shape = desc;
        shape.mip_levels = 2;
        auto alias = *cluster->create_image(shape);
        deren::vk_test::probe_scoped_alias_image(*core, *alias.get(), *image.get());
        ex::builder frame(core);
        CHECK(frame.import_texture(image, {}, "original"));
        CHECK(!frame.import_texture(alias, {}, "bad shape") && !frame.prepare());
    }
    {
        using function = rhi::error (*)(parameters const&, rhi::command_buffer&, ex::record_context const&) noexcept;
        ex::builder frame(core);
        auto pass = frame.append([](rg::builder& graph) noexcept { return graph.add_pass("null callback", parameters{1}, {true}); });
        CHECK(!frame.bind_pass<parameters>(*pass, static_cast<function>(nullptr)));
        CHECK(!frame.prepare());
    }
    {
        using function = std::expected<void, rg::graph_error> (*)(rg::builder&) noexcept;
        ex::builder frame(core);
        CHECK(!frame.append(static_cast<function>(nullptr)));
        CHECK(!frame.prepare());
    }
    return deren::vk_test::finish("test_render_graph_compose");
}
