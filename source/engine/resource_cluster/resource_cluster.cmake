# Resource cluster support target.
include_guard(GLOBAL)

add_library(resource_cluster_core STATIC)
target_sources(resource_cluster_core PUBLIC FILE_SET CXX_MODULES FILES
    source/engine/resource_cluster/resource_cluster.cppm)
target_link_libraries(resource_cluster_core PUBLIC shared_utility promise)
