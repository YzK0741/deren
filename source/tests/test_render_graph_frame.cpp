#include "vk_test.h"

#include <array>
#include <memory>
import deren.engine.render_graph.execute;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace rhi = deren::promise::rhi;
int main() {
    auto core = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto frames = rhi::query_extension<rhi::frame_resources>(*core);
    CHECK(frames);
    if (!frames)
        return deren::vk_test::finish("test_render_graph_frame");
    CHECK(!frames->retain_image());
    (void)core->frame_begin();
    auto shared = frames->retain_image();
    CHECK(shared && shared.get() != core->frame_image());
    CHECK(frames->retain_image().get() == shared.get());
    auto image = deren::engine::resource_lease<rhi::image>::from_shared(core, shared);
    auto identity = rg::execution::identify(image);
    CHECK(identity.object != 0);
    rg::builder graph;
    auto ref = *graph.import_texture(identity, rg::initial_contents::undefined, "frame", {}, rg::resource_state{});
    std::array<rg::resource_access, 1> write{{{ref, rg::content_access::discard_write, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}}};
    std::array<rg::resource_access, 1> present{{{ref, rg::content_access::read, {}, {rg::resource_usage::present, rg::pipeline_stage::none}}}};
    CHECK(graph.add_pass("draw", write));
    CHECK(graph.add_pass("present", present, {true}));
    auto plan = graph.compile({true});
    CHECK(plan);
    if (!plan)
        return deren::vk_test::finish("test_render_graph_frame");
    CHECK(plan->barriers.back().after.layout == rg::resource_layout::present);
    CHECK(plan->barriers.back().after.access == rg::memory_access::none);
    std::array<rg::execution::resource_binding, 1> bindings{image};
    auto ready = rg::execution::prepared_graph::prepare(core, *plan, bindings);
    CHECK(ready);
    unsigned calls = 0;
    auto callback = [&](rg::execution::record_context const&) noexcept { ++calls; return rhi::error::ok; };
    if (ready)
        CHECK(ready->record(*core->begin_commands(), callback) == rhi::error::ok);
    CHECK(calls == 2);
    CHECK(core->submit(*core->begin_commands()) == rhi::error::ok);
    (void)core->frame_begin();
    auto next = frames->retain_image();
    CHECK(next && next != shared);
    calls = 0;
    if (ready)
        CHECK(ready->record(*core->begin_commands(), callback) != rhi::error::ok);
    CHECK(calls == 0);
    CHECK(!rg::execution::prepared_graph::prepare(core, *plan, bindings));
    // A read-only pass can require no barriers. Identity checks must still reject
    // a snapshot invalidated by an earlier callback in the same recording.
    auto next_lease = deren::engine::resource_lease<rhi::image>::from_shared(core, next);
    rg::builder reads;
    auto current = *reads.import_texture(rg::execution::identify(next_lease), rg::initial_contents::defined, "current", {},
                                         rg::resource_state{rg::resource_layout::present, rg::pipeline_stage::none, rg::memory_access::none});
    present[0] = {current, rg::content_access::read, {}, {rg::resource_usage::present, rg::pipeline_stage::none}};
    CHECK(reads.add_pass("first", present, {true}));
    CHECK(reads.add_pass("second", present, {true}));
    auto read_plan = reads.compile({true});
    CHECK(read_plan && read_plan->barriers.empty());
    if (read_plan) {
        bindings[0] = next_lease;
        auto read_ready = rg::execution::prepared_graph::prepare(core, *read_plan, bindings);
        CHECK(read_ready);
        calls = 0;
        if (read_ready) {
            auto error = read_ready->record(*core->begin_commands(), [&](rg::execution::record_context const&) noexcept {
                ++calls;
                (void)core->frame_begin();
                return rhi::error::ok;
            });
            CHECK(error == rhi::error::not_ready && calls == 1);
        }
    }
    return deren::vk_test::finish("test_render_graph_frame");
}
