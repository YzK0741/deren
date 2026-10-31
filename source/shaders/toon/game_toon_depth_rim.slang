// shaders/toon/game_toon_depth_rim.slang - THE GAME-SHADER CHAIN'S SCREEN-SPACE DEPTH RIM.
//
// WHAT IT IS: the captured GameToon viewer's depth-contour rim, drawn as a fullscreen ADDITIVE pass after the
// game-shaded character stage and before the resolve. The capture computes it in the HAIR fragment stage
// (`source/shaders/toon/game_toon/captured/depth.h`, transcribed from `readable:310-318`):
//
//     offset  = shading_normal_screen_xy * rimWidth * 0.006        (wf_depth_offset_uv, depth.h:183-184,263-266)
//     factor  = smoothstep((abs(scene_view_distance) - own_view_distance - 0.1) * 10.0)   (depth.h:244-247)
//     R29     = factor * S,  i.e. with the depth image live the captured rim IS this contour
//
// That stage cannot sample the depth image: in `character_forward` the depth is the DEPTH ATTACHMENT (an image
// cannot be an attachment and a sampled texture in one instance), which is exactly why this pass exists. Here the
// depth is only sampled, so the captured comparison is legal; the pass's push block supplies the two
// depth-linearization terms, `rim_width` (`_RimWidth` / the light's `nprData.x`: 0.368 or 0.4 in the capture) and
// `rim_scale` (the capture's gain, 10.0).
//
// TWO DELIBERATE ADAPTATIONS, both forced by being a fullscreen stage instead of a surface fragment:
//  1. THE OFFSET DIRECTION. The capture offsets along the screen projection of the view-space SHADING NORMAL, which
//     a fullscreen pass does not have. It probes BOTH SCREEN AXES with the same captured length instead - the
//     sign-free reading `toon_screen_rim.slang` already documents: whichever side of the pixel contains the farther
//     surface is the silhouette, and four probes find it on a vertical or a horizontal edge.
//  2. THE COMPARISON IS ON LINEAR VIEW DISTANCES, as in the capture (`depth.h:234-239`): the stored window depth is
//     inverted with the frame's own projection terms, and the capture's 0.1 world-unit bias and 10.0 gain are
//     applied to that difference rather than to raw depth.
//
// The colour is THIS pass's own parameter (the profile's rim tint, through `runtime::set_toon_screen_rim`) and is
// multiplied by `saturate(albedo + 0.3)` so a dark material's contour keeps its own hue, the MME pack's
// `albedoRimColor` rule.
//
// 3. THE CONTOUR IS HAIR-ONLY, WHICH IS THE CAPTURED RULE RATHER THAN A TUNING. The captured depth block is
//    compiled under `#ifdef FAMILY_HAIR` (`character.frag.readable.glsl:296`), so in the capture ONLY the hair
//    surface carries this term; the eye's program has no depth term at all (`FAMILY_EYE` skips the rim-mode
//    lights outright: `readable:238-241` `if(mode==3)continue`), and the skin/cloth/brow programs never compile
//    the block. The fullscreen stage has no family of its own, so it recovers it from the G-BUFFER'S MATERIAL
//    SIGNAL: `gbuffer_material` stores the material id in two UNORM bytes, and the material table answers
//    `toon_family` for that id - the exact read `toon_screen_rim.slang:89-106` already performs for its own
//    family gate. That is why no hair MASK TARGET and no extra render-graph input is needed here: the material
//    target is one of the per-image G-buffer heap slots the renderer's stage preamble publishes for sampling,
//    and this pass already indexes the same family of slots for depth and albedo. The gate is binary because the
//    capture's is: the term exists for a hair fragment and does not exist for any other fragment.
//
// WHAT THE GATE REMOVES: without it the pass outlines every toon-shaded silhouette (face against hair, cloth
// against the background, a glove against the forearm). Those contours are not in the capture - the reference
// viewer's `R29` is computed inside the hair fragment - and they were the "family-independent contour" this
// pass started as.

#include "common/heap_access.slang"
#include "common/material_record.slang"
#include "toon/toon_params.slang"

