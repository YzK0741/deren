cmake_minimum_required(VERSION 4.3)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(deren_managed_host LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)
    get_filename_component(DEREN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    include("${DEREN_REPO_ROOT}/source/vstd/vstd.cmake")
    option(VR_BUILD_TESTS "Build the headless unit tests" ON)
endif()

if(NOT WIN32 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The deren managed host currently targets Windows x64.")
endif()

find_program(DOTNET_EXECUTABLE dotnet HINTS "$ENV{DOTNET_ROOT}" REQUIRED)
get_filename_component(dotnet_executable "${DOTNET_EXECUTABLE}" REALPATH)
get_filename_component(dotnet_root "${dotnet_executable}" DIRECTORY)
file(GLOB host_packs LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/10.*")
list(SORT host_packs COMPARE NATURAL ORDER DESCENDING)
if(NOT host_packs)
    message(FATAL_ERROR "Install the .NET 10 x64 SDK for ${dotnet_executable}.")
endif()
list(GET host_packs 0 host_pack)
set(host_native "${host_pack}/runtimes/win-x64/native")

set(bridge_project "${CMAKE_CURRENT_SOURCE_DIR}/managed/Deren.Managed.csproj")
set(bridge_output "${CMAKE_CURRENT_BINARY_DIR}/bridge")
set(bridge_files
    "${bridge_output}/Deren.Managed.dll"
    "${bridge_output}/Deren.Managed.deps.json"
    "${bridge_output}/Deren.Managed.runtimeconfig.json"
    "${bridge_output}/nethost.dll")
add_custom_command(OUTPUT ${bridge_files}
    COMMAND "${DOTNET_EXECUTABLE}" build "${bridge_project}"
        --configuration Release --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/dotnet"
        --output "${bridge_output}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${host_native}/nethost.dll" "${bridge_output}/nethost.dll"
    DEPENDS "${bridge_project}" managed/managed_bootstrap.cs managed/behaviour.cs managed/scene_scripts.cs
        "${CMAKE_CURRENT_SOURCE_DIR}/../../.editorconfig" "${host_native}/nethost.dll"
    VERBATIM)
add_custom_target(deren_managed_bridge ALL DEPENDS ${bridge_files})
set_property(TARGET deren_managed_bridge PROPERTY DEREN_MANAGED_RUNTIME_DIR "${bridge_output}")

add_library(deren_managed_host STATIC managed_host.cpp)
target_sources(deren_managed_host PUBLIC FILE_SET CXX_MODULES FILES managed_host.cppm)
target_link_libraries(deren_managed_host PUBLIC vstd_lib)
target_include_directories(deren_managed_host SYSTEM PRIVATE "${host_native}")

if(NOT VR_BUILD_TESTS)
    return()
endif()

set(game_project "${CMAKE_CURRENT_SOURCE_DIR}/test_game/Game.csproj")
set(game_output "${CMAKE_CURRENT_BINARY_DIR}/test_game")
set(game_assembly "${game_output}/Game.dll")
configure_file(test_game/deren.toml.in "${game_output}/deren.toml" @ONLY)
configure_file(test_game/main.toml "${game_output}/main.toml" COPYONLY)
add_custom_command(OUTPUT "${game_assembly}"
    COMMAND "${DOTNET_EXECUTABLE}" build "${game_project}"
        --configuration Release --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/dotnet"
        --output "${game_output}"
    DEPENDS "${game_project}" test_game/rotate.cs ${bridge_files}
    VERBATIM)
add_custom_target(deren_managed_test_game DEPENDS "${game_assembly}")
add_dependencies(deren_managed_test_game deren_managed_bridge)

enable_testing()
add_executable(test_managed_host ../tests/test_managed_host.cpp)
target_link_libraries(test_managed_host PRIVATE deren_managed_host)
add_dependencies(test_managed_host deren_managed_test_game)
foreach(scenario IN ITEMS round_trip missing_assembly missing_runtime abi_mismatch abi_table_too_small managed_table_too_small scene_lifecycle)
    add_test(NAME managed_host_${scenario} COMMAND test_managed_host "${bridge_output}" "${scenario}" "${game_assembly}")
endforeach()
