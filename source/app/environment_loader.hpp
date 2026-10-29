#pragma once
// Private application helper, included after engine imports and the application's stb_image decoder.
// Each background task owns its decoded pixels and joins both convolution tasks before returning.
struct prepared_environment {
    deren::utility::vessel::vector<uint8_t> environment;
    deren::utility::vessel::vector<uint8_t> irradiance;
    deren::utility::vessel::string error;
};

inline prepared_environment prepare_environment(std::filesystem::path const& path,
    deren::utility::vessel::array<float, 3> sun, int env_size, int mip_count, int irr_size, float input_scale) {
    deren::utility::vessel::vector<float> cube;
    if (path.empty()) {
        cube = deren::engine::generate_environment_cubemap(env_size, sun);
    } else {
        int width = 0, height = 0, channels = 0;
        // HDR-only: do not silently gamma-decode an LDR file as an environment panorama.
        if (!stbi_is_hdr(path.string().c_str())) return {{}, {}, "Environment must be a Radiance .hdr image"};
        if (!stbi_info(path.string().c_str(), &width, &height, &channels) || width <= 0 || height <= 0 || width > 16384 || height > 8192)
            return {{}, {}, "Environment dimensions exceed 16384 x 8192"};
        float* decoded = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
        if (!decoded) return {{}, {}, "Could not decode environment HDR"};
        if (width <= 0 || height <= 0 || width > 16384 || height > 8192) {
            stbi_image_free(decoded);
            return {{}, {}, "Environment dimensions exceed 16384 x 8192"};
        }
        auto count = std::size_t(width) * std::size_t(height) * 4;
        bool finite = true;
        for (std::size_t i = 0; i < count; ++i) if (!std::isfinite(decoded[i]) || decoded[i] < 0) { finite = false; break; }
        if (!finite) { stbi_image_free(decoded); return {{}, {}, "Environment contains invalid radiance"}; }
        cube = deren::engine::generate_environment_cubemap_from_equirect(std::span<float const>(decoded, count), width, height, env_size, input_scale);
        stbi_image_free(decoded);
    }
    if (!deren::engine::environment_detail::valid_half_radiance(cube)) return {{}, {}, "Environment exceeds finite half-float radiance; reduce input intensity"};
    auto specular = deren::engine::prefilter_environment_async(cube, env_size, mip_count);
    auto diffuse = deren::engine::generate_irradiance_map_async(cube, env_size, irr_size);
    auto specular_pixels = specular.get();
    auto diffuse_pixels = diffuse.get();
    if (!deren::engine::environment_detail::valid_half_radiance(specular_pixels) || !deren::engine::environment_detail::valid_half_radiance(diffuse_pixels))
        return {{}, {}, "Prepared environment exceeds finite half-float radiance; reduce input intensity"};
    return {deren::engine::to_half_rgba(specular_pixels), deren::engine::to_half_rgba(diffuse_pixels), {}};
}
