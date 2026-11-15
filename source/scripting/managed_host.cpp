module;

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

module deren.scripting.managed_host;

import deren.utility.dynamic_link;

namespace deren::scripting {
    namespace {
#if defined(_WIN32)
        using host_character = wchar_t;
#else
        using host_character = char;
#endif

        std::string path_to_utf8(std::filesystem::path const& path) {
            auto const utf8 = path.u8string();
            return {reinterpret_cast<char const*>(utf8.data()), utf8.size()};
        }

        using configure_bridge_fn = std::int32_t (*)(native_api const*, std::uint32_t, utf8_view, startup_options*,
                                                     std::int32_t, char**);
        using initialize_bridge_fn = std::int32_t (*)(native_api const*, managed_api*);

        struct bridge_entrypoints {
            configure_bridge_fn configure = nullptr;
            initialize_bridge_fn initialize = nullptr;
        };

        bridge_entrypoints g_bridge;

        host_result bridge_status(std::int32_t const status) {
            if (status == static_cast<std::int32_t>(script_status::module_unavailable)) {
                return {host_status::scene_module_unavailable, status};
            }
            if (status == static_cast<std::int32_t>(script_status::module_invalid)) {
                return {host_status::scene_module_invalid, status};
            }
            switch (static_cast<host_status>(status)) {
            case host_status::success:
                return {host_status::success};
            case host_status::abi_mismatch:
            case host_status::invalid_api:
                return {static_cast<host_status>(status)};
            default:
                return {.status = host_status::bridge_initialization_failed, .detail = status};
            }
        }

