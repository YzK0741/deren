#ifndef DEREN_GAME_TOON_GAME_TOON_DEPTH_H
#define DEREN_GAME_TOON_GAME_TOON_DEPTH_H

// ============================================================================
// GAME_TOON VIEWER: THE CAPTURED SCENE-DEPTH TERM OF THE HAIR RIM (`Q285`/`Q272`)
// ============================================================================
//
// WHAT THIS FILE IS. The captured GameToon Studio viewer keeps a SCREEN-SPACE OPAQUE DEPTH image in a uniform
// pair - `Q285` (a `sampler2D` bound to a depth texture) and `Q272` (a 3-state availability word) - and the ONE
// consumer that is actually wired in the recovered code is the character fragment shader's `npr:"rim"` arm:
//
//     character.frag.readable.glsl:296-319, inside `R114`'s `mode == 3` branch:
//
//         #ifdef FAMILY_HAIR                                  // :296 - HAIR ONLY
//         if(Q272>0.5&&Q272<1.5){                             // :297 - only when a depth image was drawn
//         #ifdef Q198                                         // :299 - LIT-OUTLINE program: gl_FragCoord arm
//         vec2 R247=gl_FragCoord.xy/Q76; float R219=isOrthographic? ... : 1.0/gl_FragCoord.w;
//         #else
//         vec4 R258=viewMatrix*vec4(vWorldPosition,1.0);      // :304 - MAIN program: project world position
//         vec4 R158=projectionMatrix*R258;
//         vec2 R247=R158.xy/R158.w*0.5+0.5;                  // :306 - screen uv
//         float R219=abs(R258.z);                            // :307 - this fragment's view distance
//         #endif
//         vec2 R166=(mat3(viewMatrix)*N).xy;                 // :310 - view-space shading normal, screen xy
//         R166*=inversesqrt(max(dot(R166,R166),1e-8));       // :311
//         vec2 R289=vec2(textureSize(Q285,0));               // :312 - (width, height)
//         R166.x*=R289.y/R289.x;                             // :313 - pixel-isotropic offset
//         vec2 R80=R247+R166*R164.x*0.006;                   // :314 - offset by rimWidth * 0.006
//         R80=clamp(R80,1.0/R289,1.0-1.0/R289);              // :315
//         float R118=texture2D(Q285,R80).x*2.0-1.0;          // :316 - window depth -> GL NDC
//         float R68=isOrthographic?(R118-projectionMatrix[3][2])/projectionMatrix[2][2]
//                                 :projectionMatrix[3][2]/(R118+projectionMatrix[2][2]);  // :317
//         R29=R24((abs(R68)-R219-0.1)*10.0)*S;               // :318 - R24 = x*x*(3-2x) after clamp
//         }                                                    //     (:317 is the depth LINEARIZATION)
//         #endif
//
// (`R164.x` is the rim light's captured `nprData.x` / `rimWidth`: 0.368 and 0.4 for the two Laevatain rim
// lights, `2fcd9c7ad0-...-laevatain.json`; `S` is `clamp(dot(N,L)+1,0,1)`, `readable:244`, i.e. deren's
// `visibility` in `game_toon_lights.h:68`; `R24` is `x*x*(3-2*x)` after a clamp, `readable:171-174`.)
//
// WHAT IT DOES, stated without over-claiming intent: it samples the opaque scene depth one small screen-space
// normal offset away and keeps the rim only where THAT sample is FARTHER than this fragment by more than 0.1
// world units (`10.0` is the gain). Nearer or equal -> the rim term is 0. In `Q272 == 1` the depth factor
// REPLACES the view-angle `band` (`R29 = depth * S`, not `band * depth * S`, readable:318) - i.e. with the
// depth image live the captured "rim" is a depth contour, not the view-angle band.
//
// WHAT THE DEPTH IMAGE IS, from the recovered JS (this is the part that decides whose/depth and when):
//   * `tmp/game_toon-studio-audit/final/responses/eadba1af4f-DIaZSo8d.js:38` (the bundled app chunk; the saved
//     `final/viewer-lighting.js` is only a 2,511-byte lighting-rig excerpt and does NOT contain it):
//       `const Nm=bt,pt={Q285:{value:Nm},Q272:{value:0}},...`               - the shared uniform pair
//       `class YQ`  = the scene-depth prepass: its own `WebGLRenderTarget` at renderer size with a
//                     `DepthTexture`, `NearestFilter`; an override material `{colorWrite:false, side:Ir}` named
//                     `"scene-depth"`; render() hides every mesh that is NOT opaque/depth-writing/non-instanced
//                     (and all lines/points/sprites), clears DEPTH ONLY, renders the camera, then publishes
//                     `pt.Q285.value = target.depthTexture; pt.Q272.value = 1;`
//       `Q272` states: constructor and the "not requested this frame" early return set 2; a drawn prepass sets 1;
//                     `dispose()` sets 0 (and `Q285` back to the 1x1 white `bt`).
//       `Zt={frame,drawnAt,requested}`, `My()`=requested, `Im()`=drawnAt=frame: a prepass is drawn when a soft
//                     particle asks for one (`My`, particle draw) OR when a rim-mode material rendered LAST frame
//                     (`Im`, called from `Xf`'s `onBeforeRender` whenever the rig has a `mode==3` light).
//   * `tmp/game_toon-studio-audit/final/material-uniform-bindings.js:1`:
//       `function Km(){return{Q285:{value:null},Q272:{value:0}}}` and, in the uniform assembly,
//       `...n.fam==="hair"?Km():{},` - THE DEPTH UNIFORMS EXIST ONLY ON HAIR MATERIALS.
//       `function Xf(n,e,t){... e.uniforms.Q285.value=pt.Q285.value; e.uniforms.Q272.value=pt.Q272.value;
//        L3(t)&&Im() ...}` `L3(n)` = "does the rig's light list contain `npr` mode 3"; `Xf` is called as
//       `f==="hair"&&Xf(a,T,s.rig.uniforms)` for surface materials and `l==="hair"&&Xf(g,v,...)` for outlines
//       (`...eadba1af4f...js:606`).
//
// SO THE EYE DOES NOT READ THIS IMAGE. `eye.slang` has no depth term in the capture, the shared `R114` loop
// skips `mode==3` for `FAMILY_EYE` (`readable:238-241`), and the uniform builder only merges `Km()` for
// `fam==="hair"` (`material-uniform-bindings.js:1`). `hair-notes.md:217`'s "eye-through-hair depth test" is a
// misreading of this block - it is the HAIR RIM's own occlusion test. Do not add a depth read to the eye.
// (The same `Q285/Q272` names appear again in the VFX `_USE_SOFTBLEND` template - `...js:690`, `#ifdef Q271`,
// `Q259=_SoftDistance`, `Q130=_SoftBias` at `...js:744` - but no recovered code ever writes `pt.Q285/Q272` into
// a VFX material, so that program is either inert in this build or supplied by a path the audit did not
// recover. It is NOT a character-shader consumer and this header does not model it.)
//
// WHY THIS IS A HEADER AND NOT A SHADER. deren's `character_forward` pass holds `gbuffer_depth` as its DEPTH
// ATTACHMENT (`render_resource.cppm:980-993`, recorded at `character_forward.cpp:75-81`), and an image cannot be
// a depth attachment and a sampled texture in one instance. THERE IS NO SAMPLED SCENE-DEPTH TEXTURE IN THIS
// STAGE TODAY. The host therefore has to supply one (`depth-notes.md` in `tmp/game_toon-port/` gives the two
// paths: a dedicated prepass, or a depth copy), and until it does, this header is deliberately INERT: the sample
// function below is the single injection point, it returns the captured "no prepass" value, and
// `GAME_TOON_DEPTH_RESOURCE_BOUND` is 0 so the rim path never calls it.
//
// INCLUDE ORDER. Include this AFTER `toon/character_forward.slang` (which brings `common/heap_access.slang` for
// `camera_at`/`heap_camera_slot` and `lighting/shading.slang` for `CameraUBO`) - exactly the precondition
// `game_toon_lights.h` already has, because this file reads `camera_at(heap_camera_slot)`. Recommended insertion:
// `#include "toon/game_toon/captured/depth.h"` inside `game_toon_lights.h` immediately after its include guard, so
// whichever family file reaches `game_toon_lights.h` first also gets the declaration it needs at the call site.
//
// ============================================================================
// THE Deren ADAPTERS, EACH STATED WHERE IT MATTERS
// ============================================================================
// 1. DEPTH IMAGE -> RAW [0,1] WINDOW DEPTH. `game_toon_depth_scene_raw`. The capture samples a three.js
//    `DepthTexture` (nearest) and reads `.x`, which is the window depth in [0,1]. A Vulkan depth copy read
//    through `Texture2D<float4>.r` gives the same quantity for the same near/far. The function is a HOST HOOK:
//    replace its body once the resource exists; nothing else in this file names a binding, a heap slot or a
//    texture. `sample_uv` arrives already clamped.
// 2. PROJECTION TERMS. The capture's `projectionMatrix[3][2]` / `[2][2]` are GLSL element spellings. deren's
//    `CameraUBO.proj` is a `VR_MAT4` = `row_major float4x4` holding glm's column-major bytes, so Slang's
//    `proj[i][j]` is the SAME element as GLSL's `projectionMatrix[i][j]` (this is the documented reason the
//    buffer uses `VR_MAT4`; `shading.slang:44-45`, `mesh_fetch.slang:20`). deren builds `perspectiveRH_ZO`
//    (`primitive.cpp:360-366`) - Vulkan's [0,1] depth, not GL's [-1,1] - so the capture's `*2-1` step and its
//    GL constants are replaced by deren's own inversion, the one this engine already uses
//    (`toon_screen_rim.slang:63-71`, `goo_rim.slang:218-236`):
//        window depth d, z_ndc_vk = d;  z_view = proj[3][2] / (d + proj[2][2])       (perspective)
//    EQUIVALENCE, so the transcription is provable rather than a reinterpretation: for the same near/far the
//    window depth a GL projection and a ZO projection store is the SAME number (both are [0,1] window depth),
//    and a_gl = 2*a_zo + 1, b_gl = 2*b_zo, so
//        b_gl / (2d - 1 + a_gl) = 2b_zo / (2d + 2a_zo) = b_zo / (d + a_zo).
//    The capture's `*2-1` therefore folds into deren's terms; the helper below applies deren's form.
// 3. VIEW CONSTANTS. `CameraUBO.view` (same `VR_MAT4`). deren's shaders multiply column-vector style -
//    `mul(view, float4(...))` (see `game_toon_lights.h:39,44-47`) - which is GLSL's `viewMatrix * vec4(...)`, so
//    `mul(view, float4(world_pos,1)).z` is the capture's `R258.z`, and `R219 = abs(view.z)` is
//    `game_toon_depth_own_view_distance`. The capture's `1.0/gl_FragCoord.w` arm is the same distance; it is not
//    needed because the capture's MAIN character program takes the `#else` arm anyway: `Q198` (the lit-outline
//    define) is never set on a character material (`...js:606` `addOutline` does `this.litOutlines&&(c.Q198="")`),
//    and `character.vert.glsl` has neither `Q198` nor `Q76` (only `outline.vert.glsl:6,17-18,39-40` does).
// 4. SCREEN UV. `camera.proj` is the CURRENT projection INCLUDING the TAA jitter when TAA is on
//    (`primitive.cppm:111-126`) - the same matrix `gbuffer_depth` was rasterized with - so projecting
//    `world_pos` with it reproduces the depth image's own pixel grid. deren's projection has `proj[1][1] *= -1`
//    (`primitive.cpp:364`), the Vulkan y-down framebuffer flip, so `clip.y/w*0.5+0.5` is the top-left-origin v
//    Vulkan textures use: the uv expression is the capture's, unchanged. What DOES change is the DIRECTION of
//    the screen-space normal's y: the capture's uv is bottom-up (GL), deren's is top-down, so the offset uses
//    `-viewNormal.y` to point the sample at the same screen feature (see `wf_depth_offset_normal`).
// 5. ASPECT. The capture uses `textureSize(Q285).y/.x` (height/width). When the render extent is not available
//    to this stage (it is not in `surface.slang`'s push block - only `frame_slot`/`image_index` are),
//    `game_toon_depth_height_over_width()` derives width-normalised pixels from the projection:
//    height/width = proj[0][0] / |proj[1][1]| (the |.| absorbs the y flip). If the Lead threads the extent
//    through, pass it as `depth_extent` instead and the captured clamp-to-one-texel is reproduced exactly;
//    with `float2(0)` the clamp degrades to `saturate` (at most one pixel of difference at the screen edge).
//
// ============================================================================
// HOW THE CAPTURED STATES MAP (Q272). Constants are floats so the captured comparisons are transcribed as-is.
// ============================================================================
//   GAME_TOON_DEPTH_MODE_OFF   = 0.0  - no depth image (disposed / never bound): rim = band * S (today's port)
//   GAME_TOON_DEPTH_MODE_LIVE  = 1.0  - a depth image was drawn this frame: rim = depth_factor * S
//   GAME_TOON_DEPTH_MODE_STALE = 2.0  - a depth image exists but was not drawn this frame: rim = band * S
// In deren the "stale" state has no use: a depth copy/prepass runs on every frame the character stage runs, so
// the mode is either OFF (resource not wired yet, or the pass is gated off) or LIVE. `GAME_TOON_DEPTH_MODE` is the
// compile-time source; the weight function takes it as a float so a runtime lane can replace it later
// (`ToonRigUBO.reference_modes.z/w` are reserved, `character_forward.slang:257`).
//
// ============================================================================
// WHAT THIS FILE DOES NOT DO (each deliberate)
// ============================================================================
// * It does not sample `gbuffer_depth` and must not: in `character_forward` that image is the depth attachment
//   (`character_forward_targets = {scene_color, gbuffer_depth}`, `render_resource.cppm:980-983`). The fetch
//   below is for a NEW sampled image the host creates (prepass or copy); see `tmp/game_toon-port/depth-notes.md`.
// * It does not model the lit-OUTLINE program's `gl_FragCoord`/`Q76` arm (`readable:299-301`). That program
//   needs `gl_FragCoord.xy/Q76` and `1.0/gl_FragCoord.w`, which a deren character fragment stage does not
//   receive, and deren's outline is a different pipeline (`outline.slang`, the article's inverted hull). The
//   hair SURFACE path is what this header serves.
// * It does not reproduce the capture's 0.1 world-unit bias in any normalised unit: 0.1 is a distance in metres
//   along the view axis, compared against linearised view distances. Keep it a distance.
// * It does not port the `R75 * Q164` depth FADE (`R200`, `readable:340-348,1177`) - a different mechanism
//   (`Q185 < 0.5` in the capture, so it contributes nothing) and already recorded as not ported in
//   `hair-notes.md:190-191`.

