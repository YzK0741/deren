#include "../backends/vulkan/core/push_range_validation.h"
#include "../utility/shared/memory_test_hooks.hpp"
#include "vk_test.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>
import deren.promise.render_operations;
import deren.utility.shared;
namespace rec = deren::promise::rhi::recording;

int main() {
    // Validate the union of written push bytes, independent of how many draws overwrite them.
    deren::utility::vessel::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    written.reserve(128);
    deren::vulkan::detail::merge_push_range(written, 8, 8);
    deren::vulkan::detail::merge_push_range(written, 24, 8);
    CHECK(written.size() == 2);
    deren::vulkan::detail::merge_push_range(written, 12, 16);
    CHECK(written.size() == 1 && written[0].first == 8 && written[0].second == 24);
    for (unsigned i = 0; i < 4096; ++i)
        deren::vulkan::detail::merge_push_range(written, 12, 8);
    CHECK(written.size() == 1);
    deren::vulkan::detail::merge_push_range(written, 0, 0);
    CHECK(written.size() == 1);
    std::array<bool, 256> written_reference{};
    written.clear();
    std::uint32_t range_seed = 54321;
    for (unsigned i = 0; i < 512; ++i) {
        range_seed = range_seed * 1664525u + 1013904223u;
        auto offset = range_seed % 240;
        auto size = (range_seed >> 24) % 16 + 1;
        for (auto byte = offset; byte < offset + size; ++byte)
            written_reference[byte] = true;
        deren::vulkan::detail::merge_push_range(written, offset, size);
        for (unsigned byte = 0; byte < written_reference.size(); ++byte) {
            bool covered = false;
            for (auto [start, length] : written)
                covered |= byte >= start && byte < start + length;
            CHECK(covered == written_reference[byte]);
        }
    }
    // Wrong last-writer liveness would change the vertex identity observed by draw.
    rec::operation_stream stream;
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::vertex>({2});
    stream.append<rec::operation_kind::draw>({3});
    stream.append<rec::operation_kind::draw>({3});
    auto result = stream.simplify(true);
    CHECK(result.size() == 3);
    CHECK(result[0]->data[0] == 2);
    CHECK(result[1]->is_unique());
    CHECK(*result[1] == *result[2]);
    stream.clear();

    // A state used by a consumer cannot disappear because the same value returns later.
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::draw>({3});
    stream.append<rec::operation_kind::vertex>({2});
    stream.append<rec::operation_kind::draw>({3});
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::draw>({3});
    CHECK(stream.simplify(true).size() == 6);
    stream.clear();

    // Intermediate application publishes state even without a consumer; terminal application need not.
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::vertex>({1});
    CHECK(stream.simplify(false).size() == 1);
    CHECK(stream.simplify(true).empty());
    stream.clear();

    // Adjacent duplicate STATE collapses (the earlier copy is dead); repeated EXECUTION is still protected.
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::draw_indexed>({2});
    result = stream.simplify(true);
    CHECK(result.size() == 2);
    CHECK(result[0]->kind() == rec::operation_kind::vertex);
    CHECK(result[1]->kind() == rec::operation_kind::draw_indexed);
    stream.clear();

    // Independent state kinds deduplicate separately, and execution still follows the surviving state.
    stream.append<rec::operation_kind::cull>({1});
    stream.append<rec::operation_kind::cull>({1});
    stream.append<rec::operation_kind::depth_write>({1});
    stream.append<rec::operation_kind::depth_write>({1});
    stream.append<rec::operation_kind::draw>({3});
    result = stream.simplify(true);
    CHECK(result.size() == 3);
    CHECK(result[0]->kind() == rec::operation_kind::cull);
    CHECK(result[1]->kind() == rec::operation_kind::depth_write);
    CHECK(result[2]->kind() == rec::operation_kind::draw);
    stream.clear();

    // Push bytes are owned; partial overlaps must preserve the earlier non-overwritten bytes.
    std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                     std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    stream.append<rec::operation_kind::push>({0}, bytes);
    bytes.fill(std::byte{9});
    stream.append<rec::operation_kind::push>({4}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    result = stream.simplify(true);
    CHECK(result.size() == 3);
    CHECK(result[0]->bytes[0] == std::byte{1});
    stream.clear();
    stream.append<rec::operation_kind::push>({0}, bytes);
    stream.append<rec::operation_kind::push>({0}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::push>({4}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    CHECK(stream.simplify(true).size() == 3);
    stream.clear();

    // Compute does not consume vertex/index state; it does consume pipeline and push state.
    stream.append<rec::operation_kind::vertex>({1});
    stream.append<rec::operation_kind::pipeline>({7});
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    result = stream.simplify(true);
    CHECK(result.size() == 2);
    CHECK(result[0]->kind() == rec::operation_kind::pipeline);

    // Reusing a warmed selection buffer must not allocate for ordinary push ranges.
    stream.clear();
    bytes.fill(std::byte{1});
    stream.append<rec::operation_kind::push>({0}, bytes);
    stream.append<rec::operation_kind::push>({4}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    deren::utility::vessel::vector<rec::render_operation const*> selection;
    selection.reserve(128);
    utility_memory_test_fail_after(0);
    stream.simplify_into(selection, true);
    auto refused = deren::utility::memory::try_allocate(1);
    CHECK(!refused); // the injected failure must still be available after simplification
    if (refused)
        deren::utility::memory::deallocate(*refused);
    CHECK(selection.size() == 3);
    CHECK(selection[0]->bytes[0] == std::byte{1});
    CHECK(selection[1]->data[0] == 4 && selection[2]->is_unique());
    stream.simplify_into(selection, false, false);
    CHECK(selection.size() == 3);

    // A push straddling the inline coverage boundary must preserve partial overlaps.
    stream.clear();
    stream.append<rec::operation_kind::push>({1020}, bytes);
    stream.append<rec::operation_kind::push>({1024}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    stream.simplify_into(selection, true);
    CHECK(selection.size() == 3);
    stream.clear();
    stream.append<rec::operation_kind::push>({1020}, bytes);
    stream.append<rec::operation_kind::push>({1020}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::push>({1024}, std::span(bytes).first(4));
    stream.append<rec::operation_kind::dispatch>({1, 1, 1});
    stream.simplify_into(selection, true);
    CHECK(selection.size() == 3 && selection[0]->bytes.size() == 4);
    // Canonical state selection may borrow records from separate streams, without copying payloads.
    rec::operation_stream prior;
    prior.append<rec::operation_kind::push>({0}, bytes);
    prior.simplify_into(selection, false);
    rec::operation_stream next_state;
    next_state.append<rec::operation_kind::push>({0}, std::span(bytes).first(4));
    next_state.append<rec::operation_kind::push>({4}, std::span(bytes).first(4));
    auto next_selection = next_state.simplify(false);
    selection.insert(selection.end(), next_selection.begin(), next_selection.end());
    rec::simplify_selection(selection, false);
    CHECK(selection.size() == 2);
    CHECK(selection[0] == next_selection[0] && selection[1] == next_selection[1]);
    // Deterministic generated streams compare consumer observations, not optimizer internals.
    std::uint32_t random = 1234567;
    auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
    for (unsigned trial = 0; trial < 256; ++trial) {
        stream.clear();
        for (unsigned i = 0; i < 96; ++i) {
            auto value = next();
            switch (value % 4) {
            case 0:
                stream.append<rec::operation_kind::vertex>({value % 7});
                break;
            case 1:
                stream.append<rec::operation_kind::index>({value % 5});
                break;
            case 2:
                stream.append<rec::operation_kind::pipeline>({value % 3});
                break;
            default:
                stream.append<rec::operation_kind::draw_indexed>({value % 11});
                break;
            }
        }
        auto observe = [](auto const& operations) {
            std::array<std::uint64_t, 3> state{};
            deren::utility::vessel::vector<std::array<std::uint64_t, 4>> observations;
            for (auto* op : operations) {
                if (op->kind() == rec::operation_kind::vertex)
                    state[0] = op->data[0];
                if (op->kind() == rec::operation_kind::index)
                    state[1] = op->data[0];
                if (op->kind() == rec::operation_kind::pipeline)
                    state[2] = op->data[0];
                if (op->kind() == rec::operation_kind::draw_indexed)
                    observations.push_back({state[0], state[1], state[2], op->data[0]});
            }
            return observations;
        };
        CHECK(observe(stream.simplify(true)) == observe(stream.simplify(true, false)));
    }
    return deren::vk_test::finish("render_operations");
}
