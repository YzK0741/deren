# Vendored mimalloc target.
include_guard(GLOBAL)

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