// ------------------------------------------------------------------------------------------------------------
// THE HOST HOOKS AND THE CAPTURE'S NUMERIC CONSTANTS
// ------------------------------------------------------------------------------------------------------------

/// Set to 1 ONLY after `game_toon_depth_scene_raw` below has been given a real fetch (see the notes' integration
/// steps). While 0 the rim path cannot sample anything and `game_toon_depth_rim_weight` returns the capture's
/// depth-disabled result.
#ifndef GAME_TOON_DEPTH_RESOURCE_BOUND
#define GAME_TOON_DEPTH_RESOURCE_BOUND 0
#endif

/// THE CAPTURED `Q272` VALUE (see the state table above). OFF by default: the port must be pixel-identical to
/// today's `band * visibility` until the host supplies a depth image.
#ifndef GAME_TOON_DEPTH_MODE
#define GAME_TOON_DEPTH_MODE 0.0f
#endif

/// `GAME_TOON_DEPTH_MODE`'s three captured states (`Q272`: 0 dispose, 1 drawn, 2 exists-but-stale).
#define GAME_TOON_DEPTH_MODE_OFF 0.0f
#define GAME_TOON_DEPTH_MODE_LIVE 1.0f
#define GAME_TOON_DEPTH_MODE_STALE 2.0f

/// `readable:314` - `R166 * R164.x * 0.006`: the screen-space offset is `rimWidth * 0.006` of the uv extent.
static const float game_toon_depth_rim_offset_scale = 0.006f;
/// `readable:318` - the 0.1 world-unit bias: a neighbour closer than this is "not a contour".
static const float game_toon_depth_bias = 0.1f;
/// `readable:318` - the gain on the depth difference (`* 10.0`).
static const float game_toon_depth_gain = 10.0f;
/// `readable:311` - `max(dot(R166,R166),1e-8)`: the squared-length floor before the normalise.
static const float game_toon_depth_normal_epsilon = 1.0e-8f;

