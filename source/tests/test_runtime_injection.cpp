// -*- C++ -*-
// ============================================================================
// file: source/tests/test_runtime_injection.cpp
//
// THE LAYERING'S OWN TEST (batch ⑥): the engine builds a renderer from a device root it was HANDED,
// and refuses the two roots it must not accept - with no loader, no DLL and no GPU in the picture.
//
// WHY THE HANDED-ROOT PATH IS THE EVIDENCE THE SPLIT IS REAL: before this batch the runtime loaded the
// backend itself, so "the engine can be given a root" was not expressible - every engine test either
// loaded the real DLL or could not build a runtime at all. Now the constructor takes
// `rhi::shared_object<rhi::api_core>` plus the same `create_info`, and this test injects a PROBE core: the
// first time the engine uses the probe backend for something other than the loader's contract walk.
//
// WHAT A PROBE CAN AND CANNOT PROVE, MEASURED RATHER THAN ASSUMED: the constructor's FIRST act is the
// consumption-side handshake (`runtime_detail::verify_contract_version`, called through `require_core`),
// and that is what a probe root can exercise end to end. Everything AFTER it legitimately needs a real
// device - the construction asks the escape for the presentation format and derives the graphics queue's
// FAMILY by walking the device's families and comparing `vkGetDeviceQueue()` against the escape's own
// queue handle (source/engine/runtime/runtime.cpp) - so a device-less probe cannot carry the constructor to
// completion, and this test does not pretend otherwise. What it does instead is OBSERVE THE LINE BETWEEN
// THE TWO: with a probe whose version AGREES, the constructor gets PAST the handshake and dies later at
// the device facts; with one whose version lies, it dies AT the handshake. The child processes below
// tell those apart by the panic text, so both outcomes are real evidence:
//
//   1. THE AGREEING PATH, IN-PROCESS: `verify_contract_version()` on a probe attesting
//      `rhi::abi_version` returns normally (no panic) - the check the runtime performs, called as the
//      runtime calls it - and the probe's own `api_version()` is the contract's number.
//   2. THE REFUSAL, IN A CHILD PROCESS: a probe reporting `rhi::abi_version + 1` handed to the RUNTIME
//      makes the constructor panic with both numbers. A panic cannot be observed from inside the process
//      it kills, so the case runs in a child of this same executable and the parent reads its status and
//      the panic line it wrote.
//   3. THE EMPTY ROOT, IN A CHILD PROCESS: `runtime{nullptr, options}` panics as well - `vulkan_core` is
//      a reference, so "no root" must not be representable.
//   4. THE PROBE IS USABLE, WHICH #2 AND #3 RELY ON: the same probe with an AGREEING version taken
//      through the runtime's construction dies at the DEVICE-FACT panic, not at the handshake one - it
//      was accepted first. A handshake that refused everything would fail this check.
//
// HEADLESS ON PURPOSE (in VR_TEST_TARGETS): no window, no instance, no device. The REAL acquisition path
// is the scaffold test's (source/tests/test_runtime_dyn.cpp calls deren::engine::load_api_core() against the
// real DLL and asserts the runtime took that very object).
// ============================================================================
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <windows.h> // CreateProcessW: the child-process form of the panic cases (see the banner)

import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.runtime;
import deren.engine.scene_tree;
import deren.engine.pass;
import deren.engine.pass.chain;
import deren.engine.pass.shadow;
import deren.engine.pass.cluster;
import deren.engine.pass.scene_constants;
import deren.engine.pass.scene_deformation;
import deren.engine.pass.scene_tables;
import deren.engine.pass.output_targets;
import deren.engine.pass.taa;
import deren.engine.pass.ray_traced_shadow;
import deren.engine.primitive;
import deren.engine.render_resource;
import deren.engine.render_layout;

// The injection source: the probe backend's test-only factory (see the header's own note).
#include "buffer_creation_contract.hpp"
#include "probe_backend.hpp"
#include "recording_scope_contract.hpp"

namespace {
    namespace rhi = deren::promise::rhi;
    namespace pass = deren::engine::pass;
    struct shared_pipeline_provider : pass::frame_pass {
        rhi::pipeline* shared = nullptr;
        explicit shared_pipeline_provider(rhi::pipeline* pipeline)
            : shared(pipeline) {
        }
        deren::engine::render_resource::pass_io const& io() const noexcept override {
            static deren::engine::render_resource::pass_io const declaration{.name = "shared_provider"};
            return declaration;
        }
        pass::behaviour const& behaviour() const noexcept override {
            static pass::behaviour const value{};
            return value;
        }
        pass::owned_pipeline named_pipeline(std::string_view name) const noexcept override {
            return name == "post_hdr" ? pass::owned_pipeline{.contract = shared} : pass::owned_pipeline{};
        }
        std::string_view feature() const noexcept override {
            return {};
        }
        void create(pass::pass_context const&) override {
        }
        void on_swapchain_recreated(pass::pass_host const&) override {
        }
    };

    namespace managed = deren::engine::render_resource::instances;
    template <typename Chain>
    rhi::error upload_slot(Chain& chain, uint32_t slot, deren::engine::render_resource::resource_id id, std::span<std::byte const> bytes) {
        if constexpr (requires { chain.write_frame_buffer(slot, id, bytes); })
            return chain.write_frame_buffer(slot, id, bytes);
        else
            return rhi::error::unsupported;
    }
    struct resource_provider : shared_pipeline_provider {
        managed::request need;
        managed::resource_snapshot bound;
        explicit resource_provider(managed::request value)
            : shared_pipeline_provider(nullptr)
            , need(value) {
        }
        void describe_resources(managed::resource_context const&, deren::utility::vessel::vector<managed::request>& out) const override {
            out.push_back(need);
        }
        void bind_resources(managed::resource_snapshot const& value) override {
            bound = value;
        }
    };
    /// The child's modes: each constructs a runtime that MUST abort, and the parent checks which.
    constexpr char const* mismatch_mode = "--child-version-mismatch";
    constexpr char const* empty_root_mode = "--child-empty-root";
    constexpr char const* accepted_probe_mode = "--child-accepted-probe";
    constexpr char const* portable_frame_mode = "--child-portable-frame";
    constexpr char const* skin_history_mode = "--child-skin-history";
    constexpr char const* async_present_mode = "--child-async-present";

    /// The contract's concrete-identity mismatch: `rhi::do_concrete` TERMINATES on a name that does not
    /// match the handle's own concrete name, so the diagnostic is read from a child process exactly
    /// like the two contract-version refusals above.
    constexpr char const* do_concrete_panic_mode = "--child-do-concrete-panic";

    [[nodiscard]] bool has_flag(int const argc, char** const argv, char const* const flag) {
        for (int i = 1; i < argc; ++i) {
            if (deren::utility::vessel::string{argv[i]} == flag) {
                return true;
            }
        }
        return false;
    }

    /// The child's outcome: whether it RAN, and what it exited with. The two are different questions.
    ///
    /// WHY THEY ARE SPLIT (a review finding on the batch that added the portable-frame child): "could not
    /// spawn", "did not exit in time" and "the exit code could not be read" are NOT "exited with 0", and a
    /// call site that compares a bare status against 0 reads a spawn failure as SUCCESS - a green check
    /// over a frame that was never run. So the launch/wait/read chain reports `ran`, and every call site
    /// requires it, on the refusal paths and on the accepting one alike.
    struct child_outcome {
        bool ran = false; ///< created, waited for, and its exit code read
        DWORD status = 0; ///< meaningful only when `ran`
    };

