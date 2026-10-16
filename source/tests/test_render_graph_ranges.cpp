#include "vk_test.h"

#include <algorithm>
#include <array>
#include <limits>
#include <variant>
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;
namespace {
    using deren::utility::vessel::vector;
    auto add(rg::builder& b, char const* name, std::initializer_list<rg::resource_access> accesses, bool side = false) {
        return b.add_pass(name, {accesses.begin(), accesses.size()}, {side});
    }
    rg::texture_range mip(uint32_t index, rg::texture_aspect aspect = rg::texture_aspect::all) {
        return {index, 1, 0, 1, aspect};
    }
    void texture_tests() {
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "mips", {2, 1, rg::texture_aspect::color});
            add(b, "mip0", {{image, rg::content_access::discard_write, mip(0)}});
            add(b, "mip1", {{image, rg::content_access::discard_write, mip(1)}});
            CHECK(b.export_resource(image, mip(1)));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{1});
            if (plan)
                CHECK(plan->dependencies.empty());
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "partial", {2, 1, rg::texture_aspect::color});
            add(b, "mip0", {{image, rg::content_access::discard_write, mip(0)}});
            add(b, "read all", {{image, rg::content_access::read}}, true);
            auto result = b.compile();
            CHECK(!result && result.error().code == rg::error_code::undefined_read);
            if (!result) {
                CHECK(result.error().range.has_value());
                if (result.error().range)
                    CHECK(std::get<rg::texture_range>(*result.error().range).base_mip == 1);
            }
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "both", {2, 1, rg::texture_aspect::color});
            add(b, "mip0", {{image, rg::content_access::discard_write, mip(0)}});
            add(b, "mip1", {{image, rg::content_access::discard_write, mip(1)}});
            add(b, "read all", {{image, rg::content_access::read}}, true);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1, 2}));
            if (plan)
                CHECK(plan->resources[0].defined && plan->resources[0].last_producer == rg::invalid_index && plan->reads.size() == 2);
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::defined, "downsample", {2, 1, rg::texture_aspect::color});
            CHECK(add(b, "same image", {{image, rg::content_access::read, mip(0)}, {image, rg::content_access::discard_write, mip(1)}}));
            b.export_resource(image, mip(1));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{0} && plan->dependencies.empty());
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "depth stencil", {2, 2, rg::texture_aspect::depth | rg::texture_aspect::stencil});
            rg::texture_range depth{0, 1, 1, 1, rg::texture_aspect::depth};
            rg::texture_range stencil{0, 1, 1, 1, rg::texture_aspect::stencil};
            add(b, "depth", {{image, rg::content_access::discard_write, depth}});
            add(b, "stencil", {{image, rg::content_access::discard_write, stencil}});
            b.export_resource(image, depth);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{0});
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "layers", {1, 2, rg::texture_aspect::color});
            rg::texture_range layer0{0, 1, 0, 1, rg::texture_aspect::color};
            rg::texture_range layer1{0, 1, 1, 1, rg::texture_aspect::color};
            add(b, "layer0", {{image, rg::content_access::discard_write, layer0}});
            add(b, "layer1", {{image, rg::content_access::read, layer1}}, true);
            auto plan = b.compile();
            CHECK(!plan && plan.error().code == rg::error_code::undefined_read);
        }
    }
    void overlapping_read_write_tests() {
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "read-write union", {16});
            CHECK(add(b, "overlap read-write", {{r, rg::content_access::read_write, rg::buffer_range{0, 8}}, {r, rg::content_access::read_write, rg::buffer_range{4, 8}}}, true));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{0});
            if (plan) {
                CHECK(plan->dependencies.empty());
                CHECK(plan->resources[0].last_version == 1);
                CHECK(std::all_of(plan->reads.begin(), plan->reads.end(), [](auto const& read) { return read.producer == rg::invalid_index && read.version == 0; }));
            }
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::defined, "rectangle union", {3, 3, rg::texture_aspect::depth | rg::texture_aspect::stencil});
            rg::texture_range first{0, 2, 0, 2, rg::texture_aspect::depth};
            rg::texture_range second{1, 2, 1, 2, rg::texture_aspect::depth | rg::texture_aspect::stencil};
            CHECK(add(b, "overlap rectangles", {{image, rg::content_access::read_write, first}, {image, rg::content_access::read_write, second}}, true));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{0});
            if (plan) {
                CHECK(plan->dependencies.empty());
                CHECK(std::all_of(plan->reads.begin(), plan->reads.end(), [](auto const& read) { return read.producer == rg::invalid_index && read.version == 0; }));
                uint32_t written = 0;
                for (auto const& state : plan->resources[0].final_ranges)
                    if (state.producer == 0) {
                        auto range = std::get<rg::texture_range>(state.range);
                        auto mask = static_cast<uint32_t>(range.aspects);
                        auto count = static_cast<uint32_t>((mask & 2U) != 0) + static_cast<uint32_t>((mask & 4U) != 0);
                        written += range.mip_count * range.layer_count * count;
                    }
                CHECK(written == 11);
            }
        }
    }
    void buffer_tests() {
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::undefined, "bytes", {32});
            add(b, "left", {{r, rg::content_access::discard_write, rg::buffer_range{0, 8}}});
            add(b, "right", {{r, rg::content_access::discard_write, rg::buffer_range{8, 8}}});
            add(b, "read right", {{r, rg::content_access::read, rg::buffer_range{8, 8}}}, true);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({1, 2}));
            if (plan)
                for (auto const& edge : plan->dependencies) {
                    CHECK(edge.from != 0);
                    CHECK(edge.range && std::get<rg::buffer_range>(*edge.range) == rg::buffer_range(8, 8));
                }
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::undefined, "coverage", {16});
            add(b, "left", {{r, rg::content_access::discard_write, rg::buffer_range{0, 8}}});
            add(b, "right", {{r, rg::content_access::discard_write, rg::buffer_range{8, 8}}});
            b.export_resource(r);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1}) && plan->resources[0].defined);
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "partial invalidate", {16});
            add(b, "invalidate", {{r, rg::content_access::invalidate, rg::buffer_range{8, 8}}});
            b.export_resource(r, rg::buffer_range{0, 8});
            auto plan = b.compile();
            CHECK(plan && plan->execution_order.empty());
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "hole", {16});
            add(b, "invalidate", {{r, rg::content_access::invalidate, rg::buffer_range{8, 8}}});
            add(b, "read", {{r, rg::content_access::read, rg::buffer_range{4, 8}}}, true);
            auto plan = b.compile();
            CHECK(!plan && plan.error().code == rg::error_code::undefined_read);
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::undefined, "overlap", {16});
            CHECK(add(b, "union", {{r, rg::content_access::discard_write, rg::buffer_range{0, 8}}, {r, rg::content_access::discard_write, rg::buffer_range{4, 8}}}));
            CHECK(add(b, "read modify", {{r, rg::content_access::read_write, rg::buffer_range{0, 12}}}, true));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1}));
            if (plan)
                for (auto const& edge : plan->dependencies)
                    CHECK(edge.from != edge.to);
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "conflict", {16});
            CHECK(!add(b, "bad", {{r, rg::content_access::read, rg::buffer_range{0, 8}}, {r, rg::content_access::discard_write, rg::buffer_range{4, 8}}}));
            auto good = add(b, "good", {{r, rg::content_access::read, rg::buffer_range{0, 8}}, {r, rg::content_access::discard_write, rg::buffer_range{8, 8}}}, true);
            CHECK(good && good->index() == 0);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{0});
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "WAR", {16});
            auto reader = *add(b, "reader", {{r, rg::content_access::read, rg::buffer_range{0, 8}}}, true);
            auto writer = *add(b, "disjoint writer", {{r, rg::content_access::discard_write, rg::buffer_range{8, 8}}}, true);
            b.order_after(reader, writer);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({1, 0}));
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "WAR overlap", {16});
            auto reader = *add(b, "reader", {{r, rg::content_access::read, rg::buffer_range{0, 8}}}, true);
            auto writer = *add(b, "writer", {{r, rg::content_access::discard_write, rg::buffer_range{4, 8}}}, true);
            b.order_after(reader, writer);
            auto plan = b.compile();
            CHECK(!plan && plan.error().code == rg::error_code::cycle);
        }
    }
    void validation_tests() {
        rg::builder b;
        CHECK(!b.import_texture({1, 1}, rg::initial_contents::defined, "zero", {0, 1, rg::texture_aspect::color}));
        CHECK(!b.import_buffer({1, 2}, rg::initial_contents::defined, "empty", {0}));
        auto image = *b.import_texture({1, 1}, rg::initial_contents::defined, "valid", {2, 3, rg::texture_aspect::color});
        CHECK(!b.import_texture({1, 1}, rg::initial_contents::defined, "different shape", {3, 3, rg::texture_aspect::color}));
        for (auto range : {rg::texture_range{2, 1, 0, 1, rg::texture_aspect::color}, rg::texture_range{0, 0, 0, 1, rg::texture_aspect::color}, rg::texture_range{0, 1, 3, 1, rg::texture_aspect::color}, rg::texture_range{0, 1, 0, 1, rg::texture_aspect::depth}, rg::texture_range{1, 2, 0, 1, rg::texture_aspect::color}, rg::texture_range{0, 1, 0, 1, rg::texture_aspect::none}}) {
            auto bad = add(b, "bad range", {{image, rg::content_access::read, range}}, true);
            CHECK(!bad && bad.error().code == rg::error_code::invalid_range);
        }
        auto buffer = *b.import_buffer({1, 2}, rg::initial_contents::defined, "buffer", {16});
        for (auto range : {rg::buffer_range{0, 0}, rg::buffer_range{17, 1}, rg::buffer_range{15, 2}, rg::buffer_range{std::numeric_limits<uint64_t>::max() - 1, 8}}) {
            CHECK(!add(b, "bad bytes", {{buffer, rg::content_access::read, range}}, true));
            CHECK(!b.export_resource(buffer, range));
        }
        auto good = add(b, "remaining", {{buffer, rg::content_access::read, rg::buffer_range{8}}}, true);
        CHECK(good && good->index() == 0);
        auto plan = b.compile();
        CHECK(plan && plan->passes[0].accesses.size() == 1);
        if (plan)
            CHECK(std::get<rg::buffer_range>(plan->passes[0].accesses[0].range) == rg::buffer_range(8, 8));
        {
            rg::builder huge;
            auto r = *huge.import_buffer({1, 1}, rg::initial_contents::undefined, "huge", {std::numeric_limits<uint64_t>::max()});
            auto offset = std::numeric_limits<uint64_t>::max() - 1;
            add(huge, "last byte", {{r, rg::content_access::discard_write, rg::buffer_range{offset, 1}}});
            huge.export_resource(r, rg::buffer_range{offset, 1});
            auto result = huge.compile();
            CHECK(result && result->execution_order == vector<uint32_t>{0} && result->resources[0].final_ranges.size() <= 3);
        }
        {
            rg::builder huge;
            auto r = *huge.import_texture({1, 1}, rg::initial_contents::undefined, "sparse image", {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), rg::texture_aspect::depth | rg::texture_aspect::stencil});
            rg::texture_range last{std::numeric_limits<uint32_t>::max() - 1, 1, std::numeric_limits<uint32_t>::max() - 1, 1, rg::texture_aspect::stencil};
            add(huge, "last cell", {{r, rg::content_access::discard_write, last}});
            huge.export_resource(r, last);
            auto result = huge.compile();
            CHECK(result && result->resources[0].final_ranges.size() <= 6);
        }
    }
} // namespace
namespace {
    // Dense independent oracle: intentionally unlike the production sparse rectangle partition.
    void rectangle_oracle_tests() {
        uint32_t random = 0x18a52f39U;
        auto next = [&] {random=random*1664525U+1013904223U;return random; };
        constexpr uint32_t mips = 3, layers = 4, aspects = 2, cells = mips * layers * aspects;
        uint32_t accepted = 0, rejected = 0;
        for (uint32_t trial = 0; trial < 128; ++trial) {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "oracle", {mips, layers, rg::texture_aspect::depth | rg::texture_aspect::stencil});
            std::array<uint32_t, cells> producers;
            producers.fill(rg::invalid_index);
            std::array<bool, cells> defined{};
            auto each = [&](rg::texture_range range, auto visit) {
                for (uint32_t a = 0; a < aspects; ++a) {
                    auto flag = a == 0 ? rg::texture_aspect::depth : rg::texture_aspect::stencil;
                    if (!(static_cast<uint32_t>(range.aspects) & static_cast<uint32_t>(flag)))
                        continue;
                    for (uint32_t mip = range.base_mip; mip < range.base_mip + range.mip_count; ++mip)
                        for (uint32_t layer = range.base_layer; layer < range.base_layer + range.layer_count; ++layer)
                            visit((a * mips + mip) * layers + layer);
                }
            };
            auto make_range = [&] {
                uint32_t mip = next() % mips, layer = next() % layers;
                auto mask = next() % 3 + 1;
                return rg::texture_range{mip, next() % (mips - mip) + 1, layer, next() % (layers - layer) + 1, static_cast<rg::texture_aspect>(mask << 1)};
            };
            constexpr uint32_t writes = 20;
            for (uint32_t p = 0; p < writes; ++p) {
                auto range = make_range();
                bool valid = (next() % 4) != 0;
                add(b, "write", {{image, valid ? rg::content_access::discard_write : rg::content_access::invalidate, range}});
                each(range, [&](uint32_t cell) {producers[cell]=p;defined[cell]=valid; });
            }
            auto output = make_range();
            bool valid = true;
            std::array<bool, writes> needed{};
            each(output, [&](uint32_t cell) {valid=valid&&defined[cell];if(producers[cell]!=rg::invalid_index) needed[producers[cell]]=true; });
            b.export_resource(image, output);
            auto plan = b.compile();
            CHECK(static_cast<bool>(plan) == valid);
            if (plan)
                ++accepted;
            else
                ++rejected;
            if (!plan) {
                CHECK(plan.error().code == rg::error_code::undefined_export);
                continue;
            }
            vector<uint32_t> order;
            for (uint32_t p = 0; p < writes; ++p)
                if (needed[p])
                    order.push_back(p);
            CHECK(plan->execution_order == order);
            std::array<uint32_t, cells> covered{};
            bool correct = true;
            for (auto const& state : plan->resources[0].final_ranges) {
                each(std::get<rg::texture_range>(state.range), [&](uint32_t cell) {
                    ++covered[cell];
                    correct = correct && state.producer == producers[cell] && state.defined == defined[cell] && state.version == (producers[cell] == rg::invalid_index ? 0 : static_cast<uint64_t>(producers[cell]) + 1);
                });
            }
            CHECK(correct);
            CHECK(std::all_of(covered.begin(), covered.end(), [](auto count) { return count == 1; }));
        }
        CHECK(accepted > 0 && rejected > 0);
    }
} // namespace
int main() {
    rectangle_oracle_tests();
    texture_tests();
    buffer_tests();
    overlapping_read_write_tests();
    validation_tests();
    return deren::vk_test::finish("test_render_graph_ranges");
}
