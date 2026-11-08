# Vendored meshoptimizer target.
include_guard(GLOBAL)

add_library(meshoptimizer_vendored STATIC
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/allocator.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/clusterizer.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/partition.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/simplifier.cpp
    ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src/indexgenerator.cpp
)
target_include_directories(meshoptimizer_vendored PRIVATE ${DEREN_REPO_ROOT}/source/third_party/meshoptimizer/src)
