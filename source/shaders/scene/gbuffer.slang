// shaders/gbuffer.slang - the SLANG source of the G-buffer write stage.
//
// PRODUCES `gbuffer.frag.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/gbuffer.frag (now in `shaders/glsl.old/`): the body is the GLSL body, and
// what changes is only what a different language requires - the entry point's parameters and the output
// struct carry `[[vk::location(n)]]` instead of `layout(location = n)`, and the heap is reached through
// the shared accessors (see docs/slang_migration.md).
//
// THE INCLUDE ORDER IS PART OF THE CONTRACT: the shim first, then surface.slang (which declares the push
// constant block and the heap_frame_slot / heap_image_index aliases that the slot macros expand against),
// then the rest of the shared bodies. Compiling a body before surface.slang reports heap_frame_slot as an
// undefined identifier - measured.
//
// WHAT THE FRAGMENT STILL OWNS, since visibility-buffer v0.4b: ONLY THE INTERPOLATORS. The five-target
// composition, the motion vector and the material decode live in `scene/gbuffer_body.slang` +
// `common/surface_body.slang`, because the visibility path's COMPUTE resolve writes the same five targets from
// the same material decode and a second copy of either is the divergence this file exists to prevent. What is
// left here is what only a rasterized fragment has: the varyings the vertex stage produced, the front-face test,
// and the surface gather (whose tangent frame comes from screen-space derivatives - a compute thread has none).
//
// THE OUTPUT VALUES ARE UNCHANGED BY THAT EXTRACTION, and the capture gate is the acceptance that says so: the
// gate's deferred scenarios are byte-identical to the previous commit.

#include "common/heap_access.slang"
#include "common/surface.slang"
#include "lighting/shading.slang"
// ... and the shared G-buffer composition (the five targets + the motion vector), included AFTER shading because
// it reads the camera's matrices through `camera_at`, whose block type shading.slang declares.
#include "scene/gbuffer_body.slang"

/// @brief write the fragment's surface into the G-buffer targets
/// @note no lighting, no tonemapping, no output alpha semantics: what the surface carries and lighting
///       does not is stored as-is, and the lighting stage decides what to do with it.
[shader("fragment")]
gbuffer_output main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    [[vk::location(3)]] float3 v_prev_world_pos : TEXCOORD3, bool v_is_front : SV_IsFrontFace)
{
    // the material decode (the MASK `discard` included) is `gather_surface`'s; the frame it derives from ddx/ddy
    // is this stage's own, which is why the gather could not be a compute body itself.
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv, v_is_front);
    // ... and the write is the SHARED composition: the same expression the visibility resolve runs per pixel, so
    // the two producers cannot disagree about what a G-buffer texel means.
    const float2 velocity = motion_vector(v_world_pos, v_prev_world_pos, camera_at(heap_camera_slot).view_proj_unjittered, camera_at(heap_camera_slot).prev_view_proj);
    return gbuffer_from_surface(s, push.material_index, velocity);
}
