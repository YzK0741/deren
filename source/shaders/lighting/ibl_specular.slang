/**
 * @file shaders/ibl_specular.slang
 * @brief The specular half of the split-sum IBL, shared by every pass that has to agree about it.
 * @ingroup shaders
 *
 * WHY THIS FILE EXISTS. More than one call site needs the same two things - "what does the prefiltered
 * environment contribute along this reflection direction" and "what weight does the BRDF give it" - and
 * they have to agree to the last bit, because two of them are the two sides of a SUBTRACTION: the
 * lighting stage ADDS `ibl_specular_radiance(...) * ibl_specular_fresnel(...) * ao` for every pixel, and a
 * pass that replaces that term has to remove exactly the same expression. Before this file the expression
 * was written twice, once in shaders/shading.slang (the lighting stage's ambient) and once inline in a
 * shaded hit's ambient; both now call these functions, so the definition went from two to one.
 *
 * WHAT THE INCLUDER MUST PROVIDE: the environment (heap_slots::env_cube, declared as `env_texture[]` by
 * shaders/shading.slang) and the BRDF LUT (`brdf_lut_texture[]`), plus the slot constants that name them. A
 * binding declared twice in one
 * translation unit does not compile, which is why nothing
 * is declared here - and why this file is included AFTER those declarations.
 */

#ifndef DEREN_IBL_SPECULAR_SLANG
#define DEREN_IBL_SPECULAR_SLANG

// ---- THE THREE FETCHES THIS FILE MAKES, each behind a NAME ----
//
// WHY NAMED. A GLSL fetch builds a combined sampler at the point of use
// (`sampler2D(brdf_lut_texture[slot], heap_samplers[s])`) and Slang cannot express that at all: there the
// two halves travel in ONE `DescriptorHandle<Sampler2D>` whose .x indexes the resource heap and .y the
// sampler heap. Naming the three fetches is what lets the functions below be shared by both languages -
// see docs/slang_migration.md, and shaders/heap_access.slang for the other half.

/// @brief the prefiltered chain's mip level for a roughness: the level count is QUERIED from the
///        sampler rather than hardcoded, so it always matches whatever `env_mip_count` the CPU baked
/// @note the sampler is `heap_sampler_texture` - the one with LINEAR filtering and the full mip chain, which
///       is what a prefiltered lookup needs. There used to be a seventh `heap_sampler_env` name here, and the
///       host writes exactly six samplers: the env reads were reaching an UNWRITTEN slot, which the grid
///       contract test now fails on (see tests/test_render_resources.cpp). A cube map's addressing mode is
///       ignored at face edges, so the texture sampler's repeat costs nothing here.
float ibl_specular_lod(float roughness) {
    return roughness * float(max(env_cube_levels() - 1, 0));
}

/// @brief one sample of the prefiltered environment: the radiance arriving from @p reflection
float3 ibl_specular_sample(float3 reflection, float lod) {
    return env_cube_sample_lod(reflection, lod);
}

/// @brief Prefiltered GGX environment radiance for a reflection ray, at the mip that matches
///        @p roughness
float3 ibl_specular_radiance(float3 n, float3 v, float roughness) {
    const float3 reflection = normalize(reflect(-v, n));
    return ibl_specular_sample(reflection, ibl_specular_lod(roughness));
}

/// @brief Single-scatter plus multi-scatter-compensated Fresnel weights from the BRDF LUT
///        (Fdez-Aguera); @p specular_weight is the material's monochrome specular amount, which every
///        caller in this engine passes as 1.0 - the parameter exists because the forward path's BRDF
///        presets scale it, and dropping it would make this function and the one it replaced disagree.
float3 ibl_specular_fresnel(float3 n, float3 v, float roughness, float3 f0, float specular_weight) {
    const float ndotv = clamp(dot(n, v), 0.0, 1.0);
    const float2 brdf_sample_point = clamp(float2(ndotv, roughness), float2(0.0), float2(1.0));
    const float2 f_ab = brdf_lut_sample(brdf_sample_point);
    const float3 fr = max(float3(1.0 - roughness), f0) - f0;
    const float3 k_s = f0 + fr * pow(1.0 - ndotv, 5.0);
    const float3 fssess = specular_weight * (k_s * f_ab.x + f_ab.y);

    const float ems = 1.0 - (f_ab.x + f_ab.y);
    const float3 f_avg = specular_weight * (f0 + (1.0 - f0) / 21.0);
    const float3 fmsems = ems * fssess * f_avg / (1.0 - f_avg * ems);

    return fssess + fmsems;
}

#endif // DEREN_IBL_SPECULAR_SLANG
