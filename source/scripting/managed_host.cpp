module;

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#include <windows.h>

module deren.scripting.managed_host;

namespace deren::scripting {
    namespace {
        using library_handle = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;

        library_handle load_library(wchar_t const* path) {
            return {LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS), FreeLibrary};
        }
    } // namespace

    host_result initialize_managed_host(host_paths const& paths, native_api const& api, managed_api& scripts) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(paths.assembly, error)) {
            return {.status = host_status::assembly_missing, .detail = error.value()};
        }
        if (!std::filesystem::is_regular_file(paths.runtime_config, error)) {
            return {.status = host_status::runtime_config_missing, .detail = error.value()};
        }

        auto const nethost = load_library(std::filesystem::absolute(paths.nethost).c_str());
        if (!nethost) {
            return {host_status::nethost_load_failed, static_cast<std::int32_t>(GetLastError())};
        }
        auto const get_hostfxr_path = reinterpret_cast<decltype(&::get_hostfxr_path)>(
            GetProcAddress(nethost.get(), "get_hostfxr_path"));
        if (get_hostfxr_path == nullptr) {
            return {host_status::nethost_load_failed, static_cast<std::int32_t>(GetLastError())};
        }

        auto const assembly = std::filesystem::absolute(paths.assembly);
        auto const dotnet_root = paths.dotnet_root.empty() ? std::filesystem::path{} : std::filesystem::absolute(paths.dotnet_root);
        get_hostfxr_parameters const parameters{
            sizeof(get_hostfxr_parameters), assembly.c_str(),
            dotnet_root.empty() ? nullptr : dotnet_root.c_str()};
        std::vector<wchar_t> hostfxr_path(MAX_PATH);
        std::size_t path_length = hostfxr_path.size();
        auto status = get_hostfxr_path(hostfxr_path.data(), &path_length, &parameters);
        if (status != 0 && path_length > hostfxr_path.size()) {
            hostfxr_path.resize(path_length);
            status = get_hostfxr_path(hostfxr_path.data(), &path_length, &parameters);
        }
        if (status != 0) {
            return {host_status::runtime_discovery_failed, status};
        }

        // Retain the selected hostfxr for the process lifetime, like the CLR it starts.
        auto const candidate = load_library(hostfxr_path.data());
        if (!candidate) {
            return {host_status::hostfxr_load_failed, static_cast<std::int32_t>(GetLastError())};
        }
        static auto const hostfxr = load_library(hostfxr_path.data());
        if (candidate.get() != hostfxr.get()) {
            return {host_status::hostfxr_load_failed};
        }

        auto const initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
            GetProcAddress(hostfxr.get(), "hostfxr_initialize_for_runtime_config"));
        auto const get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
            GetProcAddress(hostfxr.get(), "hostfxr_get_runtime_delegate"));
        auto const close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(hostfxr.get(), "hostfxr_close"));
        if (initialize == nullptr || get_delegate == nullptr || close == nullptr) {
            return {host_status::hostfxr_load_failed, ERROR_PROC_NOT_FOUND};
        }

        hostfxr_handle context = nullptr;
        auto const runtime_config = std::filesystem::absolute(paths.runtime_config);
        hostfxr_initialize_parameters const initialize_parameters{
            sizeof(hostfxr_initialize_parameters), nullptr, parameters.dotnet_root};
        status = initialize(runtime_config.c_str(), &initialize_parameters, &context);
        if (status < 0 || context == nullptr) {
            if (context != nullptr) {
                close(context);
            }
            return {host_status::runtime_initialization_failed, status};
        }

        void* load_assembly_address = nullptr;
        status = get_delegate(context, hdt_load_assembly_and_get_function_pointer, &load_assembly_address);
        close(context);
        if (status != 0 || load_assembly_address == nullptr) {
            return {host_status::delegate_lookup_failed, status};
        }

        auto const load_assembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(load_assembly_address);
        void* initialize_bridge_address = nullptr;
        status = load_assembly(assembly.c_str(), L"deren.scripting.managed_bootstrap, Deren.Managed",
                               L"initialize_from_engine", UNMANAGEDCALLERSONLY_METHOD, nullptr, &initialize_bridge_address);
        if (status != 0 || initialize_bridge_address == nullptr) {
            return {host_status::bridge_load_failed, status};
        }

        using initialize_bridge_fn = std::int32_t(__cdecl*)(native_api const*, managed_api*);
        auto *const initialize_bridge = reinterpret_cast<initialize_bridge_fn>(initialize_bridge_address);
        status = initialize_bridge(&api, &scripts);
        switch (static_cast<host_status>(status)) {
        case host_status::success:
        case host_status::abi_mismatch:
        case host_status::invalid_api:
            return {static_cast<host_status>(status)};
        default:
            return {.status = host_status::bridge_initialization_failed, .detail = status};
        }
    }
} // namespace deren::scripting
