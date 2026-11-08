# Vulkan backend and support targets.
include_guard(GLOBAL)

add_library(vulkan_constant_init STATIC)
target_link_libraries(vulkan_constant_init PUBLIC shared_utility)
target_sources(vulkan_constant_init PUBLIC FILE_SET CXX_MODULES FILES
    source/backends/vulkan/constant_init/constant_init.cppm)
target_link_libraries(vulkan_constant_init PUBLIC Vulkan::Vulkan)

# THE VULKAN MAIN-THREAD CONTEXT (abi 39): the concrete layout behind the contract's opaque
# `rhi::main_context_view`, compiled by BOTH halves that touch it - `deren_vulkan` (creates and fills it) and
# `deren_gui_vulkan` (casts it back). A STATIC library carrying the interface unit, for the same reason
# `vulkan_constant_init` and `renderer_layout` are: the backend DLL exports exactly one name, so a module
# attachment cannot be served from it, and both images compile the same unit instead.
add_library(vulkan_main_context STATIC)
target_sources(vulkan_main_context PUBLIC FILE_SET CXX_MODULES FILES
    source/backends/vulkan/main_context/main_context.cppm
    source/backends/vulkan/shared/command_buffer.cppm)
target_link_libraries(vulkan_main_context PUBLIC promise Vulkan::Vulkan)

add_library(vulkan_synchronization STATIC)
target_sources(vulkan_synchronization PUBLIC FILE_SET CXX_MODULES FILES
    source/backends/vulkan/synchronization/synchronization.cppm)
target_link_libraries(vulkan_synchronization PUBLIC rhi_sync_contract)
target_include_directories(vulkan_synchronization SYSTEM PUBLIC ${Vulkan_INCLUDE_DIRS})
if(NOT MSVC)
    target_compile_options(rhi_sync_contract PRIVATE -fno-sanitize=all)
    target_compile_options(vulkan_synchronization PRIVATE -fno-sanitize=all)
endif()

add_library(renderer_layout STATIC)
target_link_libraries(renderer_layout PUBLIC shared_utility)
target_sources(renderer_layout PUBLIC FILE_SET CXX_MODULES FILES
    source/engine/render_layout/render_layout.cppm)
target_link_libraries(renderer_layout PUBLIC promise)

# ============================================================================================
# vulkan_error_tables - THE ERROR TRANSLATORS, CARRIED BY A TARGET BOTH SIDES LINK (the SHARED
# flip's third move of this kind, after vulkan_constant_init and both utility halves).
#
# WHY THEY LEFT `deren_vulkan`'s MODULE: until the flip they were `export` declarations of
# `deren.vulkan.core` with definitions in a module implementation unit. A module attachment is a
# LINK-TIME fact - a consumer needs the module's initializer and its decorated symbols - and a DLL
# can serve those only by exporting its module surface. `deren_vulkan.dll` exports EXACTLY ONE name
# by design (source/promise/rhi/backend_entry.hpp), so the tables had to become ordinary functions in a
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
        source/backends/vulkan/core/support/error_tables.cppm
)
# The contract's module is the dependency of the interface unit itself (it re-exports it, so an
# importer needs no second import just to name an `error`).
target_link_libraries(vulkan_error_tables PUBLIC promise Vulkan::Vulkan)

# ============================================================================================
# vulkan_core_support - the backend-detail templates that are tested in isolation.
#
# `lazy_loading` and `merge_push_range` are tiny header-sized utilities, but they are part of the
# module surface now: `lazy_loading` appears in exported core class layouts and `merge_push_range`
# is used by the recording implementation. Keeping them as core partitions made their unit tests
# import the backend module (and therefore link the DLL); a standalone module keeps the same
# definitions without that dependency. The backend re-exports this module, so core importers still
# see `detail::lazy_loading` through `deren.vulkan.core`.
# ============================================================================================
add_library(vulkan_core_support STATIC)
target_sources(vulkan_core_support
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/backends/vulkan/core/support/core_support.cppm
        source/backends/vulkan/core/support/lazy_loading.cppm
        source/backends/vulkan/core/support/push_range_validation.cppm
)
target_link_libraries(vulkan_core_support PUBLIC vstd_lib)

