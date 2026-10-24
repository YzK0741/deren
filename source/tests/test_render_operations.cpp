#include "vk_test.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>
import deren.promise.render_operations;
import deren.utility.shared;
namespace rec = deren::promise::rhi::recording;

int main() {
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
