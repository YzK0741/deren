/**
 * @file source/app/app_animation.cppm
 * @defgroup app_animation Application Animation Wiring
 * @brief the host surface an animation::controller drives: it is assembled from the runtime's
 *        scene tree, per-frame-slot morph/skin buffers and shared task pool, so the controller
 *        itself never depends on deren.engine.runtime. The application layer holds the glue.
 * @note interface only - the implementations live in app_animation.cpp (the module follows the
 *       split interface/implementation pattern of the other app modules)
 */

export module deren.app.animation;

import deren.engine.animation;
import deren.engine.runtime;

export namespace deren::app {
    /**
     * @ingroup app_animation
     * @brief assemble an animation::backend for @p runtime: the host surface an
     *        animation::controller drives (scene + per-slot buffer callbacks + the task pool),
     *        wired to the runtime's scene, frame-slot buffers and run_tasks. Pass it to
     *        controller::init(); the controller itself never depends on deren::engine::runtime.
     */
    deren::engine::animation::backend make_animation_backend(deren::engine::runtime& runtime);
} // namespace deren::app
