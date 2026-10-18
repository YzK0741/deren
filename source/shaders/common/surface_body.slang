// shaders/surface_body.slang - THE MATERIAL DECODE of one surface point, declared where BOTH a fragment stage
// and a COMPUTE stage can reach it (visibility-buffer v0.4b).
//
// WHY THE DECODE IS SPLIT OUT OF surface.slang, and it is a HARD constraint rather than tidiness: the visibility
// resolve is a COMPUTE pass, and a compute pipeline cannot include surface.slang at all - that file declares the
// shared scene push block, which is 144 bytes because it carries the mesh geometry lanes, over the 128-byte
// `max_push_bytes` a heap-native pipeline allows. So the decode moved HERE, into a file that declares no push
// block and no images, and both consumers include it:
//   * `common/surface.slang`  - the fragment stages' `gather_surface`: it runs the MASK test FIRST (a `discard`
//     before any derivative is requested), derives the tangent frame from the SCREEN-SPACE DERIVATIVES of
//     world_pos/uv (ddx/ddy are fragment-only) only for a material that has a normal map, and finishes the decode;
//   * `scene/visibility_resolve.slang` - reconstructs the frame from the TRIANGLE's own edges (a compute thread
//     has no derivatives and cannot discard) and answers a masked fragment with a flag instead of an exit.
//
// ONE DECODE, TWO CALLERS, and that is the point: a material fetched twice by two implementations would drift
// silently - the resolve would shade a surface the G-buffer path shades differently, and the picture would differ
// only in the pixels this path owns, which is the hardest kind of difference to read.
//
// IT COMES IN TWO PHASES, AND THE SPLIT IS A CORRECTNESS REQUIREMENT (measured: the capture gate's sponza scenario
// moved on 10.5% of its pixels when the decode was one function). The legacy fragment shader:
//   1. fetched the material and sampled the albedo;
//   2. DISCARDED an alphaMode MASK fragment below its cutoff, BEFORE any derivative;
//   3. sampled the remaining maps and, only for a material with a normal map, asked for the uv derivatives.
// Asking for a derivative QUADRATICALLY involves the other invocations of the pixel quad, and a killed invocation
// stops contributing to them - so step 2's position relative to step 3 is visible in the TBN of a normal-mapped,
// alpha-tested surface (sponza's foliage), and moving the derivative earlier changed the normal by enough to move a
// speckle of highlights. `gather_surface_base` is steps 1-2, `gather_surface_finish` is step 3 onward; the fragment
// caller keeps the legacy order, and the compute caller (which has no discard to place) runs them back to back.
//
// INCLUDE ORDER IS PART OF THE CONTRACT: like every body in this directory the file expects the heap shim
// (`common/heap_access.slang`) to be included FIRST by the leaf - the shim has no include guard (it is a set of
// function definitions), so including it here as well would define every accessor twice. The one include below is
// guarded and therefore safe from either route.

#ifndef DEREN_SURFACE_BODY_SLANG
#define DEREN_SURFACE_BODY_SLANG

// The Material record's layout (std430, 80 bytes): `material_at` resolves it through the heap slot the caller's
// shim exposed. INCLUDED RATHER THAN ASSUMED, because a fullscreen stage can reach this file too - it may not have
// taken the surface.slang route that used to be the only route to the type.
#include "common/material_record.slang"

// THE TEXTURE FETCH THE DECODE USES, chosen by the INCLUDING LEAF (a macro because only the leaf knows its stage):
//  - a FRAGMENT stage keeps `heap_sample` (implicit LOD): the sampler picks the mip from the pixel's derivatives,
//    which is what the G-buffer/forward pictures have always looked like;
//  - a COMPUTE stage must use `heap_sample_lod0` (explicit LOD): Slang turns an implicit `Sample` in compute into a
//    derivative-group quad instruction and DECLARES `ComputeDerivativeGroupQuadsKHR`, and vkCreateShaderModule then
//    refuses the module unless the device enabled that feature (measured on the visibility resolve). Explicit LOD 0
//    is the same mip the compute stage would have implicitly chosen, so the VALUE is unchanged - see the consuming
//    files (surface.slang leaves it default, scene/visibility_resolve.slang overrides it).
// A leaf that includes this file directly and defines nothing gets the FRAGMENT spelling, which is the safe default:
// it is the one every existing caller wants, and the compute caller is the one that has to say so.
#ifndef DEREN_SURFACE_SAMPLE_TEXTURE
#define DEREN_SURFACE_SAMPLE_TEXTURE heap_sample
#endif

/// @brief the FIRST PHASE's answer: the material record, the sampled base colour and the MASK verdict
/// @note the Material RECORD travels with the base colour because phase two needs its factors and texture lanes;
///       fetching it again would be a second material-table read whose only purpose is to be cheap.
struct surface_base {
    Material material;
    float4 base_color;  // base_color_factor * albedo texture (linear, NOT premultiplied)
    bool alpha_masked;  // the material is alphaMode MASK and this fragment's coverage is below its cutoff
};

