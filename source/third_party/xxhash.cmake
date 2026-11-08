# Vendored xxHash target.
include_guard(GLOBAL)

# xxHash (vendored under source/third_party/xxhash, see the LICENSE there): non-cryptographic
# content hashing (XXH3_128bits) used by deren::utility::xxh3_128bits for image/texture dedup.
# Compiled as its own plain-C target, mirroring the imgui policy above (vendored
# code does not follow the project's -Werror set). static_utility links it (the hash functions are
# pure and per-half - they are not process-wide state).

add_library(xxhash STATIC
        ${DEREN_REPO_ROOT}/source/third_party/xxhash/xxhash.c
)

target_include_directories(xxhash
    PUBLIC
        ${DEREN_REPO_ROOT}/source/third_party/xxhash
)
