#pragma once

// Private application helper, included after the module imports (main.cpp and toon_material.cpp both do
// that): it names module entities, and a header cannot forward-declare those - a global-module
// redeclaration is ill-formed.
//
// THE APPLICATION-SIDE JOIN between the loader's toon data and the runtime's lookup contract. The engine
// is loader-agnostic (it links neither `gltf_loader` nor the sidecar module), so the runtime cannot hold a
// sidecar and must not learn what one is. What it CAN ask for is "the texture input for this material and
// this lane", and the application is the layer that links both: the tables, the procedural bakes and the
// three callbacks live in toon_material.cpp.
//
// The bakes are CPU work done once at startup: a neutral ramp, a diffuse ramp, a shadow LUT and a matcap
// ball, all of them replacement content for textures the asset license forbids redistributing. (They are
// what main used to bake inline before this file existed; the bytes are unchanged.)

#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>

namespace deren::app::toon_material {
    /// @brief the toon material lookup: tables + baked replacement textures + the three runtime callbacks
    /// @note must outlive every material import that reads it (main constructs it before the first frame)
    class lookup {
    public:
        lookup(deren::gltf::scenes const& scenes, deren::toon::sidecar const* sidecar);
        lookup(lookup const&) = delete;
        lookup& operator=(lookup const&) = delete;

        /// @brief the value handed to `runtime::set_toon_lookup`; `owner` is this object
        [[nodiscard]] deren::engine::runtime::toon_lookup runtime_lookup() const noexcept;

    private:
        static deren::engine::texture_input texture_callback(void* owner, std::string_view material_name, deren::engine::toon_slot lane);
        static glm::vec4 colour_callback(void* owner, std::string_view material_name, deren::engine::toon_colour_lane lane);
        static float scalar_callback(void* owner, std::string_view material_name, std::string_view row, float fallback);

        deren::toon::sidecar const* sidecar = nullptr;
        deren::gltf::scenes const* scenes = nullptr;
        /// the PROCEDURALLY BAKED neutral unit ramp (see the baker in toon_material.cpp), kept alive here
        /// because the texture input the lookup returns is a SPAN INTO IT and `register_material` reads it
        /// during the import. ONE buffer serves BOTH ramp lanes: the diffuse ramp and the specular ramp are
        /// the same neutral step, and what tells them apart is which family numbers the shader moves it with
        deren::utility::vessel::vector<uint8_t> baked_unit_ramp = {};
        uint32_t ramp_width = 0;
        uint32_t ramp_height = 0;
        /// the DIFFUSE ramp lane's neutral content (see the baker) - a different SHAPE from the unit step
        /// above, because the two lanes read their ramp differently: one as a threshold, this one as a colour
        /// mapping plus a lightness
        deren::utility::vessel::vector<uint8_t> baked_neutral_ramp = {};
        uint32_t neutral_ramp_width = 0;
        uint32_t neutral_ramp_height = 0;
        /// the SHADOW LUT cube (see the baker) - a different shape from the ramp and so a different buffer,
        /// kept alive for the same reason: the lookup hands out a SPAN INTO IT
        deren::utility::vessel::vector<uint8_t> baked_shadow_lut = {};
        uint32_t lut_width = 0;
        uint32_t lut_height = 0;
        /// the MATCAP ball (see the baker) - a third shape again, and a black one, because this stage had
        /// no matcap term to reproduce
        deren::utility::vessel::vector<uint8_t> baked_matcap = {};
        uint32_t matcap_size = 0;
        /// the METALLIC/GLOSS lane has NO neutral here on purpose - see the note where its baker used to be.
        /// A material with no such map is shaded by its family's own `roughness` / `reflectivity`.
    };

    /// @brief log one sidecar's resolved slots/rows (the diagnostic main used to print inline)
    void log_sidecar(deren::gltf::scenes const& scenes, deren::toon::sidecar const& sidecar);
} // namespace deren::app::toon_material