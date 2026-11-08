cmake_minimum_required(VERSION 4.3)

project(deren VERSION 0.5.0)

# Generate compile_commands.json for clangd and the VS Code C/C++ linter. This is
# build metadata only - it does not change any compiled artifact.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Generate compile_commands.json for clangd and VS Code.")

# Unsanitized DLLs import the same libc++ BMIs as sanitizer-enabled executables.
# Their templates must not retain optional container-overflow runtime calls.
# Compiler ASan/UBSan instrumentation remains enabled in the sanitized targets.
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_definitions(__SANITIZER_DISABLE_CONTAINER_OVERFLOW__)
endif()


set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
# NOTE: do NOT add "-std=c++26" to CMAKE_CXX_FLAGS here - CMAKE_CXX_STANDARD already injects it,
# and doing both duplicates the flag on every TU (same for -O3 / -g, which the default per-config
# flags already provide - see COMPILE_OPTIONS below).
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)


# =============================================================================================
# COMPILER DIALECT - the one place a GNU/Clang spelling and an MSVC spelling are told apart.
#
# Everything downstream consumes these flag sets by NAME (COMPILE_OPTIONS, VR_*), so no target grows a
# compiler test of its own: a dialect question is answered here, once. Applied to every first-party C++
# target via a single foreach (see "compile options" further down). Per-config additions ride on the
# DEFAULT per-config flags (Release already adds -O3 -DNDEBUG, Debug -g), so only the extras the
# defaults do not cover are listed - duplicating them here AND in the defaults would put -O3 / -g on
# the command line twice.
#
# TWO PROPERTIES ARE NON-NEGOTIABLE:
#   * the GNU/Clang branch reproduces, value for value, the flag list this project has always built
#     with. The clang64 build is a verified artifact (deren-ab/PROGRESS.md records its hashes), so the
#     test for touching this block is `ninja -t commands` coming out byte-identical; and
#   * the MSVC branch is a MIRROR, not a translation. Where MSVC has no equivalent the flag is DROPPED
#     and the capability is LOST, and that loss is listed in deren-ab/msvc/msvc_dialect_result.md rather
#     than imitated with something that means a different thing.
#
# What the MSVC branch cannot express (all measured, MSVC 19.44.35229 / VS 17.14):
#   -fno-exceptions, -fno-rtti          nothing. /EHs-c- is not a supported spelling of "no exceptions"
#                                       beside the STL, so MSVC gets /EHsc and default /GR.
#   -flto                               nothing. The pair is /GL + /LTCG; it changes link behaviour and
#                                       has not been measured on this project, so it is left out.
#   -march=native                       nothing. /arch:AVX2 is a different, coarser contract, so
#                                       VR_RELEASE_MARCH is ignored rather than approximated.
#   -fno-unwind-tables,                 nothing, dropped (code size / debug info only).
#   -fno-asynchronous-unwind-tables,
#   -fmerge-all-constants
#   -fno-sanitize=float-cast-overflow   nothing. MSVC has no UBSan, so the app_config suppression below
#                                       disappears - its subject cannot fire there.
#   -Wl,--gc-sections, -s              /OPT:REF /OPT:ICF is the real --gc-sections equivalent; the
#                                       symbol strip has no counterpart. (The clang64 exe no longer
#                                       links -static - see the link-options block below - so it is
#                                       not the single self-contained file it used to be either: both
#                                       toolchains now ship against a shared C++ runtime.)
# =============================================================================================

# Tuning target for Release. -march=native is right for a build on your own machine and wrong for an
# artifact someone else downloads: a binary tuned for the building CPU can die with SIGILL on an older
# one, which is the opposite of what the single-file Release build is for. CI publishes the executable
# (see .github/workflows/ci.yml), so it configures with -DVR_NATIVE_ARCH=OFF to get a baseline.
# (GNU/Clang only - see the dialect table above.)
option(VR_NATIVE_ARCH "Tune Release builds for the building CPU (-march=native)" ON)
option(VR_DIAGNOSTICS "Keep optimized stack traces and debug information across executable and plugins" ON)
set(VR_RELEASE_MARCH "$<IF:$<BOOL:${VR_NATIVE_ARCH}>,-march=native,-march=x86-64-v3>")

if(MSVC)
    set(COMPILE_OPTIONS
            $<$<CONFIG:Release>:
            /Gy # function-level linking: the mirror of -ffunction-sections
            /Gw # data-level linking: the mirror of -fdata-sections
            >
            /W4
            /WX
            /permissive-
            # /Zc:__cplusplus is not what vstd reads (it takes the language level from _MSVC_LANG, which
            # reports 202400 even without this flag - see source/vstd/vstd_compat.inc), but a correct
            # __cplusplus is what every OTHER library and header in the tree expects, so it is on.
            /Zc:__cplusplus
            /utf-8
            /bigobj
            /EHsc
            # Vendored and system headers are compiled into our TUs, so they are treated as external
            # rather than silenced per directory: this is the MSVC spelling of the -isystem policy the
            # vendored targets get treated with above.
            /external:anglebrackets
            /external:W0
            $<$<BOOL:${VR_DIAGNOSTICS}>:/Zi>
    )
else()
    set(COMPILE_OPTIONS
            $<$<CONFIG:Release>:
            -fno-rtti
            -ffunction-sections
            -fdata-sections
            $<$<NOT:$<BOOL:${VR_DIAGNOSTICS}>>:-flto>
            ${VR_RELEASE_MARCH}
            $<$<NOT:$<BOOL:${VR_DIAGNOSTICS}>>:-fno-unwind-tables;-fno-asynchronous-unwind-tables>
            -fmerge-all-constants
            >
            $<$<CONFIG:Debug>:
            -fno-omit-frame-pointer
            >
            -fno-exceptions
            -Wall
            -Wextra
            -Werror
            -Wno-reserved-module-identifier
            # One spelling per flag: Debug already gets -g from CMake and its own frame pointer above,
            # Release already gets -O3 from CMake. Diagnostics only ADD what those defaults do not cover.
            $<$<AND:$<BOOL:${VR_DIAGNOSTICS}>,$<CONFIG:Release>>:-g>
            $<$<AND:$<BOOL:${VR_DIAGNOSTICS}>,$<NOT:$<CONFIG:Debug>>>:-fno-omit-frame-pointer>
            $<$<BOOL:${VR_DIAGNOSTICS}>:-fno-optimize-sibling-calls;-funwind-tables;-fasynchronous-unwind-tables>
    )
endif()