add_library(deren_vulkan SHARED)
add_library(deren::vulkan ALIAS deren_vulkan)
# Same rule as shared_utility: the backend ships as `deren_vulkan.dll`, which is what the runtime's
# loader looks for beside the executable (source/engine/runtime/runtime.constructor.cppm).
set_target_properties(deren_vulkan PROPERTIES PREFIX "")

# The dllexport side of the boundary (source/utility/abi_export.hpp). The importing side needs neither
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
    # all. The suppression below is the same one source/tests/probe_backend.cpp already carries, and it is
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
        source/backends/vulkan/core/core.cppm
        source/backends/vulkan/core/declarations/core.declarations.cppm
        source/backends/vulkan/core/core.constructor.cppm
        source/backends/vulkan/core/vma/vma.cppm
        source/backends/vulkan/core/descriptor_heap/descriptor_heap.cppm
        source/backends/vulkan/core/vma/handles/vma_handles.cppm
        source/backends/vulkan/core/handles/handles.cppm
        source/backends/vulkan/core/init_utils/init_utils.cppm
        source/backends/vulkan/init_utils/init_utils.cppm
        source/backends/vulkan/core/pipeline/pipeline.cppm
        source/backends/vulkan/core/pipeline/spirv_parser.cppm
        # (source/backends/vulkan/constant_init/constant_init.cppm MOVED OUT: it is `vulkan_constant_init` now - the
        #  engine imports it in 26 places and a backend module cannot serve the flip - see that
        #  target's comment above deren_vulkan)
    PRIVATE
        source/backends/vulkan/core/core.cpp
        # The RHI contract's virtuals for this backend (plan §11.4, S1-C): `core` derives from
        # `deren::promise::rhi::api_core`, so this is where abilities() and the frame calls live.
        source/backends/vulkan/core/resource/core.lazy_resources.cpp
        source/backends/vulkan/core/render/core.render_operations.cpp
        source/backends/vulkan/core/core.api_core.cpp
        source/backends/vulkan/core/facet/core.resource_facet.cpp
        source/backends/vulkan/core/facet/core.command_facet.cpp
        source/backends/vulkan/core/facet/core.frame_facet.cpp
        source/backends/vulkan/core/facet/core.frame_views.cpp
        source/backends/vulkan/core/command/core.scoped_synchronization.cpp
        source/backends/vulkan/core/command/core.api_draw.cpp
        source/backends/vulkan/core/frame/core.frame_resources.cpp
        source/backends/vulkan/core/frame/core.frame_checkpoint.cpp
        source/backends/vulkan/core/support/radix_sort.cpp
        source/backends/vulkan/core/resource/core.resource_groups.cpp
        source/backends/vulkan/core/resource/core.texture_groups.cpp
        source/backends/vulkan/core/resource/core.descriptor_space.cpp
        source/backends/vulkan/core/command/core.submission_lifetimes.cpp
        # The ONE C entry point, DEFINED for the real backend (source/promise/rhi/backend_entry.hpp, abi 18).
        # A plain TU rather than a module implementation unit, because the entry has to be reachable BY
        # NAME from an image that imports no module of this target - the same reason
        # source/tests/probe_backend.cpp is a plain TU. With this target SHARED it is the dllexport side of the
        # boundary, and the export table carries exactly this one `deren_*` name.
        source/backends/vulkan/core/core.entry.cpp
        source/backends/vulkan/core/handles/handles.cpp
        source/backends/vulkan/init_utils/init_utils.cpp
        source/backends/vulkan/core/pipeline/pipeline.cpp
        source/third_party/spirv-reflect/spirv_reflect.c
)
# The error translators (see the target above): a static library, so the DLL carries its own copy of
# them - which is correct, because the ENGINE never calls them; the unit test links the same target
# and gets the identical code without importing the backend's module.
target_link_libraries(deren_vulkan
        vulkan_synchronization
        vulkan_error_tables
        vulkan_core_support
        vulkan_main_context
        # The backend implements the contract, so the contract's module is a dependency of THIS target
        # and of nothing else in the backend's direction (source/promise/rhi/*.cppm imports nothing back).
        promise
        # The Vulkan constexpr builders both halves compile (see the target's comment above): the
        # backend keeps using them unchanged, they simply stop being a backend-owned module.
        vulkan_constant_init
        renderer_layout
        Vulkan::Vulkan
        glfw
        static_utility
        glm::glm
        ${CMAKE_DL_LIBS}
        vstd_lib
)
target_include_directories(deren_vulkan
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party
)
