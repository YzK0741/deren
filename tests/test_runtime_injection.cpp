// -*- C++ -*-
// ============================================================================
// file: tests/test_runtime_injection.cpp
//
// THE LAYERING'S OWN TEST (batch ⑥): the engine builds a renderer from a device root it was HANDED,
// and refuses the two roots it must not accept - with no loader, no DLL and no GPU in the picture.
//
// WHY THE HANDED-ROOT PATH IS THE EVIDENCE THE SPLIT IS REAL: before this batch the runtime loaded the
// backend itself, so "the engine can be given a root" was not expressible - every engine test either
// loaded the real DLL or could not build a runtime at all. Now the constructor takes
// `std::shared_ptr<rhi::api_core>` plus the same `create_info`, and this test injects a PROBE core: the
// first time the engine uses the probe backend for something other than the loader's contract walk.
//
// WHAT A PROBE CAN AND CANNOT PROVE, MEASURED RATHER THAN ASSUMED: the constructor's FIRST act is the
// consumption-side handshake (`runtime_detail::verify_contract_version`, called through `require_core`),
// and that is what a probe root can exercise end to end. Everything AFTER it legitimately needs a real
// device - the construction asks the escape for the presentation format and derives the graphics queue's
// FAMILY by walking the device's families and comparing `vkGetDeviceQueue()` against the escape's own
// queue handle (runtime/runtime.cpp) - so a device-less probe cannot carry the constructor to
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
// is the scaffold test's (tests/test_runtime_dyn.cpp calls deren::vulkan::load_api_core() against the
// real DLL and asserts the runtime took that very object).
// ============================================================================
#include "vk_test.h"

#include <cstdint>
#include <memory>
#include <string>
#include <windows.h> // CreateProcessW: the child-process form of the panic cases (see the banner)

import deren.promise.rhi;
import deren.vulkan.runtime;
import deren.vulkan.scene_tree;

// The injection source: the probe backend's test-only factory (see the header's own note).
#include "probe_backend.hpp"

namespace {
    namespace rhi = deren::promise::rhi;

    /// The child's modes: each constructs a runtime that MUST abort, and the parent checks which.
    constexpr char const* mismatch_mode = "--child-version-mismatch";
    constexpr char const* empty_root_mode = "--child-empty-root";
    constexpr char const* accepted_probe_mode = "--child-accepted-probe";
    constexpr char const* portable_frame_mode = "--child-portable-frame";

