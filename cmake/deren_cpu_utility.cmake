# Shared CPU dependencies, usable without the renderer's root project.
include_guard(GLOBAL)
get_filename_component(DEREN_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(MSVC)
    set(CMAKE_CXX_MODULE_STD ON)
endif()

add_library(vstd_lib STATIC)

if(MSVC)
    target_sources(vstd_lib
            PUBLIC
            FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT}
            FILES
            ${DEREN_REPO_ROOT}/source/vstd/vstd_msvc.cppm
    )
else()
    target_sources(vstd_lib
            PUBLIC
            FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT}
            FILES
            ${DEREN_REPO_ROOT}/source/vstd/vstd.cppm
    )
endif()

add_library(mimalloc_vendored STATIC
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/alloc.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/alloc-aligned.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/alloc-posix.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/arena.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/arena-meta.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/bitmap.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/heap.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/init.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/libc.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/options.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/os.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/page.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/page-map.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/random.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/stats.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/theap.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/threadlocal.c
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/src/prim/prim.c
)

target_include_directories(mimalloc_vendored
    SYSTEM PUBLIC
        ${DEREN_REPO_ROOT}/source/third_party/mimalloc/include
)

if(WIN32)
    # upstream links psapi/shell32/user32/advapi32/bcrypt on Windows (see mimalloc's
    # CMakeLists.txt mi_libraries); _WIN32_WINNT=0x600 matches the upstream MINGW define
    target_compile_definitions(mimalloc_vendored PRIVATE _WIN32_WINNT=0x600)
    target_link_libraries(mimalloc_vendored PUBLIC psapi shell32 user32 advapi32 bcrypt)
endif()

add_library(shared_utility SHARED)
# THE FILE NAME IS PART OF THE DESIGN: the process-wide half ships as `shared_utility.dll`, because
# BOTH the executable and deren_vulkan.dll import it BY THAT NAME. MinGW's default `lib` prefix would
# produce `libshared_utility.dll`, so it is removed on purpose (the import library keeps its
# conventional spelling through IMPORT_PREFIX).
set_target_properties(shared_utility PROPERTIES PREFIX "")
get_filename_component(_trace_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(VR_STACKTRACE_SYMBOLIZER NAMES llvm-symbolizer HINTS "${_trace_compiler_dir}")
if(VR_STACKTRACE_SYMBOLIZER)
    target_compile_definitions(shared_utility PRIVATE DEREN_LLVM_SYMBOLIZER="${VR_STACKTRACE_SYMBOLIZER}")
endif()
target_link_libraries(shared_utility PRIVATE ${CMAKE_DL_LIBS})

target_sources(shared_utility
        PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT}
        FILES
        # The sink, the rotation, panic convergence, the allocator hook. Its tools go into
        # `deren.utility` below, which re-exports this module.
        ${DEREN_REPO_ROOT}/source/utility/shared/shared_utility.cppm
        PRIVATE
        ${DEREN_REPO_ROOT}/source/utility/shared/shared_utility.cpp
        # The allocator hook's definitions. IT IS NOT A PARTITION, AND THAT IS MEASURED: a partition
        # of a module that is itself re-exported (`deren.utility` re-exports `deren.utility.shared`)
        # makes clang 22.1.8 crash while semantically analysing a consumer's std::format call
        # (utility.cpp's write_png, reproduced with -fsyntax-only; the crash disappears when the
        # partition re-export becomes a plain import). So the hook is declared in the primary
        # interface and defined here - no partition, nothing lost.
        ${DEREN_REPO_ROOT}/source/utility/shared/memory.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/allocation_failure.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace_native.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace_posix.cpp
        # The once-per-process rotation claim, a plain (non-module) TU for the platform_* reason:
        # <windows.h> does not belong in a module's global module fragment.
        ${DEREN_REPO_ROOT}/source/utility/shared/log_rotation_claim.cpp
)

target_link_libraries(shared_utility
        vstd_lib
        mimalloc_vendored
)
