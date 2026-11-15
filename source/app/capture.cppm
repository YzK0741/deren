module;

#include <glm/glm.hpp>

export module deren.app.application:capture;

import deren.vstd;
import deren.utility;
import deren.utility.shared;

export namespace deren::app::capture {
    /**
     * @brief The scripted-capture request a managed project's preinit fills in.
     *
     * The native preview CLI that used to parse these options is gone: a managed project sets the
     * fields through `deren::scripting::startup_options` and the engine only reads them here.
     */
    struct options {
        bool windowed = false;
        std::int32_t frames = 0;
        std::optional<deren::utility::vessel::array<float, 3>> camera = std::nullopt;
        std::optional<glm::vec3> target = std::nullopt;
        float sweep_yaw_deg_per_frame = 0.0f;
        float animation_seconds_per_frame = 0.0f;
    };
} // namespace deren::app::capture
