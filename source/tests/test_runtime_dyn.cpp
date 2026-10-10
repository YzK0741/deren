// -*- C++ -*-
// ============================================================================
// file: source/tests/test_runtime_dyn.cpp
//
// THE DYNAMIC RUNTIME'S SCAFFOLD TEST (dynamic-backend migration, step 2 / S1).
//
// WHY IT EXISTS AND WHY IT IS NOT IN `VR_TEST_TARGETS`: the second runtime is written partition by
// partition, and every slice of that work has to be able to say "the dynamic side still builds AND still
// constructs" - otherwise the port's code is unverifiable until the day the file list swaps, which is the
// day it is too late. This executable IS that reading. It is deliberately spike-shaped, like
// source/tests/spike_backend_boundary.cpp: it creates a real device, so it cannot belong to the headless ctest set
// (and source/tests/test_docs.cpp compares that list against the workflow, so putting it there would be a lie in
// CI). It is built only in the tree that has the runtime under development (`-DVR_RUNTIME=dynamic`).
//
// WHAT IT MEASURES, ONE GROUP PER FACT:
//   1. THE ABI HANDSHAKE (no device needed): the version this executable compiled from the contract
//      module is the number it PASSES to the entry (abi 18: the version is an argument, and a mismatch
//      is refused by the backend with its own number in the diagnostic) - "both sides compile the
//      contract", the premise the C ABI is built on.
//   2. THE CONSTRUCTION (`--with-device`): `deren::engine::runtime{create_info}` builds the whole device
//      (instance, device, swapchain, descriptor heap) through `deren_make_api_core()`. No `core` is named
//      anywhere on that path, which is the entire reason this runtime exists. SINCE THE SHARED FLIP that
//      call arrives BY NAME out of `deren_vulkan.dll` (the runtime's loader step), so a successful
//      construction here is also the load path's reading.
//   3. THE FACE AND THE ESCAPE (`--with-device`): the contract's `abilities()` answers with the
//      `vulkan_escape` and `device_address` bits, `query_extension<vulkan_escape>()` is non-null, and
//      `native_device()` is a live handle - the two accessors every later partition is written against.
//   4. TEARDOWN (`--with-device`): the `shared_ptr` the entry returned carries the backend's own
//      deleter in its control block, so the destruction runs on the side that allocated the object -
//      there is no deleter symbol to resolve (abi 18).
//
// THE SAFE DEFAULT: without `--with-device` it checks the ABI handshake only (no window, no device), so it
// runs anywhere. Run it with the flag in every slice of the port - that is what it is for.
// ============================================================================
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

import deren.promise.rhi;
import deren.engine.runtime;
import deren.engine.backend_loader; // load_api_core(): the acquisition lives outside the runtime

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {
    namespace rhi = deren::promise::rhi;

    /// Is this run allowed to build a device? The flag is required rather than defaulted because the device
    /// path is slow (a real instance, device, swapchain and heap) and because a machine without a Vulkan
    /// device must still be able to run the handshake half.
    [[nodiscard]] bool wants_device(int const argc, char** const argv) {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--with-device") {
                return true;
            }
        }
        return false;
    }
} // namespace