    /// Run THIS executable with @p flag and answer whether it ran and what it returned.
    ///
    /// CreateProcessW rather than std::system, deliberately: `system` goes through the command
    /// interpreter, whose exit-code translation would hide what is being checked (a fail-fast abort
    /// reports 0xC0000409 verbatim through the process API; cmd.exe would report its own number).
    [[nodiscard]] child_outcome run_child(char const* const self, char const* const flag) {
        deren::utility::vessel::string const command = "\"" + deren::utility::vessel::string{self} + "\" " + flag;
        deren::utility::vessel::wstring const wide(command.begin(), command.end());

        // Capture third-party/runtime diagnostics; deren's own reports go to files in both builds.
        SECURITY_ATTRIBUTES inherited{};
        inherited.nLength = sizeof(inherited);
        inherited.bInheritHandle = TRUE;
        HANDLE const capture = CreateFileW(L"child_output.txt", GENERIC_WRITE, FILE_SHARE_READ, &inherited,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (capture != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = capture;
            startup.hStdError = capture;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
        PROCESS_INFORMATION child{};
        BOOL const spawned =
            CreateProcessW(nullptr, const_cast<wchar_t*>(wide.c_str()), nullptr, nullptr,
                           capture != INVALID_HANDLE_VALUE ? TRUE : FALSE, 0, nullptr, nullptr, &startup, &child);
        if (capture != INVALID_HANDLE_VALUE) {
            CloseHandle(capture); // the child holds its own reference now
        }
        if (spawned == FALSE) {
            deren::vk_test::write_line("injection: could not spawn the child for {} (error {})", flag, GetLastError());
            return {}; // ran == false: the call site fails, and it fails for the right reason
        }
        child_outcome outcome;
        if (WaitForSingleObject(child.hProcess, 60000) != WAIT_OBJECT_0) {
            deren::vk_test::write_line("injection: the child for {} did not exit within 60 s", flag);
        } else if (GetExitCodeProcess(child.hProcess, &outcome.status) == FALSE) {
            deren::vk_test::write_line("injection: could not read the exit status of the child for {} (error {})", flag,
                                       GetLastError());
        } else {
            outcome.ran = true;
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return outcome;
    }

    /// The whole of a file in this directory, or an empty string when it does not exist.
    [[nodiscard]] deren::utility::vessel::string read_file(wchar_t const* const name) {
        deren::utility::vessel::string text;
        HANDLE const file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return text;
        }
        char buffer[8192];
        DWORD read = 0;
        while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read > 0) {
            text.append(buffer, read);
        }
        CloseHandle(file);
        return text;
    }

    /// Fatal diagnostics are build-independent and written directly to the emergency report.
    [[nodiscard]] deren::utility::vessel::string last_panic() {
        auto text = read_file(L"deren-crash.log");
        deren::utility::vessel::string const marker = "deren panic: ";
        std::size_t const at = text.rfind(marker);
        return at == deren::utility::vessel::string::npos ? deren::utility::vessel::string{} : text.substr(at + marker.size());
    }
    /// THE DESCRIPTOR SPACE'S POSITIVE PATH, on a CPU root (no device). `test_dynamic_link`'s probe
    /// has no image factory, so it can only check the refusals; here a registered group gets a stable
    /// position, a portable write reaches it, and releasing the group makes the old position stale -
    /// the three rules the ability exists for, all observable before any engine material uses them.
    void check_descriptor_space_semantics() {
        rhi::create_info options{};
        rhi::shared_object<rhi::api_core> core = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        CHECK(core != nullptr);
        if (core == nullptr)
            return;
        rhi::image_desc image_desc{};
        image_desc.extent = {.width = 4, .height = 4, .depth = 1};
        image_desc.format = rhi::image_format::rgba8_unorm;
        image_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        rhi::unique_object<rhi::image> image = core->resources->create_image(image_desc);
        CHECK(image);
        if (!image)
            return;
        rhi::texture_group_info info{};
        info.textures[2] = image.get();
        rhi::error assign_error = rhi::error::ok;
        rhi::texture_group_ref group = core->resources->assign_texture_group(info, &assign_error);
        CHECK(group != nullptr);
        CHECK(assign_error == rhi::error::ok);
        auto* const space = rhi::query_extension<rhi::descriptor_space>(*core);
        CHECK(space != nullptr);
        if (group != nullptr && space != nullptr) {
            rhi::descriptor_position const position = space->position(*group);
            CHECK(position.space == 0u);
            CHECK(position.base != 0u);
            CHECK(position.count == rhi::texture_group_capacity);
            CHECK(position.generation != 0u);
            CHECK(space->position(*group).base == position.base);
            rhi::descriptor_write write{};
            write.where = position;
            write.index = 0u;
            write.resource = image.get();
            CHECK(space->write(write) == rhi::error::ok);
            rhi::descriptor_write out_of_range = write;
            out_of_range.index = rhi::texture_group_capacity;
            CHECK(space->write(out_of_range) == rhi::error::invalid_argument);
            rhi::descriptor_write wrong_type = write;
            wrong_type.type = rhi::descriptor_type::storage_image;
            CHECK(space->write(wrong_type) == rhi::error::unsupported);
            rhi::descriptor_position const stale = position;
            group.reset();
            // the position's token is no longer live: the frozen generation is what tells the two apart
            CHECK(space->write(write) == rhi::error::invalid_argument);
            (void)stale;
        }
    }

} // namespace

namespace {
    void check_texture_input_contract(rhi::create_info const& creation) {
        using texture_bytes = deren::utility::vessel::vector<std::uint8_t>;
        auto pixels = std::make_shared<texture_bytes const>(64, std::uint8_t{37});
        struct owned_image {
            std::span<std::uint8_t const> data;
            std::uint32_t width = 4, height = 4, mip_levels = 1;
            bool valid = true;
            std::shared_ptr<texture_bytes const> owner;
        };
        owned_image image{.data = std::span(*pixels), .owner = pixels};
        auto input = deren::engine::texture_input_from(image, rhi::image_format::rgba8_srgb);
        CHECK(input.owner == pixels && input.data.data() == pixels->data());
        auto retained = deren::engine::share_texture_source(input);
        CHECK(retained == pixels);
        input.data = input.data.subspan(4, 8);
        auto subset = deren::engine::share_texture_source(input);
        CHECK(subset->size() == 8 && subset != pixels && subset->front() == 37);
        struct borrowed_image {
            std::span<std::uint8_t const> data;
            std::uint32_t width = 4, height = 4, mip_levels = 1;
            bool valid = true;
        };
        texture_bytes borrowed(64, 23);
        struct weak_image : borrowed_image {
            std::weak_ptr<texture_bytes const> owner;
        };
        weak_image weak_source;
        weak_source.data = std::span(*pixels);
        weak_source.owner = pixels;
        CHECK(!deren::engine::texture_input_from(weak_source, rhi::image_format::rgba8_unorm).owner);
        struct mutable_image : borrowed_image {
            std::shared_ptr<texture_bytes> owner;
        };
        auto mutable_pixels = std::make_shared<texture_bytes>(64, 55);
        mutable_image mutable_source;
        mutable_source.data = std::span(*mutable_pixels);
        mutable_source.owner = mutable_pixels;
        auto mutable_input = deren::engine::texture_input_from(mutable_source, rhi::image_format::rgba8_unorm);
        CHECK(!mutable_input.owner); // pre-existing mutable sources keep snapshot semantics
        auto mutable_snapshot = deren::engine::share_texture_source(mutable_input);
        (*mutable_pixels)[0] = 99;
        CHECK(mutable_snapshot->front() == 55);
        auto copied = deren::engine::share_texture_source(deren::engine::texture_input_from(
            borrowed_image{std::span(borrowed)}, rhi::image_format::rgba8_unorm));
        borrowed[0] = 99;
        CHECK(copied->front() == 23 && copied->data() != borrowed.data());
        std::weak_ptr<texture_bytes const> lifetime = pixels;
        image = {};
        input = {};
        pixels.reset();
        CHECK(!lifetime.expired() && retained->front() == 37);
        retained.reset();
        CHECK(lifetime.expired());
        auto fixture = deren::vk_test::probe_make_core(creation, rhi::abi_version, true);
        CHECK(fixture);
        if (fixture) {
            deren::vk_test::check_buffer_creation_contract(*fixture);
            deren::vk_test::check_recording_scope_contract(*fixture);
        }
    }
} // namespace

int main(int const argc, char** const argv) {
    rhi::create_info const creation{};
    if (argc == 1) {
        check_texture_input_contract(creation);
    }

    if (has_flag(argc, argv, do_concrete_panic_mode)) {
        // THE PANIC BRANCH OF THE CONTRACT'S VALIDATE-AND-CAST PAIR: the name is the backend's, the operation
        // is the contract's, and a mismatch is the caller's invariant being broken - so it terminates with a
        // diagnostic instead of returning a value. The parent below reads that diagnostic.
        struct concrete_name_probe final : rhi::object {
            concrete_name_probe() noexcept
                : rhi::object("deren_contract_do_concrete_probe") {
            }
        };
        concrete_name_probe probe{};
        auto const* const wrong = rhi::do_concrete<concrete_name_probe>(probe, "deren_contract_do_concrete_other");
        (void)wrong;
        return 0; // unreachable while the panic is the specified mismatch answer
    }

    if (has_flag(argc, argv, "--child-post-resources")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        pass::pass_chain application;
        managed::request need;
        need.id = rr::resource_id::taa_history;
        need.life = managed::lifetime::history;
        need.scope = rr::resource_scope::per_swapchain_image;
        need.image.extent = {.width = 4, .height = 4, .depth = 1};
        need.image.format = layout::hdr_format;
        need.image.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_destination);
        auto& provider = application.emplace<resource_provider>(need);
        auto& taa = application.emplace<pass::taa_pass>();
        engine.set_pass_chain(application, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto history = application.resources().snapshot().find(need.id);
        CHECK(history != nullptr);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::taa_history)) == history->objects[0].image.get());
        CHECK(history->description.life == managed::lifetime::history);
        CHECK(!history->description.zero_on_frame);
        CHECK(application.begin_frame(0) == rhi::error::ok);
        CHECK(application.resources().snapshot().find(need.id) == history);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        float observed_history = -1;
        pass::resolved_io io;
        io.list = device->frames->begin_commands();
        CHECK(io.list != nullptr);
        if (io.list == nullptr) {
            return deren::vk_test::finish("post_resources");
        }
        // record_owned normally runs after the host binds its pipeline. Mirror that precondition
        // in this direct pass probe now that application validates deferred consumers.
        deren::vk_test::probe_graphics_enabled(*device, true);
        auto fixture_pipeline = device->resources->create_pipeline({});
        deren::vk_test::probe_graphics_enabled(*device, false);
        CHECK(fixture_pipeline);
        if (fixture_pipeline)
            CHECK(io.list->bind_pipeline(*fixture_pipeline) == rhi::error::ok);
        // Mirror the host's two views of the same frame-owned recording object.
        io.cmd = rhi::shared_object<rhi::command_buffer>::borrow(*io.list);
        io.frame.image_count = rhi::max_swapchain_images;
        io.frame.image_index = 0;
        io.extent = {.width = 4, .height = 4, .depth = 1};
        deren::utility::vessel::array<pass::resolved_binding, 4> own{};
        io.own = own;
        own[0].image_handle = const_cast<rhi::image*>(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::taa_current)));
        own[1].image_handle = history->objects[0].image.get();
        own[2].image_handle = const_cast<rhi::image*>(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::gbuffer_velocity)));
        own[3].image_handle = const_cast<rhi::image*>(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::gbuffer_depth)));
        auto* hdr = const_cast<rhi::image*>(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color)));
        rhi::unique_object<rhi::image_view> hdr_view{hdr->make_view({})};
        deren::utility::vessel::array<pass::resolved_binding, 1> targets{pass::resolved_binding{.image_handle = hdr, .view_handle = hdr_view.get()}};
        io.targets = targets;
        io.push_block.owner = &observed_history;
        io.push_block.push = [](void* owner, rhi::command_buffer&, std::span<std::byte const> bytes, uint32_t) {
            pass::taa_pass::push_constants value{};
            if (bytes.size() < sizeof(value))
                return false;
            std::memcpy(&value, bytes.data(), sizeof(value));
            *static_cast<float*>(owner) = value.history_valid;
            return true;
        };
        taa.record_owned(io);
        CHECK(taa.wrote_history() && observed_history == 0);
        managed::resource_context context{.output_images = rhi::max_swapchain_images, .render_extent = {.width = 4, .height = 4, .depth = 1}};
        CHECK(application.prepare_resources(device, context) == rhi::error::ok);
        CHECK(taa.wrote_history()); // rebinding an unchanged generation must preserve state
        taa.record_owned(io);
        CHECK(taa.wrote_history() && observed_history == 1);
        pass::pass_chain replacement;
        auto& outputs = replacement.emplace<pass::output_targets_pass>();
        CHECK(replacement.prepare_resources(device, context) == rhi::error::ok);
        auto new_history = outputs.resources().find(need.id);
        CHECK(new_history != history);
        taa.bind_resources(outputs.resources());
        CHECK(!taa.wrote_history());
        own[1].image_handle = new_history->objects[0].image.get();
        taa.record_owned(io);
        CHECK(taa.wrote_history() && observed_history == 0);
        taa.bind_resources(outputs.resources());
        taa.record_owned(io);
        CHECK(taa.wrote_history() && observed_history == 1);
        CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        // Reject an incorrectly classified history before acquisition or descriptor changes.
        provider.need.life = managed::lifetime::output_size;
        auto* before = deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::taa_history));
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::taa_history)) == before);
        CHECK(device->frames->begin_commands() == nullptr);
        // Check Bloom rounding, history reuse and growth rollback on an independent chain.
        context.render_extent = {.width = 9, .height = 7, .depth = 1};
        CHECK(replacement.prepare_resources(device, context) == rhi::error::ok);
        auto old = replacement.resources().snapshot();
        for (size_t level = 0; level < 4; ++level) {
            auto value = old.find(pass::output_targets_pass::ids[9 + level]);
            CHECK(value->description.image.extent.width == std::max(1u, 9u >> (level + 1)));
            CHECK(value->description.image.extent.height == std::max(1u, 7u >> (level + 1)));
            CHECK(value->description.life == managed::lifetime::output_size);
        }
        CHECK(replacement.begin_frame(1) == rhi::error::ok);
        CHECK(replacement.prepare_resources(device, context) == rhi::error::ok);
        CHECK(replacement.resources().snapshot().same(old));
        context.render_extent.width = 19;
        deren::vk_test::probe_resource_failure(*device, 3);
        CHECK(replacement.prepare_resources(device, context) == rhi::error::initialization_failed);
        CHECK(replacement.resources().snapshot().same(old));
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(replacement.prepare_resources(device, context) == rhi::error::ok);
        CHECK(replacement.resources().snapshot().find(need.id) != old.find(need.id));
        return deren::vk_test::finish("post_resources");
    }

    if (has_flag(argc, argv, "--child-output-first-failure")) {
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        auto* hdr = deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color));
        deren::vk_test::probe_resource_failure(*device, 0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color)) == hdr);
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        engine.record_main_drawcalls();
        CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        return deren::vk_test::finish("output_first_failure");
    }

    if (has_flag(argc, argv, "--child-output-resources")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::shadow_map)) == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_light)).empty());
        engine.set_scene(scene);
        auto application = std::make_unique<pass::pass_chain>();
        managed::request request;
        request.id = rr::resource_id::hdr;
        request.scope = rr::resource_scope::per_swapchain_image;
        request.life = managed::lifetime::output_size;
        request.image.extent = {.width = 4, .height = 4, .depth = 1};
        request.image.format = layout::hdr_format;
        request.image.flags = rhi::to_bits(rhi::image_flag::color_attachment) | rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_source) |
                              // the G-buffer/HDR family's STORAGE usage joined the contract with visibility-buffer v0.4
                              // (the resolve writes these targets from a compute shader), and an injected provider must
                              // satisfy every flag the declaration requires - the check below is what says so.
                              rhi::to_bits(rhi::image_flag::storage);
        application->emplace<resource_provider>(request);
        engine.set_pass_chain(*application, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto hdr = application->resources().snapshot().find(request.id);
        CHECK(hdr != nullptr);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color)) == hdr->objects[0].image.get());
        auto finish_frame = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(application->resources().snapshot().find(request.id) == hdr);
        finish_frame();
        std::weak_ptr<managed::generation const> prior = hdr;
        auto replacement = std::make_unique<pass::pass_chain>();
        auto& producer = replacement->emplace<pass::output_targets_pass>();
        engine.set_pass_chain(*replacement, {});
        hdr.reset();
        application.reset(); // release the producer snapshot too; submitted slots retain the generation
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto selected = producer.resources();
        // The software depth/id families are optional and remain absent on this OFF frame.
        for (size_t family = 0; family < 15u; ++family) {
            auto value = selected.find(pass::output_targets_pass::ids[family]);
            CHECK(value != nullptr);
            bool const per_slot = pass::output_targets_pass::ids[family] == rr::resource_id::rt_shadow_visibility;
            CHECK(value->description.scope == (per_slot ? rr::resource_scope::per_frame_slot : rr::resource_scope::per_swapchain_image));
            CHECK(value->objects.size() == (per_slot ? rhi::max_frames_in_flight : rhi::max_swapchain_images));
            for (size_t i = 0; i < value->objects.size(); ++i)
                CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(pass::output_targets_pass::heap_slots[family] + i)) == value->objects[i].image.get());
        }
        CHECK(selected.find(rr::resource_id::software_depth) == nullptr);
        CHECK(selected.find(rr::resource_id::software_id) == nullptr);
        CHECK(!prior.expired()); // other submitted slot still owns the former generation
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(prior.expired());
        CHECK(producer.resources().same(selected));
        finish_frame();
        // An incompatible custom provider must not publish any of its images or acquire a frame.
        pass::pass_chain bad_chain;
        auto invalid = selected.find(rr::resource_id::hdr)->description;
        invalid.image.flags = rhi::to_bits(rhi::image_flag::sampled); // lacks attachment and copy-source usage
        bad_chain.emplace<resource_provider>(invalid);
        auto* before = deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color));
        engine.set_pass_chain(bad_chain, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color)) == before);
        pass::pass_chain allocation_failure;
        allocation_failure.emplace<pass::output_targets_pass>();
        engine.set_pass_chain(allocation_failure, {});
        deren::vk_test::probe_resource_failure(*device, 0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(layout::heap_slots::post_color)) == before);
        deren::vk_test::probe_resource_failure(*device, -1);
        engine.set_pass_chain(*replacement, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish_frame();
        // A standalone producer validates extent changes and transactional allocation failure.
        pass::pass_chain dynamic;
        dynamic.emplace<pass::output_targets_pass>();
        managed::resource_context context{.frame_slots = 2, .output_images = 3, .render_extent = {.width = 9, .height = 7, .depth = 1}};
        CHECK(dynamic.prepare_resources(device, context) == rhi::error::ok);
        auto old = dynamic.resources().snapshot();
        auto old_hdr = old.find(rr::resource_id::hdr);
        CHECK(old_hdr->objects.size() == 3 && old_hdr->description.image.extent.width == 9);
        CHECK(dynamic.prepare_resources(device, context) == rhi::error::ok);
        CHECK(dynamic.resources().snapshot().same(old));
        context.render_extent.width = 18;
        deren::vk_test::probe_resource_failure(*device, 3);
        CHECK(dynamic.prepare_resources(device, context) == rhi::error::initialization_failed);
        CHECK(dynamic.resources().snapshot().same(old));
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(dynamic.prepare_resources(device, context) == rhi::error::ok);
        CHECK(dynamic.resources().snapshot().find(rr::resource_id::hdr) != old_hdr);
        CHECK(old_hdr->description.image.extent.width == 9);
        CHECK(dynamic.resources().snapshot().find(rr::resource_id::hdr)->description.image.extent.width == 18);
        context.render_extent.width = 4;
        context.output_images = 2;
        CHECK(dynamic.prepare_resources(device, context) == rhi::error::ok);
        CHECK(dynamic.resources().snapshot().find(rr::resource_id::hdr)->objects.size() == 2);
        CHECK(dynamic.resources().snapshot().find(rr::resource_id::hdr)->description.image.extent.width == 4);
        return deren::vk_test::finish("output_resources");
    }

    // THE VISIBILITY SWITCH AND ITS FRAME FALLBACK (visibility-buffer v0.5): the switch is a request, not a
    // promise - what this probe (a device with no mesh/compute pipelines and no chain) must prove is that asking
    // for the path is SAFE: availability answers false, the request does not panic, and the frames keep recording
    // (through whatever the old path does on this device: no scene, the cleared background). The admitted path's
    // own picture is the capture gate's A/B, which needs the real device.
    if (has_flag(argc, argv, "--child-visibility-fallback")) {
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{root, options};
        engine.set_scene(scene);
        auto application = std::make_unique<pass::pass_chain>();
        // A NON-EMPTY CHAIN FOR THE FRAME'S RESOURCE PREPARATION (the empty chain has nothing to prepare, and the
        // probe's begin_recording path then has no command buffer to open). The output targets are the one
        // provider every chain has, and this test cares about the visibility switch, not about the G-buffer.
        application->emplace<pass::output_targets_pass>();
        engine.set_pass_chain(*application, {});
        auto const finish_frame = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        auto const pace = [&] { CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed); };
        // OFF is the default and must stay a pure no-op: the frames record the historic path.
        CHECK(!engine.subtilis_enabled());
        engine.set_subtilis(true);
        CHECK(engine.subtilis_enabled());
        engine.set_visibility_buffer(false);
        CHECK(!engine.visibility_available()); // no chain built the two passes on this device
        pace();
        finish_frame();
        // ON with no pipelines: a named refusal (the availability answer + the once-per-session warn inside the
        // setter), the frame still records, and the fallback is whole-frame - there is no half-captured frame here.
        engine.set_visibility_buffer(true);
        CHECK(!engine.visibility_available());
        pace();
        finish_frame(); // the first ON frame: the once-per-session fallback line is emitted here
        pace();
        finish_frame(); // ... and the second ON frame records the same old lane, without a second line
        // ... and OFF again is the same no-op, on the next frame already.
        engine.set_visibility_buffer(false);
        pace();
        finish_frame();
        return deren::vk_test::finish("visibility_fallback");
    }

    if (has_flag(argc, argv, "--child-shared-assets")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        deren::utility::vessel::array<uint8_t, 48> cube{};
        deren::utility::vessel::array<uint8_t, 4> lut{};
        deren::engine::ibl_input input{.prefiltered_env = cube, .irradiance = cube, .brdf_lut = lut, .env_size = 1, .env_mip_count = 1, .irr_size = 1, .lut_size = 1};
        auto heap = [&](uint32_t slot) { return deren::vk_test::probe_heap_image(*device, layout::heap_slot_offset(slot)); };
        auto check_publication = [&] {
            CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            auto table = engine.make_resolve_context().resources;
            CHECK(table->find(rr::resource_id::ibl_env, 0, 0).image_handle == heap(layout::heap_slots::env_cube));
            CHECK(table->find(rr::resource_id::ibl_irradiance, 0, 0).image_handle == heap(layout::heap_slots::irradiance_cube));
            CHECK(table->find(rr::resource_id::brdf_lut, 0, 0).image_handle == heap(layout::heap_slots::brdf_lut));
            if (heap(layout::heap_slots::post_lut))
                CHECK(table->find(rr::resource_id::post_lut, 0, 0).image_handle == heap(layout::heap_slots::post_lut));
            if (heap(layout::heap_slots::goo_fgd_lut))
                CHECK(table->find(rr::resource_id::goo_fgd_lut, 0, 0).image_handle == heap(layout::heap_slots::goo_fgd_lut));
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        engine.set_ibl(input);
        auto* first = heap(layout::heap_slots::env_cube);
        CHECK(first);
        check_publication();
        deren::vk_test::probe_resource_failure(*device, 1);
        engine.set_ibl(input);
        CHECK(heap(layout::heap_slots::env_cube) == first);
        check_publication();
        cube.fill(0x20);
        engine.set_ibl(input);
        CHECK(heap(layout::heap_slots::env_cube) != first);
        check_publication();
        engine.set_post_lut(lut, 1, 1);
        auto* old_lut = heap(layout::heap_slots::post_lut);
        CHECK(old_lut);
        deren::vk_test::probe_resource_failure(*device, 0);
        engine.set_post_lut(lut, 1, 1);
        CHECK(heap(layout::heap_slots::post_lut) == old_lut);
        engine.set_goo_fgd_lut(lut, 1, 1);
        CHECK(heap(layout::heap_slots::goo_fgd_lut));
        for (int fail_at = 0; fail_at < 3; ++fail_at) {
            deren::utility::vessel::array<rhi::image const*, 3> old{heap(layout::heap_slots::env_cube), heap(layout::heap_slots::irradiance_cube), heap(layout::heap_slots::brdf_lut)};
            deren::vk_test::probe_heap_image_failure(*device, fail_at);
            engine.set_ibl(input);
            CHECK(heap(layout::heap_slots::env_cube) == old[0] && heap(layout::heap_slots::irradiance_cube) == old[1] && heap(layout::heap_slots::brdf_lut) == old[2]);
            check_publication();
        }
        deren::vk_test::probe_heap_image_failure(*device, 0);
        engine.set_post_lut(lut, 1, 1);
        CHECK(heap(layout::heap_slots::post_lut) == old_lut);
        check_publication();
        auto* old_fgd = heap(layout::heap_slots::goo_fgd_lut);
        deren::vk_test::probe_heap_image_failure(*device, 0);
        engine.set_goo_fgd_lut(lut, 1, 1);
        CHECK(heap(layout::heap_slots::goo_fgd_lut) == old_fgd);
        check_publication();
        deren::vk_test::probe_heap_image_failure(*device, 1, true);
        engine.set_ibl(input);
        auto const refused = engine.pace_and_acquire();
        CHECK(refused == deren::engine::frame_status::acquire_failed);
        if (refused != deren::engine::frame_status::acquire_failed)
            return deren::vk_test::finish("shared_assets");
        CHECK(heap(layout::heap_slots::env_cube)->extent().width == 1); // Partial publication stays owned while rendering is refused.
        deren::vk_test::probe_heap_image_failure(*device, -1);
        engine.set_ibl(input);
        check_publication();
        return deren::vk_test::finish("shared_assets");
    }

    if (has_flag(argc, argv, "--child-scene-tables")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        auto original = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::materials));
        CHECK(!original.empty());
        deren::utility::vessel::vector<std::byte> material_bytes(original.begin(), original.end());
        deren::utility::vessel::array<deren::utility::vessel::vector<std::byte>, pass::scene_tables_pass::resource_count> table_bytes;
        for (size_t i = 0; i < table_bytes.size(); ++i) {
            auto slot = pass::scene_tables_pass::heap_slots[i];
            if (slot == 0)
                continue;
            auto bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(slot));
            CHECK(!bytes.empty());
            if (i != 0)
                bytes.back() = std::byte{53}; // unused tail/counter data must survive handover
            table_bytes[i].assign(bytes.begin(), bytes.end());
        }
        auto application = std::make_unique<pass::pass_chain>();
        managed::request need;
        need.id = rr::resource_id::material_table;
        need.life = managed::lifetime::scene;
        need.scope = rr::resource_scope::device_wide;
        need.type = managed::kind::buffer;
        need.buffer.size = uint64_t(deren::engine::material_capacity) * sizeof(deren::engine::material_record);
        need.buffer.usage = rhi::buffer_usage::storage_coherent;
        need.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
        need.zero_on_create = true;
        auto& tables_provider = application->emplace<pass::scene_tables_pass>();
        auto& provider = application->emplace<resource_provider>(need);
        engine.set_pass_chain(*application, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto generation = provider.bound.find(need.id);
        auto actual = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::materials));
        CHECK(actual.data() == generation->objects[0].buffer->mapped().data());
        CHECK(generation->objects.size() == 1);
        CHECK(std::equal(material_bytes.begin(), material_bytes.end(), generation->objects[0].buffer->mapped().begin()));
        auto required_tables = pass::scene_tables_pass::descriptions(managed::resource_context{.frame_slots = rhi::max_frames_in_flight});
        for (size_t i = 0; i < required_tables.size(); ++i) {
            auto value = tables_provider.resources().find(required_tables[i].id);
            CHECK(value != nullptr && value->objects.size() == 1);
            CHECK(value->description.life == required_tables[i].life);
            CHECK(!value->description.zero_on_frame);
            if (i == 6) {
                CHECK(value->description.buffer.size == uint64_t(rhi::max_frames_in_flight) * 4u * deren::engine::meshlet_capacity * rhi::mesh_task_command_size);
                CHECK((value->description.buffer.flags & rhi::to_bits(rhi::buffer_flag::indirect)) != 0);
                CHECK((value->description.buffer.flags & rhi::to_bits(rhi::buffer_flag::device_address)) != 0);
                auto bytes = value->objects[0].buffer->mapped();
                bytes.back() = std::byte{53};
                table_bytes[i].assign(bytes.begin(), bytes.end());
            } else {
                auto bytes = value->objects[0].buffer->mapped();
                CHECK(std::equal(table_bytes[i].begin(), table_bytes[i].end(), bytes.begin()));
                CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(pass::scene_tables_pass::heap_slots[i])).data() == bytes.data());
            }
        }
        auto finish = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            auto const resources = engine.make_resolve_context().resources;
            for (size_t i = 0; i < table_bytes.size(); ++i) {
                auto binding = resources->find(pass::scene_tables_pass::ids[i], 0, 0);
                CHECK(binding.buffer_handle != nullptr);
                if (binding.buffer_handle != nullptr) {
                    auto bytes = binding.buffer_handle->mapped();
                    CHECK(std::equal(table_bytes[i].begin(), table_bytes[i].end(), bytes.begin()));
                }
            }
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        finish();
        std::weak_ptr<managed::generation const> retired = generation;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(provider.bound.find(need.id) == generation);
        finish();
        pass::pass_chain replacement;
        replacement.emplace<pass::output_targets_pass>();
        engine.set_pass_chain(replacement, {});
        generation.reset();
        application.reset();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(!retired.expired());
        actual = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::materials));
        CHECK(std::equal(material_bytes.begin(), material_bytes.end(), actual.begin()));
        finish();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(retired.expired());
        finish();
        pass::pass_chain invalid;
        need.buffer.size = sizeof(deren::engine::material_record);
        auto& invalid_provider = invalid.emplace<resource_provider>(need);
        engine.set_pass_chain(invalid, {});
        auto* old_binding = actual.data();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::materials)).data() == old_binding);
        invalid_provider.need.buffer.size = required_tables[0].buffer.size;
        invalid_provider.need.scope = rr::resource_scope::per_frame_slot;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        invalid_provider.need.scope = rr::resource_scope::device_wide;
        invalid_provider.need.zero_on_frame = true;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        invalid_provider.need.zero_on_frame = false;
        deren::vk_test::probe_resource_failure(*device, 0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::materials)).data() == old_binding);
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish();
        return deren::vk_test::finish("scene_tables");
    }

    if (has_flag(argc, argv, "--child-deformation-resources")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        deren::utility::vessel::array<glm::mat4, 1> pose{glm::mat4(2.0f)};
        for (uint32_t slot = 0; slot < rhi::max_frames_in_flight; ++slot) {
            engine.set_skin_matrices(pose, slot);
            auto* morph = static_cast<float*>(engine.morph_scratch(slot));
            CHECK(morph != nullptr);
            morph[0] = 10.0f + slot;
            morph[deren::engine::scene_morph_capacity - 1] = 20.0f + slot;
            glm::mat4 const marker(99.0f + slot);
            for (auto base : {layout::heap_slots::previous_transforms, layout::heap_slots::skin_matrices_previous}) {
                auto bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(base + slot));
                CHECK(bytes.size() >= 7 * sizeof(marker));
                if (bytes.size() >= 7 * sizeof(marker))
                    std::memcpy(bytes.data() + 6 * sizeof(marker), &marker, sizeof(marker));
            }
        }
        auto application = std::make_unique<pass::pass_chain>();
        application->emplace<pass::scene_deformation_pass>();
        auto const& ids = pass::scene_deformation_pass::ids;
        auto const& sizes = pass::scene_deformation_pass::sizes;
        auto const& slots = pass::scene_deformation_pass::heap_slots;
        for (size_t i = 0; i < ids.size(); ++i) {
            managed::request need;
            need.id = ids[i];
            need.life = managed::lifetime::frame_slot;
            need.scope = rr::resource_scope::per_frame_slot;
            need.type = managed::kind::buffer;
            need.buffer.size = sizes[i];
            need.buffer.usage = rhi::buffer_usage::storage_coherent;
            need.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
            need.zero_on_create = true;
            application->emplace<resource_provider>(need);
        }
        engine.set_pass_chain(*application, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto snapshot = application->resources().snapshot();
        for (size_t i = 0; i < ids.size(); ++i) {
            auto generation = snapshot.find(ids[i]);
            CHECK(generation != nullptr);
            CHECK(generation->objects.size() == rhi::max_frames_in_flight);
            for (uint32_t slot = 0; slot < rhi::max_frames_in_flight; ++slot)
                CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(slots[i] + slot)).data() == generation->objects[slot].buffer->mapped().data());
        }
        auto check_morph = [&] {
            for (uint32_t slot = 0; slot < rhi::max_frames_in_flight; ++slot) {
                auto* morph = static_cast<float*>(engine.morph_scratch(slot));
                CHECK(morph[0] == 10.0f + slot);
                CHECK(morph[deren::engine::scene_morph_capacity - 1] == 20.0f + slot);
                for (auto base : {layout::heap_slots::previous_transforms, layout::heap_slots::skin_matrices_previous}) {
                    auto bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(base + slot));
                    glm::mat4 marker;
                    std::memcpy(&marker, bytes.data() + 6 * sizeof(marker), sizeof(marker));
                    CHECK(marker == glm::mat4(99.0f + slot));
                }
            }
        };
        check_morph();
        auto finish = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        auto current_skin = snapshot.find(rr::resource_id::skin_matrices);
        CHECK(std::memcmp(current_skin->objects[0].buffer->mapped().data(), pose.data(), sizeof(pose)) == 0);
        uint32_t const first = device->frames->walk_frames()->position();
        finish();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        uint32_t const second = device->frames->walk_frames()->position();
        pose[0] = glm::mat4(3.0f);
        engine.set_skin_matrices(pose);
        glm::mat4 first_pose{};
        std::memcpy(&first_pose, current_skin->objects[first].buffer->mapped().data(), sizeof(first_pose));
        CHECK(first_pose == glm::mat4(2.0f));
        CHECK(std::memcmp(current_skin->objects[second].buffer->mapped().data(), pose.data(), sizeof(pose)) == 0);
        CHECK(application->resources().snapshot().find(rr::resource_id::skin_matrices) == current_skin);
        check_morph();
        finish();
        std::weak_ptr<managed::generation const> retired = current_skin;
        pass::pass_chain fallback;
        fallback.emplace<pass::output_targets_pass>();
        engine.set_pass_chain(fallback, {});
        snapshot = {};
        current_skin.reset();
        application.reset();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(!retired.expired());
        check_morph();
        auto skin_bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::skin_matrices + second));
        CHECK(std::memcmp(skin_bytes.data(), pose.data(), sizeof(pose)) == 0);
        finish();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(retired.expired());
        check_morph();
        finish();
        pass::pass_chain invalid;
        managed::request bad;
        bad.id = rr::resource_id::morph_targets;
        bad.life = managed::lifetime::frame_slot;
        bad.scope = rr::resource_scope::per_frame_slot;
        bad.type = managed::kind::buffer;
        bad.buffer.size = sizeof(float);
        bad.buffer.usage = rhi::buffer_usage::storage_coherent;
        bad.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
        bad.zero_on_create = true;
        auto& invalid_producer = invalid.emplace<resource_provider>(bad);
        engine.set_pass_chain(invalid, {});
        auto* old_morph = engine.morph_scratch(0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(engine.morph_scratch(0) == old_morph);
        invalid_producer.need.buffer.size = sizes.back();
        invalid_producer.need.zero_on_frame = true;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(engine.morph_scratch(0) == old_morph);
        invalid_producer.need.zero_on_frame = false;
        invalid_producer.need.scope = rr::resource_scope::device_wide;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        invalid_producer.need.scope = rr::resource_scope::per_frame_slot;
        deren::vk_test::probe_resource_failure(*device, 0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(engine.morph_scratch(0) == old_morph);
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        check_morph();
        finish();
        return deren::vk_test::finish("deformation_resources");
    }

    if (has_flag(argc, argv, "--child-camera-resources")) {
        namespace rr = deren::engine::render_resource;
        namespace layout = deren::engine::render_layout;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        auto application = std::make_unique<pass::pass_chain>();
        auto& producer = application->emplace<pass::scene_constants_pass>();
        engine.set_pass_chain(*application, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto camera = producer.resources().find(rr::resource_id::camera_ubo);
        CHECK(camera != nullptr);
        if (!camera)
            return deren::vk_test::finish("camera_resources");
        CHECK(camera->description.scope == rr::resource_scope::per_frame_slot);
        CHECK(camera->description.life == managed::lifetime::frame_slot);
        CHECK(!camera->description.zero_on_frame);
        CHECK(camera->objects.size() == rhi::max_frames_in_flight);
        auto bytes = [&](uint32_t slot) {
            auto view = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_camera + slot));
            CHECK(view.size() >= sizeof(deren::engine::camera_ubo));
            CHECK(view.data() == camera->objects[slot].buffer->mapped().data());
            return deren::utility::vessel::vector<std::byte>(view.begin(), view.end());
        };
        auto finish = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        auto first = device->frames->walk_frames()->position();
        auto second = (first + 1u) % rhi::max_frames_in_flight;
        auto first_bytes = bytes(first);
        auto second_bytes = bytes(second);
        CHECK(first_bytes != second_bytes);
        finish();
        engine.camera.yaw += 0.4f;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(producer.resources().find(rr::resource_id::camera_ubo) == camera);
        CHECK(bytes(first) == first_bytes); // preceding submitted slot must not be overwritten
        CHECK(bytes(second) != second_bytes);
        finish();
        std::weak_ptr<managed::generation const> retired = camera;
        pass::pass_chain fallback_only;
        fallback_only.emplace<pass::output_targets_pass>();
        engine.set_pass_chain(fallback_only, {});
        camera.reset();
        application.reset();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(!fallback_only.resources().snapshot().find(rr::resource_id::camera_ubo));
        CHECK(!retired.expired()); // another submitted slot retains the application resource set
        auto fallback_bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_camera + first));
        CHECK(fallback_bytes.size() >= sizeof(deren::engine::camera_ubo));
        CHECK(fallback_bytes.data() != nullptr);
        finish();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(retired.expired());
        finish();
        managed::request bad;
        bad.id = rr::resource_id::camera_ubo;
        bad.life = managed::lifetime::frame_slot;
        bad.scope = rr::resource_scope::per_frame_slot;
        bad.type = managed::kind::buffer;
        bad.buffer.size = sizeof(deren::engine::camera_ubo) - 1;
        bad.buffer.usage = rhi::buffer_usage::uniform_coherent;
        bad.buffer.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
        bad.zero_on_create = true;
        pass::pass_chain invalid;
        auto& invalid_producer = invalid.emplace<resource_provider>(bad);
        engine.set_pass_chain(invalid, {});
        auto const old_binding = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_camera)).data();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_camera)).data() == old_binding);
        invalid_producer.need.buffer.size = sizeof(deren::engine::camera_ubo);
        invalid_producer.need.zero_on_frame = true;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        invalid_producer.need.zero_on_frame = false;
        invalid_producer.need.scope = rr::resource_scope::device_wide;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        invalid_producer.need.scope = rr::resource_scope::per_frame_slot;
        deren::vk_test::probe_resource_failure(*device, 0);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_camera)).data() == old_binding);
        deren::vk_test::probe_resource_failure(*device, -1);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish();
        pass::pass_chain camera_only;
        camera_only.emplace<pass::scene_constants_pass>().select_resources(false, false, true);
        CHECK(camera_only.prepare_resources(device, managed::resource_context{.frame_slots = 2, .output_images = 3}) == rhi::error::ok);
        CHECK(camera_only.resources().snapshot().size() == 1);
        CHECK(camera_only.resources().snapshot().find(rr::resource_id::camera_ubo)->objects.size() == 2);
        return deren::vk_test::finish("camera_resources");
    }

    if (has_flag(argc, argv, "--child-scene-constants")) {
        namespace rr = deren::engine::render_resource;
        auto root = deren::vk_test::probe_make_core(creation, rhi::abi_version, true);
        managed::request request;
        request.id = rr::resource_id::light_ubo;
        request.life = managed::lifetime::frame_slot;
        request.scope = rr::resource_scope::per_frame_slot;
        request.type = managed::kind::buffer;
        request.buffer.size = sizeof(deren::engine::light_ubo);
        request.buffer.usage = rhi::buffer_usage::uniform_coherent;
        request.buffer.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
        request.zero_on_create = true;
        pass::pass_chain chain;
        chain.emplace<resource_provider>(request);
        managed::resource_context const context{.frame_slots = 2, .output_images = 3};
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto generation = chain.resources().snapshot().find(request.id);
        CHECK(generation != nullptr);
        if (!generation)
            return deren::vk_test::finish("scene_constants");
        deren::engine::light_ubo value{};
        value.light_count = glm::vec4(7, 1.25f, 0, 0);
        auto const result = upload_slot(chain, 0, request.id, std::as_bytes(std::span(&value, 1)));
        CHECK(result == rhi::error::ok);
        if (result != rhi::error::ok)
            return deren::vk_test::finish("scene_constants");
        deren::engine::light_ubo a{}, b{};
        std::memcpy(&a, generation->objects[0].buffer->mapped().data(), sizeof(a));
        std::memcpy(&b, generation->objects[1].buffer->mapped().data(), sizeof(b));
        CHECK(a.light_count == value.light_count);
        CHECK(b.light_count == glm::vec4{});
        CHECK(upload_slot(chain, 2, request.id, std::as_bytes(std::span(&value, 1))) == rhi::error::invalid_argument);
        CHECK(upload_slot(chain, 0, rr::resource_id::none, {}) == rhi::error::invalid_argument);
        CHECK(chain.begin_frame(0) == rhi::error::ok);
        std::memcpy(&a, generation->objects[0].buffer->mapped().data(), sizeof(a));
        CHECK(a.light_count == value.light_count); // constants are never reset as scratch
        deren::utility::vessel::vector<std::byte> oversized(sizeof(value) + 1, std::byte{51});
        CHECK(upload_slot(chain, 0, request.id, oversized) == rhi::error::invalid_argument);
        std::memcpy(&a, generation->objects[0].buffer->mapped().data(), sizeof(a));
        CHECK(a.light_count == value.light_count);
        pass::pass_chain cached;
        request.buffer.usage = rhi::buffer_usage::uniform_cached;
        request.zero_on_create = false;
        cached.emplace<resource_provider>(request);
        CHECK(cached.prepare_resources(root, context) == rhi::error::ok);
        CHECK(upload_slot(cached, 0, request.id, std::as_bytes(std::span(&value, 1))) == rhi::error::unsupported);

        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        auto application = std::make_unique<pass::pass_chain>();
        auto& producer = application->emplace<pass::scene_constants_pass>();
        engine.set_pass_chain(*application, {});
        namespace layout = deren::engine::render_layout;
        auto read_light = [&](uint32_t slot) {
            deren::engine::light_ubo result{};
            auto bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_light + slot));
            CHECK(bytes.size() >= sizeof(result));
            if (bytes.size() >= sizeof(result))
                std::memcpy(&result, bytes.data(), sizeof(result));
            return result;
        };
        auto read_head = [&](uint32_t slot) {
            deren::engine::head_ubo result{};
            auto bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_head + slot));
            CHECK(bytes.size() >= sizeof(result));
            if (bytes.size() >= sizeof(result))
                std::memcpy(&result, bytes.data(), sizeof(result));
            return result;
        };
        auto finish_frame = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        deren::engine::head_ubo head{};
        head.center = glm::vec4(1, 2, 3, 1);
        engine.set_head_basis(head);
        engine.set_exposure(2.5f);
        deren::utility::vessel::array<deren::engine::punctual_light, 1> lights{};
        engine.set_point_lights(lights);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        uint32_t const first = device->frames->walk_frames()->position();
        uint32_t const second = (first + 1u) % 2u;
        CHECK(read_light(first).light_count.x == 1 && read_light(first).light_count.y == 2.5f);
        CHECK(read_head(first).center == head.center);
        CHECK(read_head(second).front == deren::engine::head_ubo{}.front); // preserve initialized fallback axes
        auto owned_light = producer.resources().find(rr::resource_id::light_ubo);
        auto owned_head = producer.resources().find(rr::resource_id::head_ubo);
        CHECK(owned_light != nullptr && owned_head != nullptr);
        CHECK(!owned_light->description.zero_on_frame && !owned_head->description.zero_on_frame);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_light + first)).data() == owned_light->objects[first].buffer->mapped().data());
        finish_frame();
        engine.set_exposure(0.5f);
        head.center = glm::vec4(4, 5, 6, 1);
        engine.set_head_basis(head);
        engine.set_point_lights({});
        CHECK(read_light(first).light_count.y == 2.5f && read_head(first).center == glm::vec4(1, 2, 3, 1));
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(read_light(second).light_count.x == 0 && read_light(second).light_count.y == 0.5f);
        CHECK(read_head(second).center == head.center);
        CHECK(read_light(first).light_count.y == 2.5f && read_head(first).center == glm::vec4(1, 2, 3, 1));
        finish_frame();
        std::weak_ptr<managed::generation const> prior_light = owned_light;
        pass::pass_chain replacement;
        auto& partial = replacement.emplace<pass::scene_constants_pass>();
        partial.select_resources(false, true); // head in application; light supplied by compatibility chain
        engine.set_pass_chain(replacement, {});
        owned_light.reset();
        owned_head.reset();
        application.reset(); // no remembered owner pointer may survive the handover
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(!replacement.resources().snapshot().find(rr::resource_id::light_ubo));
        CHECK(read_light(first).light_count.y == 0.5f && read_head(first).center == head.center);
        auto replacement_head = partial.resources().find(rr::resource_id::head_ubo);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_head + first)).data() == replacement_head->objects[first].buffer->mapped().data());
        CHECK(!prior_light.expired()); // another submitted slot still retains the former resource set
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(prior_light.expired());
        finish_frame();
        pass::pass_chain invalid_provider;
        auto bad = replacement_head->description;
        bad.zero_on_frame = true; // would wipe a head upload in begin_frame
        invalid_provider.emplace<resource_provider>(bad);
        engine.set_pass_chain(invalid_provider, {});
        auto head_before = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_head)).data();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::scene_head)).data() == head_before);
        return deren::vk_test::finish("scene_constants");
    }

    if (has_flag(argc, argv, "--child-cluster-resources")) {
        namespace rr = deren::engine::render_resource;
        pass::pass_chain chain;
        chain.emplace<pass::cluster_pass>();
        auto root = deren::vk_test::probe_make_core(creation, rhi::abi_version, true);
        deren::vk_test::probe_buffer_allocation_fill(*root, 97);
        managed::resource_context context{.frame_slots = 2, .output_images = 3};
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto snapshot = chain.resources().snapshot();
        auto counts = snapshot.find(rr::resource_id::cluster_counts);
        auto indices = snapshot.find(rr::resource_id::cluster_indices);
        CHECK(counts != nullptr);
        CHECK(indices != nullptr);
        if (!counts || !indices)
            return deren::vk_test::finish("cluster_resources");
        uint64_t const count_bytes = uint64_t(deren::engine::max_cluster_count) * sizeof(uint32_t);
        CHECK(counts->description.buffer.size == count_bytes);
        CHECK(indices->description.buffer.size == count_bytes * deren::engine::cluster_light_capacity);
        CHECK(counts->objects.size() == 2 && indices->objects.size() == 2);
        CHECK(counts->description.zero_on_frame && !indices->description.zero_on_frame);
        auto a = counts->objects[0].buffer->mapped();
        auto b = counts->objects[1].buffer->mapped();
        auto row = indices->objects[0].buffer->mapped();
        CHECK(std::all_of(a.begin(), a.end(), [](auto byte) { return byte == std::byte{}; }));
        CHECK(std::all_of(row.begin(), row.end(), [](auto byte) { return byte == std::byte{}; }));
        std::fill(a.begin(), a.end(), std::byte{42});
        std::fill(b.begin(), b.end(), std::byte{43});
        row[0] = std::byte{51};
        CHECK(chain.begin_frame(0) == rhi::error::ok);
        CHECK(a[0] == std::byte{} && b[0] == std::byte{43});
        CHECK(row[0] == std::byte{51});
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(chain.resources().snapshot().same(snapshot));
        CHECK(row[0] == std::byte{51});
        a[0] = std::byte{52};
        deren::utility::vessel::array<rr::resource_id, 1> const preserved = {rr::resource_id::cluster_counts};
        CHECK(chain.begin_frame(0, preserved) == rhi::error::ok);
        CHECK(a[0] == std::byte{52});
        CHECK(row[0] == std::byte{51});
        pass::pass_chain unmapped;
        auto invalid = indices->description;
        invalid.buffer.usage = rhi::buffer_usage::storage_gpu_only;
        unmapped.emplace<resource_provider>(invalid);
        CHECK(unmapped.prepare_resources(root, context) == rhi::error::unsupported);
        CHECK(unmapped.resources().snapshot().size() == 0);

        // Runtime integration: the installed cluster producer and fallback shadow producer coexist.
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto device = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{device, options};
        engine.set_scene(scene);
        pass::pass_chain installed;
        installed.emplace<pass::cluster_pass>();
        engine.set_pass_chain(installed, {});
        namespace layout = deren::engine::render_layout;
        auto finish_frame = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        auto check_heap = [&](managed::resource_snapshot const& view) {
            auto count_generation = view.find(rr::resource_id::cluster_counts);
            auto index_generation = view.find(rr::resource_id::cluster_indices);
            CHECK(count_generation != nullptr && index_generation != nullptr);
            if (!count_generation || !index_generation)
                return;
            for (uint32_t slot = 0; slot < 2; ++slot) {
                CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::cluster_counts + slot)).data() == count_generation->objects[slot].buffer->mapped().data());
                CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::cluster_indices + slot)).data() == index_generation->objects[slot].buffer->mapped().data());
            }
        };
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        check_heap(installed.resources().snapshot());
        auto original_counts = installed.resources().snapshot().find(rr::resource_id::cluster_counts);
        uint32_t const original_slot = device->frames->walk_frames()->position();
        original_counts->objects[original_slot].buffer->mapped()[0] = std::byte{89};
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(original_counts->objects[original_slot].buffer->mapped()[0] == std::byte{89}); // no active clustered dispatch
        finish_frame();
        pass::pass_chain shadow_only;
        shadow_only.emplace<pass::shadow_pass>();
        engine.set_pass_chain(shadow_only, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto fallback_count_bytes = deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::cluster_counts));
        CHECK(fallback_count_bytes.data() != original_counts->objects[0].buffer->mapped().data());
        finish_frame();
        pass::pass_chain incomplete;
        incomplete.emplace<resource_provider>(original_counts->description);
        engine.set_pass_chain(incomplete, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::cluster_counts)).data() == fallback_count_bytes.data());
        pass::pass_chain wrong_usage;
        auto wrong_counts = original_counts->description;
        wrong_counts.buffer.usage = rhi::buffer_usage::uniform_coherent;
        auto wrong_indices = wrong_counts;
        wrong_indices.id = rr::resource_id::cluster_indices;
        wrong_indices.buffer.size *= deren::engine::cluster_light_capacity;
        wrong_indices.zero_on_frame = false;
        wrong_usage.emplace<resource_provider>(wrong_counts);
        wrong_usage.emplace<resource_provider>(wrong_indices);
        engine.set_pass_chain(wrong_usage, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(device->frames->begin_commands() == nullptr);
        CHECK(deren::vk_test::probe_heap_buffer(*device, layout::heap_slot_offset(layout::heap_slots::cluster_counts)).data() == fallback_count_bytes.data());
        engine.set_pass_chain(installed, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        check_heap(installed.resources().snapshot());
        finish_frame();
        return deren::vk_test::finish("cluster_resources");
    }

    if (has_flag(argc, argv, "--child-managed-resources")) {
        namespace rr = deren::engine::render_resource;
        auto root = deren::vk_test::probe_make_core(creation, rhi::abi_version, true);
        managed::resource_context context{.frame_slots = 2, .output_images = 3};
        managed::request request{};
        request.id = rr::resource_id::shadow_map;
        request.life = managed::lifetime::frame_slot;
        request.scope = rr::resource_scope::per_frame_slot;
        request.image.extent = {4, 4, 1};
        request.image.format = rhi::image_format::depth;
        request.image.array_layers = 2;
        request.image.flags = rhi::to_bits(rhi::image_flag::depth_attachment) | rhi::to_bits(rhi::image_flag::sampled);
        pass::pass_chain chain;
        auto& producer = chain.emplace<resource_provider>(request);
        request.create = false;
        auto& consumer = chain.emplace<resource_provider>(request);
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto first = producer.bound;
        auto old = first.find(rr::resource_id::shadow_map);
        CHECK(old != nullptr);
        if (!old)
            return deren::vk_test::finish("managed_resources");
        CHECK(consumer.bound.find(rr::resource_id::shadow_map) == old);
        CHECK(old->objects.size() == 2);
        CHECK(old->objects[0].layers.size() == 2);
        CHECK(first.group(managed::lifetime::frame_slot).size() == 1);
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(producer.bound.same(first));
        producer.need.image.array_layers = 4;
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto grown = producer.bound.find(rr::resource_id::shadow_map);
        CHECK(grown != old && grown->objects[1].layers.size() == 4);
        producer.need.image.array_layers = 1;
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(producer.bound.find(rr::resource_id::shadow_map) == grown);
        auto before = chain.resources().snapshot();
        consumer.need.create = true;
        consumer.need.image.extent.width = 9;
        CHECK(chain.prepare_resources(root, context) == rhi::error::invalid_argument);
        CHECK(chain.resources().snapshot().same(before));
        consumer.need.create = false;
        std::weak_ptr<managed::generation const> retained = old;
        old.reset();
        CHECK(!retained.expired());
        first = {};
        CHECK(retained.expired());
        managed::request buffer{};
        buffer.id = rr::resource_id::cluster_counts;
        buffer.life = managed::lifetime::frame_scratch;
        buffer.scope = rr::resource_scope::per_frame_slot;
        buffer.type = managed::kind::buffer;
        buffer.buffer.size = 16;
        buffer.buffer.usage = rhi::buffer_usage::storage_coherent;
        buffer.zero_on_frame = true;
        auto& scratch = chain.emplace<resource_provider>(buffer);
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto allocated = scratch.bound.find(buffer.id);
        CHECK(allocated != nullptr);
        if (!allocated)
            return deren::vk_test::finish("managed_resources");
        auto a = allocated->objects[0].buffer->mapped(), b = allocated->objects[1].buffer->mapped();
        std::fill(a.begin(), a.end(), std::byte{42});
        std::fill(b.begin(), b.end(), std::byte{43});
        CHECK(chain.begin_frame(0) == rhi::error::ok);
        CHECK(a[0] == std::byte{0});
        CHECK(b[0] == std::byte{43});
        CHECK(chain.begin_frame(2) == rhi::error::invalid_argument);
        CHECK(chain.resources().snapshot().group(managed::lifetime::frame_scratch).size() == 1);
        auto stable = chain.resources().snapshot();
        producer.need.image.array_layers = 5;
        deren::vk_test::probe_resource_failure(*root, 1); // second slot fails after a partially created generation
        CHECK(chain.prepare_resources(root, context) == rhi::error::initialization_failed);
        CHECK(chain.resources().snapshot().same(stable));
        CHECK(producer.bound.same(stable));
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(producer.bound.find(producer.need.id)->description.image.array_layers == 5);

        buffer.id = rr::resource_id::taa_history;
        buffer.life = managed::lifetime::history;
        buffer.zero_on_frame = false;
        auto& history = chain.emplace<resource_provider>(buffer);
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        auto history_bytes = history.bound.find(buffer.id)->objects[0].buffer->mapped();
        std::fill(history_bytes.begin(), history_bytes.end(), std::byte{91});
        CHECK(chain.begin_frame(0) == rhi::error::ok);
        CHECK(history_bytes[0] == std::byte{91});
        history.need.zero_on_frame = true;
        CHECK(chain.prepare_resources(root, context) == rhi::error::invalid_argument);
        history.need.zero_on_frame = false;
        history.need.scope = rr::resource_scope::per_swapchain_image;
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(history.bound.find(buffer.id)->objects.size() == 3);
        context.output_images = 2;
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(history.bound.find(buffer.id)->objects.size() == 2);
        context.frame_slots = 1;
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(chain.begin_frame(1) == rhi::error::invalid_argument);
        chain.clear();
        CHECK(chain.prepare_resources(root, context) == rhi::error::ok);
        CHECK(chain.resources().snapshot().size() == 0);
        CHECK(chain.begin_frame(0) == rhi::error::ok);
        // Outstanding views keep both their generation and the device alive after callers drop the root.
        rhi::shared_object<rhi::api_core>::weak device_weak = root;
        root.reset();
        CHECK(!device_weak.expired());
        managed::resource_snapshot survivor;
        rhi::shared_object<rhi::api_core>::weak orphaned_device;
        {
            auto isolated_root = deren::vk_test::probe_make_core(creation, rhi::abi_version, true);
            orphaned_device = isolated_root;
            pass::pass_chain isolated;
            buffer.zero_on_frame = false;
            isolated.emplace<resource_provider>(buffer);
            CHECK(isolated.prepare_resources(isolated_root, context) == rhi::error::ok);
            survivor = isolated.resources().snapshot();
            isolated_root.reset();
        }
        CHECK(!orphaned_device.expired());
        CHECK(survivor.find(buffer.id)->objects[0].buffer->mapped().size() == 16);
        survivor = {};
        CHECK(orphaned_device.expired());
        return deren::vk_test::finish("managed_resources");
    }

    if (has_flag(argc, argv, "--child-managed-frames")) {
        namespace rr = deren::engine::render_resource;
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime engine{root, options};
        engine.set_scene(scene);
        managed::request request{};
        request.id = rr::resource_id::motion_vectors;
        request.type = managed::kind::buffer;
        request.life = managed::lifetime::frame_slot;
        request.scope = rr::resource_scope::per_frame_slot;
        request.buffer.size = pass::scene_deformation_pass::sizes[0];
        request.buffer.usage = rhi::buffer_usage::storage_coherent;
        request.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
        request.zero_on_create = true;
        auto old_chain = std::make_unique<pass::pass_chain>();
        auto& provider = old_chain->emplace<resource_provider>(request);
        engine.set_pass_chain(*old_chain, {});
        auto finish_frame = [&] {
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto allocated = provider.bound.find(request.id);
        CHECK(allocated != nullptr);
        if (!allocated)
            return deren::vk_test::finish("managed_frames");
        uint32_t const first = root->frames->walk_frames()->position();
        std::fill(allocated->objects[first].buffer->mapped().begin(), allocated->objects[first].buffer->mapped().end(), std::byte{9});
        finish_frame();
        std::weak_ptr<managed::generation const> submitted = allocated;
        allocated.reset();
        provider.need.buffer.size = request.buffer.size + 16;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(provider.bound.find(request.id)->description.buffer.size == request.buffer.size + 16);
        CHECK(!submitted.expired()); // prior slot is still retained while the next slot records
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(submitted.expired());
        auto slot = root->frames->walk_frames()->position();
        CHECK(provider.bound.find(request.id)->objects[slot].buffer->mapped()[0] == std::byte{9}); // persistent motion survives reuse/growth
        finish_frame();
        provider.need.buffer.size = request.buffer.size + 32;
        deren::vk_test::probe_resource_failure(*root, 1);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::acquire_failed);
        CHECK(root->frames->begin_commands() == nullptr); // failure did not open/acquire a frame
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish_frame();
        pass::pass_chain replacement;
        auto& replacement_provider = replacement.emplace<resource_provider>(request);
        engine.set_pass_chain(replacement, {});
        old_chain.reset(); // no resource-owner pointer may remain in runtime
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(replacement_provider.bound.find(request.id) != nullptr);
        finish_frame();
        // A real shadow pass describes the images; growing cascades must update the sampled bridge.
        pass::pass_chain shadow_chain;
        shadow_chain.emplace<pass::shadow_pass>();
        engine.set_pass_chain(shadow_chain, {});
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        auto original_shadow = shadow_chain.resources().snapshot().find(rr::resource_id::shadow_map);
        CHECK(original_shadow != nullptr);
        finish_frame();
        engine.set_shadow_cascades(4);
        auto expanded = shadow_chain.resources().snapshot().find(rr::resource_id::shadow_map);
        CHECK(expanded != original_shadow);
        CHECK(expanded->description.image.array_layers == 4);
        namespace layout = deren::engine::render_layout;
        for (uint32_t index = 0; index < 2; ++index)
            CHECK(deren::vk_test::probe_heap_image(*root, layout::heap_slot_offset(layout::heap_slots::shadow_map + index)) == expanded->objects[index].image.get());
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish_frame();
        engine.set_shadow_cascades(1);
        CHECK(shadow_chain.resources().snapshot().find(rr::resource_id::shadow_map) == expanded);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        finish_frame();
        return deren::vk_test::finish("managed_frames");
    }

    if (has_flag(argc, argv, async_present_mode)) {
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        options.window_visible = false;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        std::promise<rhi::error> completion;
        auto unfinished = completion.get_future().share();
        deren::vk_test::probe_present_completion(*root, unfinished);
        deren::engine::scene_tree::scene scene{};
        deren::engine::runtime engine{root, options};
        engine.set_async_present(!has_flag(argc, argv, "--force-sync-present"));
        engine.set_scene(scene);
        auto prepare = [&] {
            CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
            CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
        };
        prepare();
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        CHECK(unfinished.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        prepare(); // must proceed even though the preceding present is still pending
        CHECK(unfinished.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        completion.set_value(rhi::error::ok);
        std::promise<rhi::error> outdated;
        outdated.set_value(rhi::error::out_of_date);
        deren::vk_test::probe_present_completion(*root, outdated.get_future().share());
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        prepare();
        CHECK_MSG(engine.submit_and_present() == deren::engine::frame_status::proceed,
                  "a deferred out-of-date result must rebuild safely after consuming the current acquire");
        std::promise<rhi::error> lost;
        lost.set_value(rhi::error::device_lost);
        deren::vk_test::probe_present_completion(*root, lost.get_future().share());
        prepare();
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        prepare();
        CHECK_MSG(engine.submit_and_present() == deren::engine::frame_status::present_failed,
                  "a deferred fatal presentation result must be reported before another submission");
        return deren::vk_test::finish("async_present");
    }

    if (has_flag(argc, argv, skin_history_mode)) {
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        options.window_visible = false;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::engine::scene_tree::scene scene{};
        deren::engine::runtime engine{root, options};
        engine.set_scene(scene);
        namespace layout = deren::engine::render_layout;
        auto buffer = [&](uint32_t base, uint32_t slot) {
            return deren::vk_test::probe_heap_buffer(*root, layout::heap_slot_offset(base + slot));
        };
        auto matrix = [](std::span<std::byte> bytes, std::size_t index) {
            glm::mat4 value;
            std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
            return value;
        };
        auto finish_frame = [&] {
            engine.record_main_drawcalls();
            CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
            CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        };
        CHECK(root->frames->walk_frames()->slot_count() == 2);
        uint32_t const first = root->frames->walk_frames()->position();
        uint32_t const second = (first + 1u) % 2u;
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        auto previous = buffer(layout::heap_slots::skin_matrices_previous, first);
        CHECK(!previous.empty());
        if (previous.empty())
            return deren::vk_test::finish("skin_history");
        CHECK_MSG(matrix(previous, 0) == glm::mat4(0.0f), "a frame with no skin uploads must leave the previous buffer untouched");
        finish_frame();

        glm::mat4 const untouched(99);
        for (uint32_t slot : {first, second}) {
            auto bytes = buffer(layout::heap_slots::skin_matrices_previous, slot);
            std::memcpy(bytes.data() + 6 * sizeof(untouched), &untouched, sizeof(untouched));
        }

        deren::utility::vessel::array<glm::mat4, 5> const a = {glm::mat4(2), glm::mat4(3), glm::mat4(4), glm::mat4(5), glm::mat4(6)};
        deren::utility::vessel::array<glm::mat4, 6> const b = {glm::mat4(12), glm::mat4(13), glm::mat4(14), glm::mat4(15), glm::mat4(16), glm::mat4(17)};
        engine.set_skin_matrices(a, second);
        engine.set_skin_matrices(b, first);
        // The upload allocation is write-oriented: history must come from the CPU upload source,
        // not a read of this allocation. Corrupt its contents after the upload to detect that read.
        auto uploaded = buffer(layout::heap_slots::skin_matrices, second);
        std::memset(uploaded.data(), 0, uploaded.size());
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        CHECK(matrix(buffer(layout::heap_slots::skin_matrices_previous, second), 0) == glm::mat4(0));
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        previous = buffer(layout::heap_slots::skin_matrices_previous, first);
        CHECK_MSG(matrix(previous, 0) == glm::mat4(2), "history must use the upload source of the preceding rendered slot");
        CHECK(matrix(previous, 4) == glm::mat4(6));
        CHECK(matrix(previous, 5) == glm::mat4(0));
        CHECK_MSG(matrix(previous, 6) == glm::mat4(99), "history must not write beyond either frame's uploaded extent");
        finish_frame();

        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        deren::utility::vessel::array<glm::mat4, 3> const prefix = {glm::mat4(22), glm::mat4(23), glm::mat4(24)};
        engine.set_skin_matrices(prefix); // active-slot upload preserves the earlier upload's tail
        engine.set_skin_matrices({});     // empty uploads retain the existing block
        engine.set_skin_matrices(b, 99);  // invalid slots must not affect any history
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        previous = buffer(layout::heap_slots::skin_matrices_previous, second);
        CHECK(matrix(previous, 0) == glm::mat4(12));
        CHECK(matrix(previous, 5) == glm::mat4(17));
        finish_frame();
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        previous = buffer(layout::heap_slots::skin_matrices_previous, first);
        CHECK(matrix(previous, 0) == glm::mat4(22));
        CHECK(matrix(previous, 2) == glm::mat4(24));
        CHECK_MSG(matrix(previous, 3) == glm::mat4(5), "a short prefix upload must preserve existing joint matrices");
        CHECK(matrix(previous, 4) == glm::mat4(6));
        CHECK(matrix(previous, 5) == glm::mat4(0));
        finish_frame();
        return deren::vk_test::finish("skin_history");
    }

    if (has_flag(argc, argv, portable_frame_mode)) {
        rhi::create_info options{};
        options.window_width = 4;
        options.window_height = 4;
        options.window_visible = false;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        CHECK(root->query_extension(rhi::extension_kind::vulkan_escape) == nullptr);
        auto* const identity = root.get();
        deren::engine::scene_tree::scene scene{};
        pass::pass_chain shared_chain;
        deren::engine::runtime engine{root, options};
        engine.set_scene(scene);
        CHECK(&engine.rhi_face() == identity);
        auto* const frames = root->frames->walk_frames();
        auto const initial_slot = frames->position();
        deren::utility::vessel::array<glm::mat4, 5> const first_pose = {glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(9)};
        engine.set_skin_matrices(first_pose, initial_slot);
        CHECK(engine.pace_and_acquire() == deren::engine::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::engine::frame_status::proceed);
        auto first_history = deren::vk_test::probe_heap_buffer(*root, deren::engine::render_layout::heap_slot_offset(deren::engine::render_layout::heap_slots::skin_matrices_previous + initial_slot));
        CHECK(!first_history.empty());
        if (!first_history.empty()) {
            glm::mat4 value;
            std::memcpy(&value, first_history.data() + 4 * sizeof(value), sizeof(value));
            CHECK_MSG(value == glm::mat4(1), "a first rendered pose must start from identity skin history");
            std::memcpy(&value, first_history.data() + 5 * sizeof(value), sizeof(value));
            CHECK(value == glm::mat4(0));
        }
        CHECK(root->frames->begin_commands() != nullptr);
        engine.record_main_drawcalls();
        CHECK(engine.end_recording() == deren::engine::frame_status::proceed);
        CHECK(engine.submit_and_present() == deren::engine::frame_status::proceed);
        CHECK(frames->position() == (initial_slot + 1u) % frames->slot_count());
        CHECK(root->frames->begin_commands() == nullptr);
        CHECK(root->query_extension(rhi::extension_kind::vulkan_escape) == nullptr);
        auto* const shared = reinterpret_cast<rhi::pipeline*>(std::uintptr_t{0x1234});
        CHECK(engine.resolve_pipeline("post_hdr").contract == nullptr);
        shared_chain.emplace<shared_pipeline_provider>(nullptr);
        shared_chain.emplace<shared_pipeline_provider>(shared);
        engine.set_pass_chain(shared_chain, {});
        CHECK(engine.resolve_pipeline("post_hdr").contract == shared);
        CHECK(engine.resolve_pipeline("unknown_shared_pipeline").contract == nullptr);
        return deren::vk_test::finish("portable_runtime_frame");
    }

    // ---- THE CHILDREN: each must die in the constructor, for its own reason ------------------------
    if (has_flag(argc, argv, mismatch_mode) || has_flag(argc, argv, empty_root_mode) ||
        has_flag(argc, argv, accepted_probe_mode)) {
        rhi::shared_object<rhi::api_core> root;
        if (has_flag(argc, argv, mismatch_mode)) {
            root = deren::vk_test::probe_make_core(creation, rhi::abi_version + 1u); // a stale root
        } else if (has_flag(argc, argv, accepted_probe_mode)) {
            root = deren::vk_test::probe_make_core(creation, rhi::abi_version); // the probe, honest
            deren::vk_test::write_line("injection: the child hands the runtime a probe root that agrees on abi {}",
                                       rhi::abi_version);
        }
        // (the empty-root child leaves `root` empty on purpose)
        deren::engine::runtime doomed{std::move(root), creation};
        deren::vk_test::write_line("injection: THE RUNTIME ACCEPTED A ROOT IT MUST REFUSE - the constructor "
                                   "returned, so the check this test exists for is gone");
        return 0; // only reachable if the refusal disappeared: the parent reads this as a failure
    }

    check_descriptor_space_semantics();

    // ---- 1. THE AGREEING PATH, IN-PROCESS ---------------------------------------------------------
    rhi::shared_object<rhi::api_core> probe = deren::vk_test::probe_make_core(creation, rhi::abi_version);
    CHECK(probe != nullptr);
    if (probe == nullptr) {
        return deren::vk_test::finish("test_runtime_injection");
    }
    CHECK(probe->api_version() == rhi::abi_version);
    deren::engine::runtime_detail::verify_contract_version(*probe); // must not panic
    deren::vk_test::write_line("injection: verify_contract_version accepted a probe root reporting abi {}",
                               probe->api_version());

    // ---- 2/3. THE TWO REFUSALS, OBSERVED FROM CHILD PROCESSES -------------------------------------
    child_outcome const mismatch = run_child(argv[0], mismatch_mode);
    deren::utility::vessel::string const mismatch_panic = last_panic();
    child_outcome const empty = run_child(argv[0], empty_root_mode);
    CHECK_MSG(mismatch.ran && mismatch.status != 0, "a root reporting the wrong abi must make the runtime abort");
    CHECK_MSG(empty.ran && empty.status != 0, "an empty root must make the runtime abort");
    CHECK_MSG(mismatch_panic.find("reports abi") != deren::utility::vessel::string::npos,
              "the abi refusal names both numbers");
    CHECK_MSG(mismatch_panic.find(deren::utility::vessel::to_string(rhi::abi_version)) != deren::utility::vessel::string::npos,
              "the abi refusal names the number this image compiled");

    // ---- 2b. THE CONTRACT'S CONCRETE-IDENTITY PANIC, ALSO FROM A CHILD -------------------------
    // `do_concrete` is the one contract helper whose mismatch answer TERMINATES, so its diagnostic needs
    // the same child-process treatment as the abi refusals: status is read from the process, the message
    // from the last panic the child left (run_child captures its output, last_panic finds the marker).
    child_outcome const do_concrete_child = run_child(argv[0], do_concrete_panic_mode);
    deren::utility::vessel::string const do_concrete_panic = last_panic();
    CHECK_MSG(do_concrete_child.ran && do_concrete_child.status != 0,
              "a mismatched concrete name must terminate, not return a value");
    CHECK_MSG(do_concrete_panic.find("do_concrete") != deren::utility::vessel::string::npos,
              "the panic names the operation that was asked to cast");
    CHECK_MSG(do_concrete_panic.find("deren_contract_do_concrete_probe") != deren::utility::vessel::string::npos,
              "the panic names the handle's actual concrete name");
    CHECK_MSG(do_concrete_panic.find("deren_contract_do_concrete_other") != deren::utility::vessel::string::npos,
              "the panic names the concrete name the cast required");
    CHECK_MSG(do_concrete_panic.find("test_runtime_injection.cpp") != deren::utility::vessel::string::npos,
              "the panic reports the caller's source location");

    // ---- 4. THE PROBE IS ACCEPTED FIRST (the accepting path, one step past the handshake) ---------
    child_outcome const probe_child = run_child(argv[0], accepted_probe_mode);
    deren::utility::vessel::string const probe_panic = last_panic();
    CHECK_MSG(probe_child.ran && probe_child.status != 0, "a device-less probe root still cannot complete the construction");
    CHECK_MSG(probe_panic.find("graphics queue") != deren::utility::vessel::string::npos,
              "an agreeing probe root gets PAST the handshake - it dies at the device facts instead, which "
              "is what shows it was accepted");
    child_outcome const portable = run_child(argv[0], portable_frame_mode);
    CHECK_MSG(portable.ran && portable.status == 0,
              "a non-Vulkan root must construct, record, submit and present one frame (and the child must "
              "actually have run: a failure to start it is not a pass)");
    child_outcome const skin = run_child(argv[0], skin_history_mode);
    CHECK_MSG(skin.ran && skin.status == 0, "skin history must preserve CPU upload sources and rendered-frame ordering without touching unused matrices");
    child_outcome const managed_child = run_child(argv[0], "--child-managed-resources");
    CHECK(managed_child.ran && managed_child.status == 0u);
    child_outcome const first_output_failure = run_child(argv[0], "--child-output-first-failure");
    CHECK(first_output_failure.ran && first_output_failure.status == 0u);
    child_outcome const post_resources = run_child(argv[0], "--child-post-resources");
    CHECK_MSG(post_resources.ran && post_resources.status == 0u, read_file(L"child_output.txt").c_str());
    child_outcome const outputs = run_child(argv[0], "--child-output-resources");
    CHECK(outputs.ran && outputs.status == 0u);
    child_outcome const visibility_fallback = run_child(argv[0], "--child-visibility-fallback");
    CHECK(visibility_fallback.ran && visibility_fallback.status == 0u);
    child_outcome const constants = run_child(argv[0], "--child-scene-constants");
    CHECK(constants.ran && constants.status == 0u);
    child_outcome const camera_resources = run_child(argv[0], "--child-camera-resources");
    CHECK(camera_resources.ran && camera_resources.status == 0u);
    child_outcome const deformation_resources = run_child(argv[0], "--child-deformation-resources");
    CHECK(deformation_resources.ran && deformation_resources.status == 0u);
    child_outcome const scene_tables = run_child(argv[0], "--child-scene-tables");
    CHECK(scene_tables.ran && scene_tables.status == 0u);
    child_outcome const shared_assets = run_child(argv[0], "--child-shared-assets");
    CHECK(shared_assets.ran && shared_assets.status == 0u);
    child_outcome const clusters = run_child(argv[0], "--child-cluster-resources");
    CHECK(clusters.ran && clusters.status == 0u);
    child_outcome const managed_frames = run_child(argv[0], "--child-managed-frames");
    CHECK(managed_frames.ran && managed_frames.status == 0u);
    child_outcome const async = run_child(argv[0], async_present_mode);
    CHECK_MSG(async.ran && async.status == 0, "next-frame preparation must overlap an unfinished present and deferred errors must be collected");

    return deren::vk_test::finish("test_runtime_injection");
}