        host_result load_bridge(host_paths const& paths, native_api const& api, startup_options& options,
                                std::uint32_t const abi_version) {
            auto const game_assembly_path = std::filesystem::absolute(paths.game_assembly);
            std::error_code error;
            if (!std::filesystem::is_regular_file(paths.assembly, error)) {
                return {.status = host_status::assembly_missing, .detail = error.value()};
            }
            if (!std::filesystem::is_regular_file(paths.runtime_config, error)) {
                return {.status = host_status::runtime_config_missing, .detail = error.value()};
            }
            if (!std::filesystem::is_regular_file(paths.game_assembly, error)) {
                return {.status = host_status::game_assembly_missing, .detail = error.value()};
            }

            auto const nethost = deren::utility::dynamic_link::load(
                path_to_utf8(std::filesystem::absolute(paths.nethost)));
            if (!nethost) {
                return {host_status::nethost_load_failed, nethost.error().code};
            }
            auto const get_hostfxr_path_symbol = nethost->symbol("get_hostfxr_path");
            if (!get_hostfxr_path_symbol) {
                return {host_status::nethost_load_failed, get_hostfxr_path_symbol.error().code};
            }
            auto const get_hostfxr_path =
                reinterpret_cast<decltype(&::get_hostfxr_path)>(const_cast<void*>(*get_hostfxr_path_symbol));

            auto const assembly = std::filesystem::absolute(paths.assembly);
            auto const dotnet_root = paths.dotnet_root.empty() ? std::filesystem::path{} : std::filesystem::absolute(paths.dotnet_root);
            get_hostfxr_parameters const parameters{
                sizeof(get_hostfxr_parameters), assembly.c_str(),
                dotnet_root.empty() ? nullptr : dotnet_root.c_str()};
            std::vector<host_character> hostfxr_path(4096);
            std::size_t path_length = hostfxr_path.size();
            auto status = get_hostfxr_path(hostfxr_path.data(), &path_length, &parameters);
            if (status != 0 && path_length > hostfxr_path.size()) {
                hostfxr_path.resize(path_length);
                status = get_hostfxr_path(hostfxr_path.data(), &path_length, &parameters);
            }
            if (status != 0) {
                return {host_status::runtime_discovery_failed, status};
            }

            auto hostfxr = deren::utility::dynamic_link::load(path_to_utf8(std::filesystem::path{hostfxr_path.data()}));
            if (!hostfxr) {
                return {host_status::hostfxr_load_failed, hostfxr.error().code};
            }
            auto const initialize_symbol = hostfxr->symbol("hostfxr_initialize_for_runtime_config");
            auto const get_delegate_symbol = hostfxr->symbol("hostfxr_get_runtime_delegate");
            auto const close_symbol = hostfxr->symbol("hostfxr_close");
            if (!initialize_symbol) {
                return {host_status::hostfxr_load_failed, initialize_symbol.error().code};
            }
            if (!get_delegate_symbol) {
                return {host_status::hostfxr_load_failed, get_delegate_symbol.error().code};
            }
            if (!close_symbol) {
                return {host_status::hostfxr_load_failed, close_symbol.error().code};
            }
            auto const initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(const_cast<void*>(*initialize_symbol));
            auto const get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(const_cast<void*>(*get_delegate_symbol));
            auto const close = reinterpret_cast<hostfxr_close_fn>(const_cast<void*>(*close_symbol));
            // The CLR keeps using hostfxr for the process lifetime; do not unload it at function exit.
            static_cast<void>(hostfxr->detach());

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
            void* configure_address = nullptr;
            void* initialize_address = nullptr;
#if defined(_WIN32)
            constexpr host_character const* const bridge_type = L"deren.scripting.managed_bootstrap, Deren.Managed";
            constexpr host_character const* const configure_entry = L"configure_from_engine";
            constexpr host_character const* const initialize_entry = L"initialize_from_engine";
#else
            constexpr host_character const* const bridge_type = "deren.scripting.managed_bootstrap, Deren.Managed";
            constexpr host_character const* const configure_entry = "configure_from_engine";
            constexpr host_character const* const initialize_entry = "initialize_from_engine";
#endif
            status = load_assembly(assembly.c_str(), bridge_type, configure_entry, UNMANAGEDCALLERSONLY_METHOD,
                                   nullptr, &configure_address);
            if (status != 0 || configure_address == nullptr) {
                return {host_status::bridge_load_failed, status};
            }
            status = load_assembly(assembly.c_str(), bridge_type, initialize_entry, UNMANAGEDCALLERSONLY_METHOD,
                                   nullptr, &initialize_address);
            if (status != 0 || initialize_address == nullptr) {
                return {host_status::bridge_load_failed, status};
            }

            auto const configure_bridge = reinterpret_cast<configure_bridge_fn>(configure_address);
            auto const initialize_bridge = reinterpret_cast<initialize_bridge_fn>(initialize_address);
            auto const game_assembly = game_assembly_path.u8string();
            utf8_view const game_assembly_view{
                reinterpret_cast<char const*>(game_assembly.data()), static_cast<std::uint32_t>(game_assembly.size())};
            status = configure_bridge(&api, abi_version, game_assembly_view, &options, paths.argc, paths.argv);
            if (status != static_cast<std::int32_t>(script_status::success)) {
                return bridge_status(status);
            }

            g_bridge = {.configure = configure_bridge, .initialize = initialize_bridge};
            return {host_status::success};
        }
    } // namespace

    host_result configure_managed_host(host_paths const& paths, native_api const& api, startup_options& options,
                                      std::uint32_t const abi_version) {
        if (g_bridge.initialize != nullptr) {
            return {host_status::success};
        }
        return load_bridge(paths, api, options, abi_version);
    }

    host_result initialize_managed_host(host_paths const& paths, native_api const& api, managed_api& scripts,
                                       std::uint32_t const abi_version) {
        if (g_bridge.initialize == nullptr) {
            startup_options options{};
            auto const configured = load_bridge(paths, api, options, abi_version);
            if (configured.status != host_status::success) {
                return configured;
            }
        }
        auto const status = g_bridge.initialize(&api, &scripts);
        return bridge_status(status);
    }
} // namespace deren::scripting