get_filename_component(DEREN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

include(source/third_party/mimalloc.cmake)
include(source/third_party/fastgltf.cmake)
include(source/third_party/meshoptimizer.cmake)
include(source/third_party/yyjson.cmake)
include(source/third_party/stb.cmake)
include(source/third_party/glm.cmake)
include(source/third_party/glfw.cmake)
include(source/third_party/imgui.cmake)
include(source/third_party/xxhash.cmake)
include(source/third_party/boost.cmake)
include(source/vstd/vstd.cmake)
include(source/utility/utility.cmake)
include(source/promise/rhi/rhi.cmake)

include(source/configuration/configuration.cmake)
include(source/asset_library/asset_library.cmake)


# The portable engine owns scene storage, animation, render passes and the runtime.
# Graphics implementations live in source/backends/vulkan and are loaded by the
# engine through the RHI/GUI contracts, without linking their import libraries.
# Keep portable layout definitions in a separate shared target used by both halves.
# Native structure builders belong to the backend; renderer layout is a portable shared module.
include(source/backends/vulkan/vulkan.cmake)


# ================================================================================================
# THE RENDERER RUNTIME, AND WHY THERE IS EXACTLY ONE NOW (the dynamic-backend migration's S5, the flip).
#
# `source/engine/runtime/` at the repository root IS the renderer runtime. It reaches the backend ONLY through the
# contract (`deren_make_api_core()` + `rhi::api_core` + the raw-handle escape) and never names `core`.
# `source/engine/runtime/` - the runtime that DID name it and that lived inside the backend's own directory -
# is deleted with this batch. See docs/dynamic_runtime.md for the design.
#
# THE `-DVR_RUNTIME=legacy|dynamic` OPTION IS RETIRED WITH THOSE FILES, and that is the honest shape
# rather than a tidy-up: the option existed because two BMIs of one module name cannot both be visible
# to one translation unit, so a single build tree could not host both runtimes, and the LEGACY tree was
# the CONTROL for the dynamic one (both had to read the same fourteen render hashes). With the legacy
# sources gone there is nothing to select between and nothing to control, so the file list below is
# simply what `deren_engine` gets. `VR_RUNTIME_SERVES_THE_APP` retires in the same move: it gated the
# application, `chores` and `render_start_demo` while the port was incomplete (S1-S3) and has been
# unconditionally ON since S4's file-list swap.
#
# THE LEGACY BUILD TREE (`build-release-clang64`) IS RETIRED AS A CONFIGURATION, and its cache still
# names the old option: re-configuring that directory now builds exactly what the surviving tree
# (`build`) builds, because there is no switch to leave stale. Nothing in the
# repository points at it any more - the gates run in the surviving tree.
# ================================================================================================
set(VR_RUNTIME_MODULES
    source/engine/runtime/runtime.cppm
    source/engine/runtime/runtime.declarations.cppm
    source/engine/runtime/runtime.constructor.cppm
    source/engine/runtime/runtime.frames.cppm
    source/engine/runtime/runtime.probes.cppm
    source/engine/runtime/runtime.readback.cppm
)
set(VR_RUNTIME_PRIVATE_SOURCES
    source/engine/runtime/runtime.cpp
)
# The two places the app owns, named once so the target lists below read the same everywhere.
set(VR_FULL_API_TARGETS deren app_support)
set(VR_APP_EXE deren)

include(source/engine/resource_cluster/resource_cluster.cmake)
include(source/engine/buffer_transfer/buffer_transfer.cmake)
include(source/asset/subtilis/subtilis.cmake)


include(source/assets/assets.cmake)
include(source/engine/engine.cmake)


include(source/app/app.cmake)


# The C++ RUNTIME IS DYNAMIC, ON PURPOSE (Windows/GNU-Clang): the exe used to link with -static,
# which pinned libc++.a / libc++abi.a / libunwind.a INTO the executable. A first-party DLL cannot
# do that and still be a DLL: it takes the toolchain's default and imports libc++.dll. That left
# the process holding TWO C++ runtimes - the exe's private copy and the shared libc++.dll - which
# is the one combination that is wrong in both directions: no sharing where sharing is desirable
# (exceptions, RTTI, locale/iostream globals, thread-locals, out-of-line std::string code, weak
# symbol de-duplication) and no isolation where isolation is needed. The exe therefore takes the
# same default as the DLL: one libc++.dll, one C++ runtime for the whole program.
#
# What stays static, deliberately: glfw (named as an archive by the INTERFACE target above, so
# dropping -static did not turn the exe into a glfw3.dll consumer) and the vendored C libraries
# (mimalloc, xxhash). Their static archives mean the process still has one operator new/delete
# per image - the allocator is NOT unified by this, only the runtime around it is (see
# deren-ab/rhi/dll_compat_entry_plan.md, "the C++ runtime and the allocator are two questions").
#
# WHAT THIS COSTS: the shipped exe now needs libc++.dll beside it or on PATH - the single-file
# Release build is gone. Measured on MSYS2 clang64: libc++.dll 1659392 B, and it is the ONLY
# added file (libc++.dll's own imports are KERNEL32 plus the UCRT api-ms-win-crt-* set, and
# neither image imports libunwind.dll, so that 63488 B copy in the toolchain is not needed).
# The exe itself got smaller: 3719168 -> 3139072 B.
#
# Debug stays dynamic for faster dev iteration; POSIX builds stay dynamic (system distro
# packages are shared objects; static glibc would need extra dev packages).
#
# THE MSVC SPELLING of this is only the sections half: /OPT:REF /OPT:ICF is a real replacement for
# -Wl,--gc-sections, while the static CRT and the strip below have no counterpart - an MSVC Release exe
# keeps the dynamic CRT and its symbols (see the dialect table at the top of this file).
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
            # drop unused function/data sections at link (first-party code is compiled with /Gy /Gw;
            # see the vendored-lib section below)
            $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>
            # WIN32_EXECUTABLE (set above, toolchain-independent) is the same intent on both
            # toolchains, but the CRT entry that subsystem implies is NOT: MinGW-w64's
            # WinMainCRTStartup falls back to main(), while MSVC's - MSVCRT.lib(exe_winmain.obj) -
            # demands a WinMain this project does not have. Measured on the MSVC dialect (round 11,
            # build-dialect11): "MSVCRT.lib(exe_winmain.obj) : error LNK2019: unresolved external
            # symbol WinMain ... referenced in function __scrt_common_main_seh" + "deren.exe :
            # fatal error LNK1120: 1 unresolved externals". WinMainCRTStartup also calls the CRT
            # initialiser, so the MSVC spelling of the MinGW behaviour is to name the other CRT
            # entry - mainCRTStartup calls main() and keeps /SUBSYSTEM:WINDOWS, so Release still
            # runs with no console window. Debug's default entry already is mainCRTStartup, which is
            # why this is Release-only. (The exe logs to deren-debug.log, so no console is needed to see
            # its output - README.md, "Release builds are Windows GUI-subsystem executables".)
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
else()
    target_link_options(${PROJECT_NAME} PRIVATE
            # drop unused function/data sections at link (first-party code is compiled with
            # -ffunction-sections -fdata-sections; see the vendored-lib gc test below)
            $<$<CONFIG:Release>:-Wl,--gc-sections>
    )

    # Strip only lean Release builds. Diagnostic builds retain the exact executable
    # and debug data for online/offline symbolization.
    target_link_options(${PROJECT_NAME} PRIVATE
            $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VR_DIAGNOSTICS}>>>:-s>
    )
endif()

# ---------------------------------------------------------------------------------------------
# Headless unit tests (no GPU, no external test framework - see source/tests/vk_test.h): each test is
# an ordinary module consumer that imports the module under test and links its library, so the
# tests compile with the exact interfaces the rest of the code uses. CTest runs them in a
# per-build scratch directory (module logging writes deren-debug.log there, not into the source
# tree); tests that open fixtures/models receive the source root via VR_TEST_SOURCE_DIR.
# ---------------------------------------------------------------------------------------------
option(VR_BUILD_TESTS "Build the headless unit tests" ON)


include(source/engine/render_graph/render_graph.cmake)


include(source/tests/tests.cmake)


# ---- the runtime's scaffold test (the flip's step 2) -------------------------------------------
# IT EXISTS SO THE PORT CANNOT ROT: the runtime was ported partition by partition, and this executable -
# built and RUN in every slice of that work - is what proves the construction path
# (`deren_make_api_core()` -> `shared_ptr<api_core>` with the backend's deleter -> the face -> the raw
# handle escape -> teardown) still holds. Since S5 it is built in the ONE configuration there is.
#
# SPIKE-SHAPED ON PURPOSE, like `test_backend_boundary_spike` above and for the same reasons: it creates a
# real device (so it cannot be in the headless ctest set, and source/tests/test_docs.cpp compares that list
# against the workflow) and it is NOT in VR_TEST_TARGETS.
add_executable(test_runtime_dyn source/tests/test_runtime_dyn.cpp)
target_link_libraries(test_runtime_dyn
        backend_loader   # the scaffold calls load_api_core() explicitly (batch ⑥)
        deren_engine
        promise
        static_utility
        vstd_lib
)
# Same build-order rule as the app: this test CONSTRUCTS a runtime, so the DLL it resolves at run time
# has to exist first (it does not link it).
add_dependencies(test_runtime_dyn deren_vulkan)

# Device-only texture-group and submission-lifetime probe, outside headless CTest.
add_executable(test_texture_group source/tests/test_texture_group.cpp)
target_link_libraries(test_texture_group backend_loader promise static_utility vstd_lib vulkan_main_context Vulkan::Vulkan)
add_dependencies(test_texture_group deren_vulkan)

# Device-only completion token probe; deliberately outside headless CTest.
add_executable(test_resource_group source/tests/test_resource_group.cpp)
target_link_libraries(test_resource_group backend_loader promise static_utility vstd_lib vulkan_core_support vulkan_main_context Vulkan::Vulkan)
add_dependencies(test_resource_group deren_vulkan)

add_executable(test_submission_completion source/tests/test_submission_completion.cpp)
target_link_libraries(test_submission_completion backend_loader promise static_utility vstd_lib vulkan_main_context Vulkan::Vulkan)
add_dependencies(test_submission_completion deren_vulkan)

# ---- THE ACCELERATION-STRUCTURE DEVICE PROBE (plan S1's P0) ------------------------------------------
#
# The default mode runs CPU ownership/allocation checks and belongs to VR_TEST_TARGETS.
# --with-device additionally exercises the three-vertex Vulkan instrument. Keep that executable
# available when headless tests are disabled, without registering or creating the target twice.
if(NOT TARGET test_acceleration_structures)
    add_executable(test_acceleration_structures source/tests/test_acceleration_structures.cpp)
endif()
# This GPU probe calls native fence/queue functions itself.
target_link_libraries(test_acceleration_structures Vulkan::Vulkan)
target_link_libraries(test_acceleration_structures
        backend_loader   # load_api_core() explicitly, exactly as the scaffold test does
        deren_engine
        promise
        static_utility
        vstd_lib
        vulkan_main_context # abi 40: the shared Vulkan layout the main context is cast through
)
# Only --with-device resolves deren_vulkan.dll; CPU tests must not build an unsanitized DLL
# from sanitizer-instrumented archives. Full Release/Debug builds still produce the backend.
# For a targeted device run, build both: --target test_acceleration_structures deren_vulkan.

include(source/backends/vulkan/gui/gui.cmake)



