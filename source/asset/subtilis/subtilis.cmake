# Offline subtilis wire/reference targets.
include_guard(GLOBAL)

# Shared offline wire/reference sources; no builder, loader or GPU dependency.
add_library(asset_subtilis_core STATIC)
target_sources(asset_subtilis_core PUBLIC FILE_SET CXX_MODULES FILES
    source/asset/subtilis/types.cppm
    source/asset/subtilis/pages.cppm
    source/asset/subtilis/codec.cppm
    source/asset/subtilis/dag.cppm
    PRIVATE source/asset/subtilis/pages.cpp source/asset/subtilis/codec.cpp source/asset/subtilis/digest.cpp
    source/asset/subtilis/dag.cpp source/asset/subtilis/cut.cpp)
target_link_libraries(asset_subtilis_core PUBLIC shared_utility vstd_lib PRIVATE yyjson_vendored)
