#include "vk_test.h"

#include <array>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <vector>
import deren.engine.gaussian_splatting.gpu_frame;
import deren.engine.pass;
import deren.engine.pass.gaussian_splatting;
#include "raster_probe.hpp"
namespace gs = deren::engine::gaussian_splatting;
namespace pass = deren::engine::pass;
namespace {
    struct commands : raster_probe_commands {
        std::vector<std::string> operations;
        std::vector<rhi::buffer_use> consumers;
        void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
            operations.push_back("compute");
        }
        rhi::error bind_pipeline(rhi::pipeline const& p) override {
            operations.push_back("bind");
            return raster_probe_commands::bind_pipeline(p);
        }
        rhi::error begin_rendering(rhi::rendering_info const& info) override {
            operations.push_back("begin");
            return raster_probe_commands::begin_rendering(info);
        }
        rhi::error barrier(rhi::barrier_group const& group) override {
            for (auto const& b : group.buffers)
                consumers.push_back(b.to);
            return raster_probe_commands::barrier(group);
        }
        rhi::error draw_indirect(rhi::buffer const&, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override {
            CHECK(offset == 0u && count == 1u && stride == 16u);
            operations.push_back("indirect");
            ++draws;
            return rhi::error::ok;
        }
    };
    struct sorter : rhi::radix_sorter {
        int& releases;
        explicit sorter(int& count)
            : radix_sorter("fake_sorter")
            , releases(count) {
        }
        void release() noexcept override {
            ++releases;
            delete this;
        }
        rhi::radix_sort_requirements requirements(std::uint32_t) const noexcept override {
            return {64u, 16u, rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::transfer_destination)};
        }
        rhi::error record(rhi::command_buffer& list, rhi::radix_sort_info const& info) override {
            CHECK(info.keys && info.values && info.scratch);
            static_cast<commands&>(list).operations.push_back("sort");
            return rhi::error::ok;
        }
    };
    struct sorting : rhi::gpu_sorting {
        int& releases;
        explicit sorting(int& count)
            : gpu_sorting("fake_sorting")
            , releases(count) {
        }
        rhi::radix_sorter* create_radix_sorter() override {
            return new sorter{releases};
        }
    };
    struct core : raster_probe_core {
        int sorter_releases = 0;
        sorting sorting_view{sorter_releases};
        std::vector<raster_test::raster_probe_buffer*> allocated;
        rhi::ability_bits abilities() const noexcept override {
            return raster_probe_core::abilities() | rhi::to_bits(rhi::extension_kind::gpu_sorting);
        }
        rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            return kind == rhi::extension_kind::gpu_sorting ? &sorting_view : raster_probe_core::query_extension(kind);
        }
        rhi::buffer* create_buffer(rhi::buffer_desc const& desc) override {
            auto* buffer = static_cast<raster_test::raster_probe_buffer*>(raster_probe_core::create_buffer(desc));
            if (buffer) {
                buffer->host_enabled = desc.usage == rhi::buffer_usage::storage_coherent;
                if (buffer->host_enabled)
                    buffer->storage.resize(static_cast<std::size_t>(desc.size));
                allocated.push_back(buffer);
            }
            return buffer;
        }
    };
    gs::raster_camera camera() {
        gs::raster_camera c;
        c.extent = {33, 33};
        c.view_projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, .1f, 100.0f);
        c.view_projection[1][1] *= -1;
        return c;
    }
    deren::gaussian::asset source() {
        deren::gaussian::asset a;
        deren::gaussian::particle p;
        p.center = {0, 0, -2};
        p.scale = {.05f, .05f, .05f};
        p.rotation = {1, 0, 0, 0};
        p.opacity = .5f;
        a.particles.push_back(p);
        return a;
    }
    bool push(void* owner, rhi::command_buffer&, std::span<std::byte const> bytes, std::uint32_t) {
        static_cast<commands*>(owner)->operations.push_back("push" + std::to_string(bytes.size()));
        return true;
    }
} // namespace
int main() {
    core device;
    auto asset = gs::make_raster_asset(device, source());
    CHECK(asset);
    std::array<gs::raster_instance, 2> inputs = {gs::raster_instance{*asset, glm::mat4{1}, 20}, gs::raster_instance{*asset, glm::mat4{1}, 10}};
    auto frame = gs::prepare_gpu_frame(device, inputs, camera());
    CHECK(frame && (*frame)->candidate_count() == 2u && (*frame)->byte_size() == 752u);
    if (!frame)
        return deren::vk_test::finish("gaussian_gpu_frame");
    std::array<gs::gpu_instance_range, 2> ranges{};
    std::memcpy(ranges.data(), device.allocated[3]->mapped().data(), sizeof(ranges));
    CHECK(ranges[0].instance_index == 1u && ranges[0].begin == 0u && ranges[0].end == 1u);
    CHECK(ranges[1].instance_index == 0u && ranges[1].begin == 1u && ranges[1].end == 2u);
    std::array<gs::instance_record, 2> records{};
    std::memcpy(records.data(), device.allocated[2]->mapped().data(), sizeof(records));
    auto cached_address = [](gs::instance_record const& record) {
        return std::uint64_t{record.padding[0]} | (std::uint64_t{record.padding[1]} << 32u);
    };
    CHECK(cached_address(records[1]) != 0u && cached_address(records[0]) == cached_address(records[1]) + 48u);
    CHECK((*frame)->params().flags == 1u);
    auto const calls = device.calls;
    auto reused = gs::prepare_gpu_frame(device, inputs, camera(), {}, frame->get());
    CHECK(reused && reused->get() == frame->get() && device.calls == calls);
    auto limited = gs::prepare_gpu_frame(device, inputs, camera(), {2u, 751u}, frame->get());
    CHECK(!limited && limited.error().code == gs::error_code::limit_exceeded && device.calls == calls);
    inputs[1].stable_id = 20;
    CHECK(!gs::prepare_gpu_frame(device, inputs, camera()));
    inputs[1].stable_id = 10;
    std::weak_ptr<gs::raster_asset const> weak = *asset;
    asset->reset();
    inputs = {};
    CHECK(!weak.expired());
    commands list;
    int pipeline_releases = 0;
    raster_test::raster_probe_pipeline pipeline{pipeline_releases};
    gs::gpu_prepare_pipelines pipelines{&pipeline, &pipeline, &pipeline};
    CHECK((*frame)->record_preparation(list, pipelines, {&list, push}) == rhi::error::ok);
    CHECK((list.operations == std::vector<std::string>{"bind", "push8", "compute", "bind", "push8", "compute", "sort", "bind", "push8", "compute"}));
    CHECK((list.consumers == std::vector<rhi::buffer_use>{rhi::buffer_use::shader_read_write, rhi::buffer_use::shader_read, rhi::buffer_use::shader_read, rhi::buffer_use::indirect_read, rhi::buffer_use::host_read}));
    CHECK((*frame)->completed_stats(123u).frame_id == 123u);
    list.operations.clear();
    gs::gpu_frame_timing const timing{&list, [](void* owner, std::uint32_t phase) {
                                          static_cast<commands*>(owner)->operations.push_back("mark" + std::to_string(phase));
                                      }};
    CHECK((*frame)->record_preparation(list, pipelines, {&list, push}, timing) == rhi::error::ok);
    CHECK((list.operations == std::vector<std::string>{"bind", "push8", "compute", "bind", "push8", "compute", "mark0", "sort", "mark1", "bind", "push8", "compute", "mark2"}));
    pass::gaussian_splatting_pass stage;
    stage.set_gpu_frame(*frame);
    CHECK(!stage.gpu_ready());
    std::array<std::uint8_t, 4> const shader = {3, 2, 35, 7};
    pass::pass_context context;
    context.face = &device;
    context.depth_format = rhi::image_format::depth;
    context.owner = const_cast<void*>(static_cast<void const*>(&shader));
    context.shader = [](void* owner, std::string_view) { return std::span<std::uint8_t const>{*static_cast<std::array<std::uint8_t, 4> const*>(owner)}; };
    stage.create(context);
    CHECK(stage.gpu_ready());
    commands complete;
    pass::resolved_io io;
    io.list = &complete;
    io.extent = camera().extent;
    io.target_storage[0] = {.image_handle = reinterpret_cast<rhi::image*>(std::uintptr_t{1}), .view_handle = reinterpret_cast<rhi::image_view*>(std::uintptr_t{2})};
    io.target_storage[1] = {.image_handle = reinterpret_cast<rhi::image*>(std::uintptr_t{3}), .view_handle = reinterpret_cast<rhi::image_view*>(std::uintptr_t{4})};
    io.targets = std::span{io.target_storage}.first(2);
    io.push_block = {&complete, push};
    stage.record(io);
    CHECK(stage.last_error() == rhi::error::ok && complete.draws == 1u);
    CHECK((complete.operations == std::vector<std::string>{"bind", "push8", "compute", "bind", "push8", "compute", "sort", "bind", "push8", "compute", "bind", "push112", "begin", "indirect"}));
    auto empty = gs::prepare_gpu_frame(device, {}, camera());
    CHECK(empty);
    stage.set_gpu_frame(*empty);
    commands zero;
    io.list = &zero;
    io.push_block.owner = &zero;
    stage.record(io);
    CHECK(zero.draws == 0u && zero.operations.empty());
    stage.set_frame({});
    reused->reset();
    frame->reset();
    CHECK(weak.expired() && device.sorter_releases == 1);
    {
        core failed;
        auto a = gs::make_raster_asset(failed, source());
        std::array<gs::raster_instance, 1> one = {gs::raster_instance{*a, glm::mat4{1}, 1}};
        failed.fail_at = failed.calls + 3u;
        auto partial = gs::prepare_gpu_frame(failed, one, camera());
        CHECK(!partial && partial.error().code == gs::error_code::allocation_failure);
        CHECK(failed.releases == 2 && failed.sorter_releases == 1);
        failed.fail_at = 0u;
        auto slot0 = gs::prepare_gpu_frame(failed, one, camera());
        auto slot1 = gs::prepare_gpu_frame(failed, one, camera());
        CHECK(slot0 && slot1 && slot0->get() != slot1->get());
        std::weak_ptr<gs::raster_asset const> retained = *a;
        auto cloud = source();
        cloud.particles.resize(4u, cloud.particles.front());
        auto grown = gs::make_raster_asset(failed, std::move(cloud));
        std::array<gs::raster_instance, 1> const growth = {gs::raster_instance{*grown, glm::mat4{1}, 1}};
        auto const address = (*slot0)->params().references_address;
        failed.fail_at = failed.calls + 2u;
        auto refused = gs::prepare_gpu_frame(failed, growth, camera(), {}, slot0->get());
        CHECK(!refused && (*slot0)->candidate_count() == 1u && (*slot0)->params().references_address == address);
        failed.fail_at = 0u;
        auto updated = gs::prepare_gpu_frame(failed, growth, camera(), {}, slot0->get());
        CHECK(updated && (*slot1)->candidate_count() == 1u && (*slot1)->params().references_address != (*updated)->params().references_address);
        a->reset();
        one = {};
        CHECK(!retained.expired()); // the other pending slot still owns the original cloud
        slot1->reset();
        CHECK(retained.expired());
    }
    return deren::vk_test::finish("gaussian_gpu_frame");
}
