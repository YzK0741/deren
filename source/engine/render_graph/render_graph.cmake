# The render-graph targets.
include_guard(GLOBAL)

add_library(render_graph_core STATIC)
target_sources(render_graph_core
    PUBLIC FILE_SET CXX_MODULES FILES
        source/engine/render_graph/types.cppm
        source/engine/render_graph/parameters.cppm
        source/engine/render_graph/diagnostics.cppm
        source/engine/render_graph/render_graph.cppm
    PRIVATE source/engine/render_graph/render_graph.cpp
        source/engine/render_graph/diagnostics.cpp
        source/engine/render_graph/boost_exceptions.cpp)
target_link_libraries(render_graph_core PUBLIC shared_utility vstd_lib PRIVATE boost_graph_headers)

add_library(render_graph_rhi STATIC)
target_sources(render_graph_rhi PUBLIC FILE_SET CXX_MODULES FILES
    source/engine/render_graph/rhi.cppm)
target_link_libraries(render_graph_rhi PUBLIC render_graph_core rhi_sync_contract)

add_library(render_graph_execute STATIC)
target_sources(render_graph_execute PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/execute.cppm)
target_link_libraries(render_graph_execute PUBLIC render_graph_rhi resource_cluster_core promise)

add_library(render_graph_compose STATIC)
target_sources(render_graph_compose PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/compose.cppm)
target_link_libraries(render_graph_compose PUBLIC render_graph_execute)
add_library(render_graph_bloom STATIC)
target_sources(render_graph_bloom PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/bloom.cppm)
target_link_libraries(render_graph_bloom PUBLIC render_graph_execute)
add_library(render_graph_lighting STATIC)
target_sources(render_graph_lighting PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/lighting.cppm)
target_link_libraries(render_graph_lighting PUBLIC render_graph_core)
add_library(render_graph_post STATIC)
target_sources(render_graph_post PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/post.cppm)
target_link_libraries(render_graph_post PUBLIC render_graph_execute)
add_library(render_graph_geometry STATIC)
target_sources(render_graph_geometry PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/geometry.cppm)
target_link_libraries(render_graph_geometry PUBLIC render_graph_core)
add_library(render_graph_visibility STATIC)
target_sources(render_graph_visibility PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/visibility.cppm)
target_link_libraries(render_graph_visibility PUBLIC render_graph_core)
add_library(render_graph_cluster STATIC)
target_sources(render_graph_cluster PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/cluster.cppm)
target_link_libraries(render_graph_cluster PUBLIC render_graph_core)
add_library(render_graph_shadow STATIC)
target_sources(render_graph_shadow PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/shadow.cppm)
target_link_libraries(render_graph_shadow PUBLIC render_graph_core)
add_library(render_graph_taa STATIC)
target_sources(render_graph_taa PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/taa.cppm)
target_link_libraries(render_graph_taa PUBLIC render_graph_core)
add_library(render_graph_gbuffer_debug STATIC)
target_sources(render_graph_gbuffer_debug PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/gbuffer_debug.cppm)
target_link_libraries(render_graph_gbuffer_debug PUBLIC render_graph_core)
add_library(render_graph_transparent STATIC)
target_sources(render_graph_transparent PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/transparent.cppm)
target_link_libraries(render_graph_transparent PUBLIC render_graph_core)
add_library(render_graph_character STATIC)
target_sources(render_graph_character PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/character.cppm)
target_link_libraries(render_graph_character PUBLIC render_graph_core)
add_library(render_graph_gaussian STATIC)
target_sources(render_graph_gaussian PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/gaussian.cppm)
target_link_libraries(render_graph_gaussian PUBLIC render_graph_core)
add_library(render_graph_rt_shadow STATIC)
target_sources(render_graph_rt_shadow PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/rt_shadow.cppm)
target_link_libraries(render_graph_rt_shadow PUBLIC render_graph_core)
add_library(render_graph_rt_reflection STATIC)
target_sources(render_graph_rt_reflection PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/rt_reflection.cppm)
target_link_libraries(render_graph_rt_reflection PUBLIC render_graph_core)
add_library(render_graph_reflection_denoise STATIC)
target_sources(render_graph_reflection_denoise PUBLIC FILE_SET CXX_MODULES FILES source/engine/render_graph/reflection_denoise.cppm)
target_link_libraries(render_graph_reflection_denoise PUBLIC render_graph_core)
target_link_libraries(deren_engine PUBLIC render_graph_bloom render_graph_post render_graph_compose render_graph_lighting render_graph_geometry
                                  render_graph_cluster render_graph_shadow render_graph_taa render_graph_gbuffer_debug render_graph_transparent render_graph_character render_graph_gaussian render_graph_rt_shadow render_graph_rt_reflection render_graph_reflection_denoise render_graph_visibility)
