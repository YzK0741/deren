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
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string_view>
#include <windows.h>

import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.runtime;
import deren.engine.backend_loader; // load_api_core(): the acquisition lives outside the runtime

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {
    namespace rhi = deren::promise::rhi;

    unsigned renderer_window_count() {
        unsigned count = 0;
        EnumWindows([](HWND window, LPARAM state) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId()) {
                char name[256]{};
                GetClassNameA(window, name, sizeof(name));
                // The Vulkan driver may create its own temporary D3D HWND.
                if (std::string_view{name} == "GLFW30")
                    ++*reinterpret_cast<unsigned*>(state);
            }
            return TRUE;
        },
                    reinterpret_cast<LPARAM>(&count));
        return count;
    }

    struct foreign_image_view final : rhi::image_view {
        foreign_image_view() noexcept
            : rhi::image_view("foreign_image_view") {
        }
        void release() noexcept override {
        }
    };

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
    // ABI34: the native-vocabulary cleanup removed a virtual from vulkan_escape (vtable shift), so the
    // handshake number moved with it.
    CHECK(rhi::abi_version == 40u);

    if (!wants_device(argc, argv)) {
        deren::vk_test::write_line("runtime_dyn: device path skipped (pass --with-device to construct the runtime)");
        return deren::vk_test::finish("test_runtime_dyn");
    }

    // ---- 2/3/4. THE CONSTRUCTION, THE FACE, THE ESCAPE AND THE TEARDOWN, in one scope ----
    //
    // A null native window selects the ordinary-image path, including on native device tests.
    unsigned const windows_before = renderer_window_count();
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
        CHECK(renderer_window_count() == windows_before);
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
        CHECK(dynamic.poll_events() == deren::engine::frame_status::proceed);
        CHECK(!dynamic.enable_debug_gui());

        rhi::api_core& face = dynamic.rhi_face();

        // ABI 39: the main-thread context the GUI plugin casts. On a real backend it is the borrowed, backend-owned
        // handle whose concrete tag names the Vulkan layout; the plugin's own test covers the refusal paths.
        {
            rhi::main_context* const main = dynamic.rhi_face().main_context();
            CHECK(main != nullptr);
            if (main != nullptr) {
                CHECK(main->type == "deren_main_context_vulkan");
                CHECK(dynamic.rhi_face().main_context() == main); // one backend-owned object, answered again
            }
        }
        {
            // ABI 37: the multithreaded recording paths use mergeable segments merged in batches; the
            // retention rule (a recorded primary keeps the merged segment alive until it is released) is
            // measured on the same real device here.
            auto secondary = face.make_mergeable_command_buffer({});
            auto primary = face.make_command_buffer({});
            CHECK(secondary && primary);
            if (secondary && primary) {
                CHECK(secondary->begin_recording({}) == rhi::error::ok);
                CHECK(secondary->end_recording() == rhi::error::ok);
                CHECK(primary->begin_recording({}) == rhi::error::ok);
                rhi::mergeable_command_buffer_ref const batch[] = {secondary};
                CHECK(primary->execute(std::span<rhi::mergeable_command_buffer_ref const>(batch)) == rhi::error::ok);
                CHECK_MSG(secondary->begin_recording({}) == rhi::error::not_ready,
                          "a segment retained by a recorded primary must not be overwritten");
                primary.reset();
                CHECK(secondary->begin_recording({}) == rhi::error::ok);
                CHECK(secondary->end_recording() == rhi::error::ok);
            }
        }
        CHECK(face.type == "deren_api_core_vulkan");
        auto commands = face.make_command_buffer({});
        CHECK(commands != nullptr);
        if (commands) {
            CHECK(commands->type == "deren_command_buffer_vulkan");
            CHECK(commands->begin_recording({}) == rhi::error::ok);
            foreign_image_view foreign_view;
            deren::utility::vessel::array<rhi::color_attachment, 1> attachments{{{.view = &foreign_view}}};
            rhi::rendering_info rendering{};
            rendering.area.width = 4u;
            rendering.area.height = 4u;
            rendering.colors = attachments;
            CHECK_MSG(commands->begin_rendering(rendering) == rhi::error::invalid_argument,
                      "a different image_view implementation must be refused before a Vulkan downcast");
            CHECK(commands->end_recording() == rhi::error::ok);
        }
        // A view and a copied parent reference each retain the original image identity.
        rhi::texture_group_info empty_info{};
        rhi::error group_error = rhi::error::invalid_argument;
        auto empty_group = face.assign_texture_group(empty_info, &group_error);
        CHECK(empty_group && group_error == rhi::error::ok);
        CHECK(empty_group && empty_group->type == "deren_texture_group_vulkan");
        empty_info.textures[15] = nullptr;
        CHECK(face.assign_texture_group(empty_info, &group_error) && group_error == rhi::error::ok);
        empty_info.struct_size = sizeof(empty_info) - 1u;
        CHECK(!face.assign_texture_group(empty_info, &group_error));
        CHECK(group_error == rhi::error::invalid_argument);
        rhi::image_desc parent_desc{};
        parent_desc.extent = {.width = 4u, .height = 4u, .depth = 1u};
        parent_desc.format = rhi::image_format::rgba8_unorm;
        parent_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        rhi::object_manager<rhi::image> original{face.create_image(parent_desc)};
        CHECK(original);
        for (auto shape : {0u, 1u, 2u}) {
            auto invalid_desc = parent_desc;
            if (shape == 0)
                invalid_desc.array_layers = 2;
            if (shape == 1) {
                invalid_desc.array_layers = 6;
                invalid_desc.flags |= rhi::to_bits(rhi::image_flag::cube_compatible);
            }
            if (shape == 2)
                invalid_desc.flags = rhi::to_bits(rhi::image_flag::storage);
            rhi::object_manager<rhi::image> invalid{face.create_image(invalid_desc)};
            CHECK(invalid);
            if (invalid) {
                rhi::texture_group_info info{};
                info.textures[0] = invalid.get();
                CHECK(!face.assign_texture_group(info, &group_error));
                CHECK(group_error == rhi::error::invalid_argument);
            }
        }
        if (original) {
            {
                rhi::texture_group_info info{};
                info.textures[0] = original.get();
                info.textures[15] = original.get();
                auto retained = original->share();
                std::weak_ptr<rhi::image> lifetime = retained;
                auto group = face.assign_texture_group(info, &group_error);
                retained.reset();
                CHECK(group && group_error == rhi::error::ok);
                CHECK(!lifetime.expired());
                group.reset();
                CHECK(lifetime.expired());
            }
            {
                rhi::texture_group_info info{};
                deren::utility::vessel::array<rhi::object_manager<rhi::image>, 16> inputs{};
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    inputs[i] = rhi::object_manager<rhi::image>(face.create_image(parent_desc));
                    CHECK(inputs[i]);
                    info.textures[i] = inputs[i].get();
                }
                deren::utility::vessel::vector<rhi::texture_group_ref> groups;
                for (unsigned i = 0; i < 1024; ++i) {
                    groups.push_back(face.assign_texture_group(info, &group_error));
                    CHECK(groups.back() && group_error == rhi::error::ok);
                }
                CHECK(!face.assign_texture_group(info, &group_error));
                CHECK(group_error == rhi::error::out_of_device_memory);
                CHECK(face.assign_texture_group({}, &group_error) && group_error == rhi::error::ok);
                groups[500].reset();
                auto replacement = face.assign_texture_group(info, &group_error);
                CHECK(replacement && group_error == rhi::error::ok);
            }
            {
                auto other_device = deren::engine::load_api_core(creation);
                CHECK(other_device);
                if (other_device) {
                    rhi::object_manager<rhi::image> foreign{other_device->create_image(parent_desc)};
                    CHECK(foreign);
                    rhi::texture_group_info info{};
                    info.textures[0] = foreign.get();
                    CHECK(!face.assign_texture_group(info, &group_error));
                    CHECK(group_error == rhi::error::invalid_argument);
                }
            }
            {
                rhi::object_manager<rhi::image> released{face.create_image(parent_desc)};
                auto retained = released->share();
                std::weak_ptr<rhi::image> lifetime = retained;
                rhi::texture_group_info info{};
                info.textures[0] = released.get();
                auto group = face.assign_texture_group(info, &group_error);
                retained.reset();
                CHECK(group && group_error == rhi::error::ok);
                released.reset();
                CHECK(!lifetime.expired());
                group.reset();
                CHECK(lifetime.expired());
            }
            CHECK(original->type == "deren_image_vulkan");
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
                CHECK(view->type == "deren_image_view_vulkan");
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
        deren::utility::vessel::array<std::byte, 64> bytes{};
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
        auto const async_present = rhi::extension_kind::async_presentation;
        CHECK_MSG(rhi::has_ability(face.abilities(), async_present) == (creation.native_window != nullptr) &&
                      (face.query_extension(async_present) != nullptr) == (creation.native_window != nullptr),
                  "the Vulkan backend must expose a usable asynchronous presentation capability");
        if (auto* const overlap = rhi::query_extension<rhi::async_presentation>(face); overlap != nullptr) {
            CHECK(overlap->begin().get() == rhi::error::not_ready);
        }
        CHECK(face.frame_swapchain()->format() != rhi::image_format::unknown);
        CHECK(face.present() == rhi::error::unsupported);
        CHECK(face.frame_swapchain()->recreate() == rhi::error::unsupported);
        CHECK(face.frame_swapchain()->extent().width == 640u);
        CHECK(face.frame_swapchain()->extent().height == 480u);
        auto const acquired_frame = face.frame_begin();
        (void)acquired_frame;
        auto* const borrowed_image = face.frame_image();
        CHECK(borrowed_image != nullptr);
        if (borrowed_image != nullptr) {
            rhi::texture_group_info info{};
            info.textures[0] = borrowed_image;
            CHECK(!face.assign_texture_group(info, &group_error));
            CHECK(group_error == rhi::error::invalid_argument);
            CHECK(borrowed_image->type == "deren_frame_image_vulkan");
            CHECK(!borrowed_image->share());
            rhi::object_manager<rhi::image_view> borrowed_view{borrowed_image->make_view({})};
            CHECK(borrowed_view);
            if (borrowed_view) {
                CHECK(!borrowed_view->get_image());
            }
        }
        // More than two submissions reuse both slots and prove no unsignalled WSI semaphore is awaited.
        for (unsigned frame = 0; frame < 6; ++frame) {
            if (frame != 0)
                (void)face.walk_frames()->wait_and_acquire();
            auto* const recording = face.begin_commands();
            CHECK(recording != nullptr);
            if (recording != nullptr) {
                CHECK(recording->begin_recording({}) == rhi::error::ok);
                CHECK(recording->end_recording() == rhi::error::ok);
                CHECK(face.submit(*recording) == rhi::error::ok);
            }
            CHECK(face.present() == rhi::error::unsupported);
            face.walk_frames()->walk_to_next();
        }
        face.wait_idle();
        CHECK(renderer_window_count() == windows_before);
        deren::vk_test::write_line("runtime_dyn: constructed through deren_make_api_core(); presentation format = {}",
                                   static_cast<std::uint32_t>(face.frame_swapchain()->format()));
    }
    CHECK(renderer_window_count() == windows_before);
    deren::vk_test::write_line("runtime_dyn: torn down (the backend's own deleter ran)");

    return deren::vk_test::finish("test_runtime_dyn");
}
