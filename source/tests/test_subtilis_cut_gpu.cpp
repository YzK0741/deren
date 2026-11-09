// The GPU cut's real-device A/B: build a static DAG, upload it, run `subtilis_cut.comp.spv` on one
// item through a real Vulkan command buffer, then compare the selected meshlet run and the mesh-task
// command with the CPU `select_cut` oracle.
//
// This test exists because the viewer's only DAG-bearing asset is skinned: its visibility frame is not
// admitted, so the runtime eligibility gate never reaches the cut. The shader therefore needs a direct
// device exercise that does not depend on the app's asset set.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

import deren.promise.rhi;
import deren.utility.shared;
import deren.engine.backend_loader;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
import deren.engine.pipelines;
import deren.engine.subtilis.lod;
import deren.engine.subtilis.selection;
import deren.engine.subtilis.dag_lod;
import deren.engine.subtilis.cut;

namespace rhi = deren::promise::rhi;
namespace sub = deren::engine::subtilis;
namespace engine = deren::engine;

namespace {
    template <typename T>
    std::span<std::uint8_t const> bytes_of(std::span<T> values) {
        return {reinterpret_cast<std::uint8_t const*>(values.data()), values.size_bytes()};
    }

    std::shared_ptr<sub::built_geometry const> make_geometry() {
        constexpr unsigned side = 33;
        std::vector<std::array<float, 8>> vertices;
        std::vector<std::uint32_t> indices;
        vertices.reserve(side * side);
        for (unsigned y = 0; y < side; ++y) {
            for (unsigned x = 0; x < side; ++x) {
                float const z = 0.25f * std::sin(float(x) * .7f) * std::cos(float(y) * .7f);
                vertices.push_back({(float(x) - 16.f) * .1f, (float(y) - 16.f) * .1f, z, 0.f, 0.f, 1.f, float(x) / side, float(y) / side});
            }
        }
        for (unsigned y = 0; y + 1 < side; ++y) {
            for (unsigned x = 0; x + 1 < side; ++x) {
                auto a = y * side + x;
                for (auto id : {a, a + 1, a + side, a + 1, a + side + 1, a + side}) {
                    indices.push_back(id);
                }
            }
        }
        auto built = sub::build({{bytes_of(std::span(vertices)), 32, unsigned(vertices.size()), bytes_of(std::span(indices)), 4, 0, unsigned(indices.size()), 0}}, {.max_depth = 3});
        return built ? std::make_shared<sub::built_geometry const>(std::move(*built)) : nullptr;
    }

    std::vector<std::uint8_t> read_spirv() {
        std::ifstream file(VR_SHADER_DIR "/subtilis_cut.comp.spv", std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    rhi::unique_object<rhi::buffer> make_storage(rhi::api_core& core, std::uint64_t size) {
        return core.resources->create_buffer(rhi::buffer_desc{
            .size = size,
            .usage = rhi::buffer_usage::storage_coherent,
            .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage),
        });
    }

    struct cut_push {
        glm::uvec2 items = glm::uvec2(0u);
        std::uint32_t item_count = 0;
        std::uint32_t reserved = 0;
    };
    static_assert(sizeof(cut_push) == 16u);
} // namespace

int main() {
    rhi::create_info options{};
    options.window_width = options.window_height = 64;
    options.window_visible = false;
    options.validation_layers = true;
    auto core = engine::load_api_core(options);
    if (!core) {
        return deren::vk_test::skip("test_subtilis_cut_gpu", "no Vulkan device");
    }
    auto* addresses = rhi::query_extension<rhi::device_address>(*core);
    auto* submission = rhi::query_extension<rhi::command_submission>(*core);
    CHECK(addresses != nullptr && submission != nullptr);
    if (!addresses || !submission) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }

