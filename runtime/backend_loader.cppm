// ============================================================================
// module: deren.vulkan.backend_loader
// module version: 0.1.0a
//
// WHERE THE BACKEND IS LOADED, AND WHERE THE DEVICE ROOT IS ACQUIRED (batch ⑥, abi 19).
//
// WHY THIS IS A MODULE OF ITS OWN, AND NOT PART OF THE RUNTIME: loading a library and acquiring the
// device root are APPLICATION decisions - which backend, from where, with which search rules, and what
// to do when it is not there - while `deren.vulkan.runtime` is the RENDERER. The runtime used to carry
// both (it loaded `deren_vulkan.dll`, resolved the entry and called it before its own construction), so
// the app could not answer any of those questions and the renderer owned a policy that is not its job.
// They are now apart, and the seam is one function:
//
//     auto core = deren::vulkan::load_api_core(core_options);   // the app (main.cpp, the scaffold)
//     deren::vulkan::runtime runtime{core, core_options};       // the renderer, given a root
//
// AND NOT PART OF `deren.utility`/`static_utility` EITHER, deliberately: that half is a pure-CPU
// toolkit with no loading semantics (`dynamic_link` below is the MECHANISM - open a named image,
// resolve a named symbol - and it knows nothing about backends or contracts). This module is the
// POLICY on top of it: the backend's file name, the search rules, the handshake, the diagnosis.
// The dependency direction is one-way: this module imports the toolkit and the contract; neither
// knows it exists, and the runtime does not import IT.
//
// THE FOUR THINGS IT GUARANTEES (each one measured during the flip, see DYNAMIC_LINK_PROGRESS.md §18):
//
//   1. AN ABSOLUTE PATH INTO LoadLibraryExW. The DLL is looked for beside THIS executable
//      (`executable_directory() / "deren_vulkan.dll"`) and passed as an absolute path, which is what
//      makes the loader use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`:
//      the DLL's own directory and the system directories, NEVER %PATH% (the Q5 lesson - a bare name
//      takes the classic search order instead, which is how a planted DLL gets in).
//   2. ONCE PER PROCESS. The load and the symbol lookup happen in one function-local static, so the
//      image is opened once however many times a caller asks for a root, and a second thread cannot
//      race a second load.
//   3. NEVER UNLOADED. The handle is DETACHED from the loader object, which would otherwise close it
//      in its destructor: `FreeLibrary` after a context had been built and torn down never returned
//      (DYNAMIC_LINK_V2.md §13), and the resolved entry address stays valid for the life of the
//      process only because the image is never unmapped.
//   4. THE CREATION-SIDE HANDSHAKE. `rhi::abi_version` is the entry's FIRST ARGUMENT, so a mismatched
//      caller is refused by the backend BEFORE an object exists, with the backend's own number in the
//      `error_info` it fills. (The CONSUMPTION side - the object attesting its own number - is
//      `api_core::api_version()`, checked by the runtime; both sides are guarded because a host may
//      receive an `api_core` it did not create.)
//
// FAILURE IS A NAMED DIAGNOSIS, NOT A NULL POINTER: every failing step fills the abi 13 `error_info`
// (decision code, the raw native error, the backend's static text, the failure point) and reports it
// through `deren::utility::error` before returning an EMPTY `shared_ptr`. The caller decides what a
// failed acquisition means for ITS startup - which is exactly the layering this module exists to
// establish (the application decides, the library reports).
//
// THE ONE FIELD IT MUST TRANSLATE, and the only thing it changes about the caller's structure: the
// WINDOW. `create_info::native_window` is an opaque `void*` whose meaning is the BACKEND's, and for
// this backend (Windows, from a DLL that carries its own GLFW image) it is an `HWND`. A
// `GLFWwindow*` cannot cross that boundary: GLFW's state is per-image, so a pointer made by the
// executable's GLFW says nothing to the DLL's copy - MEASURED, and it cost all fourteen render
// scenarios a panic in `core::init_surface` (DYNAMIC_LINK_PROGRESS.md §18.3). The conversion happens
// HERE rather than in the application because this module is exactly the thing that crosses the image
// boundary, and rather than in the runtime because the runtime must keep the `GLFWwindow*` for its
// own callbacks (they run in THIS image's GLFW). Everything else in the structure is passed through
// untouched: it does not pick a backend, take a fallback, or retry.
// ============================================================================
module;

#include <cstdint>
#include <filesystem>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32 // the HWND behind the caller's GLFWwindow (see the banner)
#include <GLFW/glfw3native.h>

export module deren.vulkan.backend_loader;

export import deren.promise.rhi; // create_info / api_core / error_info / abi_version

import deren.utility;              // executable_directory() + the named-diagnosis channel
import deren.utility.dynamic_link; // the mechanism: open the image, resolve the symbol

// The entry's declaration and its C++ type. AFTER the imports, because the header names the contract's
// types (its own note says so) - and its system includes are already in the global module fragment
// above, so nothing new enters the module purview here.
#include "../promise/rhi/backend_entry.hpp"

