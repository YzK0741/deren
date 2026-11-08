# The portable engine target.
include_guard(GLOBAL)

add_library(deren_engine STATIC)
target_link_libraries(deren_engine PRIVATE asset_subtilis_core)
add_library(deren::engine ALIAS deren_engine)

target_sources(deren_engine
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/engine/filters/filters.cppm
        source/engine/gaussian_splatting/gaussian_splatting.cppm
        source/engine/gaussian_splatting/raster.cppm
        source/engine/gaussian_splatting/gpu_frame.cppm
        source/engine/gaussian_splatting/probe.cppm
        source/engine/math/math.cppm
        ${VR_RUNTIME_MODULES}
        source/engine/pipelines/pipelines.cppm
        source/engine/render_resource/render_resource.cppm
        source/engine/render_resource/schema/render_resource.schema.cppm
        source/engine/render_resource/passes/render_resource.passes.cppm
        source/engine/render_resource/shared.cppm
        source/engine/render_resource/instances.cppm
        source/engine/frame_constants/frame_constants.cppm
        source/engine/pass/pass.cppm
        source/engine/pass/taa.cppm
        source/engine/pass/scene.cppm
        source/engine/pass/transparent.cppm
        source/engine/pass/gaussian_splatting.cppm
        source/engine/pass/character_forward.cppm
        source/engine/pass/toon_screen_rim.cppm
        source/engine/pass/game_toon_depth_rim.cppm
        source/engine/pass/ray_traced_shadow.cppm
        source/engine/pass/ray_traced_reflection.cppm
        source/engine/pass/mask_bake.cppm
        source/engine/pass/compute_skin.cppm
        source/engine/pass/scene_constants.cppm
        source/engine/pass/scene_deformation.cppm
        source/engine/pass/scene_tables.cppm
        source/engine/pass/buffer_storage.cppm
        source/engine/pass/output_targets.cppm
        source/engine/pass/cluster.cppm
        source/engine/pass/deferred.cppm
        source/engine/pass/post.cppm
        source/engine/pass/fxaa.cppm
        source/engine/pass/upscale.cppm
        source/engine/pass/geometry_buffer_debug.cppm
        source/engine/pass/shadow.cppm
        source/engine/pass/visibility_raster.cppm
        source/engine/pass/visibility_resolve.cppm
        source/engine/pass/software_visibility.cppm
        source/engine/pass/software_merge.cppm
        source/engine/pass/hzb_build.cppm
        source/engine/pass/hzb_cull.cppm
        source/engine/pass/reflection_denoise.cppm
        source/engine/pass/chain.cppm
        source/engine/profiling/profiling.cppm
        source/engine/shadow_fit/shadow_fit.cppm
        source/engine/acceleration/acceleration.cppm
        source/engine/ray_tracing/ray_tracing.cppm
        source/engine/scene_tree/scene_tree.cppm
        source/engine/meshlet/meshlet.cppm
        source/engine/geometry/subtilis_lod.cppm
        source/engine/geometry/subtilis_asset.cppm
        source/engine/geometry/subtilis_dag_asset.cppm
        source/engine/geometry/subtilis_dag_lod.cppm
        source/engine/geometry/subtilis_dag_import.cppm
        source/engine/geometry/subtilis_builder.cppm
        source/engine/geometry/subtilis_upload.cppm
        source/engine/geometry/subtilis_selection.cppm
        source/engine/geometry/subtilis.cppm
        source/engine/visibility/visibility.cppm
        source/engine/visibility/texture_residency.cppm
        source/engine/visibility/visibility_software.cppm
        source/engine/visibility/classification/visibility_software.classification.cppm
        source/engine/visibility/tiling/visibility_software.tiling.cppm
        source/engine/visibility/merge/visibility_software.merge.cppm
        source/engine/visibility/reference/visibility_software.reference.cppm
        source/engine/visibility/hzb.cppm
        source/engine/primitive/primitive.cppm
        source/engine/primitive/records/primitive.records.cppm
        source/engine/primitive/toon/primitive.toon.cppm
        source/engine/primitive/primitives/primitive.primitives.cppm
        source/engine/render_environment/render_environment.cppm
        # (the GUI module MOVED to deren_gui_vulkan - see that target's comment) and the loader the
        # engine uses instead: it resolves deren_gui_<api>.dll by name through the boundary header.
        source/engine/runtime/gui_loader.cppm
        source/engine/animation/controller.cppm
        source/engine/animation/mmd_motion.cppm
    PRIVATE
        source/engine/filters/filters.cpp
        source/engine/gaussian_splatting/gaussian_splatting.cpp
        source/engine/gaussian_splatting/gaussian_splatting.gpu.cpp
        source/engine/gaussian_splatting/raster.cpp
        source/engine/gaussian_splatting/probe.cpp
        source/engine/gaussian_splatting/gpu_probe.cpp
        source/engine/gaussian_splatting/raster.gpu.cpp
        source/engine/gaussian_splatting/gpu_frame.cpp
        source/engine/math/math.cpp
        ${VR_RUNTIME_PRIVATE_SOURCES}
        source/engine/pass/taa.cpp
        source/engine/pass/scene.cpp
        source/engine/pass/transparent.cpp
        source/engine/pass/gaussian_splatting.cpp
        source/engine/pass/character_forward.cpp
        source/engine/pass/toon_screen_rim.cpp
        source/engine/pass/game_toon_depth_rim.cpp
        source/engine/pass/ray_traced_shadow.cpp
        source/engine/pass/ray_traced_reflection.cpp
        source/engine/pass/mask_bake.cpp
        source/engine/pass/compute_skin.cpp
        source/engine/pass/cluster.cpp
        source/engine/pass/deferred.cpp
        source/engine/runtime/runtime.post_graph.cpp
        source/engine/runtime/runtime.texture_residency.cpp
        source/engine/pass/post.cpp
        source/engine/pass/fxaa.cpp
        source/engine/pass/upscale.cpp
        source/engine/pass/geometry_buffer_debug.cpp
        source/engine/pass/shadow.cpp
        source/engine/pass/visibility_raster.cpp
        source/engine/pass/visibility_resolve.cpp
        source/engine/pass/software_visibility.cpp
        source/engine/pass/software_merge.cpp
        source/engine/pass/hzb_build.cpp
        source/engine/pass/hzb_cull.cpp
        source/engine/pass/reflection_denoise.cpp
        source/engine/shadow_fit/shadow_fit.cpp
        source/engine/acceleration/acceleration.cpp
        source/engine/ray_tracing/ray_tracing.cpp
        source/engine/scene_tree/scene_tree.cpp
        source/engine/primitive/primitive.cpp
        source/engine/animation/controller.cpp
        source/engine/animation/mmd_motion.cpp
)
include(source/assets/assets.cmake)