# ================================================================================================
# COMPILE OPTIONS: EVERY FIRST-PARTY TARGET, BY ENUMERATION RATHER THAN BY A HAND-KEPT LIST.
#
# WHAT THIS REPLACES, AND WHY IT IS A FIX RATHER THAN A TIDY-UP: the shared set used to be attached to
# a target list spelled out here plus SEVEN explicit `target_compile_options(... ${COMPILE_OPTIONS})`
# calls at the targets that were not on it (probe_backend, probe_backend_static,
# test_backend_boundary_spike, test_runtime_dyn, test_acceleration_structures, deren_gui_vulkan,
# test_gui_plugin). Two mechanisms for one rule means a target added to this file and to NEITHER list
# builds without -Werror, without -fno-exceptions and without the Release flags, and nothing says so.
# The measured cost of that shape here: renaming one target (gltf_loader -> deren_assets) needed an
# edit in two separate lists, and missing either would have been silent. This is also the list the
# MSVC branch's dialect table is written against, so a silent omission would have meant an MSVC-only
# difference nobody could see.
#
# THE RULE IS NOW INVERTED - COVERED UNLESS EXCLUDED. `BUILDSYSTEM_TARGETS` is the directory's own
# list of targets, read here after every target in this file exists, so a new first-party target is
# covered the moment it is added. The ONLY opt-out is VR_VENDORED_TARGETS, and it is policy rather
# than oversight: imgui / fastgltf+simdjson / xxhash / mimalloc are upstream code whose warnings this
# project does not own, which is why they get their own Release-only set below instead of the
# project's -Werror set (see their targets above). Targets that compile nothing of their own are
# skipped by TYPE - INTERFACE libraries (the glm/glfw/stb headers) and custom targets (shaders,
# clang-format, check-backend-boundary).
#
# THE PER-TARGET EXCEPTIONS STAY EXPLICIT further down, each with its own measured reason: vstd_lib's
# /wd4996 (MSVC re-export of deprecated STL names), deren_assets' -Wno-unused-parameter (vendored
# fastgltf templates under -fno-exceptions), app_config's -fno-sanitize=float-cast-overflow (toml++'s
# own UB), and -fno-sanitize=all on the two images that cannot be instrumented (deren_vulkan,
# probe_backend). Every one of them is a flag this project-wide set must NOT carry.
#
# VERIFIED FLAG-NEUTRAL: `ninja -t commands` before and after the refactor differs on exactly one
# edge - probe_backend's compile line, whose flags are the same SET in a different ORDER, because its
# -fno-sanitize=all used to be appended before this block ran. The two flag families are independent,
# so the meaning is unchanged, and the build recompiled exactly that one object and relinked its image
# before reporting `ninja: no work to do`.
# ================================================================================================
set(VR_VENDORED_TARGETS imgui fastgltf_vendored xxhash mimalloc_vendored meshoptimizer_vendored yyjson_vendored)
get_property(_deren_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_deren_target IN LISTS _deren_targets)
    if(_deren_target IN_LIST VR_VENDORED_TARGETS)
        continue()
    endif()
    get_target_property(_deren_type ${_deren_target} TYPE)
    if(NOT _deren_type OR _deren_type STREQUAL "INTERFACE_LIBRARY" OR _deren_type STREQUAL "UTILITY")
        continue()
    endif()
    target_compile_options(${_deren_target} PRIVATE ${COMPILE_OPTIONS})
    if(MSVC AND VR_DIAGNOSTICS AND (_deren_type STREQUAL "EXECUTABLE" OR _deren_type STREQUAL "SHARED_LIBRARY"))
        target_link_options(${_deren_target} PRIVATE /DEBUG)
    endif()
endforeach()
unset(_deren_targets)
unset(_deren_target)
unset(_deren_type)
# One warning is suppressed for vstd_lib under MSVC, and the reason is the module's job description:
# vstd re-exports the standard library's surface, which necessarily includes the names C++17/20/23
# deprecated (std::iterator, std::wbuffer_convert/wstring_convert, std::rel_ops, std::is_pod,
# std::aligned_storage/aligned_union, std::float_denorm_style). MSVC's STL announces each of those
# through C4996, so with the MSVC branch's /W4 /WX the module cannot compile its own re-exports:
# measured 11 C4996 in source/vstd/std/{iterator(245),limits(12),locale(73,76),type_traits(338,340,342),
# utility(94-97)}.inc, the first of which becomes `error C2220: warnings treated as errors` and stops
# the build. Suppressed for vstd_lib ONLY - every other target keeps the full /W4 /WX set, so this
# costs the project's own code nothing. The alternative MSVC offers (_SILENCE_ALL_CXX2x_*_WARNINGS
# defines) is STL-specific and would also stop this layer from noticing a real future deprecation.
if(MSVC)
    target_compile_options(vstd_lib PRIVATE /wd4996)
endif()
# vendored fastgltf headers are upstream code: relax one warning their templates trip under
# -fno-exceptions (raise() leaves its arguments unused on the abort() path). The MSVC spelling of that
# warning is /wd4100, and it is only needed there because the MSVC branch has no -fno-exceptions - the
# unused-parameter chatter can also come from /W4 itself.
if(MSVC)
    target_compile_options(deren_assets PRIVATE /wd4100)
else()
    target_compile_options(deren_assets PRIVATE -Wno-unused-parameter)
endif()

# toml++ is a SYSTEM header-only package, so its code is compiled into OUR translation unit and a
# sanitizer sees it as ours. app_config is the only TU that includes it (it is the only toml++ user
# in the project), which is what makes a per-target suppression the honest scope rather than a
# project-wide one.
#
# The finding it silences: toml++'s formatter computes a node's printed width with
# `static_cast<size_t>(log10(val))`, and for a float in (0, 1) - e.g. the DEFAULT
# `clear_color = [0.02, 0.02, 0.03]` - log10 is negative, so the cast is UB
# ("-1.69897 is outside the range of representable values of type 'unsigned long long'",
# toml++/impl/toml_formatter.inl:94). It is reachable from configuration.cpp:29 alone, which dumps every
# key of the loaded file into the log. Upstream's bug, not this project's - and it fires from a
# default config value, so it is not a corner case.
#
# Only float-cast-overflow is dropped: every other UBSan check, and all of ASan, stay active for this
# target, so a real float->int bug elsewhere here is still reported. A __attribute__((no_sanitize))
# on the dump function does NOT work for this (measured): UBSan attributes the diagnostic to the
# template instantiation, not to the function it was instantiated from.
#
# MSVC GETS NOTHING HERE, and needs nothing: there is no UBSan to suppress. Suppressing a flag MSVC
# would reject (D8021, "invalid numerical argument") is exactly the failure this dialect layer exists
# to remove, so the suppression is clang-only by construction rather than translated.
if(NOT MSVC)
    target_compile_options(app_config PRIVATE -fno-sanitize=float-cast-overflow)
endif()

# CMake 4.4 synthesizes BMIs for each distinct target option list. These warning
# and instrumentation exceptions apply to source compilation, not the public
# module interface. Keep module consumers on one canonical target option list;
# otherwise a diamond through promise and utility names two BMIs for one module.
block()
    get_property(_vessel_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_vessel_target IN LISTS _vessel_targets)
        get_target_property(_vessel_options ${_vessel_target} COMPILE_OPTIONS)
        foreach(_vessel_option IN ITEMS -fno-sanitize=all -fno-sanitize=float-cast-overflow -Wno-unused-parameter /wd4100 /wd4996)
            if(_vessel_options AND _vessel_option IN_LIST _vessel_options)
                list(REMOVE_ITEM _vessel_options "${_vessel_option}")
                get_target_property(_vessel_sources ${_vessel_target} SOURCES)
                get_target_property(_vessel_module_sets ${_vessel_target} CXX_MODULE_SETS)
                if(_vessel_module_sets)
                    foreach(_vessel_module_set IN LISTS _vessel_module_sets)
                        get_target_property(_vessel_modules ${_vessel_target} CXX_MODULE_SET_${_vessel_module_set})
                        list(APPEND _vessel_sources ${_vessel_modules})
                    endforeach()
                endif()
                list(REMOVE_DUPLICATES _vessel_sources)
                foreach(_vessel_source IN LISTS _vessel_sources)
                    set_property(SOURCE "${_vessel_source}" APPEND PROPERTY COMPILE_OPTIONS
                        "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${_vessel_target}>:${_vessel_option}>")
                endforeach()
            endif()
        endforeach()
        if(_vessel_options)
            set_property(TARGET ${_vessel_target} PROPERTY COMPILE_OPTIONS "${_vessel_options}")
        endif()
    endforeach()
    unset(_vessel_targets)
    unset(_vessel_target)
    unset(_vessel_options)
    unset(_vessel_option)
    unset(_vessel_sources)
    unset(_vessel_source)
    unset(_vessel_module_sets)
    unset(_vessel_module_set)
    unset(_vessel_modules)
endblock()

# Vendored libs get function/data sections AND -flto in Release too (plain C/C++ flags, no
# -Werror set involved): without sections --gc-sections can only drop whole objects, so unused
# imgui / fastgltf / xxhash / mimalloc functions would survive into the exe; without -flto the
# linker cannot see into their translation units, so cross-TU dead code stays. Both cost build
# time only - runtime speed is unaffected (LTO is at worst neutral).
# Under MSVC the sections half is /Gy /Gw and the LTO half does not exist (see the dialect table).
#
# THE TARGET LIST IS `VR_VENDORED_TARGETS`, the same variable the project-wide sweep reads: the
# excluded set and the specially-treated set are one list, so a vendored library can never be given
# the -Werror set by accident and a first-party one can never be quietly left out of both.
if(MSVC)
    set(VR_VENDORED_RELEASE_OPTIONS $<$<CONFIG:Release>:/Gy;/Gw>)