/// @brief PHASE ONE of the decode: material table fetch, base-colour sample, glTF alphaMode MASK test
/// @param material_index the material record this surface is shaded with (the fragment path reads it from its push
///        block; the resolve reads it from the draw-item row)
/// @param uv interpolated texture coordinates
/// @return the material record, the base colour and whether the surface is CUT OUT
/// @note THE CALLER MUST DECIDE WHAT `alpha_masked` MEANS, and must decide it BEFORE it asks for a derivative: a
///       fragment stage calls `discard` here (that is the legacy order the capture gate pins); the compute resolve
///       cannot discard (a rasterized fragment's depth is already written), so v0 excludes MASK materials from the
///       visibility path and treats this as "no surface" rather than shading a cut-out.
surface_base gather_surface_base(const uint material_index, const float2 uv) {
    surface_base base;
    base.material = material_at(heap_slots_materials, material_index);
    base.base_color = base.material.base_color_factor * DEREN_SURFACE_SAMPLE_TEXTURE(base.material.tex_indices.x, uv);
    // glTF alphaMode MASK (record flag bit4): coverage is base_color_factor.a * albedo.a, compared with alphaCutoff
    base.alpha_masked = (base.material.flags & 16u) != 0u && base.base_color.a < base.material.alpha_cutoff;
    return base;
}

/// @brief everything the lighting code needs to know about one surface point
/// @note the names mirror the glTF metallic-roughness material model so the fields map 1:1 onto the
///       spec (albedo = baseColor, ao = occlusion, ...)
struct surface_sample {
    float3 albedo;    // base color: base_color_factor * albedo texture (linear, NOT premultiplied)
    float alpha;    // coverage: base_color_factor.a * albedo.a (only meaningful for MASK/BLEND)
    float3 normal;    // shading normal in world space: normal-mapped, double-sided flipped
    float3 emissive;  // emissive_factor * emissive texture (linear HDR-ish radiance)
    float roughness; // roughness_factor * metallic-roughness texture .g
    float metallic;  // metallic_factor * metallic-roughness texture .b
    float ao;        // mix(1, occlusion texture .r, occlusion_strength)
    uint flags;      // the material record's flag bits (see Material)
    uint toon_family; // the toon material family (see Material::toon_family); 0 == none
    // THE TOON TEXTURE LANES (see Material::toon_indices): x = diffuse ramp, y = shadow LUT, z = specular
    // ramp, w = matcap. ELEMENT 0 IS "DO NOT READ" - it is the white fallback, and a stage that read it as a
    // ramp would tint from a constant rather than from a no-op.
    uint4 toon_indices;
};

/**
 * @brief PHASE TWO of the decode: the remaining maps, the tangent-space normal and the glTF normal rules
 * @param base phase one's answer (its base colour is copied into @ref surface_sample::albedo)
 * @param geo_normal interpolated geometric normal (used when the material has no normal map)
 * @param uv interpolated texture coordinates
 * @param is_front the rasterizer's front/back decision for this fragment: the smooth normal of a DOUBLE-SIDED
 *        material is mirrored on a back face (glTF rule)
 * @param frame_tangent / @param frame_bitangent the tangent frame at this point, in WORLD space, normalized
 * @param frame_valid false when the caller's UV derivatives collapsed (a zero-area UV triangle or a seam), in
 *        which case the normal-map path falls back to the geometric normal instead of inventing a frame
 * @return the surface sample of this point
 * @note THE GLTF SEMANTICS LIVE HERE, ONCE: the double-sided normal flip, occlusion_strength and the tangent-space
 *       normal decode are the rules every pass that shades a surface must answer the same way. The MASK cutoff is
 *       phase one's (it has to happen before a derivative exists).
 */
surface_sample gather_surface_finish(const surface_base base,
                                     const float3 geo_normal,
                                     const float2 uv,
                                     const bool is_front,
                                     const float3 frame_tangent,
                                     const float3 frame_bitangent,
                                     const bool frame_valid) {
    const Material mat = base.material;
    surface_sample s;
    s.albedo = base.base_color.rgb;
    s.alpha = base.base_color.a;

    s.metallic = mat.metallic_factor * DEREN_SURFACE_SAMPLE_TEXTURE(mat.tex_indices.y, uv).b;
    s.roughness = mat.roughness_factor * DEREN_SURFACE_SAMPLE_TEXTURE(mat.tex_indices.y, uv).g;
    // occlusion: sampled AO modulated by occlusion_strength; without an occlusion map the slot is
    // the white fallback (ao = 1) and the strength has no effect
    s.ao = lerp(1.0, DEREN_SURFACE_SAMPLE_TEXTURE(mat.tex_indices.w, uv).r, mat.occlusion_strength);
    s.emissive = mat.emissive_factor.rgb * DEREN_SURFACE_SAMPLE_TEXTURE(mat.emissive_index, uv).rgb;
    s.flags = mat.flags;
    s.toon_family = mat.toon_family;
    s.toon_indices = mat.toon_indices;

    // ---- normal: optional tangent-space normal map, else the interpolated normal ----
    if ((mat.flags & 1u) != 0u) {
        const float3 normal = normalize(geo_normal);
        if (!frame_valid) {
            s.normal = normal; // degenerate UV derivatives: fall back to the interpolated normal
        } else {
            float3 tbn_normal = DEREN_SURFACE_SAMPLE_TEXTURE(mat.tex_indices.z, uv).rgb * 2.0 - 1.0;
            tbn_normal.xy *= mat.normal_scale;
            tbn_normal = normalize(tbn_normal);
            s.normal = normalize(mul(tbn_normal, float3x3(frame_tangent, frame_bitangent, normal)));
        }
    } else {
        s.normal = normalize(geo_normal);
    }
    // double-sided material (record flag bit3): mirror the normal on back faces, as the glTF spec
    // requires, so the inner side of a shell is lit by its inward-facing normal
    if ((mat.flags & 8u) != 0u && !is_front) {
        s.normal = -s.normal;
    }
    return s;
}

#endif // DEREN_SURFACE_BODY_SLANG