target_include_directories(deren_engine SYSTEM PRIVATE source/third_party/meshoptimizer/src)
target_link_libraries(deren_engine PRIVATE meshoptimizer_vendored)
target_sources(deren_engine PRIVATE
    source/engine/geometry/subtilis_upload.cpp
    source/engine/geometry/subtilis_builder.cpp
    source/engine/geometry/subtilis_builder_simplify.cpp
    source/engine/geometry/subtilis_builder_topology.cpp
)


# deren_assets is deliberately absent from deren_engine's link interface: the engine core is
# loader-agnostic (see the library comment above). The executable links deren_assets itself - the
# import library, so the image is loaded before main - and source/app/main.cpp is its only consumer, passing the
# parsed deren::gltf::scenes down to the runtime at the call site.
target_link_libraries(deren_engine
        PUBLIC
        resource_cluster_core
        buffer_transfer_core
        gaussian_loader
        glfw
        static_utility
        glm::glm
        ${CMAKE_DL_LIBS}
        vstd_lib
        # (imgui MOVED OUT: deren_gui_vulkan links it PRIVATE and the engine resolves the plugin by name
        #  through deren.engine.gui_loader - see that target below. No engine target links it any more.)
        # The constexpr builders the engine imports directly (26 sites): the module is
        # neutral (compiled once for both halves), which is what takes those imports off the flip
        # gate's meter - see the target's comment above deren_vulkan.
        renderer_layout
)

option(DEREN_VERIFY_ENGINE_NO_VULKAN "Reject Vulkan headers while compiling the engine" ON)
if(DEREN_VERIFY_ENGINE_NO_VULKAN)
    set(_engine_header_guard "${CMAKE_CURRENT_BINARY_DIR}/engine-vulkan-deny")
    file(MAKE_DIRECTORY "${_engine_header_guard}/vulkan")
    file(WRITE "${_engine_header_guard}/vulkan/vulkan.h"
        "#error Vulkan headers are forbidden in engine translation units\n")
    target_include_directories(deren_engine BEFORE PRIVATE "${_engine_header_guard}")
endif()

# Vendored VMA (source/third_party/vma/vk_mem_alloc.h): vma.cppm includes <vma/vk_mem_alloc.h>,
# resolved against this third_party include root. The header also ships in the Vulkan SDK's
# Include/vma, but Linux distro Vulkan packages do not provide it, so the vendored copy is
# preferred (it precedes Vulkan::Vulkan's include dirs). Marked SYSTEM so upstream VMA's
# warnings are suppressed (the SDK copy is an isystem include for the same reason - VMA code
# trips -Werror on nullability annotations otherwise).
target_include_directories(deren_engine
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party
)

# The engine side gets the backend's modules (BMIs) and symbols from deren_vulkan. PUBLIC, because a
# static library's link interface is what carries the module file set to the exe and to the tests
# (§11.2): deren_engine's own name and its outward link surface are unchanged by S1-A.
# THE ENGINE DOES NOT LINK THE BACKEND ANY MORE, and that is what makes the backend a DLL rather than
# an auto-import library: the runtime resolves `deren_make_api_core` BY NAME out of deren_vulkan.dll
# (source/engine/runtime/runtime.constructor.cppm, through deren.utility.dynamic_link, detaching the handle so the
# image is never unloaded - the measured FreeLibrary deadlock), and the boundary's one whitelisted
# symbol disappears from the link with it. The module BMIs this line used to carry PUBLIC were needed
# by NOTHING on the engine side (the import meter reads 0 sites / 0 files); the tests that do import a
# backend module link deren_vulkan themselves.
#
# `deren_vulkan` still exists as a target and CMake still knows how to build it - it is simply not in
# this link interface. The APP depends on it as a build ORDER (the DLL must exist before a test runs).