// ------------------------------------------------------------------------------------------------------------
// THE PURE MATH (dual-language: this file compiles under Slang and under a C++ test, the way
// `toon/game_toon_math.h` does, so the linearisation and the offset can be pinned numerically)
// ------------------------------------------------------------------------------------------------------------

#ifdef __cplusplus
#include <glm/glm.hpp>
#include <cmath>
using WdFloat2 = glm::vec2;
#define WD_INLINE inline
#define WD_ABS(x) ::std::fabs(x)
#define WD_MAX(x, y) ((x) > (y) ? (x) : (y))
#define WD_MIN(x, y) ((x) < (y) ? (x) : (y))
#define WD_CLAMP(x, lo, hi) WD_MIN(WD_MAX((x), (lo)), (hi))
#define WD_RSQRT(x) (1.0f / ::std::sqrt(x))
#else
#define WdFloat2 float2
#define WD_INLINE
#define WD_ABS(x) abs(x)
#define WD_MAX(x, y) max(x, y)
#define WD_MIN(x, y) min(x, y)
#define WD_CLAMP(x, lo, hi) clamp((x), (lo), (hi))
#define WD_RSQRT(x) rsqrt(x)
#endif

/// the captured `R24` (`readable:171-174`): `x = clamp(x,0,1); return x*x*(3-2x);`
WD_INLINE float wf_depth_smoothstep(float x) {
    x = WD_CLAMP(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/// the captured depth LINEARIZATION (`readable:317`), in deren's projection terms.
///
/// @param window_depth  the depth image's stored value, in [0,1] window depth (both GL and Vulkan ZO store the
///                      same number for the same near/far - see the header's equivalence note)
/// @param proj_22       `proj[2][2]` (GLSL `projectionMatrix[2][2]`; glm's column 2 row 2)
/// @param proj_32       `proj[3][2]` (GLSL `projectionMatrix[3][2]`; glm's column 3 row 2 = the z translation)
/// @param orthographic  the captured `isOrthographic` branch. deren's main camera is `perspectiveRH_ZO`
///                      (`primitive.cpp:360-366`), so the caller passes false; the ortho arm is transcribed for
///                      completeness in `(proj_32 - d) / proj_22` form (the ZO ortho inverse, whose distance is
///                      positive; the capture's GL form returns a negative z there, which its `abs()` absorbs).
/// @return the view-axis DISTANCE (positive for perspective, positive for ZO ortho).
WD_INLINE float wf_depth_view_distance(float window_depth, float proj_22, float proj_32, bool orthographic) {
    if (orthographic) {
        return (proj_32 - window_depth) / proj_22;
    }
    return proj_32 / (window_depth + proj_22);
}

/// the captured comparison itself (`readable:318`), on two LINEAR view distances:
/// `R24((abs(scene) - own - 0.1) * 10.0)`.
/// 1 = the neighbour is farther than this fragment (rim kept); 0 = nearer/equal within the 0.1 bias (rim cut).
WD_INLINE float wf_depth_occlusion(float scene_view_distance, float own_view_distance) {
    return wf_depth_smoothstep((WD_ABS(scene_view_distance) - own_view_distance - game_toon_depth_bias) *
                               game_toon_depth_gain);
}

/// the captured screen-space normal (`readable:310-313`): view-space shading normal -> normalised screen
/// direction, x scaled by height/width so the offset is isotropic in PIXELS rather than in uv.
///
/// THE Y SIGN IS THE Deren ADAPTER: the capture's uv is bottom-up (GL: `clip.y` grows with view-space +y), while
/// deren's projection carries the y-flip that makes the framebuffer y-down, so its
/// `clip.y/clip.w*0.5+0.5` grows with view-space -y. Negating the offset's y therefore points the sample at the
/// SAME screen feature the capture samples (up the screen for a normal that points up).
WD_INLINE WdFloat2 wf_depth_offset_normal(WdFloat2 view_normal_xy, float height_over_width) {
    const float length_sq = WD_MAX(view_normal_xy.x * view_normal_xy.x + view_normal_xy.y * view_normal_xy.y,
                                   game_toon_depth_normal_epsilon);
    const float inverse_length = WD_RSQRT(length_sq);
    return WdFloat2(view_normal_xy.x * (inverse_length * height_over_width), -view_normal_xy.y * inverse_length);
}

/// the captured offset uv (`readable:314`): `screen_uv + normal * rimWidth * 0.006`.
WD_INLINE WdFloat2 wf_depth_offset_uv(WdFloat2 screen_uv, WdFloat2 offset_normal, float rim_width) {
    return screen_uv + offset_normal * (rim_width * game_toon_depth_rim_offset_scale);
}

/// the captured clamp (`readable:315`): `clamp(uv, 1/size, 1 - 1/size)`.
/// @param texel  1/size, or (0,0) when the extent is unknown (then the clamp degrades to `saturate`).
WD_INLINE WdFloat2 wf_depth_clamp_uv(WdFloat2 uv, WdFloat2 texel) {
    if (texel.x > 0.0f && texel.y > 0.0f) {
        return WdFloat2(WD_CLAMP(uv.x, texel.x, 1.0f - texel.x), WD_CLAMP(uv.y, texel.y, 1.0f - texel.y));
    }
    return WdFloat2(WD_CLAMP(uv.x, 0.0f, 1.0f), WD_CLAMP(uv.y, 0.0f, 1.0f));
}

#ifdef __cplusplus
#undef WD_INLINE
#undef WD_ABS
#undef WD_MAX
#undef WD_MIN
#undef WD_CLAMP
#undef WD_RSQRT
#undef WdFloat2
#else
// the same, for the Slang side: these private spellings must not leak into the family shaders' namespace.
#undef WD_INLINE
#undef WD_ABS
#undef WD_MAX
#undef WD_MIN
#undef WD_CLAMP
#undef WD_RSQRT
#undef WdFloat2
#endif

// ------------------------------------------------------------------------------------------------------------
// THE GPU ADAPTERS (Slang only: they read the camera UBO and call the host hook)
// ------------------------------------------------------------------------------------------------------------

#ifndef __cplusplus

/// ============================ THE ONE PLACE THE HOST WIRES THE RESOURCE ============================
///
/// THE CAPTURED SAMPLE IS `texture2D(Q285, R80).x` (`readable:316`) through a nearest-filtered depth texture,
/// returning the RAW [0,1] window depth. Replace this body with the staged resource's fetch, e.g. (with the
/// resource's heap slot and the engine's nearest/clamp G-buffer sampler, the same fetch spelling the screen rim
/// uses - `toon_screen_rim.slang:73-76`):
///
///     return heap_texel(heap_at<Texture2D<float4>>(heap_slots_<the-new-depth-copy> + heap_image_index),
///                       heap_sampler_gbuffer, uv).r;
///
/// and then set `GAME_TOON_DEPTH_RESOURCE_BOUND` to 1. DO NOT sample `heap_slots_gbuffer_depth` from
/// `character_forward`: it is the pass's own depth attachment.
///
/// Until then this returns the captured no-prepass default. The viewer's `pt.Q285` starts as a 1x1 all-white
/// texture (`const Nm=bt`, `...js:38`), i.e. window depth 1.0 = the far plane; mode OFF is what actually keeps
/// the term inert, this value only keeps any accidental mode LIVE from reading uninitialised memory.
static float game_toon_depth_scene_raw(float2 uv) {
    return 1.0f;
}

/// height/width of the render target, from the projection (`proj[0][0] = f/aspect`, `proj[1][1] = -f` after the
/// Vulkan y flip, so `proj[0][0] / |proj[1][1]|` is `1/aspect`). Used only when the caller passes no extent.
static float game_toon_depth_height_over_width() {
    const float width_term = camera_at(heap_camera_slot).proj[0][0];
    const float height_term = abs(camera_at(heap_camera_slot).proj[1][1]);
    return width_term / max(height_term, 1.0e-8f);
}

/// the captured `R68` (`readable:317`) for the current frame's projection. `proj[2][2]`/`proj[3][2]` are the
/// same elements whose host copies the engine passes to its own screen-rim stages
/// (`toon_screen_rim.cpp:175-176`, `goo_rim.cpp:162-163`); `false` because the camera is `perspectiveRH_ZO`.
static float game_toon_depth_view_distance(float window_depth) {
    const float proj_22 = camera_at(heap_camera_slot).proj[2][2];
    const float proj_32 = camera_at(heap_camera_slot).proj[3][2];
    return wf_depth_view_distance(window_depth, proj_22, proj_32, false);
}

/// the captured `R219` (`readable:307`) - this fragment's view-axis distance, `abs(view.z)`.
static float game_toon_depth_own_view_distance(float3 world_pos) {
    return abs(mul(camera_at(heap_camera_slot).view, float4(world_pos, 1.0)).z);
}

/// the captured `R247` (`readable:304-306`) - the SCREEN uv of a world position, through the frame's own
/// (TAA-jittered, y-flipped, ZO) camera. This is not the mesh uv: the capture projects the world position.
static float2 game_toon_depth_screen_uv(float3 world_pos) {
    const float4 view_pos = mul(camera_at(heap_camera_slot).view, float4(world_pos, 1.0));
    const float4 clip = mul(camera_at(heap_camera_slot).proj, view_pos);
    return clip.xy / clip.w * 0.5f + 0.5f;
}

/// the captured depth factor (`readable:310-318`) in isolation: 1 where the screen-space neighbour is farther
/// than this fragment by more than the 0.1 bias, 0 where it is nearer or equal.
///
/// @param world_pos     the shaded point (the capture's `vWorldPosition`)
/// @param world_normal  the SHADING normal passed to `R114` (the capture's `N`; in the port, hair.slang's
///                      `shading_normal`, which carries the same normal-map decode)
/// @param rim_width     the captured `R164.x` = the light's `nprData.x` / `rimWidth` (0.368 / 0.4)
/// @param depth_extent  the depth image's (width,height), or `float2(0)` when the stage does not have it
static float game_toon_depth_rim_factor(float3 world_pos, float3 world_normal, float rim_width,
                                       float2 depth_extent) {
    const float2 screen_uv = game_toon_depth_screen_uv(world_pos);
    const float2 texel = (depth_extent.x > 0.0f && depth_extent.y > 0.0f) ? (1.0f / depth_extent)
                                                                         : float2(0.0f, 0.0f);
    const float height_over_width = (depth_extent.x > 0.0f)
                                        ? (depth_extent.y / depth_extent.x)
                                        : game_toon_depth_height_over_width();
    const float3 view_normal = mul(camera_at(heap_camera_slot).view, float4(world_normal, 0.0)).xyz;
    const float2 offset_normal = wf_depth_offset_normal(view_normal.xy, height_over_width);
    const float2 sample_uv = wf_depth_clamp_uv(wf_depth_offset_uv(screen_uv, offset_normal, rim_width), texel);
    const float scene_view_distance = game_toon_depth_view_distance(game_toon_depth_scene_raw(sample_uv));
    const float own_view_distance = game_toon_depth_own_view_distance(world_pos);
    return wf_depth_occlusion(scene_view_distance, own_view_distance);
}

/// THE CAPTURED `R29` COMPOSITION, depth gate included (`readable:296-297, 318`):
///
///     if (Q272 > 0.5 && Q272 < 1.5) R29 = depth_factor * S;     // readable:318 (band is REPLACED)
///     else                          R29 = band * S;             // readable:294 (the captured disabled path)
///
/// Call it where `game_toon_lights.h` builds `const float rim = band * visibility;` and pass
/// `depth_mode = GAME_TOON_DEPTH_MODE`, `rim_width = npr.x`. With the resource unbound or the mode OFF this returns
/// `band * light_side`, i.e. today's port exactly.
///
/// HAIR ONLY: the capture's block is inside `#ifdef FAMILY_HAIR` (`readable:296`); the cloth and skin callers of
/// `game_toon_add_rim` must pass `GAME_TOON_DEPTH_MODE_OFF`.
static float game_toon_depth_rim_weight(float band, float light_side, float depth_mode, float3 world_pos,
                                       float3 world_normal, float rim_width, float2 depth_extent) {
#if GAME_TOON_DEPTH_RESOURCE_BOUND
    if (depth_mode > 0.5f && depth_mode < 1.5f) {
        return game_toon_depth_rim_factor(world_pos, world_normal, rim_width, depth_extent) * light_side;
    }
#else
    // No resource: the depth branch cannot run even if a caller passes mode 1 - this header never samples an
    // unbound texture. (The parameters are unused here on purpose; the mode-0 result is the only valid one.)
#endif
    return band * light_side;
}

#endif // __cplusplus

#endif // DEREN_GAME_TOON_GAME_TOON_DEPTH_H
