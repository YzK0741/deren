// ============================================================================
// module: deren.engine.primitive  (peer of deren.engine.scene_tree / deren.engine.runtime - the
//         GPU primitives that live in the scene-tree leaves, plus the GPU
//         material / camera / light UBO records of the scene block; versioned in
//         lock-step with deren.engine.runtime, see that module's banner)
// module version: 1.1.2  (independent of the app version in CMakeLists project(VERSION))
//
// GPU scene contents (namespace deren::engine):
//   - deren::engine::primitive (owns geometry buffers + material push constants,
//     implements the pure-CPU abstract scene_tree::primitive) and its draw
//     strategies normal_draw_primitive / instanced_draw_primitive /
//     static_draw_primitive
//   - material / UBO records (material_record, material_push_constants,
//     material_id, camera_ubo, light_ubo, punctual_light) and the input
//     records (texture_input / ibl_input / scene_import_result) the runtime
//     facade's scene-set API is built on
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - in lock-step with vulkan.runtime.
// ============================================================================

/**
 * @file primitive.cppm
 * @defgroup engine_primitive Engine GPU Primitives
 * @brief the drawable GPU primitives plus the material / camera / light UBO
 *        records of the GPU scene set.
 * @details each primitive owns its geometry buffers and material push
 *          constants and implements the abstract scene_tree::primitive leaf
 *          (set_world); draw(render_environment&) records through the
 *          per-recording-worker environment.
 */
module;

export module deren.engine.primitive;

// The public surface is unchanged: these re-exports are the same modules the
// monolithic unit used to export, and the three partitions below are the
// implementation domains.
export import deren.utility.shared;
export import deren.vstd;
export import deren.engine.render_environment;
export import deren.engine.scene_tree;
export import deren.engine.meshlet;

export import :records;
export import :toon;
export import :primitives;