namespace {
    namespace rhi = deren::promise::rhi;

    /// The backend's file name, in ONE place: the package is `deren_vulkan.dll` + `shared_utility.dll`
    /// beside the executable (CMake clears MinGW's `lib` prefix for exactly this reason).
    constexpr std::string_view backend_file_name = "deren_vulkan.dll";
    /// The ONE symbol the backend exports (promise/rhi/backend_entry.hpp, abi 18+).
    constexpr std::string_view backend_entry_symbol = "deren_make_api_core";

    /// Report one failed step as the abi 13 diagnostic, with the failure POINT, and remember that the
    /// acquisition failed. `rhi::error::abi_mismatch` is used for the version case and
    /// `initialization_failed` for everything else - the two decisions a caller can act on.
    void report(std::string_view what, rhi::error code, std::int32_t native_code, std::string_view detail,
                std::source_location where = std::source_location::current()) {
        rhi::error_info const info{.code = code,
                                   .api = rhi::graphics_api::vulkan,
                                   .native_code = native_code,
                                   .message = detail,
                                   .where = where};
        deren::utility::error("backend_loader: {} (error code {}, native {}, at {}:{}) - {}", what,
                              static_cast<std::uint32_t>(info.code), info.native_code, where.file_name(), where.line(),
                              detail);
    }

    /// THE ENTRY, LOADED AND RESOLVED ONCE PER PROCESS, or nullptr after a reported failure.
    ///
    /// The failure is STICKY on purpose: a missing DLL or a missing export is a PACKAGING error, not a
    /// transient one - the environment cannot change between two calls in one process, and reporting the
    /// same diagnosis on every call would only make the log lie about how many times it happened. The
    /// caller still sees an empty `shared_ptr` every time, so its own error path stays simple.
    [[nodiscard]] deren_make_api_core_fn backend_entry() {
        static deren_make_api_core_fn const resolved = []() -> deren_make_api_core_fn {
            std::filesystem::path const dll = deren::utility::executable_directory() / std::string{backend_file_name};
            auto loaded = deren::utility::dynamic_link::load(dll.string());
            if (!loaded.has_value()) {
                report("the backend library could not be loaded", rhi::error::initialization_failed, loaded.error().code,
                       loaded.error().message);
                return nullptr;
            }
            auto const symbol = loaded->symbol(backend_entry_symbol);
            if (!symbol.has_value()) {
                report("the backend library does not export its entry point", rhi::error::initialization_failed,
                       symbol.error().code, symbol.error().message);
                return nullptr;
            }
            // NEVER UNLOAD (see the banner): the handle leaves the loader object here and is never closed,
            // which is what keeps the resolved address valid for the rest of the process.
            static_cast<void>(loaded->detach());
            return reinterpret_cast<deren_make_api_core_fn>(const_cast<void*>(*symbol));
        }();
        return resolved;
    }
} // namespace

namespace deren::vulkan {
    /**
     * @brief load the backend (once per process) and acquire a device root from it
     * @param desc the contract's ONE creation structure, passed to the backend unchanged
     * @return the owning handle, or an EMPTY `shared_ptr` when the backend could not be loaded, does not
     *         export its entry point, or REFUSED the creation (the diagnosis is reported before the
     *         return; `abi_mismatch` means the caller's `rhi::abi_version` is not the backend's)
     * @ingroup runtime
     *
     * The handshake is inside: `rhi::abi_version` travels as the entry's first argument and the
     * `error_info` the backend fills carries its own number on a mismatch. The `api_core` that comes
     * back attests its own number through `api_version()`, which `deren::vulkan::runtime` checks before
     * it uses the object - two sides of one fact, guarded where each can be observed.
     */
    export [[nodiscard]] std::shared_ptr<rhi::api_core> load_api_core(rhi::create_info const& desc) {
        deren_make_api_core_fn const entry = backend_entry();
        if (entry == nullptr) {
            return {}; // the diagnosis was reported while the failure was found
        }
        // THE WINDOW, CONVERTED FOR THE WIRE (see the banner): a `GLFWwindow*` from THIS image's GLFW
        // means nothing to the DLL's own copy, so the native handle is what crosses. The caller's
        // structure is otherwise handed over unchanged, and it is a cheap trivially-copyable POD.
        rhi::create_info wire = desc;
#if defined(_WIN32)
        wire.native_window = desc.native_window != nullptr
                                 ? static_cast<void*>(glfwGetWin32Window(static_cast<GLFWwindow*>(desc.native_window)))
                                 : nullptr;
#endif
        rhi::error_info status{};
        std::shared_ptr<rhi::api_core> core = entry(rhi::abi_version, &wire, &status);
        if (!core) {
            report("the backend refused the creation", status.code, status.native_code,
                   status.message.empty() ? std::string_view{"no diagnostic text"} : status.message, status.where);
            return {};
        }
        return core;
    }
} // namespace deren::vulkan