    [[nodiscard]] bool has_flag(int const argc, char** const argv, char const* const flag) {
        for (int i = 1; i < argc; ++i) {
            if (std::string{argv[i]} == flag) {
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
        std::string const command = "\"" + std::string{self} + "\" " + flag;
        std::wstring const wide(command.begin(), command.end());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (CreateProcessW(nullptr, const_cast<wchar_t*>(wide.c_str()), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &child) == FALSE) {
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

    /// The LAST panic message in this directory's debug.log (the sink flushes before it terminates).
    [[nodiscard]] std::string last_panic() {
        std::string text;
        HANDLE const file = CreateFileW(L"debug.log", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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
        std::string const marker = "[ERROR] error info: ";
        std::size_t const at = text.rfind(marker);
        return at == std::string::npos ? std::string{} : text.substr(at + marker.size());
    }
} // namespace

int main(int const argc, char** const argv) {
    rhi::create_info const creation{};

    if (has_flag(argc, argv, portable_frame_mode)) {
        rhi::create_info options{};
        options.window_width = 4;
        options.window_height = 4;
        options.window_visible = false;
        auto root = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        CHECK(root->query_extension(rhi::extension_kind::vulkan_escape) == nullptr);
        auto* const identity = root.get();
        deren::vulkan::scene_tree::scene scene{};
        deren::vulkan::runtime engine{root, options};
        engine.set_scene(scene);
        CHECK(&engine.rhi_face() == identity);
        auto* const frames = root->walk_frames();
        auto const initial_slot = frames->position();
        CHECK(engine.pace_and_acquire() == deren::vulkan::frame_status::proceed);
        CHECK(engine.begin_recording() == deren::vulkan::frame_status::proceed);
        CHECK(root->begin_commands() != nullptr);
        engine.record_main_drawcalls();
        CHECK(engine.end_recording() == deren::vulkan::frame_status::proceed);
        CHECK(engine.submit_and_present() == deren::vulkan::frame_status::proceed);
        CHECK(frames->position() == (initial_slot + 1u) % frames->slot_count());
        CHECK(root->begin_commands() == nullptr);
        CHECK(root->query_extension(rhi::extension_kind::vulkan_escape) == nullptr);
        return deren::vk_test::finish("portable_runtime_frame");
    }

    // ---- THE CHILDREN: each must die in the constructor, for its own reason ------------------------
    if (has_flag(argc, argv, mismatch_mode) || has_flag(argc, argv, empty_root_mode) ||
        has_flag(argc, argv, accepted_probe_mode)) {
        std::shared_ptr<rhi::api_core> root;
        if (has_flag(argc, argv, mismatch_mode)) {
            root = deren::vk_test::probe_make_core(creation, rhi::abi_version + 1u); // a stale root
        } else if (has_flag(argc, argv, accepted_probe_mode)) {
            root = deren::vk_test::probe_make_core(creation, rhi::abi_version); // the probe, honest
            deren::vk_test::write_line("injection: the child hands the runtime a probe root that agrees on abi {}",
                                       rhi::abi_version);
        }
        // (the empty-root child leaves `root` empty on purpose)
        deren::vulkan::runtime doomed{std::move(root), creation};
        deren::vk_test::write_line("injection: THE RUNTIME ACCEPTED A ROOT IT MUST REFUSE - the constructor "
                                   "returned, so the check this test exists for is gone");
        return 0; // only reachable if the refusal disappeared: the parent reads this as a failure
    }

    // ---- 1. THE AGREEING PATH, IN-PROCESS ---------------------------------------------------------
    std::shared_ptr<rhi::api_core> probe = deren::vk_test::probe_make_core(creation, rhi::abi_version);
    CHECK(probe != nullptr);
    if (probe == nullptr) {
        return deren::vk_test::finish("test_runtime_injection");
    }
    CHECK(probe->api_version() == rhi::abi_version);
    deren::vulkan::runtime_detail::verify_contract_version(*probe); // must not panic
    deren::vk_test::write_line("injection: verify_contract_version accepted a probe root reporting abi {}",
                               probe->api_version());

    // ---- 2/3. THE TWO REFUSALS, OBSERVED FROM CHILD PROCESSES -------------------------------------
    child_outcome const mismatch = run_child(argv[0], mismatch_mode);
    std::string const mismatch_panic = last_panic();
    child_outcome const empty = run_child(argv[0], empty_root_mode);
    CHECK_MSG(mismatch.ran && mismatch.status != 0, "a root reporting the wrong abi must make the runtime abort");
    CHECK_MSG(empty.ran && empty.status != 0, "an empty root must make the runtime abort");
    CHECK_MSG(mismatch_panic.find("reports abi") != std::string::npos,
              "the abi refusal names both numbers");
    CHECK_MSG(mismatch_panic.find(std::to_string(rhi::abi_version)) != std::string::npos,
              "the abi refusal names the number this image compiled");

    // ---- 4. THE PROBE IS ACCEPTED FIRST (the accepting path, one step past the handshake) ---------
    child_outcome const probe_child = run_child(argv[0], accepted_probe_mode);
    std::string const probe_panic = last_panic();
    CHECK_MSG(probe_child.ran && probe_child.status != 0, "a device-less probe root still cannot complete the construction");
    CHECK_MSG(probe_panic.find("graphics queue") != std::string::npos,
              "an agreeing probe root gets PAST the handshake - it dies at the device facts instead, which "
              "is what shows it was accepted");
    child_outcome const portable = run_child(argv[0], portable_frame_mode);
    CHECK_MSG(portable.ran && portable.status == 0,
              "a non-Vulkan root must construct, record, submit and present one frame (and the child must "
              "actually have run: a failure to start it is not a pass)");

    return deren::vk_test::finish("test_runtime_injection");
}
