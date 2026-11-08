# Buffer transfer support target.
include_guard(GLOBAL)

add_library(buffer_transfer_core STATIC)
target_sources(buffer_transfer_core PUBLIC FILE_SET CXX_MODULES FILES
    source/engine/buffer_transfer/buffer_transfer.cppm)
target_link_libraries(buffer_transfer_core PUBLIC resource_cluster_core shared_utility promise)