else()
    set(VR_VENDORED_RELEASE_OPTIONS $<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VR_DIAGNOSTICS}>>>:-flto>
        $<$<BOOL:${VR_DIAGNOSTICS}>:-g;-fno-omit-frame-pointer;-fno-optimize-sibling-calls;-funwind-tables;-fasynchronous-unwind-tables>)
endif()

foreach(vendored_target IN LISTS VR_VENDORED_TARGETS)
    target_compile_options(${vendored_target} PRIVATE ${VR_VENDORED_RELEASE_OPTIONS})
endforeach()

target_compile_definitions(deren_engine PRIVATE
        $<$<CONFIG:Debug>:
        _DEBUG
        >
)

# S1-A (§11.2's fourth list): the backend half had this define while it was compiled by deren_engine,
# so the new target carries it too - the split is supposed to be invisible in the products.
target_compile_definitions(deren_vulkan PRIVATE
        $<$<CONFIG:Debug>:
        _DEBUG
        >
)

# -- version: single source of truth is project(VERSION) above. Injected into the C++ targets
#    that surface it: the Vulkan app_info (source/backends/vulkan/core/core.cpp, applicationVersion /
#    engineVersion), the app lifecycle module (--version flag and the startup log banner in
#    source/app/app_application.cpp), and deren_vulkan. Bump the number in project(VERSION) and keep
#    README/docs' "current version" lines in sync.
foreach(version_target IN ITEMS deren_engine app_support ${VR_APP_EXE} deren_vulkan)
    target_compile_definitions(${version_target} PRIVATE
            DEREN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            DEREN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            DEREN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    )
endforeach()

# ---------------------------------------------------------------------------------------------
# clang-format: run automatically before compiling. Only formats when a source is newer than
# the stamp (i.e. changed), avoiding reformatting on every build, which would touch source
# mtimes and trigger endless recompiles.
# ---------------------------------------------------------------------------------------------
find_program(CLANG_FORMAT_EXECUTABLE clang-format)