int main(int const argc, char** const argv) {
    // ---- 1. THE ABI HANDSHAKE: the number this executable compiled is the number it HANDS OVER ----
    // abi 18: the version is the call's first argument (`deren_make_api_core(rhi::abi_version, ...)`,
    // done inside the runtime), so there is no `deren_abi_version()` to ask any more - the handshake
    // below is that the contract's own constant is what every caller passes, checked where it is
    // refused (`check_core_contract` in source/tests/test_dynamic_link.cpp drives the mismatch case).
    // abi 21 is the recording face's owner half (`api_core::make_command_buffer`); this pin moves with the
    // contract's constant, and the two spellings must never drift (see test_dynamic_link.cpp's own note).
    deren::vk_test::write_line("runtime_dyn: this executable compiled abi {} (the entry takes it as its first argument)", rhi::abi_version);
    CHECK(rhi::abi_version == 29u);

    if (!wants_device(argc, argv)) {
        deren::vk_test::write_line("runtime_dyn: device path skipped (pass --with-device to construct the runtime)");
        return deren::vk_test::finish("test_runtime_dyn");
    }

    // ---- 2/3/4. THE CONSTRUCTION, THE FACE, THE ESCAPE AND THE TEARDOWN, in one scope ----
    //
    // `window_visible = false` is a MEASURED requirement of a sessioned run rather than a preference: a
    // VISIBLE window does not come up in this environment at all (source/tests/spike_backend_boundary.cpp's own
    // note records the hang), while the hidden path runs to completion. The pixels, the handles and the
    // lifetime ordering are identical either way.
    {
        rhi::create_info creation{};
        creation.window_width = 640;
        creation.window_height = 480;
        creation.window_visible = false;

        // THE ACQUISITION, IN THE OPEN (batch ⑥): the loader loads deren_vulkan.dll beside this
        // executable, resolves its one export and performs the creation-side handshake - and the runtime
        // is then handed the root together with the same options. A `nullptr` here is a named diagnosis
        // that the loader already wrote; the runtime would refuse it anyway (loudly).
        std::shared_ptr<deren::promise::rhi::api_core> core = deren::engine::load_api_core(creation);
        CHECK(core != nullptr);
        if (core == nullptr) {
            return deren::vk_test::finish("test_runtime_dyn");
        }
        // THE ACQUISITION IS THE APP’S, THE OWNERSHIP IS THE RUNTIME’S: take the identity before the
        // move and check both afterwards - the renderer holds THAT object (not a wrapper, not a copy of its
        // own), so the layering claim of batch ⑥ is observable on a REAL root rather than only argued.
        rhi::api_core* const acquired = core.get();
        deren::engine::runtime dynamic{std::move(core), creation};
        CHECK(core == nullptr);                 // moved into the runtime, not shared by accident
        CHECK(&dynamic.rhi_face() == acquired); // the face IS the object the loader handed over

        CHECK(dynamic.valid());

        rhi::api_core& face = dynamic.rhi_face();
        // A view and a copied parent reference each retain the original image identity.
        rhi::image_desc parent_desc{};
        parent_desc.extent = {.width = 4u, .height = 4u, .depth = 1u};
        parent_desc.format = rhi::image_format::rgba8_unorm;
        parent_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        rhi::object_manager<rhi::image> original{face.create_image(parent_desc)};
        CHECK(original);
        if (original) {
            auto* const identity = original.get();
            auto first_owner = original->share();
            CHECK(first_owner != nullptr);
            std::weak_ptr<rhi::image> first_lifetime = first_owner;
            first_owner.reset();
            CHECK(first_lifetime.expired());
            CHECK(original->extent().width == 4u); // factory reference is independent
            rhi::object_manager<rhi::image_view> view{original->make_view({})};
            CHECK(view);
            if (view) {
                auto parent = view->get_image();
                CHECK_MSG(parent != nullptr, "an owned image view must retain its parent");
                if (parent) {
                    CHECK(parent.get() == identity);
                    auto direct = original->share();
                    CHECK(!parent.owner_before(direct) && !direct.owner_before(parent));
                    std::weak_ptr<rhi::image> lifetime = parent;
                    direct.reset();
                    parent.reset();
                    original.reset();
                    CHECK(!lifetime.expired()); // only the view retains it
                    parent = view->get_image();
                    CHECK(parent.get() == identity);
                    view.reset();
                    CHECK(parent->extent().width == 4u);
                    CHECK(parent->format() == rhi::image_format::rgba8_unorm);
                    rhi::object_manager<rhi::image_view> sibling{parent->make_view({})};
                    CHECK(sibling);
                    parent.reset();
                    CHECK(!lifetime.expired());
                    sibling.reset();
                    CHECK(lifetime.expired());
                }
            }
        }
        // The allocation size may exceed the initialization span only when it is empty.
        // The backing array stays large enough to make the old over-read deterministic and safe here.
        std::array<std::byte, 64> bytes{};
        std::fill(bytes.begin(), bytes.end(), std::byte{0x5A});
        auto const short_input = std::span<std::byte const>(bytes).first(16);
        for (auto const usage : {rhi::buffer_usage::storage_coherent, rhi::buffer_usage::vertex, rhi::buffer_usage::index}) {
            rhi::object_manager<rhi::buffer> invalid{face.create_buffer({.size = bytes.size(), .usage = usage, .initial_bytes = short_input})};
            CHECK_MSG(!invalid, "a nonempty initialization span shorter than the allocation must be refused");
        }
        rhi::object_manager<rhi::buffer> initialized{face.create_buffer({.size = short_input.size(), .usage = rhi::buffer_usage::storage_coherent, .initial_bytes = short_input})};
        CHECK(initialized);
        if (initialized) {
            CHECK(std::equal(short_input.begin(), short_input.end(), initialized->mapped().begin()));
        }
        rhi::object_manager<rhi::buffer> allocation_only{face.create_buffer({.size = bytes.size(), .usage = rhi::buffer_usage::storage_coherent})};
        CHECK(allocation_only);
        std::fflush(stdout); // retain preceding failures if an invalid heap call crashes the old backend

        auto* const heap = rhi::query_extension<rhi::descriptor_heap>(face);
        CHECK(heap != nullptr && heap->ready());
        if (heap != nullptr && heap->ready()) {
            CHECK(heap->bind({}) == rhi::error::invalid_argument);
            CHECK(heap->push_data({.data = short_input.first(4)}) == rhi::error::invalid_argument);
        }
        rhi::ability_bits const abilities = face.abilities();
        CHECK(rhi::has_ability(abilities, rhi::extension_kind::vulkan_escape));
        CHECK(rhi::has_ability(abilities, rhi::extension_kind::device_address));

        CHECK(face.frame_swapchain() != nullptr);
        CHECK(face.frame_swapchain()->format() != rhi::image_format::unknown);
        auto const acquired_frame = face.frame_begin();
        (void)acquired_frame;
        auto* const borrowed_image = face.frame_image();
        CHECK(borrowed_image != nullptr);
        if (borrowed_image != nullptr) {
            CHECK(!borrowed_image->share());
            rhi::object_manager<rhi::image_view> borrowed_view{borrowed_image->make_view({})};
            CHECK(borrowed_view);
            if (borrowed_view) {
                CHECK(!borrowed_view->get_image());
            }
        }
        deren::vk_test::write_line("runtime_dyn: constructed through deren_make_api_core(); presentation format = {}",
                                   static_cast<std::uint32_t>(face.frame_swapchain()->format()));
    }
    deren::vk_test::write_line("runtime_dyn: torn down (the backend's own deleter ran)");

    return deren::vk_test::finish("test_runtime_dyn");
}
