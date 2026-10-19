#include "vk_test.h"

#include <memory>
#include <type_traits>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.resource_cluster;
import deren.engine.runtime;
import deren.engine.scene_tree;
#include "probe_backend.hpp"

namespace rhi = deren::promise::rhi;
using deren::engine::resource_cluster;
int main() {
    static_assert(!std::is_copy_constructible_v<resource_cluster>);
    CHECK(!resource_cluster::create(nullptr, "invalid"));
    auto device = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto other_device = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto cluster = resource_cluster::create(device, "view");
    auto shared = resource_cluster::create(device, "shared");
    auto foreign = resource_cluster::create(other_device, "foreign");
    CHECK(cluster && cluster->empty());
    CHECK(cluster->name() == "view");
    CHECK(cluster->device() == device.get());
    char mutable_name[] = "name";
    auto named = resource_cluster::create(device, mutable_name);
    mutable_name[0] = 'x';
    CHECK(named->name() == "name");
    named.reset();
    CHECK(deren::vk_test::probe_idle_wait_count(*device) == 0);
    auto const images_before = deren::vk_test::probe_live_image_count();
    rhi::buffer_desc buffer_desc{};
    buffer_desc.size = 64;
    auto buffer = cluster->create_buffer(buffer_desc);
    CHECK(buffer.has_value());
    CHECK(cluster->resource_count() == 1);
    if (buffer) {
        CHECK((*buffer)->size() == 64);
        CHECK(shared->share(*buffer) == rhi::error::ok);
        CHECK(shared->share(*buffer) == rhi::error::ok);
        CHECK(shared->resource_count() == 1);
        CHECK(foreign->share(*buffer) == rhi::error::invalid_argument);
        CHECK(foreign->empty());
    }
    CHECK(shared->share(deren::engine::resource_lease<rhi::buffer>{}) == rhi::error::invalid_argument);
    deren::vk_test::probe_resource_failure(*device, 0);
    auto failed = cluster->create_buffer(buffer_desc);
    CHECK(!failed && failed.error() == rhi::error::operation_failed);
    CHECK(cluster->resource_count() == 1);
    rhi::image_desc image_desc{};
    image_desc.extent = {4, 4, 1};
    image_desc.format = rhi::image_format::rgba8_unorm;
    auto image = cluster->create_image(image_desc);
    CHECK(image.has_value());
    CHECK(deren::vk_test::probe_live_image_count() == images_before + 1);
    auto sampler = cluster->create_sampler({});
    CHECK(sampler.has_value());
    if (image) {
        auto view = shared->create_image_view(*image, {});
        CHECK(view.has_value());
        auto const before_bad_view = shared->resource_count();
        rhi::image_view_desc invalid_view{};
        invalid_view.base_layer = 99;
        CHECK(!shared->create_image_view(*image, invalid_view));
        CHECK(shared->resource_count() == before_bad_view);
        CHECK(!foreign->create_image_view(*image, {}));
        rhi::shared_object<rhi::api_core>::weak weak_device = device;
        // Views keep their parent image and core alive independently of its creating cluster.
        auto* borrowed_buffer = buffer ? buffer->get() : nullptr;
        buffer = std::unexpected(rhi::error::not_ready);
        cluster.reset();
        CHECK(borrowed_buffer && borrowed_buffer->size() == 64);
        shared.reset();
        sampler = std::unexpected(rhi::error::not_ready);
        image = std::unexpected(rhi::error::not_ready);
        CHECK(deren::vk_test::probe_idle_wait_count(*device) == 0);
        device.reset();
        CHECK(!weak_device.expired());
        CHECK(deren::vk_test::probe_live_image_count() == images_before + 1);
        view = std::unexpected(rhi::error::not_ready);
        CHECK(weak_device.expired());
        CHECK(deren::vk_test::probe_live_image_count() == images_before);
    }
    {
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{root, options};
        engine.set_scene(scene);
        auto frame_cluster = engine.create_resource_cluster("frame");
        CHECK(frame_cluster && frame_cluster->empty());
        CHECK(engine.retain_resource_cluster_for_frame(frame_cluster) == rhi::error::not_ready);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.retain_resource_cluster_for_frame(foreign) == rhi::error::invalid_argument);
        CHECK(engine.retain_resource_cluster_for_frame(nullptr) == rhi::error::invalid_argument);
        CHECK(engine.retain_resource_cluster_for_frame(frame_cluster) == rhi::error::ok);
        auto const references = frame_cluster.use_count();
        CHECK(engine.retain_resource_cluster_for_frame(frame_cluster) == rhi::error::ok);
        CHECK(frame_cluster.use_count() == references);
        std::weak_ptr<resource_cluster> weak = frame_cluster;
        frame_cluster.reset();
        CHECK(!weak.expired());
        auto finish = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        finish();
        CHECK(!weak.expired());
        CHECK(engine.retain_resource_cluster_for_frame(engine.create_resource_cluster()) == rhi::error::not_ready);
        for (std::uint32_t i = 1; i < rhi::max_frames_in_flight; ++i) {
            CHECK(engine.render_frame() == deren::engine::frame_status::proceed);
            CHECK(!weak.expired());
        }
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(weak.expired());
        finish();
    }
    return deren::vk_test::finish("test_resource_cluster");
}