if(CLANG_FORMAT_EXECUTABLE)
    # source/promise/rhi/ holds the contract module (`*.cppm`) AND the one header that is deliberately not
    # part of it (`backend_entry.hpp`); source/utility/abi_export.hpp is the macro header. All three are
    # compiled by both sides of the boundary, so they are listed by extension rather than by the
    # *.cpp* wildcard the others use.
    file(GLOB_RECURSE PROJECT_FORMAT_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/source/app/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/source/app/app_*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/configuration/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/gltf_loader/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/utility/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/vulkan/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/runtime/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/vulkan/core/vma/texture_upload_layout.h
            ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/source/promise/rhi/*.cppm
            ${CMAKE_CURRENT_SOURCE_DIR}/source/promise/rhi/*.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/source/utility/abi_export.hpp)

    set(CLANG_FORMAT_STAMP "${CMAKE_CURRENT_BINARY_DIR}/.clang-format.stamp")

    add_custom_command(
            OUTPUT "${CLANG_FORMAT_STAMP}"
            COMMAND ${CLANG_FORMAT_EXECUTABLE} -i --style=file ${PROJECT_FORMAT_SOURCES}
            COMMAND ${CMAKE_COMMAND} -E touch "${CLANG_FORMAT_STAMP}"
            DEPENDS ${PROJECT_FORMAT_SOURCES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Formatting sources with clang-format"
            VERBATIM)

    add_custom_target(clang-format DEPENDS "${CLANG_FORMAT_STAMP}")

    # Format all project targets before compiling. The app support modules are part of the glob now:
    # the old root-level `chores` module needed a manual clang-format because its source fed both the
    # format stamp and a target that depended on other format-listed targets. Splitting it into
    # source/app/app_*.cpp* removed that cycle.
    foreach(_tgt shared_utility static_utility backend_loader promise rhi_sync_contract app_config deren_vulkan vulkan_constant_init vulkan_synchronization renderer_layout vulkan_error_tables deren_engine deren_assets resource_cluster_core render_graph_core render_graph_rhi render_graph_execute render_graph_compose render_graph_bloom render_graph_post render_graph_lighting render_graph_geometry app_support ${VR_APP_EXE})
        add_dependencies(${_tgt} clang-format)
    endforeach()

    # Check only, do not modify (useful for CI)
    add_custom_target(clang-format-check
            COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run -Werror --style=file ${PROJECT_FORMAT_SOURCES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Checking sources with clang-format (dry-run)"
            VERBATIM)
else()
    message(WARNING "clang-format not found, skipping auto-format before build")
endif()

# The platform halves of utility live in plain (non-module) TUs so that <windows.h> stays out of a
# module's global module fragment - including it there made libc++'s operator new ambiguous in every
# TU that imported `deren.utility`. One file per platform facility; the module side only sees their C
# entry points (see source/utility/platform_sleep.cpp, platform_path.cpp and platform_dialog.cpp). They are
# per-half (no state), so they live in static_utility; shared_utility has its own plain TU for the
# process-wide rotation claim (source/utility/shared/log_rotation_claim.cpp), kept there for the same reason.
target_sources(static_utility PRIVATE
        source/utility/platform_sleep.cpp
        source/utility/platform_path.cpp
        source/utility/platform_dialog.cpp
)
# ---- SLANG -> SPIR-V: the build owns the step (docs/slang_migration.md) ----
# Editing a .glsl without recompiling used to leave you running stale SPIR-V ("my shader change does
# nothing"), so the compiled binaries are no longer committed and are never hand-synced: every build
# regenerates them from the sources (and the shared includes they pull in). SLANGC IS REQUIRED rather
# than optional - a configuration without the compiler is exactly the one where a stale binary could
# be used without anyone noticing, which is the failure this removes.
#
# Output goes to the BUILD tree (<build>/shaders), not next to the .glsl:
#  - the source tree keeps exactly the files a human edits, so .spv is never something to commit,
#    gitignore by hand, or forget to regenerate;
#  - the runtime finds it without any search-order guesswork, because that directory is a sibling of
#    the executable and locate_shaders_dir() prefers a sibling of the running binary;
#  - -I still points at the SOURCE directory, so #include "common/surface.slang" resolves against the real
#    sources rather than against copies.
# A mirror/ POST_BUILD step was tried first and is worse: a POST_BUILD command only runs when the
# executable is RELINKED, so a shader-only edit recompiled the .spv and left the copy stale (measured).
# Note this assumes a single-configuration generator, which is what this project configures - the
# executable lands in the build directory, so build/shaders is its sibling. A multi-config generator
# would put the binary in a per-config subdirectory and the runtime would fall back to its cwd walk.
#
# source/shaders/compile_shaders.ps1 / .sh remain as a manual escape hatch for a machine without CMake; they
# compile into the repository shaders/ output directory used by the runtime fallback.
set(VR_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source/shaders")
set(VR_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
# The Vulkan SDK ships slangc in Bin/ and sets VULKAN_SDK when its environment script is sourced, but
# that directory is not necessarily on PATH - notably in an MSVC build, where the developer shell
# (Enter-VsDevShell) sets up the compiler and knows nothing about Slang. HINTS are searched BEFORE the
# system PATH, so the SDK's own slangc wins over a stale copy elsewhere, and an already-cached
# VR_SLANGC_EXECUTABLE is left alone (which is what keeps the clang64 build's command lines unchanged).
find_program(VR_SLANGC_EXECUTABLE NAMES slangc slangc.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "${Vulkan_ROOT}/Bin" "${Vulkan_DIR}/../Bin")
if(NOT VR_SLANGC_EXECUTABLE)
    message(FATAL_ERROR
        "slangc not found. It ships with a Slang release (the Vulkan SDK's Bin directory carries one too), "
        "and every shader in this project is built from a .slang source - the GLSL FALLBACK THIS RULE USED "
        "TO HAVE IS GONE, along with the GLSL stage sources themselves (source/reference/shaders/glsl.old/) - so a build "
        "without it would be the one that silently compiles modules the project no longer uses. Install "
        "Slang or the Vulkan SDK, or add slangc to PATH.")
endif()
# the shared includes: every generated shader lists them all as dependencies. The chain is three
# files and glslc takes milliseconds, so recompiling an unaffected shader is cheaper than the
# dependency bookkeeping that would avoid it.
set(VR_SHADER_INCLUDES
    "${VR_SHADER_SRC_DIR}/common/surface.slang"
    # material_record.slang is the Material LAYOUT, split out of surface.slang so a FULLSCREEN stage can name
    # the type it reads from the material table - a fullscreen stage cannot include surface.slang at all (its
    # shared scene push block is 144 bytes, over the 128-byte limit). It is here for the same reason the
    # other bodies are: a field added to it must rebuild every stage that reads the table.
    "${VR_SHADER_SRC_DIR}/common/material_record.slang"
    # rt_alpha_mask.slang is the MASK cut shared by the shadow and reflection any-hit stages. It is here so
    # editing the predicate rebuilds both ray-tracing pipelines; a stale stage would disagree with the raster
    # paths about which texels are solid.
    "${VR_SHADER_SRC_DIR}/common/rt_alpha_mask.slang"
    # toon_params.slang is the TOON FAMILY TABLE, shared by every stage that shades a character (the
    # character-forward stage and the screen-space rim). It is here for the reason the bodies below are:
    # editing a family's parameters must rebuild every stage that read them, and a stage left on stale SPIR-V
    # would shade with the previous table - which looks like a tuning that did not take rather than like a
    # build error.
    "${VR_SHADER_SRC_DIR}/scene/pbr.slang"
    "${VR_SHADER_SRC_DIR}/post/game_toon_tonemap.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon_math.h"
    "${VR_SHADER_SRC_DIR}/toon/toon_params.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/face_diffuse.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/face_frame.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/face_rim.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/skin_material.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/game_toon_profile.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/game_toon_lights.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/encoding.h"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/outline.slang"
    # The AMD FidelityFX reflection denoiser port: the five MIT-licensed stage headers plus the wrapper
    # include that adapts deren's G-buffer/heap conventions to the AMD callbacks.
    "${VR_SHADER_SRC_DIR}/third_party/ffx_denoiser/ffx_denoiser_reflections_common.h"
    "${VR_SHADER_SRC_DIR}/third_party/ffx_denoiser/ffx_denoiser_reflections_config.h"
    "${VR_SHADER_SRC_DIR}/third_party/ffx_denoiser/ffx_denoiser_reflections_reproject.h"
    "${VR_SHADER_SRC_DIR}/third_party/ffx_denoiser/ffx_denoiser_reflections_prefilter.h"
    "${VR_SHADER_SRC_DIR}/third_party/ffx_denoiser/ffx_denoiser_reflections_resolve_temporal.h"
    "${VR_SHADER_SRC_DIR}/lighting/reflection_denoise_common.slang"
    "${VR_SHADER_SRC_DIR}/toon/reference_shape.h"
    "${VR_SHADER_SRC_DIR}/lighting/shading.slang"
    "${VR_SHADER_SRC_DIR}/lighting/sky.slang"
    # ibl_specular.slang is included by shading.slang, so it belongs here for the same reason the three above
    # do: editing it alone must not leave stale SPIR-V behind.
    "${VR_SHADER_SRC_DIR}/lighting/ibl_specular.slang"
    "${VR_SHADER_SRC_DIR}/post/pbr_neutral.slang"
    # heap_slot_constants.slang is the SLOT GRID's shader-side source of truth (render_layout.cppm is the
    # host's): a shader that includes it and is not rebuilt after it changes would index the heap by stale
    # numbers, which validation cannot see. The C++ side is compared against it by a test (see
    # test_render_resources). heap_slots.slang and heap_access.slang include the same constants.
    "${VR_SHADER_SRC_DIR}/common/heap_slot_constants.slang"
    "${VR_SHADER_SRC_DIR}/common/heap_slots.slang"
    # Visibility's shared fetch/projection helpers and M4's shared candidate rule are leaf includes; list them so
    # changes rebuild the raster and resolve SPIR-V instead of leaving a stale geometry path behind.
    "${VR_SHADER_SRC_DIR}/common/mesh_fetch.slang"
    "${VR_SHADER_SRC_DIR}/common/software_candidate.slang"
    "${VR_SHADER_SRC_DIR}/common/software_route.slang"
    "${VR_SHADER_SRC_DIR}/scene/software_visibility.slang" # atomic variant reuses its coverage helpers
    # heap_access.slang and mesh_geometry.slang are the two OTHER includes every leaf pulls in - the shim's heap
    # accessors, and the device-address geometry fetch a mesh stage uses instead of the input assembler. Both were
    # missing from this list, and the mesh one cost a measurement to find: the build tree's pbr.vert.spv and
    # shadow.frag.spv were STALE (built before the geometry lanes existed) while the mesh entries sharing their
    # source had been rebuilt, so the two halves of one pipeline declared different blocks - visible only as a
    # byte difference against the escape-hatch scripts' output, never as a wrong picture.
    # `test_shader_sources` now compares this list against every #include in source/shaders/, so the next include cannot
    # be forgotten the same way.
    "${VR_SHADER_SRC_DIR}/common/heap_access.slang"
    "${VR_SHADER_SRC_DIR}/common/texture_group.slang"
    # hzb.slang is THE HZB LAYOUT (M3 phase A2): the pyramid's level recurrence and the header's word layout,
    # mirrored by hand from deren.engine.visibility.hzb (which the host sizes the buffers with) exactly as
    # heap_slot_constants.slang mirrors the slot grid. Editing either side without rebuilding the build stage would
    # leave the two halves of the contract writing/reading different offsets - a wrong picture, not an error, which
    # is why it is a build dependency of every shader that includes it.
    "${VR_SHADER_SRC_DIR}/common/hzb.slang"
    "${VR_SHADER_SRC_DIR}/geometry/mesh_geometry.slang"
    # surface_body.slang and gbuffer_body.slang are the SHARED G-BUFFER BODY (visibility-buffer v0.4b): the material
    # decode and the five-target composition are included by the fragment G-buffer stage AND by the visibility
    # resolve compute stage, so editing either must rebuild BOTH .spv - a stale half is the two-pictures-drift this
    # extraction exists to prevent. surface.slang includes surface_body.slang (the fragment wrapper) and gbuffer.slang
    # includes gbuffer_body.slang; the test below requires every include to be listed here.
    "${VR_SHADER_SRC_DIR}/common/surface_body.slang"
    "${VR_SHADER_SRC_DIR}/scene/gbuffer_body.slang"
    "${VR_SHADER_SRC_DIR}/gaussian/gaussian_common.slang"
    "${VR_SHADER_SRC_DIR}/gaussian/gaussian_projection.slang"
    # character_forward.slang IS INCLUDED BY A LEAF NOW, which is why it is here: the outline fragment stage
    # includes it for `toon_diffuse` (the same reason `toon_params.slang` is above - a second copy of that chain
    # is the duplicate this repository refuses), so editing it must rebuild `outline.frag.spv` too.
    # `test_shader_sources` requires every `#include` found in source/shaders/ to be listed here, which is what would
    # have caught the omission: an unlisted include leaves STALE SPIR-V that nothing but a byte comparison sees.
    "${VR_SHADER_SRC_DIR}/toon/character_forward.slang"
    # goo_toon.slang IS INCLUDED BY THE REWRITTEN CHAIN'S OWN STAGE, and it is listed here for the SAME reason
    # character_forward.slang is one line above: `source/shaders/toon/goo_toon.slang` includes it for `toon_diffuse` (the old
    # body every non-iris family is still shaded by), so editing it must rebuild `goo_toon.frag.spv` as well -
    # otherwise the old chain's fix lands in one .spv and not the other, which is a difference between two stages
    # that must agree, visible only as a byte comparison. See `goo_toon_pipeline_name`'s note for why the two
    # stages share one body on purpose.
    # game_toon.slang IS THE GAME-SHADER MODEL'S STAGE and it is listed for the SAME reason
    # character_forward.slang is above: it includes the shared body (toon_diffuse and the surface
    # gather), so an edit to that body must rebuild game_toon.frag.spv as well. The stage is opt-in
    # and OFF by default ([render] game_toon), so its landing is behaviour-neutral.
    "${VR_SHADER_SRC_DIR}/toon/game_toon.slang"
    # the per-family game-shader ports included by game_toon.slang; each edit must rebuild game_toon.frag.spv
    "${VR_SHADER_SRC_DIR}/toon/game_toon/cloth.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/hair.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/face.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/skin.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/eye.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/cloth.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/hair.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/captured/skin_face.slang"
    "${VR_SHADER_SRC_DIR}/toon/game_toon/eye.slang"
    # goo_rim.slang is THE REWRITTEN CHAIN'S THIRD STAGE and it is listed for the reason the two lines above are:
    # it reads the same two shared tables those stages do (`toon_params.slang`'s family table and
    # `material_record.slang`'s layout), so an edit to either must rebuild `goo_rim.frag.spv` as well - a stage left
    # on stale SPIR-V would read a material record with different field offsets, which is a wrong texture or a
    # wrong family rather than a build error.
)
# (THE GLSL STAGE LIST USED TO LIVE HERE: twenty-four names, glslc's input, kept in step with the sources by
# hand. Every one of them is in source/reference/shaders/glsl.old/ now and built from Slang instead, so the list is gone -
# VR_SLANG_SOURCES below IS the list of what this build compiles.)
set(VR_SHADER_OUTPUTS "")
# ---- SLANG SOURCES: the GLSL -> Slang migration, one shader at a time (docs/slang_migration.md) --------
#
# Each entry is "<source>:<entry>:<stage>:<output .spv name>", and the OUTPUT NAME IS THE SAME .spv THE GLSL
# VERSION PRODUCED: the runtime loads shaders by file name, so a port needs no host change at all, and the
# acceptance for every port is the capture gate staying byte-identical.
#
# THE FLAGS ARE THE BINDING CONTRACT, and every one of them was measured rather than guessed:
#   -capability spvDescriptorHeapEXT ... without it Slang emits the descriptor-set model instead of the heap.
#   -spirv-resource-heap-stride 64 / -spirv-sampler-heap-stride 32 ... this renderer's grid is a flat 64-byte
#                                   slot grid plus a 32-byte sampler heap, not Slang's per-type default.
#   -fvk-use-gl-layout          ... std430, which is what the CPU structs and the GLSL blocks already use.
#   -matrix-layout-column-major ... see the note on `row_major` below; kept even though this build appears to
#                                   ignore it (measured: all four flag combinations emit the same decoration).
#
# A MATRIX MEMBER MUST BE DECLARED `row_major` TO GET SPIR-V `ColMajor`, which is what glslang emits for a GLSL
# `mat4` and therefore what matches the host's column-major glm::mat4. MEASURED, and the HLSL keyword reads
# backwards here: `column_major float4x4` emits RowMajor, `row_major float4x4` emits ColMajor.
set(VR_SLANG_SOURCES
    scene/unlit.slang:main:fragment:unlit.frag.spv
    # character_forward.slang is the toon character stage: the same leaves the scene pass drew, shaded a
    # second time OVER the deferred result. It is gated behind the `character_forward` feature, which is OFF
    # by default, so none of the fourteen capture scenarios records it until a scenario turns it on - which is
    # what makes its landing behaviour-neutral.
    toon/character_forward.slang:main:fragment:character_forward.frag.spv
    # goo_toon.slang is THE REWRITTEN TOON CHAIN's fragment stage: the same geometry, the same pass and the same
    # frame as the entry above, with the shading model replaced by the one XIYAG's Goo node presets describe
    # (see build-release-clang64/deren-ab/goo_toon_plan.md). ITS ENTRY IS `goo_toon_frag_main`, NOT `main`, for the
    # reason `outline.slang`'s is: it INCLUDES character_forward.slang, which brings that file's own
    # `[shader("fragment")] main` with it, and a plain `-entry main` then names an overload set that Slang refuses
    # to resolve (E38000). It is selected at run time by `[render] goo_toon` - OFF by default, so none of the ten
    # capture scenarios draws it, and its landing is behaviour-neutral until a run asks for it.
    # game_toon.slang is THE GAME-SHADER TOON MODEL's fragment stage. Same pass, same frame and same
    # state as the two entries above; only the fragment module differs, selected by `[render]
    # game_toon` (OFF by default). Its entry is `game_toon_frag_main`, not `main`, for the same reason
    # goo_toon's is: it includes character_forward.slang, which brings that file's own
    # `[shader("fragment")] main` with it, and a plain `-entry main` names an overload set Slang
    # refuses (E38000). PHASE 1 IS THE PLUMBING MILESTONE: the entry still calls the shared body, so
    # selecting it draws the old chain's pixels while config/pipeline/GUI are proven.
    toon/game_toon.slang:game_toon_frag_main:fragment:game_toon.frag.spv
    # toon_screen_rim.slang is the toon stage's SECOND rim: a fullscreen additive contour from the depth,
    # gated by the SAME `character_forward` feature, so no capture scenario records it either.
    toon/toon_screen_rim.slang:main:fragment:toon_screen_rim.frag.spv
    # game_toon_depth_rim.slang is THE GAME CHAIN's rim: the same fullscreen additive depth-contour shape as
    # toon_screen_rim.slang above, but family-independent and coloured by ITS OWN push block, because the game
    # chain's profile-on contour is the captured viewer's hair/face rim rather than the article's family term.
    # It is gated at the post-graph lane by `runtime::game_toon_active()`, so no capture scenario records it
    # unless one selects the game-shader model.
    toon/game_toon_depth_rim.slang:main:fragment:game_toon_depth_rim.frag.spv
    # goo_rim.slang is THE REWRITTEN CHAIN'S rim, and it is a stage of its own for the constraint its header
    # records: the reference's rim is a PRODUCT of three attenuations and a screen-space depth factor, and the
    # depth it samples is the character-forward stage's own ATTACHMENT - so the surface shader cannot compute it.
    # It is gated by `[render] goo_toon` (through a name the chain owner composes), so, like the two entries above,
    # no capture scenario records it until one turns the rewritten chain on.
    # overlay.slang is the article's TWO FRAMEBUFFER MULTIPLIES (an eye shadow and a hair shadow), drawn by the
    # `character_forward` stage's overlay group with the multiply blend and gated by that same feature - so, like
    # the two entries above, no capture scenario records it until one turns the toon stage on. It shares
    # pbr.mesh.spv as its geometry entry: an overlay is a normal mesh (see the file's header).
    toon/overlay.slang:main:fragment:overlay.frag.spv
    post/fxaa.slang:main:fragment:fxaa.frag.spv
    # upscale.slang is the RESOLVE: the render chain's display-referred LDR image onto the presented swapchain,
    # by a linear (bilinear) filter. It runs only below `[render] render_scale = 1.0` - at 1.0 the chain IS the
    # output and the pass's feature gate is false, which is why no capture scenario records it.
    post/upscale.slang:main:fragment:upscale.frag.spv
    post/post.slang:main:vertex:post.vert.spv
    gaussian/gaussian_raster.slang:vertex_main:vertex:gaussian_raster.vert.spv
    gaussian/gaussian_raster.slang:fragment_main:fragment:gaussian_raster.frag.spv
    gaussian/gaussian_prepare.slang:reset_main:compute:gaussian_reset.comp.spv
    gaussian/gaussian_prepare.slang:prepare_main:compute:gaussian_prepare.comp.spv
    gaussian/gaussian_prepare.slang:finalize_main:compute:gaussian_finalize.comp.spv
    # post.frag is the post chain's stage for ALL THREE modes (bright pass / downsample / composite), so every
    # scenario records it - the widest verification a single port can have.
    post/post.slang:frag_main:fragment:post.frag.spv
    debug/gbuffer_debug.slang:main:fragment:gbuffer_debug.frag.spv
    post/taa.slang:main:fragment:taa.frag.spv
    # gbuffer.slang IS the first stage that reads a shared body AND a heap UBO, and it cost two bugs, both
    # documented in docs/slang_migration.md section 9 because neither is visible in the SPIR-V's shape:
    # `mul(M, v)` on a `row_major float4x4` emits the TRANSPOSED product (the GLSL spelling `M * v` does
    # not), and a heap descriptor's TYPE has to match the storage class the shader reads it through - a
    # UNIFORM descriptor read through Slang's StorageBuffer pointer returns zeros with NO validation
    # finding. That is also why the camera and the light are written as STORAGE descriptors.
    scene/gbuffer.slang:main:fragment:gbuffer.frag.spv
    # pbr.slang is the FORWARD path's shader PAIR: the fragment stage is where the sun/shadow/punctual/
    # cluster/IBL fetches are proven under Slang, and its GEOMETRY entry is what the G-BUFFER pass loads for
    # every opaque draw - so it is verified by all fourteen scenarios, and by the transparent pass's
    # forward default too. THAT ENTRY IS THE MESH ONE SINCE STEP 4 (docs/mesh_shaders.md): `pbr.vert.spv`
    # was removed with the rest of the vertex geometry path, so there is no vertex entry to list here.
    scene/pbr.slang:main:fragment:pbr.frag.spv
    # ... the same file's MESH entry (docs/mesh_shaders.md step 2): the G-buffer/forward geometry as
    # dispatches, which is what the scene's opaque, forward and transparent leaves are drawn with.
    scene/pbr.slang:mesh_main:mesh:pbr.mesh.spv
    # ... and the MESHLET entry (docs/mesh_shaders.md step 3): one workgroup per meshlet, camera-culled.
    scene/pbr.slang:meshlet_main:mesh:pbr.meshlet.spv
    # ... and THE OUTLINE'S MESH ENTRY: `mesh_main`'s triangle loop with the author's normal expansion applied
    # to the clip position before the vertex is written (`build-release-clang64/deren-ab/outline_plan.md` ①). It is
    # the SAME FILE because the hull has to be skinned, morphed and placed by the same body as the surface it
    # outlines - a second copy of that body is how an outline detaches from a deforming character.
    toon/game_toon/captured/outline.slang:game_toon_outline_mesh_main:mesh:outline.mesh.spv
    # visibility.slang is THE VISIBILITY BUFFER's two stages (visibility-buffer v0.3): the mesh entry
    # rasterizes triangles and carries ONE FLAT PACKED ID per triangle, the fragment writes it into the
    # R32_UINT visibility target. The raster pass builds its pipeline from them, but the lane is CAPTURE-ONLY
    # until the resolve consumed its outputs (v0.4): the frame's picture still comes from the old geometry lane,
    # which is why the frames cannot move. The test that compares the four places a .spv is named covers them.
    scene/visibility.slang:mesh_main:mesh:visibility.mesh.spv
    scene/visibility.slang:main:fragment:visibility.frag.spv
    # visibility_resolve.slang -> visibility_resolve.comp.spv is THE VISIBILITY BUFFER'S RESOLVE (visibility-buffer
    # v0.4b): the compute stage that reads the id + capture depth + item/meshlet tables and writes the five G-buffer
    # targets through the shared body. It has no pipeline yet (v0.4c wires the pass), so no frame can move; the entry
    # is here so the build compiles it and the naming-contract test above covers it from the start.
    scene/visibility_resolve.slang:main:compute:visibility_resolve.comp.spv
    scene/visibility_feedback.slang:main:compute:visibility_feedback.comp.spv
    # software_visibility.slang / software_merge.slang are THE SOFTWARE RASTER'S TWO PASSES (M4 Task 2,
    # docs/superpowers/specs/2026-10-22-software-raster-design.md): the tiled compute stage that writes the private
    # R32_UINT depth/id pair (one 8x8 workgroup per 16x16 CSR tile), and the depth-only fullscreen merge that folds it
    # into the frame's G-buffer depth with LESS_OR_EQUAL + depth write. Both build their own pipelines from these
    # modules through pass_context::shader, so they are registered (app_pipelines.cpp) as well as compiled; the merge file has
    # TWO entries. Task 2 lands the entry points; Task 3 fills the tiled walk and the SV_Depth value.
    scene/software_visibility.slang:main:compute:software_visibility.comp.spv
    scene/software_visibility_atomic.slang:main:compute:software_visibility_atomic.comp.spv
    scene/software_merge.slang:vert_main:vertex:software_merge.vert.spv
    scene/software_merge.slang:frag_main:fragment:software_merge.frag.spv
    # hzb_build.slang -> hzb_build.comp.spv is THE HZB OCCLUSION CULLING's BUILD STAGE (M3 phase A2): one dispatch
    # per pyramid level over the frame's G-buffer depth, plus the header (camera/extent snapshot) its thread 0
    # writes. It reads/writes only heap slots and is default-OFF behind DEREN_HZB_BUILD, so it changes no frame
    # until the gate is set; the readback probe (scripts/measure/hzb_verify.py) is its acceptance.
    scene/hzb_build.slang:main:compute:hzb_build.comp.spv
    # hzb_cull.slang -> hzb_cull.comp.spv is THE HZB CULL STAGE (M3 phase B1): one 64-thread workgroup per item,
    # walking that item's host-selected candidates against the HISTORY max pyramid and writing the per-candidate
    # flags, the compacted survivor list, the per-item survivor counts and the stats block. Its gate is
    # DEREN_HZB_BUILD + DEREN_HZB_CULL and it consumes nothing in the frame (phase B2 does), so it changes no
    # pixel; its acceptance is the in-engine CPU-reference verification (scripts/measure/hzb_cull_verify.py).
    scene/hzb_cull.slang:main:compute:hzb_cull.comp.spv
    # ... and the outline's FRAGMENT stage, which shades that hull with the G-buffer normal of the surface
    # BEHIND it (the author's `MyZmdOutlineShader.shader:196-198`). ITS ENTRY IS `frag_main`, NOT `main`, and the
    # reason is measured rather than stylistic: it INCLUDES character_forward.slang for `toon_diffuse`, which
    # brings that file's own `[shader("fragment")] main` with it, and Slang then refuses to resolve a plain
    # `-entry main` at all (E38000, "no function found matching entry point name 'main'") because the name is an
    # overload set. One file with several distinctly-named entries is this repository's existing shape
    # (`post.slang`'s `frag_main`, `heap_probe.slang`'s three), and the .spv name - which is what the host loads
    # - is unaffected by the entry's name.
    toon/game_toon/captured/outline.slang:game_toon_outline_frag_main:fragment:outline.frag.spv
    # shadow.slang shares no body: the material table and the albedo fetch through the shim's accessors
    # (`material_at`, `heap_sample`), and the entry returns void because a depth-only stage has no output.
    # Its MESH entry reuses the same matrix-array accessors pbr.slang does, which is the point of the pair:
    # both stages must place the geometry identically or the shadow silhouette detaches from the surface.
    # Its vertex entry went the same way as pbr's in step 4.
    lighting/shadow.slang:main:fragment:shadow.frag.spv
    # ... and the SAME file's MESH entry (docs/mesh_shaders.md step 1): the depth pass drawn as dispatches
    # instead of indexed draws. It REPLACES the vertex entry rather than joining it (one pipeline is MESH +
    # fragment), and the host keeps both - a device without VK_EXT_mesh_shader has no mesh stage at all.
    lighting/shadow.slang:mesh_main:mesh:shadow.mesh.spv
    # ... and the MESHLET entry (docs/mesh_shaders.md step 3): one workgroup per meshlet, window read from the table.
    lighting/shadow.slang:meshlet_main:mesh:shadow.meshlet.spv
    # light_cluster.slang is the FIRST compute stage: the clustered-light sort, dispatched once per frame by
    # default (clustered_lights = true), so every scenario records it. Its writable buffers are the heap slots
    # the GLSL declares as `writeonly buffer`.
    lighting/light_cluster.slang:main:compute:light_cluster.comp.spv
    # heap_probe.slang is the graphics half of the heap-native probe. No gate scenario renders it, but it is
    # the one group that VERIFIES ITSELF: the runtime dispatches it at startup, reads a pixel back and logs it
    # against the value it knows, so this port is accepted by comparing those log lines (see the file).
    debug/heap_probe.slang:main:vertex:heap_probe.vert.spv
    debug/heap_probe.slang:frag_main:fragment:heap_probe.frag.spv
    # The MESH half of the same probe (docs/mesh_shaders.md step 0): the same triangle, the same fragment
    # stage, the same 4x4 readback, emitted by a mesh pipeline instead of a vertex one. A mesh stage is a
    # different stage type, so this .spv name is new - the host learns it explicitly.
    debug/heap_probe.slang:mesh_main:mesh:heap_probe.mesh.spv
    # The COMPUTE half is its own file because it has its own push layout (one push block per stage).
    debug/heap_probe_comp.slang:comp_main:compute:heap_probe.comp.spv
    debug/resource_group_graphics_probe.slang:vertex_resource_main:vertex:resource_group_depth_probe.vert.spv
    debug/resource_group_graphics_probe.slang:vertex_main:vertex:resource_group_probe.vert.spv
    debug/resource_group_graphics_probe.slang:fragment_main:fragment:resource_group_probe.frag.spv
    debug/resource_group_probe.slang:main:compute:resource_group_probe.comp.spv
    debug/texture_group_probe.slang:main:compute:texture_group_probe.comp.spv
    # compute_skin.slang feeds the acceleration structures the deformed vertex positions; its A/B needs the
    # skinned fixture with rt_shadow on (see docs/slang_migration.md).
    geometry/compute_skin.slang:comp_main:compute:compute_skin.comp.spv
    # mask_bake.slang is the alphaMode MASK bake (off by default; [render] rt_mask_bake turns it on, which is
    # also how its A/B is run).
    geometry/mask_bake.slang:comp_main:compute:mask_bake.comp.spv
    # rt_shadow.slang is the ray-traced shadow's shader file, growing one stage at a time; it starts with the
    # closest-hit stage, whose whole job is the payload contract (see the file). No gate scenario enables
    # rt_shadows, so this port is shape-verified and belongs to the A/B capture route in docs/slang_migration.md.
    lighting/rt_shadow.slang:chit_main:closesthit:rt_shadow.rchit.spv
    lighting/rt_shadow.slang:miss_main:miss:rt_shadow.rmiss.spv
    lighting/rt_shadow.slang:rgen_main:raygeneration:rt_shadow.rgen.spv
    lighting/rt_shadow.slang:ahit_main:anyhit:rt_shadow.rahit.spv
    # rt_reflection.slang is the ray-traced reflection tracer: radiance in rgb and hit distance in a, plus the
    # alphaMode MASK any-hit shared with the shadow tracer. Off by default ([render] rt_reflections).
    lighting/rt_reflection.slang:rgen_main:raygeneration:rt_reflection.rgen.spv
    lighting/rt_reflection.slang:chit_main:closesthit:rt_reflection.rchit.spv
    lighting/rt_reflection.slang:ahit_main:anyhit:rt_reflection.rahit.spv
    lighting/rt_reflection.slang:miss_main:miss:rt_reflection.rmiss.spv
    # The AMD reflection denoiser stages: reproject -> prefilter -> resolve -> copy history. Off by default with
    # [render] rt_reflections.
    lighting/reflection_reproject.slang:main:compute:reflection_reproject.comp.spv
    lighting/reflection_prefilter.slang:main:compute:reflection_prefilter.comp.spv
    lighting/reflection_resolve.slang:main:compute:reflection_resolve.comp.spv
    lighting/reflection_copy_history.slang:main:compute:reflection_copy_history.comp.spv

    # deferred.slang is the deferred path's lighting stage: the shared shading code again (so the forward and
    # the deferred image are comparable rather than merely similar), plus the screen-space AO it owns. It is
    # on every scenario's path, and it is the ONE stage whose frames differ from the GLSL build - by ONE pixel
    # of 1036800, a front-end codegen difference rather than a porting mistake (both modules call the same
    # GLSL.std.450 instructions; their SEQUENCES differ, and the driver contracts the multiply-adds
    # differently). It was accepted by an explicit re-baseline with the reason written down: see
    # docs/slang_migration.md section 9, "the last stage, and the one-pixel re-baseline".
    lighting/deferred.slang:main:fragment:deferred.frag.spv
)
# These entries belong to the GPU test, rather than the application's shader catalogue.
if(TARGET test_acceleration_structures)
    list(APPEND VR_SLANG_SOURCES
        debug/rt_traversal_probe.slang:rgen_main:raygeneration:rt_traversal_probe.rgen.spv
        debug/rt_traversal_probe.slang:native_main:raygeneration:rt_traversal_probe.native.spv
        debug/rt_traversal_probe.slang:chit_main:closesthit:rt_traversal_probe.rchit.spv
        debug/rt_traversal_probe.slang:miss_main:miss:rt_traversal_probe.rmiss.spv
    )
endif()
set(VR_SLANG_OUTPUTS "")
foreach(entry IN LISTS VR_SLANG_SOURCES)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 slang_source)
    list(GET parts 1 slang_entry)
    list(GET parts 2 slang_stage)
    list(GET parts 3 slang_output)
    add_custom_command(
        OUTPUT "${VR_SHADER_OUT_DIR}/${slang_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VR_SHADER_OUT_DIR}"
        COMMAND "${VR_SLANGC_EXECUTABLE}" "${VR_SHADER_SRC_DIR}/${slang_source}" -I "${VR_SHADER_SRC_DIR}"
                -target spirv -profile spirv_1_6 -capability spvDescriptorHeapEXT
                -spirv-resource-heap-stride 64 -spirv-sampler-heap-stride 32
                -fvk-use-gl-layout -matrix-layout-column-major
                # NO -fp-mode HERE, and that is a measurement rather than an omission: `-fp-mode precise`
                # was tried against the byte-identical standard and made NINE scenarios differ, including
                # two that had been identical for eleven stages - so Slang's DEFAULT mode is the one whose
                # codegen matches glslang's, and the alternative is strictly worse. The difference the
                # default mode leaves is the one re-baselined for deferred.frag; see
                # docs/slang_migration.md section 9, "the last stage, and the one-pixel re-baseline".
                -entry ${slang_entry} -stage ${slang_stage} -o "${VR_SHADER_OUT_DIR}/${slang_output}"
        DEPENDS "${VR_SHADER_SRC_DIR}/${slang_source}" ${VR_SHADER_INCLUDES}
        COMMENT "slangc ${slang_source} -> ${slang_output}"
        VERBATIM
    )
    list(APPEND VR_SLANG_OUTPUTS "${VR_SHADER_OUT_DIR}/${slang_output}")
endforeach()
message(STATUS "slangc: ${VR_SLANGC_EXECUTABLE} - ${VR_SLANG_SOURCES}")
list(APPEND VR_SHADER_OUTPUTS ${VR_SLANG_OUTPUTS})
# THERE IS NO glslc RULE ANY MORE, and no VR_GLSL_SOURCES to compute: every stage is built from a .slang
# source above, so the split ("which sources does glslc still have to build?") that this section used to
# compute while the migration ran has no members left. The retired GLSL stage sources are archived in
# source/reference/shaders/glsl.old/ and are NOT part of the build - they exist to be read next to their ports.
# the runtime's log names the SOURCE file it is loading the SPIR-V for, and it looks for that name in
# the same directory: copy the sources there so the log points at a file a reader of the build tree
# can actually open. Not a correctness requirement - the .spv beside it is what gets loaded. The list is
# TAKEN FROM VR_SLANG_SOURCES rather than written out again: it used to be a hand-maintained GLSL list, which
# is exactly the kind of duplication that goes stale (this one did, twice).
set(VR_SHADER_SOURCE_COPIES "")
foreach(entry IN LISTS VR_SLANG_SOURCES)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 slang_source)
    list(APPEND VR_SHADER_SOURCE_COPIES "${VR_SHADER_SRC_DIR}/${slang_source}")
endforeach()
add_custom_command(
    OUTPUT "${VR_SHADER_OUT_DIR}/.sources-copied"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${VR_SHADER_SRC_DIR}" "${VR_SHADER_OUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${VR_SHADER_OUT_DIR}/.sources-copied"
    DEPENDS ${VR_SHADER_OUTPUTS} ${VR_SHADER_SOURCE_COPIES} ${VR_SHADER_INCLUDES}
    COMMENT "copying the Slang sources beside the compiled shaders"
    VERBATIM
)
add_custom_target(shaders ALL DEPENDS ${VR_SHADER_OUTPUTS} "${VR_SHADER_OUT_DIR}/.sources-copied")
if(TARGET test_acceleration_structures)
    add_dependencies(test_acceleration_structures shaders)
endif()
if(TARGET deren)
    add_dependencies(deren shaders)
endif()
# The dynamic runtime's own scaffold test needs the compiled shaders as soon as the port starts building
# pipelines from them (S2/S3); in S1 it needs none, and asking anyway keeps the dependency from being
# forgotten when the test grows.
if(TARGET test_runtime_dyn)
    add_dependencies(test_runtime_dyn shaders)
endif()
add_dependencies(test_resource_group shaders)
target_compile_definitions(test_render_graph_frame PRIVATE VR_FEEDBACK_SHADER_PATH="${VR_SHADER_OUT_DIR}/visibility_feedback.comp.spv")
target_compile_definitions(test_resource_group PRIVATE VR_RESOURCE_GROUP_SHADER_PATH="${VR_SHADER_OUT_DIR}/resource_group_probe.comp.spv"
    VR_RESOURCE_GROUP_VERTEX_PATH="${VR_SHADER_OUT_DIR}/resource_group_probe.vert.spv"
    VR_RESOURCE_GROUP_FRAGMENT_PATH="${VR_SHADER_OUT_DIR}/resource_group_probe.frag.spv"
    VR_RESOURCE_GROUP_DEPTH_VERTEX_PATH="${VR_SHADER_OUT_DIR}/resource_group_depth_probe.vert.spv")
add_dependencies(test_texture_group shaders)
target_compile_definitions(test_texture_group PRIVATE VR_TEXTURE_GROUP_SHADER_PATH="${VR_SHADER_OUT_DIR}/texture_group_probe.comp.spv")
message(STATUS "slangc: ${VR_SLANGC_EXECUTABLE} - every build compiles source/shaders/*.spv into ${VR_SHADER_OUT_DIR}")

target_link_libraries(test_render_operations promise vulkan_core_support)

# Buffer transfer service probe; the device lane passes --with-device explicitly.
add_executable(test_buffer_transfer source/tests/test_buffer_transfer.cpp)
target_link_libraries(test_buffer_transfer buffer_transfer_core backend_loader static_utility vstd_lib vulkan_main_context Vulkan::Vulkan)
add_dependencies(test_buffer_transfer deren_vulkan)

add_executable(test_subtilis_transfer source/tests/test_subtilis_transfer.cpp)
target_link_libraries(test_subtilis_transfer deren_engine backend_loader static_utility vstd_lib)
add_dependencies(test_subtilis_transfer deren_vulkan)

# ---- THE DEVICE LANE'S TESTS -------------------------------------------------------------------------
#
# These are built by the default build but are NOT headless tests: they want a Vulkan device (and some of
# them load deren_vulkan.dll / the GUI plugin). Before this block they were invisible - built and never run
# by anything - and a run without `--with-device` answered a PASS after asserting nothing. CTest now knows
# them: `ctest -N` names them, the `device` label lets the GPU-less CI jobs exclude them with `-LE device`,
# and a machine WITH a device (the local gate) runs them for real. SKIP_RETURN_CODE makes "could not run"
# read as Skipped rather than a PASS: the tests return deren::vk_test::skip_code (77) when they decline.
#
# The boundary spike (test_backend_boundary_spike) stays OUT of this list on purpose: it is a report-shaped
# spike, not a pass/fail test, and source/tests/test_docs.cpp allowlists it by name so it cannot be forgotten.
set(VR_DEVICE_TEST_TARGETS
    test_runtime_dyn
    test_texture_group
    test_resource_group
    test_submission_completion
    test_buffer_transfer
    test_subtilis_transfer
    test_gui_plugin
)
foreach(device_target IN ITEMS ${VR_DEVICE_TEST_TARGETS})
    if(TARGET ${device_target})
        add_test(NAME ${device_target} COMMAND $<TARGET_FILE:${device_target}>)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run/${device_target}")
        set_tests_properties(${device_target} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run/${device_target}"
            TIMEOUT 600
            LABELS device
            SKIP_RETURN_CODE 77)
    endif()
endforeach()

# Opt-in device benchmark, not a headless test or part of the default build.
if(VR_BUILD_TESTS)
    add_executable(bench_image_upload EXCLUDE_FROM_ALL source/tests/bench_image_upload.cpp source/tests/image_upload_bench_vma.cpp)
    # CPU oracle for a standalone CUDA experiment; CUDA is not a project/build dependency.
    # The oracle is an untracked scratch tool in some worktrees, so the opt-in target only exists when it does.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ibl_cuda_spike/cpu_reference.cpp")
        add_executable(bench_ibl_cpu EXCLUDE_FROM_ALL tools/ibl_cuda_spike/cpu_reference.cpp)
        target_link_libraries(bench_ibl_cpu PRIVATE deren_engine vstd_lib)
    endif()
    target_include_directories(bench_image_upload PRIVATE source/third_party source/backends/vulkan/core/vma)
    target_link_libraries(bench_image_upload PRIVATE backend_loader promise static_utility vstd_lib vulkan_main_context Vulkan::Vulkan)
    add_dependencies(bench_image_upload deren_vulkan)
    if(MSVC)
        set_source_files_properties(source/tests/image_upload_bench_vma.cpp PROPERTIES COMPILE_OPTIONS "/w")
    else()
        set_source_files_properties(source/tests/image_upload_bench_vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()
