// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

module;

#include <cstdint>
#include <glm/glm.hpp>

export module deren.app.toon;
export import deren.vstd;

import deren.utility;
import deren.engine.primitive;
import deren.gltf_loader;
import deren.toon_material_sidecar;

/**
 * @file source/app/app_toon.cppm
 * @brief the toon material lookup state and the callbacks the runtime invokes through it
 *
 * This is the part of the former main.cpp's anonymous namespace that is pure CPU setup: the table
 * lookups, the procedural bakes, and the three callbacks. The definitions live in app_toon.cpp.
 */
export namespace deren::app::detail {

    /// the per-asset lookup state; pointers borrow the imported scene and its sidecar
    struct toon_lookup_state {
        deren::toon::sidecar const* sidecar = nullptr;
        deren::gltf::scenes const* scenes = nullptr;
        deren::utility::vessel::vector<uint8_t> baked_unit_ramp = {};
        uint32_t ramp_width = 0;
        uint32_t ramp_height = 0;
        deren::utility::vessel::vector<uint8_t> baked_neutral_ramp = {};
        uint32_t neutral_ramp_width = 0;
        uint32_t neutral_ramp_height = 0;
        deren::utility::vessel::vector<uint8_t> baked_shadow_lut = {};
        uint32_t lut_width = 0;
        uint32_t lut_height = 0;
        deren::utility::vessel::vector<uint8_t> baked_matcap = {};
        uint32_t matcap_size = 0;
    };

    void bake_toon_textures(toon_lookup_state& state);
    deren::engine::texture_input toon_texture_callback(void* parent, std::string_view material_name, deren::engine::toon_slot lane);
    glm::vec4 toon_colour_callback(void* parent, std::string_view material_name, deren::engine::toon_colour_lane lane);
    float toon_scalar_callback(void* parent, std::string_view material_name, std::string_view row, float fallback);
    void log_toon_sidecar(deren::gltf::scenes const& scenes, deren::toon::sidecar const& sidecar);

} // namespace deren::app::detail
