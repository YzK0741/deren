# Vendored CPU asset dependencies. No upstream configuration or downloads.
include_guard(GLOBAL)
get_filename_component(DEREN_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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

add_library(meshoptimizer_vendored STATIC
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/allocator.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/clusterizer.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/partition.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/simplifier.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/indexgenerator.cpp
)
target_include_directories(meshoptimizer_vendored PRIVATE ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src)
add_library(yyjson_vendored STATIC ${DEREN_REPO_ROOT}/source/third_party/yyjson/src/yyjson.c)
target_include_directories(yyjson_vendored SYSTEM PUBLIC ${DEREN_REPO_ROOT}/source/third_party/yyjson/src)