struct GameToonDepthRimPush
{
    float proj_22;      // projection[2][2]: the depth-linearization terms (see rim_view_distance)
    float proj_32;      // projection[3][2]
    float rim_width;    // the captured `_RimWidth` / `nprData.x`: the offset is rim_width * 0.006 of the uv extent
    float rim_scale;    // the captured gain on the biased depth difference (10.0)
    float rim_strength; // 0 disables the whole pass (the profile's on/off)
    float pad0;
    float pad1;
    float pad2;
    float4 rim_colour;  // THIS pass's contour colour (the profile's captured rim tint)
    uint frame_slot;    // THE HEAP INDICES, at the END so every field above keeps its offset
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<GameToonDepthRimPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// the captured offset (`depth.h:183-184`): `rimWidth * 0.006` of the uv extent
static const float game_rim_offset_scale = 0.006;
/// the captured comparison bias and gain (`depth.h:185-188`): 0.1 world units, `* 10.0`
static const float game_rim_depth_bias = 0.1;

/// @brief view-space distance of a depth-buffer sample (the capture's linearization, in deren's ZO projection)
/// @note the same inversion `gbuffer_debug.slang` and `toon_screen_rim.slang` document: for the perspective
///       projection the CPU builds, z_view = proj_32 / (z_ndc + proj_22). The capture takes `abs()` of it
///       (`depth.h:241-247`), and so does this shader.
float rim_view_distance(float depth)
{
    return abs(pc.proj_32 / (depth + pc.proj_22));
}

float rim_depth_at(float2 uv)
{
    return heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, uv).r;
}

/// THE CAPTURED FAMILY WEIGHT (header adaptation 3): 1 on a HAIR pixel and 0 on every other family.
///
/// The capture's depth block is `#ifdef FAMILY_HAIR` (`character.frag.readable.glsl:296`), and its eye program
/// explicitly skips the rim-mode lights that would reach it (`readable:238-241`), so hair is the only family the
/// term belongs to. This pass has no surface of its own, so the family is recovered from the G-BUFFER'S MATERIAL
/// SIGNAL: `gbuffer_material` holds the material id as two UNORM bytes, and the material table answers
/// `toon_family` for that id - the same decode `toon_screen_rim.slang:95-99` performs. The gate is binary because
/// the capture's is: the term exists for a hair fragment and does not exist for any other.
float rim_family_weight(float2 uv)
{
    const float4 material_target = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_material + heap_image_index), heap_sampler_gbuffer, uv);
    const uint material_id = uint(round(material_target.r * 255.0)) | (uint(round(material_target.g * 255.0)) << 8u);
    const Material material = material_at(heap_slots_materials, material_id);
    return material.toon_family == VR_FAMILY_HAIR ? 1.0 : 0.0;
}

/// the captured `R24` (`depth.h:217-221`): `x = clamp(x,0,1); return x*x*(3-2x);`
float rim_smoothstep(float x)
{
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    const float stored_depth = rim_depth_at(v_uv);
    // THE CLEARED DEPTH IS THE FAR PLANE: those pixels hold no surface, so there is no contour to draw and
    // returning zero leaves the frame exactly as the character stage left it.
    if (stored_depth >= 1.0 || pc.rim_strength <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // THE HAIR GATE (header adaptation 3). Before any probe: a non-hair pixel has no captured depth contour at
    // all, so the four depth fetches below are not even taken there. This is the one line that turns the old
    // family-independent silhouette into the capture's hair-only term.
    if (rim_family_weight(v_uv) <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float own_distance = rim_view_distance(stored_depth);
    // THE CAPTURED OFFSET LENGTH, probed on both screen axes (adaptation 1 above). `rim_width * 0.006` is a
    // fraction of the uv extent: 0.368 -> 0.0022 uv = a little over two pixels at 1000 px, which is the width the
    // captured viewer draws. (The ARTICLE's contour instead divides by `(eye_depth + 3)`, which at the same
    // rimWidth is a THIRD of a pixel - the reason it is invisible on the game chain.)
    const float step = pc.rim_width * game_rim_offset_scale;
    const float2 probes[4] = {float2(step, 0.0), float2(-step, 0.0), float2(0.0, step), float2(0.0, -step)};
    float rise = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const float neighbour_depth = rim_depth_at(saturate(v_uv + probes[i]));
        // A FAR-PLANE NEIGHBOUR IS THE STRONGEST CONTOUR there is (the character against the background); the
        // captured comparison would linearize it to the far distance and saturate the same way.
        const float neighbour_distance = neighbour_depth >= 1.0 ? 1.0e6 : rim_view_distance(neighbour_depth);
        rise = max(rise, neighbour_distance - own_distance);
    }

    // THE CAPTURED COMPARISON (`depth.h:244-247`): smoothstep of the biased difference times the pass's gain.
    const float rim_area = rim_smoothstep((rise - game_rim_depth_bias) * pc.rim_scale);
    if (rim_area <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // THE CONTOUR LIGHTENS THE SURFACE IT OUTLINES rather than adding a flat colour (the MME pack's
    // `albedoRimColor = saturate(albedo + 0.3)`), so a dark material's contour is still recognisably its colour.
    const float3 albedo = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_albedo + heap_image_index), heap_sampler_gbuffer, v_uv).rgb;
    const float3 rim = pc.rim_colour.rgb * rim_area * pc.rim_strength * saturate(albedo + 0.3);

    // The pipeline blends ONE/ONE, so this is an ADDITION to the scene colour and the alpha lane is unused.
    return float4(rim, 0.0);
}
