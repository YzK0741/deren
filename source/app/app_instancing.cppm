/**
 * @file source/app/app_instancing.cppm
 * @defgroup app_instancing Application Instancing Stress
 * @brief the optional instancing stress grid: draws the first imported "pbr" primitive as a
 *        grid_side x grid_side instance grid in ONE instanced draw call. An application-layer
 *        helper - it knows the runtime and its primitives, never the loader.
 * @note interface only - the implementations live in app_instancing.cpp (the module follows the
 *       split interface/implementation pattern of the other app modules)
 */

export module deren.app.instancing;

import deren.engine.runtime;
import deren.vstd;

export namespace deren::app {
    /**
     * @ingroup app_instancing
     * @brief optional instancing stress: when @p grid_side > 1 (config or argv), draw the first
     *        imported "pbr" primitive as a grid_side x grid_side grid in ONE instanced draw call
     *        (an instanced_draw_primitive appended to the scene tree — the frame loop is
     *        untouched). No-op when grid_side <= 1 or the scene has no pbr primitive.
     * @param runtime the initialized runtime holding the imported scene
     * @param grid_side grid side length from settings.grid_side (> 1 enables the grid)
     * @param scene_radius radius of the imported scene (grid spacing = 2.5 x radius, so
     *        instances stay apart: the demo measures draw scaling, not overdraw)
     */
    void add_instancing_grid(deren::engine::runtime& runtime, std::int32_t grid_side, float scene_radius);
} // namespace deren::app
