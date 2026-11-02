module;

// Textual glm include: the definitions below name glm types, and including glm in the global
// module fragment (the same pattern source/engine/animation/controller.cpp uses) keeps clang's
// merged libc++ allocator declarations from clashing with the vendored std module under
// -fno-exceptions. Do not remove this include to "clean up".

#include <glm/glm.hpp>

module deren.app.animation;

import deren.engine.animation;
import deren.engine.runtime;
import deren.vstd;

namespace deren::app {
    deren::engine::animation::backend make_animation_backend(deren::engine::runtime& runtime) {
        deren::engine::animation::backend backend;
        backend.scene = &runtime.get_scene();
        backend.morph_scratch_active = [&runtime]() -> float* {
            return static_cast<float*>(runtime.morph_scratch());
        };
        backend.morph_scratch_slot = [&runtime](std::uint32_t const slot) -> float* {
            return static_cast<float*>(runtime.morph_scratch(slot));
        };
        backend.set_skin_matrices_active = [&runtime](std::span<glm::mat4 const> matrices) {
            runtime.set_skin_matrices(matrices);
        };
        backend.set_skin_matrices_slot = [&runtime](std::span<glm::mat4 const> matrices, std::uint32_t const slot) {
            runtime.set_skin_matrices(matrices, slot);
        };
        backend.scene_changed = [&runtime]() {
            runtime.scene_changed();
        };
        backend.run_tasks = [&runtime](std::span<std::function<void()>> tasks) {
            runtime.run_tasks(tasks, deren::engine::task_priority::animation);
        };
        backend.task_worker_count = [&runtime]() -> std::int32_t {
            return runtime.task_pool_threads();
        };
        return backend;
    }
} // namespace deren::app
