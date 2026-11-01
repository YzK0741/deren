// Headless unit test: deren.engine.filters - the pass-side resource table.
//
// THE CPU HALF ONLY. `user_filter` holds a device root (`rhi::shared_object<api_core>`) and is not
// constructed here; `pass_filter` touches nothing but its own table, which is what makes it identical on
// both sides of the backend boundary. The handles are stored and compared, never dereferenced.
#include "vk_test.h"

import deren.engine.filters;
import deren.promise.rhi; // the handle types resource_handles points at (stored, never dereferenced)

#include <cstdint>

namespace {
    using deren::engine::pass_filter;
    using deren::engine::resource_handles;
    using deren::engine::render_resource::resource_id;
    using deren::promise::rhi::buffer;
    using deren::promise::rhi::image;
    using deren::promise::rhi::image_view;

    // Stand-in storage for the three handle kinds: the filter only stores these addresses, so distinct
    // addresses are enough to prove which entry a lookup answered.
    int stand_in_0 = 0;
    int stand_in_1 = 0;
    int stand_in_2 = 0;

    bool is_null(resource_handles const& handles) {
        return handles.view == nullptr && handles.buffer == nullptr && handles.image == nullptr;
    }

    void test_unregistered_is_null() {
        pass_filter filter;
        CHECK(is_null(filter.resource(resource_id::camera_ubo, 0)));
        CHECK(is_null(filter.resource(resource_id::none, 0)));
        // an unregistered ELEMENT of a registered resource is null too
        filter.register_resource(resource_id::camera_ubo, 0, {.view = nullptr, .buffer = reinterpret_cast<buffer*>(&stand_in_0), .image = nullptr});
        CHECK(is_null(filter.resource(resource_id::camera_ubo, 1)));
    }

    void test_element_distinguishes_per_slot_resources() {
        pass_filter filter;
        auto* const first = reinterpret_cast<buffer*>(&stand_in_0);
        auto* const second = reinterpret_cast<buffer*>(&stand_in_1);
        filter.register_resource(resource_id::camera_ubo, 0, {.view = nullptr, .buffer = first, .image = nullptr});
        filter.register_resource(resource_id::camera_ubo, 1, {.view = nullptr, .buffer = second, .image = nullptr});
        CHECK(filter.resource(resource_id::camera_ubo, 0).buffer == first);
        CHECK(filter.resource(resource_id::camera_ubo, 1).buffer == second);
        CHECK(filter.resource(resource_id::camera_ubo, 2).buffer == nullptr);
        CHECK(filter.resource(resource_id::camera_ubo, 0).image == nullptr);
        CHECK(filter.resource(resource_id::camera_ubo, 1).view == nullptr);
    }

    void test_resource_id_distinguishes_entries() {
        pass_filter filter;
        auto* const as_buffer = reinterpret_cast<buffer*>(&stand_in_0);
        auto* const as_image = reinterpret_cast<image*>(&stand_in_1);
        auto* const as_view = reinterpret_cast<image_view*>(&stand_in_2);
        filter.register_resource(resource_id::material_table, 0, {.view = nullptr, .buffer = as_buffer, .image = nullptr});
        filter.register_resource(resource_id::hdr, 0, {.view = nullptr, .buffer = nullptr, .image = as_image});
        filter.register_resource(resource_id::taa_history, 0, {.view = as_view, .buffer = nullptr, .image = nullptr});
        CHECK(filter.resource(resource_id::material_table, 0).buffer == as_buffer);
        CHECK(filter.resource(resource_id::hdr, 0).image == as_image);
        CHECK(filter.resource(resource_id::taa_history, 0).view == as_view);
        // the same element number under another id is a different entry, not a hit
        CHECK(filter.resource(resource_id::hdr, 0).buffer == nullptr);
        CHECK(filter.resource(resource_id::material_table, 0).image == nullptr);
        CHECK(filter.resource(resource_id::taa_history, 0).buffer == nullptr);
    }

    void test_reregistration_replaces_the_entry() {
        pass_filter filter;
        auto* const first_view = reinterpret_cast<image_view*>(&stand_in_0);
        auto* const second_view = reinterpret_cast<image_view*>(&stand_in_1);
        filter.register_resource(resource_id::hdr, 0, {.view = first_view, .buffer = nullptr, .image = nullptr});
        CHECK(filter.resource(resource_id::hdr, 0).view == first_view);
        // the owner publishes once at create time; a second publication of the same (id, element) is the
        // same question asked again, so the newest answer is the one the pass gets
        filter.register_resource(resource_id::hdr, 0, {.view = second_view, .buffer = nullptr, .image = nullptr});
        CHECK(filter.resource(resource_id::hdr, 0).view == second_view);
        CHECK(filter.resource(resource_id::hdr, 0).image == nullptr);
        CHECK(filter.resource(resource_id::hdr, 0).buffer == nullptr);
    }
} // namespace

int32_t main() {
    test_unregistered_is_null();
    test_element_distinguishes_per_slot_resources();
    test_resource_id_distinguishes_entries();
    test_reregistration_replaces_the_entry();
    return deren::vk_test::finish("test_filters");
}
