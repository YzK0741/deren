# The managed host is part of the ONE root build graph: the repository root's CMakeLists.txt owns
# the compiler, the module scan and the per-target options, and includes this directory
# unconditionally. Configuring this directory on its own used to produce a second, differently
# flagged build tree whose assemblies nobody ran; it is refused now, so there is exactly one way
# to build the host, the bridge and the demo game.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    # Kept only so CMake does not stop first on CMP0000 with an unrelated policy error.
    cmake_minimum_required(VERSION 4.3)
    message(FATAL_ERROR "The managed host is built from the repository root: cmake -S . -B build")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The deren managed host requires a 64-bit target.")
endif()

if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(host_rid win-x64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(host_rid win-arm64)
    else()
        message(FATAL_ERROR "Unsupported Windows processor for the deren managed host: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(host_rid osx-arm64)
    else()
        set(host_rid osx-x64)
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(host_rid linux-x64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(host_rid linux-arm64)
    else()
        message(FATAL_ERROR "Unsupported unix processor for the deren managed host: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for the deren managed host: ${CMAKE_SYSTEM_NAME}")
endif()

if(WIN32)
    set(host_nethost nethost.dll)
elseif(APPLE)
    set(host_nethost libnethost.dylib)
else()
    set(host_nethost libnethost.so)
endif()

find_program(DOTNET_EXECUTABLE dotnet HINTS "$ENV{DOTNET_ROOT}" REQUIRED)
get_filename_component(dotnet_executable "${DOTNET_EXECUTABLE}" REALPATH)
get_filename_component(dotnet_root "${dotnet_executable}" DIRECTORY)
file(GLOB host_packs LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${dotnet_root}/packs/Microsoft.NETCore.App.Host.${host_rid}/10.*")
list(SORT host_packs COMPARE NATURAL ORDER DESCENDING)
if(NOT host_packs)
    message(FATAL_ERROR "Install the .NET 10 ${host_rid} SDK for ${dotnet_executable}.")
endif()
list(GET host_packs 0 host_pack)
set(host_native "${host_pack}/runtimes/${host_rid}/native")

set(bridge_project "${CMAKE_CURRENT_SOURCE_DIR}/managed/Deren.Managed.csproj")
file(GLOB_RECURSE bridge_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/managed/*.cs")
list(FILTER bridge_sources EXCLUDE REGEX "[/\\\\](obj|bin)[/\\\\]")
set(bridge_output "${CMAKE_CURRENT_BINARY_DIR}/bridge")
set(bridge_files
    "${bridge_output}/Deren.Managed.dll"
    "${bridge_output}/Deren.Managed.deps.json"
    "${bridge_output}/Deren.Managed.runtimeconfig.json"
    "${bridge_output}/${host_nethost}")
add_custom_command(OUTPUT ${bridge_files}
    COMMAND "${DOTNET_EXECUTABLE}" build "${bridge_project}"
        --configuration Release --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/dotnet"
        --output "${bridge_output}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${host_native}/${host_nethost}" "${bridge_output}/${host_nethost}"
    DEPENDS "${bridge_project}" ${bridge_sources}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../.editorconfig" "${host_native}/${host_nethost}"
    VERBATIM)
add_custom_target(deren_managed_bridge ALL DEPENDS ${bridge_files})
set_property(TARGET deren_managed_bridge PROPERTY DEREN_MANAGED_RUNTIME_DIR "${bridge_output}")

add_library(deren_managed_host STATIC managed_host.cpp)
target_sources(deren_managed_host PUBLIC FILE_SET CXX_MODULES FILES managed_host.cppm)
target_link_libraries(deren_managed_host PUBLIC vstd_lib static_utility deren_script_api)


target_include_directories(deren_managed_host SYSTEM PRIVATE "${host_native}")

# ================================================================================================
# THE EDITOR IS A MANAGED PROJECT, like the demo game: it compiles to Game.dll and is launched with
# `deren run build/source/editor`. Its project manifest points back at the authored TOML in
# source/editor, so the build tree holds no copy of it. Unlike the demo it is not a test fixture -
# it ships with every build - so this block sits before the VR_BUILD_TESTS early return.
# ================================================================================================
set(editor_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/../editor")
set(editor_project "${editor_source_dir}/Editor.csproj")
set(editor_output "${CMAKE_BINARY_DIR}/source/editor")
set(editor_assembly "${editor_output}/Game.dll")
file(GLOB_RECURSE editor_sources CONFIGURE_DEPENDS "${editor_source_dir}/*.cs")
list(FILTER editor_sources EXCLUDE REGEX "[/\\\\](obj|bin)[/\\\\]")
set(DEREN_EDITOR_SCENE "${editor_source_dir}/main.toml")
set(DEREN_EDITOR_ASSEMBLY "${editor_assembly}")
configure_file("${editor_source_dir}/deren.toml.in" "${editor_output}/deren.toml" @ONLY)
configure_file("${editor_source_dir}/editor_worker.toml.in" "${editor_output}/editor_worker.toml" @ONLY)
add_custom_command(OUTPUT "${editor_assembly}"
    COMMAND "${DOTNET_EXECUTABLE}" build "${editor_project}"
        --configuration Release --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/dotnet-editor"
        --output "${editor_output}"
    DEPENDS "${editor_project}" ${editor_sources} ${bridge_files}
    VERBATIM)
add_custom_target(deren_managed_editor ALL DEPENDS "${editor_assembly}")
add_dependencies(deren_managed_editor deren_managed_bridge)
# The RmlUi document and its font are packaged beside Game.dll; the scene TOMLs stay in source.
add_custom_target(deren_editor_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${editor_output}/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${editor_source_dir}/ui/editor.rml" "${editor_output}/ui/editor.rml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo/LatoLatin-Regular.ttf" "${editor_output}/ui/LatoLatin-Regular.ttf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo/FONT-LICENSE.txt" "${editor_output}/ui/FONT-LICENSE.txt"
    VERBATIM)
add_dependencies(deren_managed_editor deren_editor_assets)

if(NOT VR_BUILD_TESTS)
    return()
endif()

set(game_project "${CMAKE_CURRENT_SOURCE_DIR}/test_game/Game.csproj")
set(game_output "${CMAKE_CURRENT_BINARY_DIR}/test_game")
set(game_assembly "${game_output}/Game.dll")
# The BUILD tree holds the generated manifest only: the demo's scene, lighting profile and character
# rig stay in source/scripting/test_game and are read from there (main_scene is absolute, and the
# profile/rig resolve next to it). Nothing authored is ever copied into or overwritten in the build
# tree - editing the source TOMLs is the single source of truth.
set(DEREN_DEMO_SCENE "${CMAKE_CURRENT_SOURCE_DIR}/test_game/main.toml")
set(DEREN_DEMO_ASSEMBLY "${game_assembly}")
configure_file(test_game/deren.toml.in "${game_output}/deren.toml" @ONLY)
add_custom_command(OUTPUT "${game_assembly}"
    COMMAND "${DOTNET_EXECUTABLE}" build "${game_project}"
        --configuration Release --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/dotnet"
        --output "${game_output}"
    DEPENDS "${game_project}" test_game/game_host.cs test_game/demo_debug_windows.cs test_game/rotate_behaviour.cs
        test_game/helmet_controls.cs test_game/audio_selftest.cs test_game/demo_audio_panel.cs test_game/rml_selftest.cs
        test_game/toon_selftest.cs test_game/node_name_selftest.cs test_game/aspect_selftest.cs
        test_game/rml_layout_selftest.cs ${bridge_files}
    VERBATIM)
# Package the managed UI assets beside Game.dll; authored scene TOMLs stay in the source tree.
add_custom_target(deren_managed_ui_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${game_output}/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/test_game/ui/audio.rml" "${game_output}/ui/audio.rml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo/LatoLatin-Regular.ttf" "${game_output}/ui/LatoLatin-Regular.ttf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo/FONT-LICENSE.txt" "${game_output}/ui/FONT-LICENSE.txt"
    VERBATIM)
add_custom_target(deren_managed_test_game DEPENDS "${game_assembly}")
add_dependencies(deren_managed_test_game deren_managed_ui_assets)
add_dependencies(deren_managed_test_game deren_managed_bridge)

enable_testing()
add_executable(test_managed_host ../tests/test_managed_host.cpp)
target_link_libraries(test_managed_host PRIVATE deren_managed_host deren_scripting_audio rmlui_support)
target_compile_definitions(test_managed_host PRIVATE VR_AUDIO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/audio"
    VR_RMLUI_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo")
add_dependencies(test_managed_host deren_managed_test_game)
set(managed_host_tests "")
foreach(scenario IN ITEMS round_trip missing_assembly missing_game_assembly missing_runtime abi_mismatch abi_layout_mismatch scene_module_unavailable scene_module_invalid scene_lifecycle scene_input frame_events toon_rig node_name scene_aspect audio_lifecycle audio_module_unavailable audio_module_invalid audio_module_incomplete rml_lifecycle rml_module_unavailable rml_module_invalid rml_module_incomplete rml_layout rml_module_optional_absent)
    add_test(NAME managed_host_${scenario} COMMAND test_managed_host "${bridge_output}" "${scenario}" "${game_assembly}")
    list(APPEND managed_host_tests managed_host_${scenario})
endforeach()

# THE TEST IMAGE IS NOT WHERE THE OTHER IMAGES ARE. This executable lands in the subdirectory, while
# shared_utility.dll is produced in the build root and libc++.dll belongs to the toolchain's runtime
# directory; the loader only searches the executable's directory and PATH, which used to make every
# `ctest -R managed_host_` exit 0xc0000135 (STATUS_DLL_NOT_FOUND) on a correct build. The PATH
# entries below fix the lookup instead of copying duplicates next to the test.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-runtime-dir
    OUTPUT_VARIABLE deren_clang_runtime_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT deren_clang_runtime_dir)
    set(deren_clang_runtime_dir "$<TARGET_FILE_DIR:shared_utility>")
endif()
set_tests_properties(${managed_host_tests} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:shared_utility>;PATH=path_list_prepend:${deren_clang_runtime_dir}")
