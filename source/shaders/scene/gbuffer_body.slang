// shaders/scene/gbuffer_body.slang - WHAT A G-BUFFER WRITE IS MADE OF, declared once for BOTH producers
// (visibility-buffer v0.4b).
//
// TWO STAGES WRITE THE FIVE G-BUFFER TARGETS: `scene/gbuffer.slang`'s fragment stage (the path the frame has
// always used) and `scene/visibility_resolve.slang` (the visibility path's compute resolve, which rebuilds the
// attributes from the id and gets no interpolators or derivatives for free). If either carried its own copy of
// the composition, the two pictures would drift in exactly the pixels the visibility path owns - and a G-buffer
// difference is not a "small" difference: it multiplies by every light, so a 1% albedo drift is a lighting
// change. This file is that composition: the five outputs' shape, the material id's two-byte packing, the
// emissive term, and the motion vector's projection rule. Both producers call it.
//
// WHAT IS NOT HERE, because it is each PRODUCER'S: the surface VALUES (the fragment path interpolates them from
// the rasterizer's varyings, the resolve reconstructs them from a triangle - both end as a `surface_sample`) and
// the projection MATRICES (read from the push block by one producer's camera heap slot, from the camera block by
// the other's - passed in, so this file never has to know which).
//
// INCLUDE ORDER: `common/heap_access.slang` first (this file uses VR_MAT4 and the heap accessors through its
// caller), then this file; the material decode is included from here so a caller that only wants the composition
// still gets `surface_sample` for free. See the leaf files' headers for the full order contract.

#ifndef DEREN_GBUFFER_BODY_SLANG
#define DEREN_GBUFFER_BODY_SLANG

#include "common/surface_body.slang"

/// THE G-BUFFER'S FIVE TARGETS, in attachment order (= the fragment outputs' locations). The layout is the
/// contract with render_layout's `gbuffer_formats` / `gbuffer_velocity_format` / `hdr_format` and with the deferred
/// lighting stage's reads; see deren.engine.render_layout.
struct gbuffer_output {
    [[vk::location(0)]] float4 albedo_metallic;  // rgb albedo, a metallic
    [[vk::location(1)]] float4 normal_roughness; // xyz world normal, w roughness
    [[vk::location(2)]] float4 material;         // r/g material id, b ao, a flags
    [[vk::location(3)]] float2 velocity;         // motion vector in UV space (current - previous)
    [[vk::location(4)]] float4 scene_color;      // the EMISSIVE term, ADDED into the scene color
};

/// @brief the value a pixel that NO geometry covered keeps: every target's clear value.
/// @note the old path's clear is the render pass's own (`scene_pass::record_content` clears every colour target
///       to zero and the depth to 1.0), so the visibility resolve must WRITE this value for an empty pixel: a
///       compute pass has no load op, and "did not write" would leave whatever the target held last frame.
/// @note the DEPTH target is not in this struct: the resolve reads a depth image the raster lane produced, it does
///       not write one (see the plan's data flow - depth comes from the raster, not from the resolve).
gbuffer_output gbuffer_empty_output() {
    gbuffer_output out;
    out.albedo_metallic = float4(0.0, 0.0, 0.0, 0.0);
    out.normal_roughness = float4(0.0, 0.0, 0.0, 0.0);
    out.material = float4(0.0, 0.0, 0.0, 0.0);
    out.velocity = float2(0.0, 0.0);
    out.scene_color = float4(0.0, 0.0, 0.0, 0.0);
    return out;
}

/// @brief motion vector of this fragment in UV space: where it was last frame, relative to here
/// @param view_proj_unjittered the camera's view*proj WITHOUT this frame's TAA jitter
/// @param prev_view_proj the previous rendered frame's view*proj (unjittered by construction)
/// @note both matrices are the JITTER-FREE pair (the TAA jitter lives in `proj`; a jitter that leaked in
///       here would be read as camera motion), and the previous position is projected with
///       `prev_view_proj` alone because the object's own motion is already baked into it.
/// @note this covers camera motion, rigid object motion and a DEFORMING mesh (see
///       docs/deformation_motion_vectors.md); alpha-blended geometry is not covered, because it is
///       composited outside the G-buffer and writes no velocity at all.
float2 motion_vector(float3 world_pos, float3 prev_world_pos, VR_MAT4 view_proj_unjittered, VR_MAT4 prev_view_proj) {
    // `M * v`, NOT `mul(M, v)`: MEASURED, and the opposite of what the HLSL habit suggests. For a
    // `row_major float4x4` (the spelling that emits the ColMajor layout the host's glm::mat4 has),
    // Slang's `mul(M, v)` compiles to OpVectorTimesMatrix - the TRANSPOSED product - while the GLSL
    // operator keeps GLSL's column-vector meaning under `-allow-glsl` and compiles to exactly the
    // OpMatrixTimesVector glslc emits for gbuffer.frag. Getting this backwards is not loud: the
    // matrix still loads from the right bytes (the member offsets and MatrixStride were verified equal)
    // and only the velocity comes out wrong, which showed up as a black motion channel. See
    // docs/slang_migration.md section 9, "the first leaf port: two traps, both measured, both fixed".
    const float4 current_clip = mul(float4(world_pos, 1.0), view_proj_unjittered);
    const float4 previous_clip = mul(float4(prev_world_pos, 1.0), prev_view_proj);
    const float2 current_uv = (current_clip.xy / current_clip.w) * 0.5 + 0.5;
    const float2 previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;
    return current_uv - previous_uv;
}

/**
 * @brief the five-target write of one gathered surface: the composition both producers share
 * @param s the surface the producer gathered (its values are already in world space and glTF-decoded)
 * @param material_index the material record's index, packed into the material target's two low bytes
 * @param velocity the motion vector `motion_vector` computed for this pixel
 * @note THE MATERIAL ID IS STORED AS TWO UNORM BYTES because the old path stored it that way and the lighting
 *       stage decodes it with `round(255 * stored)`; k / 255 is exactly representable, so the byte round-trips.
 * @note The emissive term is stored with opaque replacement, matching the surface/depth winner.
 *       Multiple fragments may pass depth in far-to-near order; additive blending would retain hidden emission.
 */
gbuffer_output gbuffer_from_surface(surface_sample s, uint material_index, float2 velocity) {
    gbuffer_output out;
    // the world normal needs no encoding in an RGBA16F target (see the GLSL file's docs)
    out.albedo_metallic = float4(s.albedo, s.metallic);
    out.normal_roughness = float4(s.normal, s.roughness);

    // 16-bit material id split over two UNORM channels: k / 255 is exactly representable, so
    // round(255 * stored) returns the byte that was written even after the format's conversion
    const uint id = material_index;
    out.material = float4(
        float(id & 0xFFu) / 255.0,
        float((id >> 8u) & 0xFFu) / 255.0,
        s.ao,
        float(s.flags & 0xFFu) / 255.0);

    out.velocity = velocity;

    // The emissive term overwrites scene color with this depth-winning surface's value, because
    // emissive is lighting-independent and needs the material's emissive texture and this fragment's UVs -
    // neither of which the G-Buffer stores, so the lighting stage could not reconstruct it afterwards.
    out.scene_color = float4(s.emissive, 1.0);
    return out;
}

#endif // DEREN_GBUFFER_BODY_SLANG
