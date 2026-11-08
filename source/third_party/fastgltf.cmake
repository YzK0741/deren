# Vendored fastgltf target (bundles simdjson).
include_guard(GLOBAL)

add_library(fastgltf_vendored STATIC
        ${DEREN_REPO_ROOT}/source/third_party/fastgltf/src/fastgltf.cpp
        ${DEREN_REPO_ROOT}/source/third_party/fastgltf/src/base64.cpp
        ${DEREN_REPO_ROOT}/source/third_party/fastgltf/src/io.cpp
        ${DEREN_REPO_ROOT}/source/third_party/simdjson/singleheader/simdjson.cpp
)

target_include_directories(fastgltf_vendored
    PUBLIC
        ${DEREN_REPO_ROOT}/source/third_party/fastgltf/include
    PRIVATE
        ${DEREN_REPO_ROOT}/source/third_party/simdjson/singleheader
)
