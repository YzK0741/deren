cmake_minimum_required(VERSION 4.3)

# CMake gates its `import std;` support - which the MSVC dialect of vstd needs, see
# vstd/vstd_msvc.cppm - behind an experiment id that must be set BEFORE project(). Compiler
# detection is where CMake decides whether the toolchain can provide the standard library module,
# and with the gate closed it rejects the CXX_MODULE_STD property outright ("Experimental `import
# std` support not enabled when detecting toolchain; it must be set before `CXX` is enabled").
# The id is CMake's own, not invented here: the string `CxxImportStd.f35a9ac6-8463-4d38-8eec-5d6008153e7d`
# sits in the cmake binary next to the variable name, and CMake keeps gate ids stable across
# releases. It is set for every toolchain on purpose, because `MSVC` is not defined until after
# project() - and it is only USED by the MSVC branch at `vstd_lib` below: clang64 builds
# vstd/vstd.cppm exactly as before, and its exe was measured byte-identical with this line in place.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d" CACHE STRING
    "CMake's gate for the experimental `import std;` support (CMake >= 3.30).")

project(deren VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
# NOTE: do NOT add "-std=c++23" to CMAKE_CXX_FLAGS here - CMAKE_CXX_STANDARD already injects it,
# and doing both duplicates the flag on every TU (same for -O3 / -g, which the default per-config
# flags already provide - see COMPILE_OPTIONS below).
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

# glm (header-only). find_package() is enough for clang64 because that package ships a config, but the
# config points glm::glm's INTERFACE_INCLUDE_DIRECTORIES at the WHOLE clang64 include root. For clang
# that root is the compiler's own root and nothing is shadowed; for MSVC it is a foreign libc's headers
# and it is fatal - measured with cl.exe, which then found mingw's corecrt.h / stdlib.h and emitted a
# wall of C2146/C2081/C2061 with corecrt.h(219) at the top of it.
# Point VR_GLM_INCLUDE_DIR at a directory that CONTAINS the glm/ subtree (the clang64 include root is
# fine for headers clang already owns, but on MSVC use a shim that holds only glm/) and the target is
# built here from that directory instead of from the config package.
set(VR_GLM_INCLUDE_DIR "" CACHE PATH "Directory containing the glm/ header subtree; when set, glm::glm is built from it instead of find_package(glm) (MSVC builds need this)")
if(VR_GLM_INCLUDE_DIR)
    add_library(glm_headers INTERFACE)
    target_include_directories(glm_headers SYSTEM INTERFACE "${VR_GLM_INCLUDE_DIR}")
    add_library(glm::glm ALIAS glm_headers)
    # ...AND at directory scope, because not every target that includes <glm/...> links glm::glm:
    # `deren.utility` (BVH) and `deren.gltf_loader` both include it while linking only each other, and under clang
    # that worked purely because glm lived on the compiler's implicit include path. Measured on MSVC
    # without this line: three C1083s - utility/BVH/bvh.cppm(3), gltf_loader/gltf_loader.cppm(18),
    # gltf_loader/gltf_loader.cpp(6). This call is inside the VR_GLM_INCLUDE_DIR branch on purpose: when
    # the variable is empty (every clang build) no include directory is added anywhere and the command
    # lines stay exactly what they were.
    include_directories(SYSTEM "${VR_GLM_INCLUDE_DIR}")
else()
    find_package(glm REQUIRED)
endif()

# GLFW (window + input): linked STATICALLY so the released exe carries no glfw3.dll.
# The MSYS2 clang64 package's own CMake config exports a SHARED import (libglfw3.dll.a),
# so we do not use find_package(glfw3): a plain INTERFACE target linking the bare name
# would now resolve to that shared import (the exe no longer links with -static - see the
# link-options block below), which is why the interface names libglfw3.a outright, and
# crucially does NOT define GLFW_DLL (the dynamic-import macro static builds must not see).
# Headers live on the compiler's implicit include path; an explicit dir is added when found.
#
# THE EXCEPTION IS MSVC, where `glfw3` is not a library name at all (that bare-name convention is the
# MinGW one). The official GLFW binary release ships include/ plus lib-vc2022/glfw3.lib, so
# VR_GLFW_ROOT points at the unpacked release directory and both the headers and the .lib come from
# there. lib-vc2022/glfw3.lib is an /MD (dynamic CRT) build, which is why the MSVC branch of the
# compile options leaves the CRT at its own default.
set(VR_GLFW_ROOT "" CACHE PATH "Unpacked GLFW binary release (the directory holding include/ and lib-vc2022/); required for MSVC builds")
add_library(glfw INTERFACE)
if(WIN32)
    if(VR_GLFW_ROOT)
        target_include_directories(glfw SYSTEM INTERFACE "${VR_GLFW_ROOT}/include")
        find_library(VR_GLFW_LIBRARY NAMES glfw3 glfw3_mt
                PATHS "${VR_GLFW_ROOT}/lib-vc2022" "${VR_GLFW_ROOT}/lib-mingw-w64"
                NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(glfw INTERFACE ${VR_GLFW_LIBRARY} gdi32)
    else()
        # MINGW ONLY: a bare `glfw3` is "the linker's choice", and that choice is the SHARED import
        # (libglfw3.dll.a) whenever the executable is not linked with -static. The C++ runtime is
        # dynamic now (see the link-options block below), so the static archive is named explicitly
        # here instead - the released exe must still carry no glfw3.dll (see the note above).
        find_library(VR_GLFW_STATIC_LIBRARY NAMES libglfw3.a REQUIRED)
        target_link_libraries(glfw INTERFACE ${VR_GLFW_STATIC_LIBRARY} gdi32) # glfw3.pc: Libs.private = -lgdi32
    endif()
else()
    find_package(Threads REQUIRED)
    find_library(GLFW_LIBRARY NAMES glfw3 REQUIRED)
    target_link_libraries(glfw INTERFACE ${GLFW_LIBRARY} Threads::Threads)
endif()

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
            # reports 202400 even without this flag - see vstd/vstd_compat.inc), but a correct
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
    )
else()
    set(COMPILE_OPTIONS
            $<$<CONFIG:Release>:
            -fno-rtti
            -ffunction-sections
            -fdata-sections
            -flto
            ${VR_RELEASE_MARCH}
            -fno-unwind-tables
            -fno-asynchronous-unwind-tables
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
    )
endif()

# The project's STL module is `deren.vstd`, and there are two dialects of it - the toolchain picks:
#
#   clang64 (the shipped build): vstd/vstd.cppm - modified from libc++'s (LLVM) generated
#     std-module output and trimmed to the headers the project uses (see vstd/README.md).
#   MSVC: vstd/vstd_msvc.cppm - `export module deren.vstd; export import std;`, i.e. the MSVC
#     toolchain's own std module re-exported under the project's module name.
#
# The MSVC dialect exists because of a compiler bug, measured on this tree rather than guessed:
# the libc++-derived re-export construction (71 `export namespace std { using ... }` partitions)
# makes cl.exe 19.44 die with `fatal error C1001` on every TU that instantiates std::span /
# std::array / std::tuple through it - 7 of the 326 objects before this switch, all of them
# importers of vstd - while the same consumers compile clean against the toolchain's std module.
# The minimal reproductions and the bisection are in
# build-release-clang64/msvc/lead_lab/ICE_FINDINGS.md; vstd/vstd_msvc.cppm states what the dialect
# costs (MSVC STL instead of libc++, and no VSTD_* macro face).
#
# CXX_MODULE_STD makes CMake build that std module itself and hand it to vstd_lib's compile: a
# Visual Studio installation ships only modules/std.ixx, there is no prebuilt std.ifc. It relies
# on the experimental gate set at the top of this file (the gate has to precede project(), which
# is also why the compiler choice is made with `if(MSVC)` here and not up there).
if(MSVC)
    set(CMAKE_CXX_MODULE_STD ON)
endif()

add_library(vstd_lib STATIC)

if(MSVC)
    target_sources(vstd_lib
            PUBLIC
            FILE_SET CXX_MODULES
            FILES
            vstd/vstd_msvc.cppm
    )
else()
    target_sources(vstd_lib
            PUBLIC
            FILE_SET CXX_MODULES
            FILES
            vstd/vstd.cppm
    )
endif()

# =============================================================================================
# THE UTILITY TOOLKIT IS TWO TARGETS (batch ④, the dynamic-backend flip's prerequisite).
#
# WHY TWO, and it is a measured hazard rather than tidiness: the toolkit is STATIC and BOTH halves
# of the RHI boundary link it, so one process holds one copy of everything - the log sink included.
# THE FLIP BREAKS THAT: `deren_vulkan` becomes SHARED, the DLL carries its own copy of every
# library it links, and the process then holds TWO sinks - two `ofstream`s on the same debug.log,
# two startup rotations, and `rotate_previous_log()` TRUNCATES the file the other copy still has
# open and appends to (interleaved lines, holes, two sessions in one debug.log.old). The boundary
# gate is blind to it by construction: it measures deren_vulkan ∩ vulkancorekit, and this is a
# third library that defines no backend symbol.
#
# THE SEAM IS STATE, NOT DIRECTORY:
#   shared_utility  - the PROCESS-WIDE state: the log sink (queue, worker thread, file handle), the
#                     startup rotation, panic convergence, and the allocator hook (`:better_pmr`
#                     sets the process's default std::pmr resource).
#   static_utility  - everything per-object or pure, plus the loader: data_block, BVH, frame_clock,
#                     frame_stats, thread_pool, the platform_* TUs, deren.utility.dynamic_link, and
#                     the printing / byte-writer TEMPLATES (`log` / `error` / `panic` format in the
#                     caller's instantiation and hand the result over as a std::string_view).
#
# BOTH ARE STATIC IN THIS BATCH, on purpose: the split lands and is verified first with ZERO
# behaviour change (one sink today, exactly as before), and `shared_utility` becomes SHARED in the
# same batch as `deren_vulkan`. Flipping the two together would make the split itself unverifiable.
#
# ONE TARGET PER MODULE NAME, EVEN THOUGH BOTH HALVES COMPILE EACH MODULE: `shared_utility` owns
# `deren.utility.shared` (with its `:better_pmr` partition); `static_utility` owns `deren.utility`
# (with `:data_block` / `:bvh` / `:frame_clock` / `:frame_stats` / `:thread_pool`) and
# `deren.utility.dynamic_link`. Listing one .cppm in two targets would produce two BMIs of one
# module name, which a single translation unit cannot arbitrate - the same ruling that moved
# deren.vulkan.constant_init into a target of its own (see that target's comment above).
#
# `deren.utility` RE-EXPORTS `deren.utility.shared`, so the 52 existing `import deren.utility;`
# sites keep working unchanged: a call site does not know which half a name came from.
#
# DEPENDENCY DIRECTION: static_utility MAY depend on shared_utility (it formats text and calls the
# sink); the reverse is FORBIDDEN - the shared module writes its own stdout/stderr fallback instead
# of calling this module's `println`. The contract `rhi` depends on NEITHER, and `shared_utility`
# does not import it at all (the verdict/result templates that carry rhi::error_info are static).
#
# `dynamic_link` STAYS ON THE STATIC SIDE FOR A LOAD-BEARING REASON: it is the key that loads the
# backend, so putting it in the process-wide library would build a bootstrap chain ("the executable
# must load shared_utility before it can load the backend") - and it needs no shared state.
#
# THE EXPORT SURFACE IS C++, AND THIS BATCH USES THE RELAXED RULES THE USER SET (batch ④). The three
# of them, written here because a later reader will otherwise "fix" this split back into a C shim:
#   1. NO C SHIM. `deren.utility.shared` exports C++ directly (std::string_view, std::span,
#      std::source_location, std::format_string) - an opaque C ABI for a library both halves are built
#      from, by one toolchain, in one commit, buys nothing here.
#   2. STL IS ALLOWED: std::string / std::vector / std::function / std::span / std::pmr may cross, and
#      `log(fmt, args...)` may be a template in a module interface. THE MEASURED BASIS, not a hope:
#      the executable imports libc++.dll and uses the UCRT heap, so both images share ONE C++ runtime
#      and ONE heap - which is what makes an allocation on one side and a free on the other safe in
#      this configuration. That premise is now MACHINE-CHECKED rather than assumed: see the
#      C++-runtime check in scripts/check_backend_boundary.py (dll imports enumerated, no static
#      libc++ in any image).
#   3. THE REASON THE SPLIT EXISTS IS STATE, NOT MEMORY. One log sink per process: two copies mean two
#      `ofstream`s on one debug.log and two startup rotations, and `rotate_previous_log()` TRUNCATES
#      the file the other copy still has open (interleaved lines, holes, two sessions in one
#      debug.log.old). The heap/STL question is NOT the reason for this split and is deliberately not
#      argued here - that is the `shared_object` line of work (abi 18).
# WHERE THIS BATCH STILL PASSES A VIEW (`log_text(std::string_view)`), the reason is a design choice
# rather than a prohibition: the sink copies what it queues on its own side, and the formatting stays
# in the caller's template instantiation, so no template has to be an exported symbol. Nothing in the
# rules above requires that - it is simply the shape the sink's door settled on.
# THE PROCESS-WIDE GUARD IS THE ROTATION CLAIM (utility/shared/log_rotation_claim.cpp): an OS-named
# object keyed by the process id, so even if a second copy of the sink ever appears in one process the
# second one does NOT rotate the file the first one is writing.
# =============================================================================================
# SHARED, IN THE SAME BATCH AS deren_vulkan, AND THIS IS THE MOMENT THE SPLIT WAS FOR: until now both
# halves of the toolkit were STATIC, so the process happened to hold one sink even with two copies of
# the code. A DLL carries its own copy of what it links, so with deren_vulkan SHARED the sink WOULD be
# duplicated - two `ofstream`s on one debug.log, two startup rotations, and
# `rotate_previous_log()` TRUNCATING the file the other copy still has open. `shared_utility` being a
# DLL is what prevents it: the executable and deren_vulkan.dll both import THIS image, so the OS loads
# one copy of the sink for the whole process. The rotation claim
# (utility/shared/log_rotation_claim.cpp, an OS-named object keyed by the process id) stays as the
# second line of defence, and this batch is where it is first genuinely needed - see
# DYNAMIC_LINK_PROGRESS.md §18 for the one-rotation measurement.
add_library(shared_utility SHARED)
# THE FILE NAME IS PART OF THE DESIGN: the process-wide half ships as `shared_utility.dll`, because
# BOTH the executable and deren_vulkan.dll import it BY THAT NAME. MinGW's default `lib` prefix would
# produce `libshared_utility.dll`, so it is removed on purpose (the import library keeps its
# conventional spelling through IMPORT_PREFIX).
set_target_properties(shared_utility PROPERTIES PREFIX "")

target_sources(shared_utility
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        # The sink, the rotation, panic convergence, the allocator hook. Its tools go into
        # `deren.utility` below, which re-exports this module.
        utility/shared/shared_utility.cppm
        PRIVATE
        utility/shared/shared_utility.cpp
        # The allocator hook's definitions. IT IS NOT A PARTITION, AND THAT IS MEASURED: a partition
        # of a module that is itself re-exported (`deren.utility` re-exports `deren.utility.shared`)
        # makes clang 22.1.8 crash while semantically analysing a consumer's std::format call
        # (utility.cpp's write_png, reproduced with -fsyntax-only; the crash disappears when the
        # partition re-export becomes a plain import). So the hook is declared in the primary
        # interface and defined here - no partition, nothing lost.
        utility/shared/better_pmr.cpp
        # The once-per-process rotation claim, a plain (non-module) TU for the platform_* reason:
        # <windows.h> does not belong in a module's global module fragment.
        utility/shared/log_rotation_claim.cpp
)

add_library(static_utility STATIC)

target_sources(static_utility
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        utility/utility.cppm
        utility/data_block.cppm
        utility/BVH/bvh.cppm
        utility/frame_clock/frame_clock.cppm
        utility/frame_stats/frame_stats.cppm
        utility/thread_pool/thread_pool.cppm
        # The loader the RHI boundary is built on (plan_rhi_v4.md 7.4): the only place in the tree that
        # asks the platform for a library. It is a module of its own rather than a `utility:` partition
        # because a partition cannot be imported by another target's module without dragging `deren.utility`
        # in, and the boundary rule is that ONE module owns the platform load path. It declares the
        # platform entry points itself (no <windows.h>, no <dlfcn.h> in a module's global fragment),
        # which is what keeps its importers platform-agnostic.
        # IT STAYS HERE, NOT IN shared_utility: it is the key that loads the backend, so putting it
        # behind the process-wide library would make "load shared_utility" a prerequisite of "load
        # the backend" - a bootstrap chain with nothing to gain.
        utility/dynamic_link.cppm
        PRIVATE
        utility/utility.cpp
)

# The RHI boundary's contract (plan_rhi_v4.md §3.1, §4.1; m03159): a module of virtual base classes,
# deren.promise.rhi, which lives in promise/rhi/ - the outer segment names the family of contract
# interfaces and the inner one the graphics-API instance of it (m03716, corrected by m03931). It is a
# module rather than a directory of headers because BOTH
# sides of the boundary compile it - the engine's targets and a backend DLL each read the same
# interface unit instead of a shared header - which is what keeps the two compilations from drifting,
# and it is listed here, once, for the same reason. There is no .cpp: every declaration below this
# line is an interface, so the STATIC target exists only to carry the module's file set (the shape
# vstd_lib has for deren.vstd). The C ABI surface is deliberately NOT here (m03159): it is
# promise/rhi/backend_entry.hpp - in the same directory, but a plain header, not a partition and not
# in the file set below, because an entry point is not a virtual base class. The keyword it is
# declared with is a build concern and lives in utility/abi_export.hpp (m04074).
add_library(promise STATIC)

target_sources(promise
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        promise/rhi/rhi.cppm
        promise/rhi/rhi.contract.cppm
        # The creation parameters, as a contract type: the program fills it, the backend builds its
        # context from it, and the factory that takes it (S2) needs no backend-specific structure.
        promise/rhi/rhi.core_desc.cppm
        promise/rhi/rhi.extension.cppm
        promise/rhi/rhi.api_core.cppm
)

# Startup configuration: parses config.toml (+ --config / argv merging) into app_settings.
# Uses toml++ (header-only, system package mingw-w64-clang-x86_64-tomlplusplus; under clang64 its
# headers live on the compiler's own include root, so nothing has to be added here). Pure CPU, no
# Vulkan.
#
# MSVC IS THE EXCEPTION, and it is the same shape as glm above: cl.exe has no such implicit root, so
# VR_TOMLPP_INCLUDE_DIR is where a build that is not using the clang64 toolchain finds <toml++/toml.hpp>.
# It is a directory that CONTAINS the toml++/ subtree (either a real include root or a shim).
set(VR_TOMLPP_INCLUDE_DIR "" CACHE PATH "Directory containing the toml++/ header subtree; required for MSVC builds (toml++ is header-only)")
add_library(app_config STATIC)

target_sources(app_config
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        application_configuration/application_configuration.cppm

    PRIVATE
        application_configuration/application_configuration.cpp
)

if(VR_TOMLPP_INCLUDE_DIR)
    target_include_directories(app_config SYSTEM PRIVATE "${VR_TOMLPP_INCLUDE_DIR}")
endif()

# toml++ picks its API from the exception mode it detects in the compiler flags (TOML_EXCEPTIONS,
# toml++/impl/preprocessor.hpp): exceptions OFF gives parse_result a type with operator bool and
# error(), exceptions ON makes parse_result an alias of toml::table and gives the parse functions a
# different return type. The product code is written against the non-exception API ("parsed" /
# "parsed.error().description()" in application_configuration.cpp), and clang64 gets that mode for
# free because the project compiles with -fno-exceptions.
#
# MSVC does NOT detect it: /EHsc is deliberate there (see the dialect table above - there is no
# supported "no exceptions" spelling beside the STL), so toml++ would select the throwing API and
# BOTH translation units of this target would fail to compile against it. State the mode instead of
# inferring it, here rather than in a source file, so the module interface and the implementation are
# guaranteed to agree: they must, because toml::parse_result is otherwise one type in
# application_configuration.cppm and another in application_configuration.cpp. Only this target sees
# the header (checked: no other source in the tree includes toml++), so PRIVATE is enough.
target_compile_definitions(app_config PRIVATE TOML_EXCEPTIONS=0)

# The engine core library (vulkancorekit): Vulkan object management + math + the runtime
# facade + scene tree + the debug GUI + the format-neutral animation controller
# (vulkan.animation: plays keyframe clips / skins / morphs onto the runtime scene; it never
# imports a loader - deren::gltf::scenes satisfies its structural source concept at the call site).
# This library does NOT depend on gltf_loader (pure CPU, used by the demo layer only).
# ============================================================================================
# ============================================================================================
# deren_vulkan - THE BACKEND HALF AS A TARGET OF ITS OWN (plan_rhi_v4.md §11.2, S1-A).
#
# The minimum cut, measured with deren-ab/rhi/_s1_import_graph.py: the import closure of the seed
# {deren.vulkan.core, deren.vulkan.init_utils} is 7 modules with ZERO edges leaving the closure.
# A wider cut drags bindings / pipelines / render_resource / scene_tree in, which is not S1 - that
# is S3. So this target carries exactly the engine-service layer (core, its vma / descriptor-heap
# / handles / init_utils / pipeline / spirv_parser, and the transition constants) and the 17 files
# listed below, and NOTHING about render passes.
#
# WHY A TARGET AND NOT A DIRECTORY MOVE: every consumer of the backend keeps using vulkancorekit
# unchanged (the BMI comes across through `target_link_libraries(vulkancorekit PUBLIC deren_vulkan)`,
# the shape static_utility / vstd_lib already have). Products are expected to be byte-identical: same TUs,
# same flags, same executable - S1-A is the one step of S1 that is constructive.
#
# SHARED, AND THAT IS THE FLIP ITSELF (the migration's last batch). The order the plan asked for is
# now full: S1-C made this target implement deren::promise::rhi::api_core and prove the contract
# in-process, and this batch makes the SAME TARGET the DLL whose ONE export
# (promise/rhi/backend_entry.hpp, abi 18) the executable resolves BY NAME through
# deren.utility.dynamic_link - never by linking it. `target_link_libraries(vulkancorekit
# PUBLIC deren_vulkan)` is GONE with it (see that line): an import library would load the backend at
# process start, before main, which is exactly the load-time binding dynamic_link exists to replace -
# and a missing DLL would then fail as a system loader error instead of a diagnosable refusal.
#
# -DDEREN_BACKEND_SPIKE RETIRED IN THIS BATCH, and it retired by SUCCEEDING: it existed to build this
# target SHARED in a throwaway tree so the boundary's four known-unknowns were measured before the
# migration was paid for (plan_rhi_v4.md §9 items 14-17). SHARED is the main configuration now, so the
# option and its `if()` branches are deleted rather than left as a switch that means "the same thing,
# but hidden". Two things it carried become permanent instead of conditional: the dllexport half of
# the boundary (DEREN_API_SHARED + DEREN_API_BUILD) and the sanitizer suppression below. The spike
# TEST stays, unconditionally built and deliberately NOT registered with ctest (it loads a DLL and with
# --with-device builds a real context, so it cannot belong to the headless set that tests/test_docs.cpp
# compares against the workflow).
# ============================================================================================

# ============================================================================================
# deren.vulkan.constant_init - THE VULKAN CONSTEXPR BUILDERS, COMPILED FOR BOTH HALVES.
#
# WHY IT IS NOT IN deren_vulkan ANY MORE (③-E's import instrument, measured): the module depends on
# NOTHING but the Vulkan headers (its own header says so), yet the engine imports it in 26 sites across
# 23 files. While the file sits in deren_vulkan, every one of those imports is an import of a BACKEND
# module - which the flip's import gate refuses and which a SHARED backend cannot serve at all, because
# the BMI would have to come from the DLL. Moving the file to a target of its own makes the module
# neutral (both halves compile it into their own image through this target) and takes those 33 import
# points off the meter.
#
# WHY A TARGET AND NOT "LIST THE SAME .cppm IN BOTH TARGETS": two BMIs of the same module name would
# both be visible to one TU of a target that links both owners, and a translation unit cannot import
# the same module from two definitions. The proven shape in this repository is one owning target linked
# by both - `promise` does exactly that for the contract. (The engine no longer links deren_vulkan at
# all since the flip, so today this argument is about `promise` and `vulkan_constant_init`, both of
# which the engine and the backend DO share.)
#
# FLAGS: the shared COMPILE_OPTIONS set (the foreach below), and deliberately NOT deren_vulkan's
# `-fno-sanitize=all`: that suppression exists because an INSTRUMENTED DLL cannot carry a second ASan
# runtime, and this target is never the DLL's code path (the DLL is uninstrumented by construction -
# see deren_vulkan's sanitizer suppression; `promise`, which both halves also share, is not
# suppressed either).
# ============================================================================================
add_library(vulkan_constant_init STATIC)
target_sources(vulkan_constant_init
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        vulkan/constant_init/constant_init.cppm
        # The renderer's slot grid (③-D/E batch, ruling D): the engine names it in ~95 places and must
        # not do so through the backend's class any more. Both halves compile it HERE, so the grid
        # cannot drift, and `core::heap_slots` / the `core::heap_slot_*` constants become aliases into
        # it - with the backend's frozen numbers `static_assert`ed against the module (see
        # core.declarations.cppm), which is the drift guard ruling D asked for.
        vulkan/render_layout/render_layout.cppm
)
target_link_libraries(vulkan_constant_init PUBLIC Vulkan::Vulkan)

# ============================================================================================
# vulkan_error_tables - THE ERROR TRANSLATORS, CARRIED BY A TARGET BOTH SIDES LINK (the SHARED
# flip's third move of this kind, after vulkan_constant_init and both utility halves).
#
# WHY THEY LEFT `deren_vulkan`'s MODULE: until the flip they were `export` declarations of
# `deren.vulkan.core` with definitions in a module implementation unit. A module attachment is a
# LINK-TIME fact - a consumer needs the module's initializer and its decorated symbols - and a DLL
# can serve those only by exporting its module surface. `deren_vulkan.dll` exports EXACTLY ONE name
# by design (promise/rhi/backend_entry.hpp), so the tables had to become ordinary functions in a
# target that (a) the backend links (core.api_core.cpp / core.cpp call them) and (b) the tables'
# unit test links, WITHOUT the test having to import a backend module. Same TUs on both sides, one
# definition, no BMI: the shape this repository already proved twice.
# ============================================================================================
add_library(vulkan_error_tables STATIC)
target_sources(vulkan_error_tables
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        # The translators AND the diagnostic builder, as a module: VkResult -> contract error, once
        # per call-site family.
        vulkan/core/error_tables.cppm
)
# The contract's module is the dependency of the interface unit itself (it re-exports it, so an
# importer needs no second import just to name an `error`).
target_link_libraries(vulkan_error_tables PUBLIC promise Vulkan::Vulkan)

add_library(deren_vulkan SHARED)
add_library(deren::vulkan ALIAS deren_vulkan)
# Same rule as shared_utility: the backend ships as `deren_vulkan.dll`, which is what the runtime's
# loader looks for beside the executable (runtime/runtime.constructor.cppm).
set_target_properties(deren_vulkan PROPERTIES PREFIX "")

# The dllexport side of the boundary (utility/abi_export.hpp). The importing side needs neither
# macro: the executable resolves the entry by NAME through dynamic_link and is not linked against
# this library at all, so only the probe/test consumers ever see `dllimport`.
target_compile_definitions(deren_vulkan PRIVATE DEREN_API_SHARED DEREN_API_BUILD)

    # MEASURED, AND IT IS WORSE THAN "DOES NOT LINK": with the CI sanitizer job's flags
    # (-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined ...", which carry the runtime only on the
    # EXE link line) the `-shared` link of this library does not merely leave ASan undefined - it
    # CRASHES lld:
    #
    #     LLVM ERROR: Associative COMDAT symbol '.str.5' is not a key for its COMDAT.
    #     Exception Code: 0xC000001D   (ld.lld, clang 22.1.8, --coff)
    #
    # so the instrumented objects of a SHARED C++ library are not a shape this toolchain can link at
    # all. The suppression below is the same one tests/probe_backend.cpp already carries, and it is
    # the reason the flip COSTS THE BACKEND ITS SANITIZER COVERAGE: the DLL is uninstrumented by
    # construction, and an instrumented process cannot host a second sanitizer runtime anyway.
    # Keeping the coverage means running the sanitizers over the STATIC half, which is why this is
    # recorded as a cost of the flip rather than fixed here.
    if(NOT MSVC)
        target_compile_options(deren_vulkan PRIVATE -fno-sanitize=all)
        target_link_options(deren_vulkan PRIVATE -fno-sanitize=all)
    endif()

target_sources(deren_vulkan
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        vulkan/core/core.cppm
        vulkan/core/core.declarations.cppm
        vulkan/core/core.constructor.cppm
        vulkan/core/vma/vma.cppm
        vulkan/core/descriptor_heap/descriptor_heap.cppm
        vulkan/core/vma/handles/vma_handles.cppm
        vulkan/core/handles/handles.cppm
        vulkan/core/init_utils/init_utils.cppm
        vulkan/init_utils/init_utils.cppm
        vulkan/core/pipeline/pipeline.cppm
        vulkan/core/pipeline/spirv_parser.cppm
        # (vulkan/constant_init/constant_init.cppm MOVED OUT: it is `vulkan_constant_init` now - the
        #  engine imports it in 26 places and a backend module cannot serve the flip - see that
        #  target's comment above deren_vulkan)
    PRIVATE
        vulkan/core/core.cpp
        # The RHI contract's virtuals for this backend (plan §11.4, S1-C): `core` derives from
        # `deren::promise::rhi::api_core`, so this is where abilities() and the frame calls live.
        vulkan/core/core.api_core.cpp
        # The ONE C entry point, DEFINED for the real backend (promise/rhi/backend_entry.hpp, abi 18).
        # A plain TU rather than a module implementation unit, because the entry has to be reachable BY
        # NAME from an image that imports no module of this target - the same reason
        # tests/probe_backend.cpp is a plain TU. With this target SHARED it is the dllexport side of the
        # boundary, and the export table carries exactly this one `deren_*` name.
        vulkan/core/core.entry.cpp
        vulkan/core/handles/handles.cpp
        vulkan/init_utils/init_utils.cpp
        vulkan/core/pipeline/pipeline.cpp
        third_party/spirv-reflect/spirv_reflect.c
)
# The error translators (see the target above): a static library, so the DLL carries its own copy of
# them - which is correct, because the ENGINE never calls them; the unit test links the same target
# and gets the identical code without importing the backend's module.
target_link_libraries(deren_vulkan
        vulkan_error_tables
        # The backend implements the contract, so the contract's module is a dependency of THIS target
        # and of nothing else in the backend's direction (promise/rhi/*.cppm imports nothing back).
        promise
        # The Vulkan constexpr builders both halves compile (see the target's comment above): the
        # backend keeps using them unchanged, they simply stop being a backend-owned module.
        vulkan_constant_init
        Vulkan::Vulkan
        glfw
        static_utility
        glm::glm
        ${CMAKE_DL_LIBS}
        vstd_lib
)
target_include_directories(deren_vulkan
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# ================================================================================================
# THE RENDERER RUNTIME, AND WHY THERE IS EXACTLY ONE NOW (the dynamic-backend migration's S5, the flip).
#
# `runtime/` at the repository root IS the renderer runtime. It reaches the backend ONLY through the
# contract (`deren_make_api_core()` + `rhi::api_core` + the raw-handle escape) and never names `core`.
# `vulkan/runtime/` - the runtime that DID name it and that lived inside the backend's own directory -
# is deleted with this batch. See docs/dynamic_runtime.md for the design.
#
# THE `-DVR_RUNTIME=legacy|dynamic` OPTION IS RETIRED WITH THOSE FILES, and that is the honest shape
# rather than a tidy-up: the option existed because two BMIs of one module name cannot both be visible
# to one translation unit, so a single build tree could not host both runtimes, and the LEGACY tree was
# the CONTROL for the dynamic one (both had to read the same fourteen render hashes). With the legacy
# sources gone there is nothing to select between and nothing to control, so the file list below is
# simply what `vulkancorekit` gets. `VR_RUNTIME_SERVES_THE_APP` retires in the same move: it gated the
# application, `chores` and `render_start_demo` while the port was incomplete (S1-S3) and has been
# unconditionally ON since S4's file-list swap.
#
# THE LEGACY BUILD TREE (`build-release-clang64`) IS RETIRED AS A CONFIGURATION, and its cache still
# names the old option: re-configuring that directory now builds exactly what the surviving tree
# (`build-release-dyn-clang64`) builds, because there is no switch to leave stale. Nothing in the
# repository points at it any more - the gates run in the surviving tree.
# ================================================================================================
set(VR_RUNTIME_MODULES
    runtime/runtime.cppm
    runtime/runtime.declarations.cppm
    runtime/runtime.constructor.cppm
    runtime/runtime.frames.cppm
    runtime/runtime.probes.cppm
    runtime/runtime.readback.cppm
)
set(VR_RUNTIME_PRIVATE_SOURCES
    runtime/runtime.cpp
)
# The two places the app owns, named once so the target lists below read the same everywhere.
set(VR_FULL_API_TARGETS deren chores)
set(VR_APP_EXE deren)
set(VR_FULL_API_MODULES vulkan/render_start_demo/render_start_demo.cppm)
set(VR_FULL_API_SOURCES vulkan/render_start_demo/render_start_demo.cpp)

add_library(vulkancorekit STATIC)

target_sources(vulkancorekit
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        vulkan/core/filter/filters.cppm
        vulkan/math/math.cppm
        ${VR_RUNTIME_MODULES}
        vulkan/bindings/bindings.cppm
        vulkan/pipelines/pipelines.cppm
        vulkan/render_resource/render_resource.cppm
        vulkan/render_resource/shared.cppm
        vulkan/frame_constants/frame_constants.cppm
        vulkan/pass/pass.cppm
        vulkan/pass/taa.cppm
        vulkan/pass/scene.cppm
        vulkan/pass/transparent.cppm
        vulkan/pass/character_forward.cppm
        vulkan/pass/toon_screen_rim.cppm
        vulkan/pass/goo_rim.cppm
        vulkan/pass/megalights_trace.cppm
        vulkan/pass/megalights_temporal.cppm
        vulkan/pass/ray_traced_shadow.cppm
        vulkan/pass/mask_bake.cppm
        vulkan/pass/compute_skin.cppm
        vulkan/pass/cluster.cppm
        vulkan/pass/deferred.cppm
        vulkan/pass/post.cppm
        vulkan/pass/fxaa.cppm
        vulkan/pass/upscale.cppm
        vulkan/pass/geometry_buffer_debug.cppm
        vulkan/pass/shadow.cppm
        vulkan/pass/chain.cppm
        ${VR_FULL_API_MODULES}
        vulkan/profiling/profiling.cppm
        vulkan/shadow_fit/shadow_fit.cppm
        vulkan/acceleration_structure/acceleration_structure.cppm
        vulkan/ray_tracing/ray_tracing.cppm
        vulkan/scene_tree/scene_tree.cppm
        vulkan/meshlet/meshlet.cppm
        vulkan/primitive/primitive.cppm
        vulkan/render_environment/render_environment.cppm
        vulkan/graphical_user_interface/graphical_user_interface.cppm
        vulkan/animation/controller.cppm
        vulkan/animation/mmd_motion.cppm
    PRIVATE
        vulkan/core/filter/filters.cpp
        vulkan/math/math.cpp
        ${VR_RUNTIME_PRIVATE_SOURCES}
        vulkan/pass/taa.cpp
        vulkan/pass/scene.cpp
        vulkan/pass/transparent.cpp
        vulkan/pass/character_forward.cpp
        vulkan/pass/toon_screen_rim.cpp
        vulkan/pass/goo_rim.cpp
        vulkan/pass/megalights_trace.cpp
        vulkan/pass/megalights_temporal.cpp
        vulkan/pass/ray_traced_shadow.cpp
        vulkan/pass/mask_bake.cpp
        vulkan/pass/compute_skin.cpp
        vulkan/pass/cluster.cpp
        vulkan/pass/deferred.cpp
        vulkan/pass/post.cpp
        vulkan/pass/fxaa.cpp
        vulkan/pass/upscale.cpp
        vulkan/pass/geometry_buffer_debug.cpp
        vulkan/pass/shadow.cpp
        ${VR_FULL_API_SOURCES}
        vulkan/shadow_fit/shadow_fit.cpp
        vulkan/acceleration_structure/acceleration_structure.cpp
        vulkan/ray_tracing/ray_tracing.cpp
        vulkan/scene_tree/scene_tree.cpp
        vulkan/primitive/primitive.cpp
        vulkan/graphical_user_interface/graphical_user_interface.cpp
        vulkan/animation/controller.cpp
        vulkan/animation/mmd_motion.cpp
)
add_library(gltf_loader STATIC)
target_sources(gltf_loader
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        gltf_loader/gltf_loader.cppm
        # The TOON MATERIAL SIDECAR is a module of its own rather than more code in gltf_loader, because what
        # it reads is not glTF: a glTF material carries the spec's five texture slots and six factors, and a
        # toon character needs a second set (a diffuse ramp, a shadow LUT, a specular ramp, a matcap, a face
        # SDF) that glTF has no concept of and that lives in a sidecar keyed by MATERIAL NAME. Two formats,
        # two concerns, two modules - and it is in this target because it is a model-loading concern.
        gltf_loader/toon_material_sidecar.cppm

    PRIVATE
        gltf_loader/gltf_loader.cpp
        gltf_loader/toon_material_sidecar.cpp
)

# THE EXTRAS PARSE CALLBACK'S ONE HEADER, and it is SYSTEM on purpose: fastgltf reports a material's `extras`
# block by CALLING BACK with a `simdjson::dom::object*` (see its `ExtrasParseCallback`), so the loader's own
# translation unit has to know that type - and `simdjson.h` is upstream code whose warnings this project does
# not own, exactly like the vendored libraries above. PRIVATE, because nothing that CONSUMES gltf_loader ever
# sees a simdjson type: the extras are copied out into `deren::gltf::material`'s own maps before `load_model` returns.
target_include_directories(gltf_loader
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader
)

# fastgltf + simdjson, vendored under third_party/ (the sources ship in the repo, so no
# network FetchContent is needed and both Windows/MSYS2 and Linux build self-contained).
# fastgltf's headers deliberately do NOT include simdjson (it is hidden inside the .cpp
# files), so only two translation units ever see it: this library's own sources, and
# gltf_loader's (which needs the type because fastgltf's EXTRAS PARSE CALLBACK hands it a
# `simdjson::dom::object*` - see the SYSTEM include on that target, which is what keeps a
# vendored header out of this project's -Werror set). simdjson is compiled from its amalgamated
# single-header/single-source pair (simdjson.h + simdjson.cpp), and the three fastgltf
# translation units are compiled here as plain C++ (vendored code does not follow the
# project's -Werror set, mirroring the imgui/xxhash policy).
add_library(fastgltf_vendored STATIC
        third_party/fastgltf/src/fastgltf.cpp
        third_party/fastgltf/src/base64.cpp
        third_party/fastgltf/src/io.cpp
        third_party/simdjson/singleheader/simdjson.cpp
)

target_include_directories(fastgltf_vendored
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fastgltf/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader
)

# stb_image.h (vendored): gltf_loader.cpp decodes embedded/external textures with it
# (#define STB_IMAGE_IMPLEMENTATION + #include <stb/stb_image.h>).
add_library(stb_vendored INTERFACE)

target_include_directories(stb_vendored
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Dear ImGui (vendored under third_party/imgui, see the sources there): the Vulkan + GLFW
# backends are compiled as plain C++ (imgui is not a module and does not follow the project's
# -Werror policy — upstream code triggers warnings we do not own). No clang-format: the
# PROJECT_FORMAT_SOURCES glob only covers main/application_configuration/gltf_loader/utility/
# vulkan/tests/promise, never third_party.
add_library(imgui STATIC)

target_sources(imgui
    PRIVATE
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_glfw.cpp
        third_party/imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
)

target_link_libraries(imgui
    PUBLIC
        Vulkan::Vulkan
        glfw
)

# xxHash (vendored under third_party/xxhash, see the LICENSE there): non-cryptographic
# content hashing (XXH3_128bits) used by deren::utility::xxh3_128bits for image/texture dedup.
# Compiled as its own plain-C target, mirroring the imgui policy above (vendored
# code does not follow the project's -Werror set). static_utility links it (the hash functions are
# pure and per-half - they are not process-wide state).
add_library(xxhash STATIC
        third_party/xxhash/xxhash.c
)

target_include_directories(xxhash
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash
)

# mimalloc (vendored under third_party/mimalloc, v3.4.4, MIT — see the LICENSE there):
# allocator backing deren.utility.shared:better_pmr, which routes every std::pmr allocation through
# it (shared_utility.cppm -> :better_pmr -> mi_aligned_alloc / mi_free_aligned). It moved to
# shared_utility with the allocator hook, because setting the process's default std::pmr resource is
# process-wide state. Compiled as
# its own plain-C static target from the upstream mi_sources set, mirroring the imgui /
# xxhash policy (vendored code does not follow the project's -Werror set; headers are
# SYSTEM so the -Werror consumers that include <mimalloc.h> do not see upstream warnings).
add_library(mimalloc_vendored STATIC
        third_party/mimalloc/src/alloc.c
        third_party/mimalloc/src/alloc-aligned.c
        third_party/mimalloc/src/alloc-posix.c
        third_party/mimalloc/src/arena.c
        third_party/mimalloc/src/arena-meta.c
        third_party/mimalloc/src/bitmap.c
        third_party/mimalloc/src/heap.c
        third_party/mimalloc/src/init.c
        third_party/mimalloc/src/libc.c
        third_party/mimalloc/src/options.c
        third_party/mimalloc/src/os.c
        third_party/mimalloc/src/page.c
        third_party/mimalloc/src/page-map.c
        third_party/mimalloc/src/random.c
        third_party/mimalloc/src/stats.c
        third_party/mimalloc/src/theap.c
        third_party/mimalloc/src/threadlocal.c
        third_party/mimalloc/src/prim/prim.c
)

target_include_directories(mimalloc_vendored
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include
)

if(WIN32)
    # upstream links psapi/shell32/user32/advapi32/bcrypt on Windows (see mimalloc's
    # CMakeLists.txt mi_libraries); _WIN32_WINNT=0x600 matches the upstream MINGW define
    target_compile_definitions(mimalloc_vendored PRIVATE _WIN32_WINNT=0x600)
    target_link_libraries(mimalloc_vendored PUBLIC psapi shell32 user32 advapi32 bcrypt)
endif()

target_link_libraries(gltf_loader
        PRIVATE
        fastgltf_vendored
        stb_vendored
        vstd_lib
        static_utility
)

# gltf_loader is deliberately absent here: the engine core is loader-agnostic (see the
# library comment above). The executable links gltf_loader itself (main.cpp is its only
# consumer) and passes the parsed deren::gltf::scenes down to the runtime at the call site.
target_link_libraries(vulkancorekit
        PUBLIC
        Vulkan::Vulkan
        glfw
        static_utility
        glm::glm
        ${CMAKE_DL_LIBS}
        vstd_lib
        imgui
        # The constexpr builders the engine imports directly (26 sites): the module is
        # neutral (compiled once for both halves), which is what takes those imports off the flip
        # gate's meter - see the target's comment above deren_vulkan.
        vulkan_constant_init
)

# Vendored VMA (third_party/vma/vk_mem_alloc.h): vma.cppm includes <vma/vk_mem_alloc.h>,
# resolved against this third_party include root. The header also ships in the Vulkan SDK's
# Include/vma, but Linux distro Vulkan packages do not provide it, so the vendored copy is
# preferred (it precedes Vulkan::Vulkan's include dirs). Marked SYSTEM so upstream VMA's
# warnings are suppressed (the SDK copy is an isystem include for the same reason - VMA code
# trips -Werror on nullability annotations otherwise).
target_include_directories(vulkancorekit
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# The engine side gets the backend's modules (BMIs) and symbols from deren_vulkan. PUBLIC, because a
# static library's link interface is what carries the module file set to the exe and to the tests
# (§11.2): vulkancorekit's own name and its outward link surface are unchanged by S1-A.
# THE ENGINE DOES NOT LINK THE BACKEND ANY MORE, and that is what makes the backend a DLL rather than
# an auto-import library: the runtime resolves `deren_make_api_core` BY NAME out of deren_vulkan.dll
# (runtime/runtime.constructor.cppm, through deren.utility.dynamic_link, detaching the handle so the
# image is never unloaded - the measured FreeLibrary deadlock), and the boundary's one whitelisted
# symbol disappears from the link with it. The module BMIs this line used to carry PUBLIC were needed
# by NOTHING on the engine side (the import meter reads 0 sites / 0 files); the tests that do import a
# backend module link deren_vulkan themselves.
#
# `deren_vulkan` still exists as a target and CMake still knows how to build it - it is simply not in
# this link interface. The APP depends on it as a build ORDER (the DLL must exist before a test runs).

# THE APPLICATION (the runtime switch and the S1-S3 gate that used to wrap this are retired: see the
# comment above `vulkancorekit`).
add_executable(deren main.cpp)
# Release builds link as a Windows GUI-subsystem executable: no console window appears when
# running. On the MSYS2 clang64 / MinGW-w64 toolchain WIN32_EXECUTABLE maps to -mwindows; the CRT
# still calls main(), only the subsystem changes. Debug keeps the console for terminal log output.
set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE $<$<CONFIG:Release>:ON>)

# static_utility: the per-half toolkit. utility.cpp implements the xxHash-based hash functions
# (deren::utility::xxh3_128bits) on top of the vendored xxhash library, so the dependency is
# declared here, on the library that actually uses the XXH3_128bits symbol. It propagates to
# consumers (vulkancorekit / gltf_loader / deren) at link time, so the executable does not need to
# link xxhash itself. The platform_* TUs live here too - they are per-half by nature (no state).
target_link_libraries(static_utility
        # The verdict family (pass_failure / fatal) carries the contract's error_info as a value, so
        # the contract's module is a dependency of THIS target. The direction stays one-way: the
        # contract imports nothing back (promise/rhi/*.cppm), and nothing in utility crosses a
        # process boundary carrying one - these are same-binary types.
        promise
        vstd_lib
        xxhash
        # PUBLIC because this module re-exports deren.utility.shared: every consumer that imports
        # `deren.utility` needs that module's BMI (and, at the flip, its import library) as well.
        shared_utility
)

# shared_utility: the process-wide half. better_pmr routes std::pmr allocations through the
# vendored mimalloc (third_party/mimalloc), so the dependency moved here with the allocator hook -
# the executable no longer needs MSYS2's libmimalloc.dll. vstd_lib is all it needs besides: the
# module imports NEITHER the contract nor static_utility.
target_link_libraries(shared_utility
        vstd_lib
        mimalloc_vendored
)

# ============================================================================================
# backend_loader - WHERE THE BACKEND IS LOADED AND THE DEVICE ROOT IS ACQUIRED (batch ⑥).
#
# WHY A TARGET OF ITS OWN: the runtime used to do this (it loaded deren_vulkan.dll, resolved the one
# export and called it before its own construction), so the APPLICATION could not answer which
# backend, from where, with which search rules, or what a failure means - and the renderer owned a
# policy that is not its job. The module now sits between them:
#
#     auto core = deren::vulkan::load_api_core(core_options);   // main.cpp (the app decides)
#     deren::vulkan::runtime runtime{core, core_options};       // the renderer (given a root)
#
# NOT IN static_utility, deliberately: that half is the pure-CPU toolkit with no loading semantics,
# and deren.utility.dynamic_link (which it owns) is the MECHANISM - open a named image, resolve a
# named symbol - not a policy about backends. This target is the policy: the file name, the search
# rules, the creation-side handshake, the named diagnosis. It links BOTH the toolkit and glfw, because
# it is also the module that crosses the image boundary and therefore translates the caller's window
# handle into the native one the DLL can understand (see the module's banner).
# ============================================================================================
add_library(backend_loader STATIC)
target_sources(backend_loader
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        runtime/backend_loader.cppm
)
target_link_libraries(backend_loader
        promise          # the contract: create_info / api_core / abi_version
        static_utility   # deren.utility (executable_directory / the diagnosis channel) + dynamic_link
        glfw             # glfwGetWin32Window: the ONE field translated for the wire
)

# The file dialog (utility/platform_dialog.cpp, for the config's `model = "ask"`) is GetOpenFileNameW,
# which lives in the common dialog library. Windows-only, like the backend itself.
if(WIN32)
    target_link_libraries(static_utility comdlg32)
endif()

target_link_libraries(app_config
        static_utility
        vstd_lib
)

# Demo bootstrap chores (root-level module, next to main.cpp): main's startup-config analysis
# (analyse_config: config-file + argv merge, shaders/model location) and shader-loading /
# pipeline helpers, kept out of main.cpp. Split like the other modules: chores.cppm
# (export import std pattern) declares, chores.cpp implements. Demo/application layer only -
# these helpers know about app_config (settings), the runtime (deren.vulkan.runtime) and utility
# log/panic, never about glTF.
#
# `chores` takes `runtime&` and therefore names the full runtime API - which is exactly what the ONE
# runtime now serves (its S1-S3 gate retired with the legacy runtime: see `vulkancorekit`'s comment).
add_library(chores STATIC)

target_sources(chores
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        chores.cppm

    PRIVATE
        chores.cpp
)

target_link_libraries(chores
        app_config
        vulkancorekit
        static_utility
        vstd_lib
)

target_link_libraries(${PROJECT_NAME}
        backend_loader   # main.cpp calls load_api_core(): the app owns the acquisition (batch ⑥)
        static_utility
        vulkancorekit
        chores
        gltf_loader
        app_config
        vstd_lib
        glm::glm
        imgui
)
# BUILD ORDER, NOT LINKAGE: the runtime resolves deren_make_api_core out of deren_vulkan.dll at run
# time (runtime/runtime.constructor.cppm), so the DLL has to exist beside the executable - but the
# executable must NOT link it (an import library would load the backend before main, which is the
# load-time binding the flip removes). A missing DLL is then a NAMED loader panic instead of a system
# 'entry point not found' dialog. shared_utility.dll comes in through static_utility's link interface;
# listing deren_vulkan here also drags it in, and the explicit pair below documents the package: the
# backend ships as deren_vulkan.dll + shared_utility.dll, both beside the executable.
add_dependencies(${PROJECT_NAME} deren_vulkan shared_utility)

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
            # why this is Release-only. (The exe logs to debug.log, so no console is needed to see
            # its output - README.md, "Release builds are Windows GUI-subsystem executables".)
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
else()
    target_link_options(${PROJECT_NAME} PRIVATE
            # drop unused function/data sections at link (first-party code is compiled with
            # -ffunction-sections -fdata-sections; see the vendored-lib gc test below)
            $<$<CONFIG:Release>:-Wl,--gc-sections>
    )

    # Release exe: strip all symbols at link time (smaller single-file build; llvm-strip saves
    # ~0.8 MB here). Debug keeps its symbols for debugging. Crash reports on a shipped build are
    # symbol-less by design - keep the unstripped exe around if you need to map addresses.
    target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:-s>
    )
endif()

# ---------------------------------------------------------------------------------------------
# Headless unit tests (no GPU, no external test framework - see tests/vk_test.h): each test is
# an ordinary module consumer that imports the module under test and links its library, so the
# tests compile with the exact interfaces the rest of the code uses. CTest runs them in a
# per-build scratch directory (module logging writes debug.log there, not into the source
# tree); tests that open fixtures/models receive the source root via VR_TEST_SOURCE_DIR.
# ---------------------------------------------------------------------------------------------
option(VR_BUILD_TESTS "Build the headless unit tests" ON)

if(VR_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME test_backend_boundary
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_backend_boundary.py -v)
    set_tests_properties(test_backend_boundary PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run"
        ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/test-run;TEMP=${CMAKE_CURRENT_BINARY_DIR}/test-run")
    # 完整产品先构建，再量 main/chores/引擎归档，避免半构建产生假性下降。
    add_custom_target(check-backend-boundary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_backend_boundary.py"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
                --report "${CMAKE_CURRENT_BINARY_DIR}/backend_boundary_report.json"
        DEPENDS deren
        VERBATIM)

    set(VR_TEST_TARGETS test_app_config test_utility test_math test_animation test_gltf_loader test_toon_material_sidecar test_shadow_fit test_render_resources test_pass test_shader_sources test_meshlet test_goo_toon_math test_docs test_dynamic_link test_vulkan_capabilities test_texture_upload_layout test_error_mapping test_runtime_injection)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run")

    foreach(test_target IN ITEMS ${VR_TEST_TARGETS})
        add_executable(${test_target} tests/${test_target}.cpp)
        add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
        set_tests_properties(${test_target} PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run"
                TIMEOUT 120)
    endforeach()

    # Execute the real module-partition query bodies with driver query doubles.
    # This test links no Vulkan loader and needs no GPU or window.
    set(VR_VULKAN_QUERY_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/test-generated/vulkan_query_fixture.h")
    set(VR_TEXTURE_REGIONS_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/test-generated/texture_regions_fixture.h")
    add_custom_command(OUTPUT "${VR_VULKAN_QUERY_FIXTURE}" "${VR_TEXTURE_REGIONS_FIXTURE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_vulkan_query_fixture.py"
                --source "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/core/init_utils/init_utils.cppm"
                --output "${VR_VULKAN_QUERY_FIXTURE}"
                --vma-source "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/core/vma/vma.cppm"
                --regions-output "${VR_TEXTURE_REGIONS_FIXTURE}"
        DEPENDS tests/generate_vulkan_query_fixture.py vulkan/core/init_utils/init_utils.cppm vulkan/core/vma/vma.cppm
        VERBATIM)
    target_sources(test_vulkan_capabilities PRIVATE "${VR_VULKAN_QUERY_FIXTURE}")
    target_include_directories(test_vulkan_capabilities PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/test-generated" "${Vulkan_INCLUDE_DIRS}")
    target_include_directories(test_texture_upload_layout PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/core/vma" "${Vulkan_INCLUDE_DIRS}" "${CMAKE_CURRENT_BINARY_DIR}/test-generated")
    target_sources(test_texture_upload_layout PRIVATE "${VR_TEXTURE_REGIONS_FIXTURE}")

    # source root for fixture/model paths (tests no longer assume the repo root is the cwd)
    target_compile_definitions(test_app_config PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(test_gltf_loader PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_toon_material_sidecar reads the sidecars under tests/fixtures/toon/, so it needs the source root
    # for the same reason test_gltf_loader does - and it asserts the MISSING-file path too, which is a path
    # that must NOT exist, so the root has to be the real one rather than the build tree.
    target_compile_definitions(test_toon_material_sidecar PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_render_resources reads shaders/heap_slots.glsl and vulkan/core/core.declarations.cppm to compare the slot grid's two
    # sources of truth (see the last block of that test): both are text, neither is generated from the other.
    target_compile_definitions(test_render_resources PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_shader_sources parses the FOUR places a .spv is named (VR_SLANG_SOURCES, the two compile scripts, and
    # chores.cpp's load_shader calls) and compares them: one shader built by the build and not by the scripts is
    # the drift that test exists for, and it is the mesh path's CI gate (see the test's header).
    target_compile_definitions(test_shader_sources PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_goo_toon_math evaluates the REWRITTEN toon chain's iris arithmetic (read out of the reference's own
    # node graph) and checks the SOURCES' TEXT for the five places a new toon lane has to be spelled - the lane
    # enum, the upload format table, the sidecar slot/flag names, the colour-row name and the two neutral tables.
    # None of those is a compiler-checkable contract: a missing one reads another material's data rather than
    # failing (see `deren::vulkan::toon_colour_lane`'s note on the dedup key). It reads those files, so it needs the
    # source root and no library at all - like test_meshlet, it must run in CI, where there is no device.
    target_compile_definitions(test_goo_toon_math PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_docs parses the WRITTEN artifacts the manual and CI are built from - Doxyfile's INPUT, the
    # documents themselves, CMakeLists' VR_TEST_TARGETS and the workflow that mirrors it - so it needs
    # the source root and no library at all (see the test's header for the invariants).
    target_compile_definitions(test_docs PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # deren.vulkan.meshlet is pure CPU alongside vulkan.math: the split's coverage/bounds/determinism checks need no
    # device, which is the only way they can run in CI (the capture gate's references are machine-specific).
    target_link_libraries(test_meshlet
            vulkancorekit
            vstd_lib
    )

    target_link_libraries(test_app_config
            app_config
            vstd_lib
    )
    target_link_libraries(test_utility
            static_utility
            glm::glm
            vstd_lib
    )
    # deren.vulkan.math lives in vulkancorekit; the module itself is pure CPU (see math.cppm), the
    # link chain just rides the library's PUBLIC dependencies (Vulkan/glfw are not pulled in
    # unless the math code actually references them).
    target_link_libraries(test_math
            vulkancorekit
            vstd_lib
    )
    # deren.vulkan.render_resource is pure CPU on purpose (its own enums, no Vulkan type): the invariants it exists
    # for - usage is a subset of the schema, kind and access fit, a pass's own bindings are contiguous, a pool
    # count is derivable - are checkable WITHOUT a device, which is the only way they can be checked in CI.
    target_link_libraries(test_render_resources
            vulkancorekit
            vstd_lib
    )
    # deren.vulkan.pass is the framework: its base class and behaviour kinds name Vulkan handle types but never call
    # Vulkan, so its contract - resolve, apply the behaviour, record, mark, recreate - is testable with a fake
    # host and no device at all.
    target_link_libraries(test_pass
            vulkancorekit
            vstd_lib
    )
    # deren.vulkan.animation is format-neutral / runtime-agnostic; the sampling tests only touch the
    # pure functions, so nothing Vulkan is pulled in.
    target_link_libraries(test_animation
            vulkancorekit
            glm::glm
            vstd_lib
    )
    # gltf_loader is pure CPU (fastgltf + stb vendored, linked by the library itself); the
    # test loads the repo's gltf_model/DamagedHelmet sample from the working directory.
    target_link_libraries(test_gltf_loader
            gltf_loader
            glm::glm
            vstd_lib
    )
    # The sidecar reader lives in the gltf_loader TARGET (it is a model-loading concern) but not in the
    # gltf_loader MODULE, so this test links the same library and imports a different module.
    target_link_libraries(test_toon_material_sidecar
            gltf_loader
            vstd_lib
    )
    # deren.vulkan.shadow_fit is pure CPU arithmetic (no Vulkan object is touched), and the module lives in
    # vulkancorekit next to deren.vulkan.math; the link chain brings Vulkan/glfw in, which the tests do not
    # use. This is the first test of the shadow system, which had none: the fit had no device
    # dependency, it just lived where nothing could reach it.
    target_link_libraries(test_shadow_fit
            vulkancorekit
            glm::glm
            vstd_lib
    )

    # ---- the RHI boundary's loader, and the fixture its test loads ----
    # probe_backend is a stand-in for the C ABI over promise/rhi/: ONE source file compiled twice, as a DLL (the
    # export macro in its "building a DLL" mode) and as a static library (macro inert). test_dynamic_link
    # therefore sees both halves of DEREN_API_EXPORT at once: it opens the DLL by path and calls the
    # static half directly, and the two must agree. The duplication of the source is the point - it is
    # the only way to exercise both modes without a second copy to drift from the first.
    #
    # THE SANITIZER JOB IS WHY -fno-sanitize=all IS HERE. That job compiles with
    # CMAKE_CXX_FLAGS=-fsanitize=address,undefined, but only CMAKE_EXE_LINKER_FLAGS carries the runtime:
    # an instrumented DLL would not link at all, and if it did it would put a SECOND ASan runtime into
    # the instrumented process, which aborts on load. The DLL is loaded BY a sanitized process and must
    # not be sanitized itself; the static half is linked into the test and IS instrumented.
    add_library(probe_backend SHARED tests/probe_backend.cpp)
    target_compile_definitions(probe_backend PRIVATE DEREN_API_SHARED DEREN_API_BUILD)
    target_compile_options(probe_backend PRIVATE ${COMPILE_OPTIONS})
    target_link_libraries(probe_backend PRIVATE promise)
    if(NOT MSVC)
        target_compile_options(probe_backend PRIVATE -fno-sanitize=all)
        target_link_options(probe_backend PRIVATE -fno-sanitize=all)
    endif()
    # Next to the test executables, which is where test_dynamic_link looks for it: the test asks argv[0]
    # for its own directory at run time, so no path is ever spelled inside a string literal (a Windows
    # separator in one is exactly the bug that only shows up on the machine that has one).
    set_target_properties(probe_backend PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    add_library(probe_backend_static STATIC tests/probe_backend.cpp)
    target_compile_options(probe_backend_static PRIVATE ${COMPILE_OPTIONS})
    # The same promise module the DLL half reads: the two halves compile one interface unit each,
    # which is the "both sides compile the contract" rule in §4.1, not a shared binary.
    target_link_libraries(probe_backend_static PRIVATE promise)

    target_link_libraries(test_dynamic_link
            promise
            static_utility
            probe_backend_static
            vstd_lib
    )
    # THE INJECTION TEST (batch ⑥): the engine builds a renderer from a PROBE root, with no loader, no
    # DLL and no device - which is the layering's own evidence - and covers the consumption-side
    # handshake by running the panic case in a CHILD process of itself (CreateProcessW, so the exit
    # status is not translated by a shell). It links the SAME probe TU the C entry lives in, so the knob
    # (`probe_make_core` with a chosen `api_version()`) cannot drift from the entry's own behaviour.
    target_link_libraries(test_runtime_injection
            promise
            vulkancorekit
            static_utility
            probe_backend_static
            vstd_lib
    )
    # The file NAME, not a path: the test composes the path from its own location (see above), so the
    # name stays correct whichever prefix/suffix the platform's shared-library convention uses.
    target_compile_definitions(test_dynamic_link PRIVATE
            VR_TEST_PROBE_BACKEND_FILE_NAME="$<TARGET_FILE_NAME:probe_backend>")
    # `cmake --build --target test_dynamic_link` has to build the DLL it opens, or ctest would run a
    # test whose fixture is missing and report a loader error as a test failure.
    add_dependencies(test_dynamic_link probe_backend)

    # The error mechanism's translation tables (vulkan/core/core.error.cpp), fed every plausible
    # VkResult and asserted per call-site family. Pure CPU: the backend archive is linked for the
    # translators' definitions only - the test creates no device, no window, no instance - and the
    # contract module supplies the unified values (rhi::error / error_info / zone_of).
    target_link_libraries(test_error_mapping
        vulkan_error_tables
            promise
            # NO deren_vulkan: this test imports no backend module any more (the flip moved the
            # tables into `vulkan_error_tables`, linked below), so it neither links the backend
            # nor loads its DLL.
            vstd_lib
    )

    # ---- the boundary spike (DYNAMIC_LINK_V2.md §4 step 1b) -------------------------------------
    # NOT in VR_TEST_TARGETS on purpose: it loads a DLL and (with --with-device) creates a real
    # context, so it cannot belong to the headless ctest set - and tests/test_docs.cpp compares that
    # list against the workflow, so putting it there would be a lie in CI. It is registered as a test
    # only in the spike tree, where the option is on and CI is not the consumer.
    add_executable(test_backend_boundary_spike tests/spike_backend_boundary.cpp)
    target_compile_options(test_backend_boundary_spike PRIVATE ${COMPILE_OPTIONS})
    # The ABSOLUTE path of the library under test: the spike's whole Q5 is that loading by
    # absolute path uses LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS and
    # therefore never searches %PATH%.
    target_compile_definitions(test_backend_boundary_spike PRIVATE
            VR_SPIKE_BACKEND_DLL="$<TARGET_FILE:deren_vulkan>")
    target_link_libraries(test_backend_boundary_spike
            promise
            static_utility
            vstd_lib
    )
    # The library the test opens has to exist first, or the run reports a loader error as a test
    # failure (the same rule test_dynamic_link lives by).
    add_dependencies(test_backend_boundary_spike deren_vulkan)
    # NOT `add_test`: this spike loads a DLL and, with --with-device, builds a real context, so it
    # cannot be in the headless ctest set (tests/test_docs.cpp compares that set against the
    # workflow). It is RUN by the batch that touches the boundary, which is what its report is for.
endif()

# ---- the runtime's scaffold test (the flip's step 2) -------------------------------------------
# IT EXISTS SO THE PORT CANNOT ROT: the runtime was ported partition by partition, and this executable -
# built and RUN in every slice of that work - is what proves the construction path
# (`deren_make_api_core()` -> `shared_ptr<api_core>` with the backend's deleter -> the face -> the raw
# handle escape -> teardown) still holds. Since S5 it is built in the ONE configuration there is.
#
# SPIKE-SHAPED ON PURPOSE, like `test_backend_boundary_spike` above and for the same reasons: it creates a
# real device (so it cannot be in the headless ctest set, and tests/test_docs.cpp compares that list
# against the workflow) and it is NOT in VR_TEST_TARGETS.
add_executable(test_runtime_dyn tests/test_runtime_dyn.cpp)
target_compile_options(test_runtime_dyn PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(test_runtime_dyn
        backend_loader   # the scaffold calls load_api_core() explicitly (batch ⑥)
        vulkancorekit
        promise
        static_utility
        vstd_lib
)
# Same build-order rule as the app: this test CONSTRUCTS a runtime, so the DLL it resolves at run time
# has to exist first (it does not link it).
add_dependencies(test_runtime_dyn deren_vulkan)

# ---- THE ACCELERATION-STRUCTURE DEVICE PROBE (plan S1's P0) ------------------------------------------
#
# SPIKE-SHAPED LIKE `test_runtime_dyn` ABOVE, for the same reason (it creates a real device, so it cannot
# belong to the headless ctest set) - and it is the SMALL instrument the acceleration-structure migration runs
# against: a three-vertex triangle, one instance, a hidden 64x64 window and VALIDATION ON, where the repo's
# only other ray-tracing instrument is the full Sponza scene at 1080x960 (see the file's own header comment).
add_executable(test_acceleration_structures tests/test_acceleration_structures.cpp)
target_compile_options(test_acceleration_structures PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(test_acceleration_structures
        backend_loader   # load_api_core() explicitly, exactly as the scaffold test does
        vulkancorekit
        promise
        static_utility
        vstd_lib
)
# Same build-order rule again: this probe resolves deren_vulkan.dll at run time (it does not link it).
add_dependencies(test_acceleration_structures deren_vulkan)

# ============================================================================================
# deren_gui_vulkan: THE GUI AS ITS OWN PLUGIN (plan X2, the user's ruling: one DLL per API).
#
# WHAT LIVES HERE: the vendored ImGui sources (through the `imgui` static library, PRIVATE) and the whole
# `deren.vulkan.graphical_user_interface` module, plus the C entry that adapts the module's overlay to the
# `deren::gui::overlay` boundary (vulkan/graphical_user_interface/gui_dll.cpp). THE DLL IS WHAT LINKS
# `vulkan-1`: the host resolves `deren_make_gui` by NAME through its loader and links nothing of this target,
# so `deren.exe` stops importing vulkan-1.dll - every one of those imports came from
# `imgui_impl_vulkan.cpp`, which is compiled in here.
#
# IT IS NOT IN ANY `add_dependencies` YET: the engine still uses the module directly (the "write it, wire it,
# test it, then delete the old path" order). The next step is the loader plus the runtime's rewiring.
# ============================================================================================
add_library(deren_gui_vulkan SHARED)
set_target_properties(deren_gui_vulkan PROPERTIES PREFIX "") # deren_gui_vulkan.dll, beside the executable
target_sources(deren_gui_vulkan
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        vulkan/graphical_user_interface/graphical_user_interface.cppm
        PRIVATE
        vulkan/graphical_user_interface/graphical_user_interface.cpp
        vulkan/graphical_user_interface/gui_dll.cpp
)
target_compile_options(deren_gui_vulkan PRIVATE ${COMPILE_OPTIONS})
target_compile_definitions(deren_gui_vulkan PRIVATE DEREN_API_SHARED DEREN_API_BUILD)
target_link_libraries(deren_gui_vulkan
        PRIVATE
        imgui
        glfw
        Vulkan::Vulkan
        vulkan_constant_init
        static_utility
        vstd_lib
)


# ---- compile options: one shared flag set on every first-party C++ target (vendored code is
# deliberately excluded - imgui / fastgltf / simdjson / xxhash / mimalloc do not follow the
# project's -Werror set, see their own targets above).
foreach(compile_target IN ITEMS vstd_lib ${VR_FULL_API_TARGETS} deren_vulkan vulkan_constant_init vulkan_error_tables vulkancorekit shared_utility static_utility backend_loader promise gltf_loader app_config ${VR_TEST_TARGETS})
    target_compile_options(${compile_target} PRIVATE ${COMPILE_OPTIONS})
endforeach()
# One warning is suppressed for vstd_lib under MSVC, and the reason is the module's job description:
# vstd re-exports the standard library's surface, which necessarily includes the names C++17/20/23
# deprecated (std::iterator, std::wbuffer_convert/wstring_convert, std::rel_ops, std::is_pod,
# std::aligned_storage/aligned_union, std::float_denorm_style). MSVC's STL announces each of those
# through C4996, so with the MSVC branch's /W4 /WX the module cannot compile its own re-exports:
# measured 11 C4996 in vstd/std/{iterator(245),limits(12),locale(73,76),type_traits(338,340,342),
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
    target_compile_options(gltf_loader PRIVATE /wd4100)
else()
    target_compile_options(gltf_loader PRIVATE -Wno-unused-parameter)
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
# toml++/impl/toml_formatter.inl:94). It is reachable from application_configuration.cpp:29 alone, which dumps every
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

# Vendored libs get function/data sections AND -flto in Release too (plain C/C++ flags, no
# -Werror set involved): without sections --gc-sections can only drop whole objects, so unused
# imgui / fastgltf / xxhash / mimalloc functions would survive into the exe; without -flto the
# linker cannot see into their translation units, so cross-TU dead code stays. Both cost build
# time only - runtime speed is unaffected (LTO is at worst neutral).
# Under MSVC the sections half is /Gy /Gw and the LTO half does not exist (see the dialect table).
if(MSVC)
    set(VR_VENDORED_RELEASE_OPTIONS $<$<CONFIG:Release>:/Gy;/Gw>)
else()
    set(VR_VENDORED_RELEASE_OPTIONS $<$<CONFIG:Release>:-flto;-ffunction-sections;-fdata-sections>)
endif()

foreach(vendored_target IN ITEMS imgui fastgltf_vendored xxhash mimalloc_vendored)
    target_compile_options(${vendored_target} PRIVATE ${VR_VENDORED_RELEASE_OPTIONS})
endforeach()

target_compile_definitions(vulkancorekit PRIVATE
        $<$<CONFIG:Debug>:
        _DEBUG
        >
)

# S1-A (§11.2's fourth list): the backend half had this define while it was compiled by vulkancorekit,
# so the new target carries it too - the split is supposed to be invisible in the products.
target_compile_definitions(deren_vulkan PRIVATE
        $<$<CONFIG:Debug>:
        _DEBUG
        >
)

# -- version: single source of truth is project(VERSION) above. Injected into the C++ targets
#    that surface it: the Vulkan app_info (vulkan/core/core.cpp, applicationVersion /
#    engineVersion), the --version flag and the startup log banner (main.cpp). Bump the number
#    in project(VERSION) and keep README/docs' "current version" lines in sync.
foreach(version_target IN ITEMS vulkancorekit ${VR_APP_EXE} deren_vulkan)
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
    # promise/rhi/ holds the contract module (`*.cppm`) AND the one header that is deliberately not
    # part of it (`backend_entry.hpp`); utility/abi_export.hpp is the macro header. All three are
    # compiled by both sides of the boundary, so they are listed by extension rather than by the
    # *.cpp* wildcard the others use.
    file(GLOB_RECURSE PROJECT_FORMAT_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/application_configuration/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/gltf_loader/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/utility/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/core/vma/texture_upload_layout.h
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp*
            ${CMAKE_CURRENT_SOURCE_DIR}/promise/rhi/*.cppm
            ${CMAKE_CURRENT_SOURCE_DIR}/promise/rhi/*.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/utility/abi_export.hpp)

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

    # Format all project targets before compiling (chores is deliberately NOT in this list: a
    # root-level module source feeding both the format stamp and a target that depends on other
    # format-listed targets makes ninja see a cycle; format chores.cppm manually with
    # 'clang-format -i chores.cppm' - same style, just not automatic)
    foreach(_tgt shared_utility static_utility backend_loader promise app_config deren_vulkan vulkan_constant_init vulkan_error_tables vulkancorekit gltf_loader ${VR_APP_EXE})
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
# entry points (see utility/platform_sleep.cpp, platform_path.cpp and platform_dialog.cpp). They are
# per-half (no state), so they live in static_utility; shared_utility has its own plain TU for the
# process-wide rotation claim (utility/shared/log_rotation_claim.cpp), kept there for the same reason.
target_sources(static_utility PRIVATE
        utility/platform_sleep.cpp
        utility/platform_path.cpp
        utility/platform_dialog.cpp
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
#  - -I still points at the SOURCE directory, so #include "surface.glsl" resolves against the real
#    sources rather than against copies.
# A mirror/ POST_BUILD step was tried first and is worse: a POST_BUILD command only runs when the
# executable is RELINKED, so a shader-only edit recompiled the .spv and left the copy stale (measured).
# Note this assumes a single-configuration generator, which is what this project configures - the
# executable lands in the build directory, so build/shaders is its sibling. A multi-config generator
# would put the binary in a per-config subdirectory and the runtime would fall back to its cwd walk.
#
# shaders/compile_shaders.ps1 / .sh remain as a manual escape hatch for a machine without CMake; they
# compile in place, which is what a reader of the source tree would expect from a manual tool.
set(VR_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
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
        "TO HAVE IS GONE, along with the GLSL stage sources themselves (shaders/glsl.old/) - so a build "
        "without it would be the one that silently compiles modules the project no longer uses. Install "
        "Slang or the Vulkan SDK, or add slangc to PATH.")
endif()
# the shared includes: every generated shader lists them all as dependencies. The chain is three
# files and glslc takes milliseconds, so recompiling an unaffected shader is cheaper than the
# dependency bookkeeping that would avoid it.
set(VR_SHADER_INCLUDES
    "${VR_SHADER_SRC_DIR}/surface.glsl"
    # material_record.glsl is the Material LAYOUT, split out of surface.glsl so a FULLSCREEN stage can name
    # the type it reads from the material table - a fullscreen stage cannot include surface.glsl at all (its
    # shared scene push block is 144 bytes, over the 128-byte limit). It is here for the same reason the
    # other bodies are: a field added to it must rebuild every stage that reads the table.
    "${VR_SHADER_SRC_DIR}/material_record.glsl"
    # toon_params.slang is the TOON FAMILY TABLE, shared by every stage that shades a character (the
    # character-forward stage and the screen-space rim). It is here for the reason the bodies below are:
    # editing a family's parameters must rebuild every stage that read them, and a stage left on stale SPIR-V
    # would shade with the previous table - which looks like a tuning that did not take rather than like a
    # build error.
    "${VR_SHADER_SRC_DIR}/toon_params.slang"
    "${VR_SHADER_SRC_DIR}/shading.glsl"
    "${VR_SHADER_SRC_DIR}/sky.glsl"
    # ibl_specular.glsl is included by shading.glsl, so it belongs here for the same reason the three above
    # do: editing it alone must not leave stale SPIR-V behind.
    "${VR_SHADER_SRC_DIR}/ibl_specular.glsl"
    # heap_slot_constants.glsl is the SLOT GRID's shader-side source of truth (core.declarations.cppm is the
    # host's): a shader that includes it and is not rebuilt after it changes would index the heap by stale
    # numbers, which validation cannot see. The C++ side is compared against it by a test (see
    # test_render_resources). It is its own file rather than part of heap_slots.glsl because the Slang
    # migration needs the same numbers - see docs/slang_migration.md - and heap_slots.glsl (the GLSL heap
    # arrays and the fetch helper) includes it.
    "${VR_SHADER_SRC_DIR}/heap_slot_constants.glsl"
    "${VR_SHADER_SRC_DIR}/heap_slots.glsl"
    # heap_access.slang and mesh_geometry.slang are the two OTHER includes every leaf pulls in - the shim's heap
    # accessors, and the device-address geometry fetch a mesh stage uses instead of the input assembler. Both were
    # missing from this list, and the mesh one cost a measurement to find: the build tree's pbr.vert.spv and
    # shadow.frag.spv were STALE (built before the geometry lanes existed) while the mesh entries sharing their
    # source had been rebuilt, so the two halves of one pipeline declared different blocks - visible only as a
    # byte difference against the escape-hatch scripts' output, never as a wrong picture.
    # `test_shader_sources` now compares this list against every #include in shaders/, so the next include cannot
    # be forgotten the same way.
    "${VR_SHADER_SRC_DIR}/heap_access.slang"
    "${VR_SHADER_SRC_DIR}/mesh_geometry.slang"
    # character_forward.slang IS INCLUDED BY A LEAF NOW, which is why it is here: the outline fragment stage
    # includes it for `toon_diffuse` (the same reason `toon_params.slang` is above - a second copy of that chain
    # is the duplicate this repository refuses), so editing it must rebuild `outline.frag.spv` too.
    # `test_shader_sources` requires every `#include` found in shaders/ to be listed here, which is what would
    # have caught the omission: an unlisted include leaves STALE SPIR-V that nothing but a byte comparison sees.
    "${VR_SHADER_SRC_DIR}/character_forward.slang"
    # goo_toon.slang IS INCLUDED BY THE REWRITTEN CHAIN'S OWN STAGE, and it is listed here for the SAME reason
    # character_forward.slang is one line above: `shaders/goo_toon.slang` includes it for `toon_diffuse` (the old
    # body every non-iris family is still shaded by), so editing it must rebuild `goo_toon.frag.spv` as well -
    # otherwise the old chain's fix lands in one .spv and not the other, which is a difference between two stages
    # that must agree, visible only as a byte comparison. See `goo_toon_pipeline_name`'s note for why the two
    # stages share one body on purpose.
    "${VR_SHADER_SRC_DIR}/goo_toon.slang"
    # goo_rim.slang is THE REWRITTEN CHAIN'S THIRD STAGE and it is listed for the reason the two lines above are:
    # it reads the same two shared tables those stages do (`toon_params.slang`'s family table and
    # `material_record.glsl`'s layout), so an edit to either must rebuild `goo_rim.frag.spv` as well - a stage left
    # on stale SPIR-V would read a material record with different field offsets, which is a wrong texture or a
    # wrong family rather than a build error.
    "${VR_SHADER_SRC_DIR}/goo_rim.slang"
)
# (THE GLSL STAGE LIST USED TO LIVE HERE: twenty-four names, glslc's input, kept in step with the sources by
# hand. Every one of them is in shaders/glsl.old/ now and built from Slang instead, so the list is gone -
# VR_SLANG_SOURCES below IS the list of what this build compiles.)
set(VR_SHADER_OUTPUTS "")
# ---- SLANG SOURCES: the GLSL -> Slang migration, one shader at a time (docs/slang_migration.md) --------
#
# Each entry is "<source>:<entry>:<stage>:<output .spv name>", and the OUTPUT NAME IS THE SAME .spv THE GLSL
# VERSION PRODUCED: the runtime loads shaders by file name, so a port needs no host change at all, and the
# acceptance for every port is the capture gate staying byte-identical.
#
# THE FLAGS ARE THE BINDING CONTRACT, and every one of them was measured rather than guessed:
#   -allow-glsl                 ... the shared slot constants are a plain `const uint` file (valid GLSL);
#                                   Slang rejects a non-static global const without this (E31224).
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
    unlit.slang:main:fragment:unlit.frag.spv
    # character_forward.slang is the toon character stage: the same leaves the scene pass drew, shaded a
    # second time OVER the deferred result. It is gated behind the `character_forward` feature, which is OFF
    # by default, so none of the fourteen capture scenarios records it until a scenario turns it on - which is
    # what makes its landing behaviour-neutral.
    character_forward.slang:main:fragment:character_forward.frag.spv
    # goo_toon.slang is THE REWRITTEN TOON CHAIN's fragment stage: the same geometry, the same pass and the same
    # frame as the entry above, with the shading model replaced by the one XIYAG's Goo node presets describe
    # (see build-release-clang64/deren-ab/goo_toon_plan.md). ITS ENTRY IS `goo_toon_frag_main`, NOT `main`, for the
    # reason `outline.slang`'s is: it INCLUDES character_forward.slang, which brings that file's own
    # `[shader("fragment")] main` with it, and a plain `-entry main` then names an overload set that Slang refuses
    # to resolve (E38000). It is selected at run time by `[render] goo_toon` - OFF by default, so none of the ten
    # capture scenarios draws it, and its landing is behaviour-neutral until a run asks for it.
    goo_toon.slang:goo_toon_frag_main:fragment:goo_toon.frag.spv
    # toon_screen_rim.slang is the toon stage's SECOND rim: a fullscreen additive contour from the depth,
    # gated by the SAME `character_forward` feature, so no capture scenario records it either.
    toon_screen_rim.slang:main:fragment:toon_screen_rim.frag.spv
    # goo_rim.slang is THE REWRITTEN CHAIN'S rim, and it is a stage of its own for the constraint its header
    # records: the reference's rim is a PRODUCT of three attenuations and a screen-space depth factor, and the
    # depth it samples is the character-forward stage's own ATTACHMENT - so the surface shader cannot compute it.
    # It is gated by `[render] goo_toon` (through a name the chain owner composes), so, like the two entries above,
    # no capture scenario records it until one turns the rewritten chain on.
    goo_rim.slang:main:fragment:goo_rim.frag.spv
    # overlay.slang is the article's TWO FRAMEBUFFER MULTIPLIES (an eye shadow and a hair shadow), drawn by the
    # `character_forward` stage's overlay group with the multiply blend and gated by that same feature - so, like
    # the two entries above, no capture scenario records it until one turns the toon stage on. It shares
    # pbr.mesh.spv as its geometry entry: an overlay is a normal mesh (see the file's header).
    overlay.slang:main:fragment:overlay.frag.spv
    fxaa.slang:main:fragment:fxaa.frag.spv
    # upscale.slang is the RESOLVE: the render chain's display-referred LDR image onto the presented swapchain,
    # by a linear (bilinear) filter. It runs only below `[render] render_scale = 1.0` - at 1.0 the chain IS the
    # output and the pass's feature gate is false, which is why no capture scenario records it.
    upscale.slang:main:fragment:upscale.frag.spv
    post.slang:main:vertex:post.vert.spv
    # post.frag is the post chain's stage for ALL THREE modes (bright pass / downsample / composite), so every
    # scenario records it - the widest verification a single port can have.
    post.slang:frag_main:fragment:post.frag.spv
    gbuffer_debug.slang:main:fragment:gbuffer_debug.frag.spv
    taa.slang:main:fragment:taa.frag.spv
    # gbuffer.slang IS the first stage that reads a shared body AND a heap UBO, and it cost two bugs, both
    # documented in docs/slang_migration.md section 9 because neither is visible in the SPIR-V's shape:
    # `mul(M, v)` on a `row_major float4x4` emits the TRANSPOSED product (the GLSL spelling `M * v` does
    # not), and a heap descriptor's TYPE has to match the storage class the shader reads it through - a
    # UNIFORM descriptor read through Slang's StorageBuffer pointer returns zeros with NO validation
    # finding. That is also why the camera and the light are written as STORAGE descriptors.
    gbuffer.slang:main:fragment:gbuffer.frag.spv
    # pbr.slang is the FORWARD path's shader PAIR: the fragment stage is where the sun/shadow/punctual/
    # cluster/IBL fetches are proven under Slang, and its GEOMETRY entry is what the G-BUFFER pass loads for
    # every opaque draw - so it is verified by all fourteen scenarios, and by the transparent pass's
    # forward default too. THAT ENTRY IS THE MESH ONE SINCE STEP 4 (docs/mesh_shaders.md): `pbr.vert.spv`
    # was removed with the rest of the vertex geometry path, so there is no vertex entry to list here.
    pbr.slang:main:fragment:pbr.frag.spv
    # ... the same file's MESH entry (docs/mesh_shaders.md step 2): the G-buffer/forward geometry as
    # dispatches, which is what the scene's opaque, forward and transparent leaves are drawn with.
    pbr.slang:mesh_main:mesh:pbr.mesh.spv
    # ... and the MESHLET entry (docs/mesh_shaders.md step 3): one workgroup per meshlet, camera-culled.
    pbr.slang:meshlet_main:mesh:pbr.meshlet.spv
    # ... and THE OUTLINE'S MESH ENTRY: `mesh_main`'s triangle loop with the author's normal expansion applied
    # to the clip position before the vertex is written (`build-release-clang64/deren-ab/outline_plan.md` ①). It is
    # the SAME FILE because the hull has to be skinned, morphed and placed by the same body as the surface it
    # outlines - a second copy of that body is how an outline detaches from a deforming character.
    pbr.slang:outline_mesh_main:mesh:outline.mesh.spv
    # ... and the outline's FRAGMENT stage, which shades that hull with the G-buffer normal of the surface
    # BEHIND it (the author's `MyZmdOutlineShader.shader:196-198`). ITS ENTRY IS `frag_main`, NOT `main`, and the
    # reason is measured rather than stylistic: it INCLUDES character_forward.slang for `toon_diffuse`, which
    # brings that file's own `[shader("fragment")] main` with it, and Slang then refuses to resolve a plain
    # `-entry main` at all (E38000, "no function found matching entry point name 'main'") because the name is an
    # overload set. One file with several distinctly-named entries is this repository's existing shape
    # (`post.slang`'s `frag_main`, `heap_probe.slang`'s three), and the .spv name - which is what the host loads
    # - is unaffected by the entry's name.
    outline.slang:frag_main:fragment:outline.frag.spv
    # shadow.slang shares no body: the material table and the albedo fetch through the shim's accessors
    # (`material_at`, `heap_sample`), and the entry returns void because a depth-only stage has no output.
    # Its MESH entry reuses the same matrix-array accessors pbr.slang does, which is the point of the pair:
    # both stages must place the geometry identically or the shadow silhouette detaches from the surface.
    # Its vertex entry went the same way as pbr's in step 4.
    shadow.slang:main:fragment:shadow.frag.spv
    # ... and the SAME file's MESH entry (docs/mesh_shaders.md step 1): the depth pass drawn as dispatches
    # instead of indexed draws. It REPLACES the vertex entry rather than joining it (one pipeline is MESH +
    # fragment), and the host keeps both - a device without VK_EXT_mesh_shader has no mesh stage at all.
    shadow.slang:mesh_main:mesh:shadow.mesh.spv
    # ... and the MESHLET entry (docs/mesh_shaders.md step 3): one workgroup per meshlet, window read from the table.
    shadow.slang:meshlet_main:mesh:shadow.meshlet.spv
    # light_cluster.slang is the FIRST compute stage: the clustered-light sort, dispatched once per frame by
    # default (clustered_lights = true), so every scenario records it. Its writable buffers are the heap slots
    # the GLSL declares as `writeonly buffer`.
    light_cluster.slang:main:compute:light_cluster.comp.spv
    # heap_probe.slang is the graphics half of the heap-native probe. No gate scenario renders it, but it is
    # the one group that VERIFIES ITSELF: the runtime dispatches it at startup, reads a pixel back and logs it
    # against the value it knows, so this port is accepted by comparing those log lines (see the file).
    heap_probe.slang:main:vertex:heap_probe.vert.spv
    heap_probe.slang:frag_main:fragment:heap_probe.frag.spv
    # The MESH half of the same probe (docs/mesh_shaders.md step 0): the same triangle, the same fragment
    # stage, the same 4x4 readback, emitted by a mesh pipeline instead of a vertex one. A mesh stage is a
    # different stage type, so this .spv name is new - the host learns it explicitly.
    heap_probe.slang:mesh_main:mesh:heap_probe.mesh.spv
    # The COMPUTE half is its own file because it has its own push layout (one push block per stage).
    heap_probe_comp.slang:comp_main:compute:heap_probe.comp.spv
    # compute_skin.slang feeds the acceleration structures the deformed vertex positions; its A/B needs the
    # skinned fixture with rt_shadow on (see docs/slang_migration.md).
    compute_skin.slang:comp_main:compute:compute_skin.comp.spv
    # mask_bake.slang is the alphaMode MASK bake (off by default; [render] rt_mask_bake turns it on, which is
    # also how its A/B is run).
    mask_bake.slang:comp_main:compute:mask_bake.comp.spv
    # megalights_temporal.slang is the stochastic lighting's denoise (off by default; [render] megalights = true
    # turns the path on, which is also how its A/B is run).
    megalights_temporal.slang:comp_main:compute:megalights_temporal.comp.spv
    # megalights_trace.slang is the stochastic punctual lighting tracer the temporal pass denoises.
    megalights_trace.slang:comp_main:compute:megalights_trace.comp.spv
    # rt_shadow.slang is the ray-traced shadow's shader file, growing one stage at a time; it starts with the
    # closest-hit stage, whose whole job is the payload contract (see the file). No gate scenario enables
    # rt_shadows, so this port is shape-verified and belongs to the A/B capture route in docs/slang_migration.md.
    rt_shadow.slang:chit_main:closesthit:rt_shadow.rchit.spv
    rt_shadow.slang:miss_main:miss:rt_shadow.rmiss.spv
    rt_shadow.slang:rgen_main:raygeneration:rt_shadow.rgen.spv
    rt_shadow.slang:ahit_main:anyhit:rt_shadow.rahit.spv

    # deferred.slang is the deferred path's lighting stage: the shared shading code again (so the forward and
    # the deferred image are comparable rather than merely similar), plus the screen-space AO it owns. It is
    # on every scenario's path, and it is the ONE stage whose frames differ from the GLSL build - by ONE pixel
    # of 1036800, a front-end codegen difference rather than a porting mistake (both modules call the same
    # GLSL.std.450 instructions; their SEQUENCES differ, and the driver contracts the multiply-adds
    # differently). It was accepted by an explicit re-baseline with the reason written down: see
    # docs/slang_migration.md section 9, "the last stage, and the one-pixel re-baseline".
    deferred.slang:main:fragment:deferred.frag.spv
)
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
        COMMAND "${VR_SLANGC_EXECUTABLE}" "${VR_SHADER_SRC_DIR}/${slang_source}" -I "${VR_SHADER_SRC_DIR}" -allow-glsl -DVR_SLANG
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
# shaders/glsl.old/ and are NOT part of the build - they exist to be read next to their ports.
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
    COMMENT "copying the GLSL sources beside the compiled shaders"
    VERBATIM
)
add_custom_target(shaders ALL DEPENDS ${VR_SHADER_OUTPUTS} "${VR_SHADER_OUT_DIR}/.sources-copied")
if(TARGET deren)
    add_dependencies(deren shaders)
endif()
# The dynamic runtime's own scaffold test needs the compiled shaders as soon as the port starts building
# pipelines from them (S2/S3); in S1 it needs none, and asking anyway keeps the dependency from being
# forgotten when the test grows.
if(TARGET test_runtime_dyn)
    add_dependencies(test_runtime_dyn shaders)
endif()
message(STATUS "slangc: ${VR_SLANGC_EXECUTABLE} - every build compiles shaders/*.spv into ${VR_SHADER_OUT_DIR}")
