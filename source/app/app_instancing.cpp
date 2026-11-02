module;

// Textual glm includes: the definition below names glm types AND instantiates
// deren::utility::vessel::vector<glm::mat4>. Including glm in the global module fragment (the
// same pattern source/engine/animation/controller.cpp uses) keeps clang's merged libc++ allocator
// declarations from clashing with the vendored std module under -fno-exceptions, so the
// container instantiations resolve. Do not remove these includes to "clean up".

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

module deren.app.instancing;

import deren.engine.runtime;
import deren.utility;
import deren.vstd;

namespace deren::app {
    void add_instancing_grid(deren::engine::runtime& runtime, std::int32_t const grid_side, float const scene_radius) {
        if (grid_side <= 1) {
            return;
        }
        deren::utility::vessel::vector<deren::engine::primitive const*> const pbr_primitives = runtime.get_primitives("pbr");
        if (pbr_primitives.empty()) {
            return;
        }
        deren::engine::primitive const& source = *pbr_primitives[0];
        deren::utility::vessel::vector<glm::mat4> transforms;
        transforms.reserve(static_cast<std::size_t>(grid_side) * grid_side);
        float const spacing = 2.5f * scene_radius; // keep instances apart: measure draw scaling, not overdraw
        for (std::int32_t i = 0; i < grid_side; ++i) {
            for (std::int32_t j = 0; j < grid_side; ++j) {
                float const dx = (static_cast<float>(i) - static_cast<float>(grid_side - 1) * 0.5f) * spacing;
                float const dz = (static_cast<float>(j) - static_cast<float>(grid_side - 1) * 0.5f) * spacing;
                transforms.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(dx, 0.0f, dz)) * source.push.model);
            }
        }
        runtime.make_instanced_primitive(source, transforms);
        deren::utility::log("instancing stress: {} x {} grid ({} instances, 1 draw call)", grid_side, grid_side, transforms.size());
    }
} // namespace deren::app