    auto geometry = make_geometry();
    CHECK(geometry && geometry->hierarchy_dag() != nullptr);
    if (!geometry || !geometry->hierarchy_dag()) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }
    auto const* dag = geometry->hierarchy_dag();
    CHECK(dag->groups().size() <= sub::cut_max_groups);
    CHECK(!dag->groups().empty() && !dag->clusters().empty());

    auto uploaded = sub::upload_geometry(core, geometry, rhi::to_bits(rhi::buffer_flag::device_address), sub::geometry_upload_mode::synchronous);
    CHECK(uploaded && (*uploaded)->cut_groups() != nullptr && (*uploaded)->cut_references() != nullptr && (*uploaded)->cut_clusters() != nullptr);
    if (!uploaded || (*uploaded)->cut_groups() == nullptr || (*uploaded)->cut_references() == nullptr || (*uploaded)->cut_clusters() == nullptr) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }

    constexpr std::uint32_t width = 512, height = 512;
    constexpr std::uint32_t capacity = 4096;
    auto model = glm::mat4(1.0f);
    auto view = glm::lookAt(glm::vec3(1.4f, 1.1f, 2.6f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    auto projection = glm::perspective(glm::radians(60.f), float(width) / float(height), .1f, 100.f);
    auto item_view = sub::make_item_view(model, view, projection, width, height);
    CHECK(std::isfinite(item_view.selection.near_plane));
    item_view.selection.target_error_px = 8.f;
    item_view.selection.force_finest = false;

    deren::utility::vessel::vector<engine::meshlet> cpu;
    auto cpu_result = sub::select_cut(*dag, item_view.selection, capacity, cpu);
    CHECK(cpu_result && !cpu.empty());

    auto item_buffer = make_storage(*core, sub::cut_frame_bytes());
    auto meshlet_buffer = make_storage(*core, std::uint64_t(capacity) * sizeof(engine::meshlet));
    auto command_buffer = make_storage(*core, 12);
    CHECK(item_buffer && meshlet_buffer && command_buffer);
    if (!item_buffer || !meshlet_buffer || !command_buffer) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }

    auto* frame = reinterpret_cast<sub::cut_frame*>(item_buffer->mapped().data());
    auto* item = reinterpret_cast<sub::cut_item*>(item_buffer->mapped().data() + sizeof(sub::cut_frame));
    std::memcpy(frame->view, item_view.selection.view.data(), sizeof(frame->view));
    std::memcpy(frame->projection, item_view.selection.projection.data(), sizeof(frame->projection));
    frame->width = width;
    frame->height = height;
    frame->near_plane = item_view.selection.near_plane;
    frame->target_error_px = item_view.selection.target_error_px;
    frame->kind = 1;
    frame->item_offset = sizeof(sub::cut_frame);
    frame->item_stride = sizeof(sub::cut_item);
    frame->item_capacity = 1;

    std::memcpy(item->model, item_view.selection.model.data(), sizeof(item->model));
    item->groups = addresses->buffer_address(*(*uploaded)->cut_groups(), 0);
    item->references = addresses->buffer_address(*(*uploaded)->cut_references(), 0);
    item->clusters = addresses->buffer_address(*(*uploaded)->cut_clusters(), 0);
    item->meshlets = addresses->buffer_address(*meshlet_buffer, 0);
    item->command = addresses->buffer_address(*command_buffer, 0);
    item->group_count = static_cast<std::uint32_t>(dag->groups().size());
    item->cluster_count = static_cast<std::uint32_t>(dag->clusters().size());
    item->output_base = 0;
    item->output_capacity = capacity;
    item->command_index = 0;
    item->flags = 0;
    item->page_arena = 0;
    item->page_table = 0;
    item->page_requests = 0;
    item->page_count = 0;
    item->request_capacity = 0;
    item->request_frame = 0;
    item->lod0_count = 0;

    auto spirv = read_spirv();
    CHECK(!spirv.empty());
    auto pipeline = engine::pipelines::build_subtilis_cut(*core, spirv);
    CHECK(pipeline && pipeline->trace.has_value() && pipeline->trace->contract != nullptr);
    if (!pipeline || !pipeline->trace || pipeline->trace->contract == nullptr) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }

    auto commands = core->commands->make_command_buffer({});
    CHECK(commands != nullptr);
    if (!commands) {
        return deren::vk_test::finish("test_subtilis_cut_gpu");
    }
    std::uint64_t const items_address = addresses->buffer_address(*item_buffer, 0);
    cut_push const push{
        .items = glm::uvec2(std::uint32_t(items_address & 0xFFFFFFFFu), std::uint32_t(items_address >> 32u)),
        .item_count = 1,
        .reserved = 0,
    };
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->bind_pipeline(*pipeline->trace->contract) == rhi::error::ok);
    CHECK(commands->push_constants(0, std::as_bytes(std::span(&push, 1))) == rhi::error::ok);
    commands->dispatch(1, 1, 1);
    CHECK(commands->end_recording() == rhi::error::ok);
    auto submitted = submission->submit(*commands);
    CHECK(submitted.has_value());
    if (submitted) {
        CHECK(submission->wait(*submitted, UINT64_MAX) == rhi::error::ok);
    }

    std::uint32_t command[3]{};
    std::memcpy(command, command_buffer->mapped().data(), sizeof(command));
    CHECK(command[0] == cpu.size() && command[1] == 1 && command[2] == 1);
    CHECK(command[0] <= capacity);
    deren::vk_test::write_line("GPU cut: {} selected, CPU cut: {} selected, {} clusters total", command[0], cpu.size(), dag->clusters().size());

    auto const* gpu = reinterpret_cast<engine::meshlet const*>(meshlet_buffer->mapped().data());
    bool same = command[0] == cpu.size();
    for (std::size_t i = 0; same && i < cpu.size(); ++i) {
        same = std::memcmp(&gpu[i], &cpu[i], sizeof(engine::meshlet)) == 0;
    }
    CHECK(same);
    if (!same) {
        deren::vk_test::write_line("GPU cut: {} selected, CPU cut: {} selected, {} clusters total", command[0], cpu.size(), dag->clusters().size());
        for (std::size_t i = 0; i < std::min<std::size_t>(cpu.size(), command[0]); ++i) {
            if (std::memcmp(&gpu[i], &cpu[i], sizeof(engine::meshlet)) != 0) {
                deren::vk_test::write_line("first mismatch at {}: gpu first={} count={} cpu first={} count={}", i, gpu[i].first_index, gpu[i].index_count, cpu[i].first_index, cpu[i].index_count);
                break;
            }
        }
    }
    return deren::vk_test::finish("test_subtilis_cut_gpu");
}
