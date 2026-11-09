# Asset loader DLL and Gaussian loader.
include_guard(GLOBAL)

# ================================================================================================
# deren_assets - THE ASSET-LOADING STACK, AS AN AUTO-IMPORTED DLL (the binary-balance batch).
#
# WHY THIS AND NOTHING ELSE MOVED. Measured in Release (the per-target object bytes in
# `CMakeFiles/<target>.dir/**/*.obj`, which under -flto are LLVM bitcode and therefore a code-volume
# proxy): the executable carried ~7.1 MB of NON-rendering code, and this target plus its vendored
# implementation is 3.4 MB of it (gltf_loader 1.73 + fastgltf_vendored 1.65, simdjson included).
# It is the only large non-rendering block that CAN move, because it depends on nothing in
# `deren_engine` - and a DLL cannot import a symbol out of the executable, so anything that names
# the engine (`chores`) or IS the executable (`source/app/main.cpp`) has to stay. `backend_loader` was left
# alone too: it links glfw, and a DLL would put a THIRD copy of GLFW's global state in the process.
#
# AUTO-IMPORT, DELIBERATELY - THE OPPOSITE RULE FROM THE TWO GRAPHICS PLUGINS. The backend and the
# GUI plugin are resolved BY NAME through deren.utility.dynamic_link so that a missing or wrong
# plugin is a NAMED refusal rather than a loader error, and so the application can choose one at run
# time. An asset loader has neither property to preserve: there is exactly one of it, nothing selects
# it, and a missing loader fails startup under either regime. The executable therefore links the
# IMPORT LIBRARY and the image is loaded before main - the shape that would be a defect for a
# graphics plugin is the right shape here.
#
# NO `dllexport` ANYWHERE IN THIS TARGET, AND THAT IS LOAD-BEARING: MinGW's ld auto-exports every
# global symbol of a `-shared` link unless the link carries at least one explicit `dllexport`, which
# switches auto-export off. `shared_utility.dll` is the measured precedent for the first shape (623
# exports, no macro in the module); `deren_vulkan.dll` / `deren_gui_vulkan.dll` export exactly one
# name each because their C entry is marked. This target takes the shared_utility shape on purpose,
# so the module SOURCE does not have to know it is in a DLL and no declaration needs a per-symbol
# macro. The cost is stated rather than hidden: everything global is exported, the vendored
# fastgltf/simdjson objects linked PRIVATE below included.
#
# WHAT DOES NOT CHANGE: the module names (`deren.gltf_loader`, `deren.toon_material_sidecar`), the
# sources, the flags (the "compile options" foreach below still names this target, by its new name),
# and the types passed across the seam (`deren::gltf::scenes`). Both images compile `deren.vstd`
# from the same interface unit and both import libc++.dll - the premise the utility split already
# rests on, and the one scripts/checks/check_backend_boundary.py checks on the artifacts.
#
# NO SANITIZER SUPPRESSION, AND THAT IS MEASURED RATHER THAN ASSUMED. The CI sanitizers job builds
# these tests with -fsanitize=address,undefined and then RUNS them, which is the shape that made
# deren_vulkan and probe_backend carry -fno-sanitize=all (an instrumented `-shared` link was not a
# shape that toolchain could make). This target does not need it: measured in a Debug tree configured
# exactly like that job, an instrumented test_gltf_loader links, loads this instrumented DLL, and
# passes 183/0, and the DLL carries no unresolved `asan*` symbol. Copying the suppression here would
# cost the loader's sanitizer coverage and buy nothing.
# ================================================================================================
add_library(gaussian_loader STATIC)
target_link_libraries(gaussian_loader PUBLIC shared_utility)
target_sources(gaussian_loader PUBLIC FILE_SET CXX_MODULES FILES source/gaussian_loader/gaussian_loader.cppm
    PRIVATE source/gaussian_loader/gaussian_loader.cpp)

add_library(deren_assets SHARED)
# The file NAME is part of the design, like shared_utility's: MinGW's default `lib` prefix would
# produce libderen_assets.dll, so it is removed on purpose (the import library keeps its
# conventional spelling through IMPORT_PREFIX).
set_target_properties(deren_assets PROPERTIES PREFIX "")
target_sources(deren_assets
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/gltf_loader/gltf_loader.cppm
        # The separate TOON MATERIAL SIDECAR is a module rather than more code in gltf_loader, because what
        # it reads is not glTF: a glTF material carries the spec's five texture slots and six factors, and a
        # toon character needs a second set (a diffuse ramp, a shadow LUT, a specular ramp, a matcap, a face
        # SDF) that glTF has no concept of and that lives in a sidecar keyed by MATERIAL NAME. Two formats,
        # two concerns, two modules - and it is in this target because it is a model-loading concern.
        source/gltf_loader/toon_material_sidecar.cppm

    PRIVATE
        source/gltf_loader/gltf_loader.cpp
        source/gltf_loader/toon_material_sidecar.cpp
)

# THE EXTRAS PARSE CALLBACK'S ONE HEADER, and it is SYSTEM on purpose: fastgltf reports a material's `extras`
# block by CALLING BACK with a `simdjson::dom::object*` (see its `ExtrasParseCallback`), so the loader's
# translation unit has to know that type - and `simdjson.h` is upstream code whose warnings this project does
# not hold, exactly like the vendored libraries above. PRIVATE, because nothing that CONSUMES this DLL ever
# sees a simdjson type: the extras are copied out into `deren::gltf::material`'s hold maps before `load_model` returns.
target_include_directories(deren_assets
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/simdjson/singleheader
)

# fastgltf + simdjson, vendored under source/third_party/ (the sources ship in the repo, so no
# network FetchContent is needed and both Windows/MSYS2 and Linux build self-contained).
# fastgltf's headers deliberately do NOT include simdjson (it is hidden inside the .cpp
# files), so only two translation units ever see it: this library's sources, and
# gltf_loader's (which needs the type because fastgltf's EXTRAS PARSE CALLBACK hands it a
# `simdjson::dom::object*` - see the SYSTEM include on that target, which is what keeps a
# vendored header out of this project's -Werror set). simdjson is compiled from its amalgamated
# single-header/single-source pair (simdjson.h + simdjson.cpp), and the three fastgltf
# translation units are compiled here as plain C++ (vendored code does not follow the
# project's -Werror set, mirroring the imgui/xxhash policy).
# stb_image.h (vendored): gltf_loader.cpp decodes embedded/external textures with it
# (#define STB_IMAGE_IMPLEMENTATION + #include <stb/stb_image.h>).


# mimalloc (vendored under source/third_party/mimalloc, v3.4.4, MIT — see the LICENSE there):
# allocator backing deren.utility.shared's memory service and utility::vessel.
# Allocation and deallocation stay in one DLL; no default PMR resource is installed. Compiled as
# its plain-C static target from the upstream mi_sources set, mirroring the imgui /
# xxhash policy (vendored code does not follow the project's -Werror set; headers are
# SYSTEM so the -Werror consumers that include <mimalloc.h> do not see upstream warnings).
# PRIVATE throughout, and that is what the DLL shape changes: the vendored loader, stb and the
# per-half toolkit are compiled INTO deren_assets.dll, so no consumer of theirs is a consumer of this
# target's link interface any more. `vstd_lib` stays here because the module's interface re-exports
# it (`export import deren.vstd`), and the consumers that import the module link vstd_lib themselves.
target_link_libraries(deren_assets
        PUBLIC
        shared_utility
        PRIVATE
        fastgltf_vendored
        stb_vendored
        yyjson_vendored
        vstd_lib
        static_utility
)
