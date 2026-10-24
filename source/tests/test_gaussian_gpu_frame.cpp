#include "vk_test.h"

#include <array>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <vector>
import deren.utility.shared;
import deren.engine.gaussian_splatting.gpu_frame;
import deren.engine.render_resource;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.pass;
import deren.engine.pass.gaussian_splatting;
#include "raster_probe.hpp"
namespace gs = deren::engine::gaussian_splatting;
namespace pass = deren::engine::pass;
namespace {
    struct commands : raster_probe_commands {
        deren::utility::vessel::vector<deren::utility::vessel::string> operations;
        deren::utility::vessel::vector<rhi::buffer_use> consumers;
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
        rhi::error record(rhi::command_recorder& list, rhi::radix_sort_info const& info) override {
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
        int idle_waits = 0;
        void wait_idle() override {
            ++idle_waits;
        }
        sorting sorting_view{sorter_releases};
        deren::utility::vessel::vector<raster_test::raster_probe_buffer*> allocated;
        rhi::ability_bits abilities() const noexcept override {
            return raster_probe_core::abilities() | rhi::to_bits(rhi::extension_kind::gpu_sorting);
        }
        rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            return kind == rhi::extension_kind::gpu_sorting ? &sorting_view : raster_probe_core::query_extension(kind);
        }
        rhi::unique_object<rhi::buffer> create_buffer(rhi::buffer_desc const& desc) override {
            rhi::unique_object<rhi::buffer> owned = raster_probe_core::create_buffer(desc);
            auto* buffer = static_cast<raster_test::raster_probe_buffer*>(owned.release());
            if (buffer) {
                buffer->host_enabled = desc.usage == rhi::buffer_usage::storage_coherent;
                if (buffer->host_enabled)
                    buffer->storage.resize(static_cast<std::size_t>(desc.size));
                allocated.push_back(buffer);
            }
            return rhi::unique_object<rhi::buffer>{buffer};
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
        static_cast<commands*>(owner)->operations.push_back("push" + deren::utility::vessel::to_string(bytes.size()));
        return true;
    }
} // namespace
template <typename Frame>
void check_managed_storage(Frame const& frame) {
    if constexpr (requires { frame.resources(); }) {
        CHECK(frame.resources().size() == 10);
        namespace rr = deren::engine::render_resource;
        CHECK(frame.resources().group(rr::instances::lifetime::frame_slot).size() == 10);
        CHECK(frame.resources().find(rr::resource_id::gaussian_keys)->objects[0].buffer.get() == frame.keys_buffer());
        CHECK(frame.resources().find(rr::resource_id::gaussian_draw)->objects[0].buffer.get() == frame.draw_buffer());
    } else {
        CHECK_MSG(false, "GPU frame buffers must be backed by a passchain resource snapshot");
    }
}

template <typename Request>
void check_initial_upload() {
    if constexpr (requires(Request r) { r.initial_data; r.initial_data_size; r.initialization_key; }) {
        auto device = std::make_shared<core>();
        deren::engine::pass::pass_chain chain;
        auto& provider = chain.emplace<deren::engine::pass::buffer_storage_pass>();
        auto payload = std::make_shared<deren::utility::vessel::vector<std::byte>>(4, std::byte{0x37});
        std::weak_ptr<deren::utility::vessel::vector<std::byte>> weak = payload;
        Request request;
        namespace rr = deren::engine::render_resource;
        request.id = rr::resource_id::gaussian_instances;
        request.type = rr::instances::kind::buffer;
        request.buffer.size = 4;
        request.buffer.usage = rhi::buffer_usage::storage_coherent;
        request.initial_data = std::shared_ptr<std::byte const>{payload, payload->data()};
        request.initial_data_size = payload->size();
        request.initialization_key = std::make_shared<int const>(1);
        provider.configure(std::span{&request, 1});
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::ok);
        auto old = chain.resources().snapshot();
        CHECK(old.find(request.id)->objects[0].buffer->mapped()[0] == std::byte{0x37});
        request.initial_data.reset();
        payload.reset();
        provider.configure(std::span{&request, 1});
        CHECK(weak.expired()); // Neither generation metadata nor the producer retains upload bytes.
        auto calls = device->calls;
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::ok);
        CHECK(chain.resources().snapshot().same(old) && device->calls == calls);
        auto other = std::make_shared<core>();
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{other}, {}) != rhi::error::ok);
        CHECK(chain.resources().snapshot().same(old) && other->calls == 0);
        auto replacement = std::make_shared<deren::utility::vessel::vector<std::byte>>(4, std::byte{0x71});
        request.initialization_key = std::make_shared<int const>(2);
        request.initial_data = std::shared_ptr<std::byte const>{replacement, replacement->data()};
        provider.configure(std::span{&request, 1});
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::ok);
        CHECK(!chain.resources().snapshot().same(old));
        CHECK(chain.resources().snapshot().find(request.id)->objects[0].buffer->mapped()[0] == std::byte{0x71});
        CHECK(old.find(request.id)->objects[0].buffer->mapped()[0] == std::byte{0x37});
        request.initialization_key = std::make_shared<int const>(3);
        deren::utility::vessel::array<Request, 2> duplicate{request, request};
        duplicate[0].initial_data.reset();
        provider.configure(duplicate);
        calls = device->calls;
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::ok && device->calls == calls + 1);
        auto merged = chain.resources().snapshot();
        CHECK(merged.find(request.id)->objects[0].buffer->mapped()[0] == std::byte{0x71});
        request.zero_on_create = true;
        provider.configure(std::span{&request, 1});
        calls = device->calls;
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::invalid_argument);
        CHECK(device->calls == calls && chain.resources().snapshot().same(merged));
        request.zero_on_create = false;
        request.initial_data_size = 5;
        provider.configure(std::span{&request, 1});
        CHECK(chain.prepare_resources(rhi::shared_object<rhi::api_core>{device}, {}) == rhi::error::invalid_argument);
        CHECK(device->calls == calls && chain.resources().snapshot().same(merged));
    } else {
        CHECK_MSG(false, "Managed uploads need retained initial data and a stable content identity");
    }
}
int main() {
    check_initial_upload<deren::engine::render_resource::instances::request>();
    core device;
    auto asset = gs::make_raster_asset(device, source());
    CHECK(asset);
    deren::utility::vessel::array<gs::raster_instance, 2> inputs = {gs::raster_instance{*asset, glm::mat4{1}, 20}, gs::raster_instance{*asset, glm::mat4{1}, 10}};
    auto frame = gs::prepare_gpu_frame(device, inputs, camera());
    if (frame)
        check_managed_storage(**frame);
    CHECK(frame && (*frame)->candidate_count() == 2u && (*frame)->byte_size() == 752u);
    if (!frame)
        return deren::vk_test::finish("gaussian_gpu_frame");
    deren::utility::vessel::array<gs::gpu_instance_range, 2> ranges{};
    std::memcpy(ranges.data(), device.allocated[3]->mapped().data(), sizeof(ranges));
    CHECK(ranges[0].instance_index == 1u && ranges[0].begin == 0u && ranges[0].end == 1u);
    CHECK(ranges[1].instance_index == 0u && ranges[1].begin == 1u && ranges[1].end == 2u);
    deren::utility::vessel::array<gs::instance_record, 2> records{};
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
    CHECK((list.operations == deren::utility::vessel::vector<deren::utility::vessel::string>{"bind", "push8", "compute", "bind", "push8", "compute", "sort", "bind", "push8", "compute"}));
    CHECK((list.consumers == deren::utility::vessel::vector<rhi::buffer_use>{rhi::buffer_use::shader_read_write, rhi::buffer_use::shader_read, rhi::buffer_use::shader_read, rhi::buffer_use::indirect_read, rhi::buffer_use::host_read}));
    CHECK((*frame)->completed_stats(123u).frame_id == 123u);
    list.operations.clear();
    gs::gpu_frame_timing const timing{&list, [](void* owner, std::uint32_t phase) {
                                          static_cast<commands*>(owner)->operations.push_back("mark" + deren::utility::vessel::to_string(phase));
                                      }};
    CHECK((*frame)->record_preparation(list, pipelines, {&list, push}, timing) == rhi::error::ok);
    CHECK((list.operations == deren::utility::vessel::vector<deren::utility::vessel::string>{"bind", "push8", "compute", "bind", "push8", "compute", "mark0", "sort", "mark1", "bind", "push8", "compute", "mark2"}));
    pass::gaussian_splatting_pass stage;
    stage.set_gpu_frame(*frame);
    CHECK(!stage.gpu_ready());
    deren::utility::vessel::array<std::uint8_t, 4> const shader = {3, 2, 35, 7};
    pass::pass_context context;
    context.face = &device;
    context.depth_format = rhi::image_format::depth;
    context.owner = const_cast<void*>(static_cast<void const*>(&shader));
    context.shader = [](void* owner, std::string_view) { return std::span<std::uint8_t const>{*static_cast<deren::utility::vessel::array<std::uint8_t, 4> const*>(owner)}; };
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
    stage.record_owned(io);
    CHECK(stage.last_error() == rhi::error::ok && complete.draws == 1u);
    CHECK((complete.operations == deren::utility::vessel::vector<deren::utility::vessel::string>{"bind", "push8", "compute", "bind", "push8", "compute", "sort", "bind", "push8", "compute", "bind", "push112", "begin", "indirect"}));
    auto empty = gs::prepare_gpu_frame(device, {}, camera());
    CHECK(empty);
    stage.set_gpu_frame(*empty);
    commands zero;
    io.list = &zero;
    io.push_block.owner = &zero;
    stage.record_owned(io);
    CHECK(zero.draws == 0u && zero.operations.empty());
    stage.set_frame({});
    reused->reset();
    frame->reset();
    CHECK(weak.expired() && device.sorter_releases == 1);
    {
        core failed;
        auto a = gs::make_raster_asset(failed, source());
        deren::utility::vessel::array<gs::raster_instance, 1> one = {gs::raster_instance{*a, glm::mat4{1}, 1}};
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
        deren::utility::vessel::array<gs::raster_instance, 1> const growth = {gs::raster_instance{*grown, glm::mat4{1}, 1}};
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
    {
        auto leased = std::make_shared<core>();
        std::weak_ptr<core> root = leased;
        auto asset = gs::make_raster_asset(*leased, source());
        deren::utility::vessel::array<gs::raster_instance, 1> input{gs::raster_instance{*asset, glm::mat4{1}, 1}};
        auto owned = gs::prepare_gpu_frame(*leased, input, camera(), {}, nullptr, leased);
        CHECK(owned.has_value());
        if (owned) {
            auto storage = (*owned)->resources();
            asset->reset();
            input = {};
            leased.reset();
            CHECK(!root.expired());
            owned->reset();
            CHECK(!root.expired()); // a retained buffer generation also retains the device
            storage = {};
            CHECK(root.expired());
        }
    }
    {
        core failed;
        auto a = gs::make_raster_asset(failed, source());
        deren::utility::vessel::array<gs::raster_instance, 1> one{gs::raster_instance{*a, glm::mat4{1}, 1}};
        auto original = gs::prepare_gpu_frame(failed, one, camera());
        auto old_storage = (*original)->resources();
        auto cloud = source();
        cloud.particles.resize(4u, cloud.particles.front());
        auto big = gs::make_raster_asset(failed, std::move(cloud));
        one[0].asset = *big;
        failed.address.zero_at = failed.calls + 1u;
        auto refused = gs::prepare_gpu_frame(failed, one, camera(), {}, original->get());
        CHECK(!refused && refused.error().code == gs::error_code::invalid_address);
        CHECK((*original)->resources().same(old_storage));
        failed.address.zero_at = 0;
        auto medium_source = source();
        medium_source.particles.resize(2u, medium_source.particles.front());
        auto medium = gs::make_raster_asset(failed, std::move(medium_source));
        one[0].asset = *medium;
        auto expected = gs::prepare_gpu_frame(failed, one, camera());
        auto budget = (*expected)->byte_size();
        auto retry = gs::prepare_gpu_frame(failed, one, camera(), {100u, budget}, original->get());
        CHECK(retry && (*retry)->byte_size() == budget);
        CHECK(retry && (*retry)->keys_buffer()->size() == (*expected)->keys_buffer()->size());
    }
    {
        auto leased = std::make_shared<core>();
        auto asset = gs::make_raster_asset(*leased, source());
        deren::utility::vessel::array<gs::raster_instance, 1> input{gs::raster_instance{*asset, glm::mat4{1}, 1}};
        auto legacy = gs::prepare_gpu_frame(*leased, input, camera());
        auto promoted = gs::prepare_gpu_frame(*leased, input, camera(), {}, legacy->get(), leased);
        CHECK(!promoted && promoted.error().code == gs::error_code::invalid_argument);
        auto reused = gs::prepare_gpu_frame(*leased, input, camera(), {}, legacy->get());
        CHECK(reused && reused->get() == legacy->get());
    }
    return deren::vk_test::finish("gaussian_gpu_frame");
}
