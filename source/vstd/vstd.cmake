# The project STL module target.
include_guard(GLOBAL)

# The public module is `deren.vstd`; source/vstd/vstd.cppm is a facade that
# re-exports exactly one backend selected here:
#
#   clang64 (the shipped build): source/vstd/vstd_libcxx.cppm - modified from
#     libc++'s (LLVM) generated std-module output and trimmed to the headers the
#     project uses (see source/vstd/README.md).
#   MSVC: source/vstd/vstd_msvc.cppm - the MSVC STL's modules/std.ixx, vendored
#     from Visual Studio 2022 17.14 / MSVC 14.44.35207 and renamed to
#     `deren.vstd.msvc`. It includes the standard headers inside the module
#     purview; `_BUILD_STD_MODULE` makes them export their entities.
#
# Only the selected backend is added to the target: compiling both would make
# clang parse the MSVC STL and cl.exe parse libc++'s headers, and both fail. The
# facade's conditional `export import` reads the same define.
#
# Both are ordinary source modules. The MSVC dialect does NOT `import std;`, and
# the build does NOT use CMake's CXX_MODULE_STD path or its experimental
# import-std gate. It must not: the libc++-derived re-export construction (71
# `export namespace std { using ... }` partitions) makes cl.exe 19.44 die with
# `fatal error C1001` on every TU that instantiates std::span / std::array /
# std::tuple through it - 7 of the 326 objects before this switch, all of them
# importers of vstd. The minimal reproductions and the bisection are in
# build-release-clang64/msvc/lead_lab/ICE_FINDINGS.md; source/vstd/vstd_msvc.cppm
# states what the vendored dialect costs (MSVC STL instead of libc++, the full
# header set instead of the trimmed whitelist, and no VSTD_* macro face).
# =============================================================================================
add_library(vstd_lib STATIC)

if(MSVC)
    set(_vstd_backend_source ${DEREN_REPO_ROOT}/source/vstd/vstd_msvc.cppm)
    set(_vstd_backend_define DEREN_VSTD_MSVC)
else()
    set(_vstd_backend_source ${DEREN_REPO_ROOT}/source/vstd/vstd_libcxx.cppm)
    set(_vstd_backend_define DEREN_VSTD_LIBCXX)
endif()

target_sources(vstd_lib
        PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT}
        FILES
        ${DEREN_REPO_ROOT}/source/vstd/vstd.cppm
        ${_vstd_backend_source}
)

target_compile_definitions(vstd_lib PRIVATE ${_vstd_backend_define})

unset(_vstd_backend_source)
unset(_vstd_backend_define)
