#include "vk_test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>
import deren.promise.rhi;
import deren.engine.backend_loader;
import deren.engine.buffer_transfer;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
namespace rhi = deren::promise::rhi;
namespace sub = deren::engine::subtilis;
namespace engine = deren::engine;
namespace sync = rhi::synchronization;
namespace {
    template <typename T>
    auto bytes(std::span<T> data) {
        return std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(data.data()), data.size_bytes());
    }
    std::shared_ptr<sub::built_geometry const> geometry(unsigned side) {
        std::vector<std::array<float, 8>> vertices;
        std::vector<std::uint32_t> indices;
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x)
                vertices.push_back({float(x), float(y), 0, 0, 0, 1, float(x) / side, float(y) / side});
        for (unsigned y = 0; y + 1 < side; ++y)
            for (unsigned x = 0; x + 1 < side; ++x) {
                auto a = y * side + x;
                for (auto i : {a, a + 1, a + side, a + 1, a + side + 1, a + side})
                    indices.push_back(i);
            }
        auto result = sub::build({{bytes(std::span(vertices)), 32, unsigned(vertices.size()), bytes(std::span(indices)), 4, 0, unsigned(indices.size()), 0}}, {.max_depth = 1});
        CHECK(result);
        return result ? std::make_shared<sub::built_geometry const>(std::move(*result)) : nullptr;
    }
    std::uint64_t stamp(rhi::api_core& core) {
        auto* submission = rhi::query_extension<rhi::command_submission>(core);
        auto commands = core.commands->make_command_buffer({});
        CHECK(commands && submission);
        if (!commands || !submission)
            return 0;
        CHECK(commands->begin_recording({}) == rhi::error::ok);
        CHECK(commands->end_recording() == rhi::error::ok);
        auto receipt = submission->submit(*commands);
        CHECK(receipt);
        if (!receipt)
            return 0;
        CHECK(submission->wait(*receipt, UINT64_MAX) == rhi::error::ok);
        return receipt->sequence;
    }
    void verify(rhi::shared_object<rhi::api_core> core, std::shared_ptr<sub::built_geometry const> source, sub::geometry_upload_mode mode) {
        auto before = stamp(*core);
        auto* capabilities = rhi::query_extension<rhi::device_capabilities>(*core);
        bool const build_supported = capabilities && capabilities->ray_query();
        auto flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::transfer_source);
        if (build_supported)
            flags |= rhi::to_bits(rhi::buffer_flag::acceleration_structure_input);
        auto result = sub::upload_geometry(core, source, flags, mode);
        CHECK(result);
        auto after = stamp(*core);
        CHECK(after - before == (mode == sub::geometry_upload_mode::synchronous ? 1u : 2u));
        if (!result)
            return;
        engine::buffer_transfer_service transfers(core);
        auto batch = transfers.make_batch();
        rhi::buffer* buffers[] = {(*result)->vertices(), (*result)->indices()};
        std::array payloads{std::as_bytes(source->vertices()), std::as_bytes(source->indices())};
        sync::scope readers{sync::stage::vertex_input | sync::stage::compute_shader | sync::stage::vertex_shader | sync::stage::fragment_shader, sync::access::read};
        for (unsigned i = 0; i < 2; ++i) {
            auto lease = engine::resource_lease<rhi::buffer>::from_shared(core, rhi::shared_object<rhi::buffer>{*result, buffers[i]});
            auto output = batch.readback(std::move(lease), 0, payloads[i].size(), readers, readers);
            CHECK(output && *output == i);
        }
        auto receipt = transfers.submit(std::move(batch));
        CHECK(receipt);
        if (!receipt)
            return;
        CHECK(receipt->wait(UINT64_MAX) == rhi::error::ok);
        for (unsigned i = 0; i < 2; ++i) {
            auto output = receipt->readback(i);
            CHECK(output && std::equal(output->begin(), output->end(), payloads[i].begin(), payloads[i].end()));
        }
        if (build_supported) {
            auto* addresses = rhi::query_extension<rhi::device_address>(*core);
            CHECK(addresses);
            if (!addresses)
                return;
            rhi::acceleration_structure_geometry input{
                .vertex_address = addresses->buffer_address(*buffers[0], 0),
                .vertex_stride = source->vertex_stride(),
                .vertex_count = source->vertex_count(),
                .index_address = addresses->buffer_address(*buffers[1], 0),
                .index_format = rhi::index_type::uint32,
                .index_count = source->lod0_index_count()};
            auto structure = core->resources->create_acceleration_structure({.geometries = &input, .geometry_count = 1});
            auto commands = core->commands->make_command_buffer({});
            CHECK(structure && commands);
            if (!structure || !commands)
                return;
            auto* space = rhi::query_extension<rhi::descriptor_space>(*core);
            CHECK(space && space->fixed_slots().count);
            if (space && space->fixed_slots().count) {
                auto range = space->fixed_slots();
                CHECK(space->write_acceleration_structure({.slot = range.base + range.count - 1, .resource = structure.get()}) == rhi::error::ok);
            }
            CHECK(commands->begin_recording({}) == rhi::error::ok);
            CHECK(commands->build_acceleration_structure(*structure) == rhi::error::ok);
            CHECK(commands->end_recording() == rhi::error::ok);
            auto* submission = rhi::query_extension<rhi::command_submission>(*core);
            auto built = submission->submit(*commands);
            CHECK(built);
            if (built)
                CHECK(submission->wait(*built, UINT64_MAX) == rhi::error::ok);
        }
    }
    void benchmark(rhi::create_info desc) {
        desc.validation_layers = false;
        auto core = engine::load_api_core(desc);
        CHECK(core);
        if (!core)
            return;
        for (unsigned side : {17u, 129u, 257u}) {
            auto source = geometry(side);
            if (!source)
                return;
            std::array<std::vector<double>, 2> times;
            for (unsigned trial = 0; trial < 16; ++trial)
                for (unsigned order = 0; order < 2; ++order) {
                    auto lane = (trial + order) % 2;
                    auto mode = lane ? sub::geometry_upload_mode::batched : sub::geometry_upload_mode::synchronous;
                    auto start = std::chrono::steady_clock::now();
                    auto result = sub::upload_geometry(core, source, rhi::to_bits(rhi::buffer_flag::device_address), mode);
                    auto stop = std::chrono::steady_clock::now();
                    CHECK(result);
                    if (!result)
                        return;
                    if (trial >= 4)
                        times[lane].push_back(std::chrono::duration<double, std::micro>(stop - start).count());
                    // Drop outside the measured region; do not retain an identical-content generation
                    // that would turn the synchronous path into a native content-cache hit.
                }
            auto median = [](std::vector<double> values) { std::sort(values.begin(), values.end()); return (values[5] + values[6]) * .5; };
            auto payload = source->vertices().size() + source->indices().size_bytes();
            std::printf("UPLOAD_BENCH side=%u bytes=%zu samples=12 synchronous_us=%.3f batched_us=%.3f\n", side, payload, median(times[0]), median(times[1]));
        }
    }
} // namespace
int main(int argc, char** argv) {
    bool device = false, timed = false;
    for (int i = 1; i < argc; ++i) {
        device |= std::string_view(argv[i]) == "--with-device";
        timed |= std::string_view(argv[i]) == "--benchmark";
    }
    if (!device && !timed)
        return deren::vk_test::finish("test_subtilis_transfer (device skipped)");
    rhi::create_info desc{};
    desc.window_width = desc.window_height = 64;
    desc.window_visible = false;
    desc.validation_layers = true;
    auto core = engine::load_api_core(desc);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_subtilis_transfer");
    auto source = geometry(17);
    if (!source)
        return deren::vk_test::finish("test_subtilis_transfer");
    for (auto mode : {sub::geometry_upload_mode::synchronous, sub::geometry_upload_mode::batched, sub::geometry_upload_mode::automatic})
        verify(core, source, mode);
    if (timed)
        benchmark(desc);
    return deren::vk_test::finish("test_subtilis_transfer");
}
