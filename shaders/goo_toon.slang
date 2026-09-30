// shaders/goo_toon.slang - THE REWRITTEN TOON CHAIN's fragment stage (`goo_toon.frag.spv`).
//
// STEP 1 OF THE REWRITE: THE IRIS' TWO-LAYER MATCAP, AND NOTHING ELSE.
//
// ================================ WHAT THE AUTHORITY IS, AND WHY IT CHANGED ================================
//
// The toon chain in this port was written against the author's article (`终末地渲染.pdf`) and the MME pack, and it
// lives in `character_forward.slang`. The rewrite takes a DIFFERENT authority, decided by the person this port is
// for: XIYAG's Goo node presets - the `Arknights: Endfield_PBRToon*` shader groups inside
// `陈_佩丽卡_莱万汀_伊冯_1.2.1_GooBlender4.2+(请阅读RM)_ By 新杨XIYAG.blend`, dumped node-for-node to
// `build-release-clang64/zmd-ab/gooblender/nodes.json`. Every constant and every connection below is read out of
// that file; the specification is `zmd-ab/goo_toon_plan.md` and the per-material values are
// `zmd-ab/goo_params_laevatain.tsv`.
//
// WHERE THE TWO SOURCES DISAGREE, THE GOO PROJECT WINS. The port also holds a second dump of the same six groups
// (`zmd-ab/endfield_addon/chen_dump/nodes.json`, the kkskaguya addon's `presets/Chen.blend`), and the two differ
// in this very group - `Chen.blend` carries three extra nodes that modulate both emission layers by the Goo
// engine's `Shader Info.Cast Shadows`, and states the albedo window's centre as `1.1` rather than `1.0`. The Goo
// project is the authority because it is the file that HAS this character's materials in it (Laevatain's eleven
// materials exist only there), and because it is the file named. Measured, not assumed: the four materials that
// the Goo project attaches `PBRToon_irisBase` to are exactly the four `*_iris_01` materials of its four
// characters, enumerated in `zmd-ab/goo_step1_result.md`.
//
// ================================ WHY A SECOND STAGE RATHER THAN AN EDIT ================================
//
// This file is compiled into its OWN module and registered under its own pipeline name
// (`runtime::goo_toon_pipeline_name`), so one run can draw the same frame, through the same pass, with the same
// descriptors and the same geometry, using either the old fragment or this one - selected by `[render] goo_toon`.
// An edit in place could not be switched back off inside a run, and a two-run comparison of two builds cannot
// separate "the new chain" from everything else that changed between them.
//
// ================================ WHAT THIS STEP DOES NOT PORT, AND WHY THAT IS THE CRITERION ================
//
// EVERY family except the iris is shaded by the OLD body below (`toon_diffuse`, `character_forward.slang`), called
// exactly as the old entry point calls it - verbatim, same arguments, same order. That is not a shortcut: this
// step's acceptance criterion is "with `goo_toon` on, the pixels that change are the iris' and no others", and a
// second implementation of the base toon in this file would make that criterion unmeasurable rather than met.
//
// WHY THE IRIS IS THE FIRST GROUP, three properties and all of them checkable:
//   * it is the SMALLEST reference group (25 nodes, 4 interface sockets in the Goo project) - the smallest state
//     surface a chain can be asked to reproduce exactly;
//   * it is exactly ONE material per character (`M_actor_*_iris_01`), i.e. one primitive, so the screen region a
//     change may land in can be pointed at by the frame itself rather than argued about;
//   * it exercises EVERY layer of the new plumbing at once - a new `.slang` module, a new pipeline name, a new
//     `[render]` switch, a new texture lane, a new colour lane and a new per-material row. Making it draw proves
//     the pipe, not one term in it.
//
// ================================ THE MECHANISM, NODE BY NODE, WITH NOTHING ADDED ==========================
//
// The reference group's whole output is `相加着色器(Emission1, Emission2)`; both halves are emission, and the
// group has no BSDF at all. Emission 1:
//
//   matcap uv   `纹理坐标.Normal -> 矢量变换(WORLD -> CAMERA) -> 矢量运算(MULTIPLY, Vector_001 = (0.5,0.5,0.0))
//               -> 矢量运算.001(ADD, Vector_001 = (0.5,0.5,0.0)) -> 图像纹理.001.Vector`
//               i.e. the SPHERE MAP of the shading normal in camera space: `n_view.xy * 0.5 + 0.5`. The
//               multiply and the add are the reference's own two nodes rather than one `*0.5+0.5`, and the
//               `.z = 0.0` of the multiply's constant is what makes the third component drop out.
//   ball        `图像纹理.001.image = T_actor_common_matcap_05_D.png` (256x256 RGBA, the group's OWN image - it is
//               not a material input, see the "per-family" note below)
//   weight      `群组.AngleThreshold -> 运算(SUBTRACT, Value = 1.0) -> 钳制(Min = 0.5, Max = 1.0)`, and
//               `组输入.D_RGB -> 混合.A_Color`, `钳制.Result -> 混合.B_Color`, `混合` = MULTIPLY at factor 1.0
//               i.e. `albedo * clamp(1.0 - AngleThreshold, 0.5, 1.0)`.
//   layer 2     `图像纹理.001.Color -> 混合.003.A_Color`, `组输入.D_RGB -> 混合.003.B_Color` (MULTIPLY, 1.0)
//               then `混合.Result -> 混合.002.A_Color`, `混合.003.Result -> 混合.002.B_Color`, and `混合.002` is an
//               ADD at factor 0.5666666626930237 - which for an ADD is `A + 0.5666666626930237 * B`.
//               So `layer2 = ball * albedo`, and emission 1's colour is `albedo * w + 0.5666667 * ball * albedo`.
//   strength    `组输入.D_Alpha -> 混合.001.Factor_Float`, `组输入.Eyes brightness -> 混合.001.A_Float`,
//               `运算.001(MULTIPLY) -> 混合.001.B_Float`, and `运算.001` takes `运算.Value` (the SAME `1.0 - angle`
//               the clamp above reads, from BEFORE the clamp - `运算`'s own `use_clamp` is FALSE) times
//               `组输入.Eyes HightLight brightness`. So
//               `strength = lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`,
//               and emission 1 is `layer2_colour * strength`.
//
// Emission 2 is a DEAD BRANCH of the reference, and it is not implemented here:
//   `图像纹理.002.image = T_actor_common_matcap_07_D.png`, its `Vector` input is UNCONNECTED (so it samples its
//   own texel (0,0) for every fragment), `图像纹理.002.Color -> 自发光.Color`, `自发光.Strength` = 0.6999999284744263
//   (an unconnected default), and `自发光.Emission -> 相加着色器.Shader_001`. Measured on the image the group
//   names: `T_actor_common_matcap_07_D.png`'s texel (0,0) is `(0, 0, 0)` - so the branch contributes exactly
//   zero to every pixel of every material, and a port that carried it would carry a texture fetch, a lane and a
//   constant in order to add black. The reference's own authors left the wire off; the evidence is recorded in
//   `zmd-ab/goo_step1_result.md` rather than reproduced here.
//
// ---- `AngleThreshold`: the reference's `calculateAngel` GROUP, WHICH THE IRIS DECLARES AS A SUB-GROUP ----
//
// `群组` in the iris group is one instance of `calculateAngel` (30 nodes), and only its `AngleThreshold` output is
// consumed (`Flip threshold` has no consumer anywhere in this group). Its whole body, from its own links:
//
//   `Vector Math.008 = DOT_PRODUCT(LightDirection, headUp)`
//   `Vector Math.009 = MULTIPLY(VM.008, headUp)`
//   `Vector Math.010 = SUBTRACT(LightDirection, VM.009)`      // the light direction, projected off the head's up
//   `Vector Math.011 = NORMALIZE(VM.010)`
//   `Vector Math.017 = DOT_PRODUCT(VM.011, headRight)`        // -> `运算.003`'s A, and `Flip threshold`
//   `Vector Math.019 = MULTIPLY(headForward, 值(明度) = -1.0)`
//   `Vector Math.020 = DOT_PRODUCT(VM.011, VM.019)`           // -> `运算.003`'s B
//   `运算.003 = ARCTAN2(VM.017, VM.020)`
//   `运算.004 = DIVIDE(运算.003, 3.141592502593994)`          // the reference's own pi, to the float
//   `运算.005 = GREATER_THAN(运算.004, 0.0)`
//   `运算.006 = ADD(1.0, 运算.004)`; `运算.007 = SUBTRACT(1.0, 运算.004)`
//   `混合.001 = MIX(factor = 运算.005, A = 运算.006, B = 运算.007)`
//
// so `AngleThreshold = 1 - |atan2(dot(proj, headRight), -dot(proj, headForward))| / pi`: the azimuth of the sun
// around the head's own up axis, as a fraction of a half turn, folded to [0,1]. THE SIGN OF THE FORWARD AXIS IS
// THE REFERENCE'S AND IT IS NOT COSMETIC: `值(明度)` is a `ShaderNodeValue` whose output default is `-1.0` in BOTH
// dumps of this group, and it is the reason a sun IN FRONT of the face gives `angle = 0` (full albedo, full
// highlight brightness) rather than `angle = 1`.
//
// ---- GOO-CONVENTION: THE MATCAP'S V AXIS ----
//
// The reference's `矢量变换` is `WORLD -> CAMERA`, and Blender's camera space has V growing UPWARD through its
// images. This renderer samples its textures with V growing DOWNWARD (the glTF/Vulkan convention), so the SAME uv
// value reads a DIFFERENT row of the same PNG; reproducing the reference's image therefore means negating V, and
// the negation below is that, not a change of formula. IT IS INDEPENDENTLY CONFIRMED BY THE OLD CHAIN: the
// `toon_matcap_uv` helper this file includes carries the same flip and its note records the measurement that
// forced it (the un-negated form killed the catchlight, 1219 lit pixels down to 6 at yaw 0). `toon_matcap_uv(n,v)`
// and the expression below are the same function - one from a view basis, this one from the camera matrix - and
// `tests/test_goo_toon_math.cpp` asserts that equivalence over a sweep of normals.
//
// ---- GOO-CONVENTION: `normalize` OF A DEGENERATE VECTOR ----
//
// Blender's `NORMALIZE` answers `(0,0,0)` for a zero-length input; Slang's answers a NaN. The input here is the
// light direction projected off the head's up axis, which is zero when the sun is exactly overhead in the head's
// frame - so the guard below reproduces Blender's answer rather than this platform's, and the case is reachable
// rather than theoretical (the sun's configured direction is a free parameter).
//
// ================================ WHERE THE PER-MATERIAL NUMBERS COME FROM ==================================
//
// `Eyes brightness` (1.5) and `Eyes HightLight brightness` (10.0) are the two MATERIAL overrides on Laevatain's
// iris in the Goo project (`goo_params_laevatain.tsv`, both `材质覆写`), and they ride
// `vulkan::toon_colour_lane::goo_eye_brightness` - the carrier this renderer already uses for a per-material pair
// of scalars (`_Specular`, `_ParallaxScale`), because the material record is inline in the per-draw push block
// and has no room for them. `< 0` in a component means "this asset states no such row FOR THAT SOCKET", tested
// one component at a time because the reference lets a material state one and not the other; the answer then is
// the REFERENCE'S OWN INTERFACE DEFAULT rather than a number chosen here: both sockets default to `0.0` in the
// group's interface, which is what a material that calls the group without stating them gets in Goo.
//
// THE MATCAP IMAGE IS A PER-FAMILY TEXTURE IN THE REFERENCE - it is inside the group, not an input of it - and it
// rides a per-material lane here anyway (`vulkan::toon_slot::goo_matcap05`). That is the asset pipeline's shape
// rather than a claim about the art: every toon texture in this port arrives as a named glTF image joined through
// the model's sidecar, and a material that states no such row simply reads no ball (see the `!= 0u` guard below,
// whose "no ball" is BLACK rather than this renderer's white fallback - white would be a much stronger statement
// about a missing texture than black is).

#define VR_GOO_TOON_CHAIN 1
#include "character_forward.slang"
// ---- AND THE ONE THING THIS INCLUSION CHANGES IN THE INCLUDED FILE: THE OLD CHAIN'S OWN RIM ----
//
// `character_forward.slang` shades a rim INSIDE `toon_diffuse` - the article's view-angle Fresnel plus its
// light-azimuth term - and this file calls that body for every non-eye family. Left alone, the character would
// wear the article's rim AND the reference's, which is the "no double rim" failure this step has to prevent.
// The define above is how the suppression is expressed and it is WORTH STATING WHY IT IS THE SHAPE AND NOT A
// SECOND PARAMETER ON THE BODY:
//
//   * `toon_diffuse` has THREE callers (`character_forward.slang`'s own entry point, `outline.slang` and this
//     file), and the truth being expressed is a property of the FILE DOING THE CALLING rather than of any one
//     call - "this module draws the reference's shading model, and that model's rim is the reference's". A
//     parameter would make every call site state the same constant and would invite a fourth caller to state it
//     wrong; a preprocessor symbol is set once, by the module that knows.
//   * the OTHER consumer, `outline.slang`, does NOT define it (it includes the same file directly), so the
//     outline keeps the old rim on every family - which is required, because the outline's hull is shaded by the
//     old body and an outline whose lighting disagreed with the surface under it would be its own bug.
//   * it leaves the unguarded arm of that file BYTE-FOR-BYTE the code it has today, including the reads of
//     `rig.rim` - which is what the "the old pipeline's output does not move by one pixel" criterion is verified
//     against, and which a runtime parameter could not promise as cheaply.
//
// WHAT THE GUARDED ARM DOES, EXACTLY: it keeps the whole old rim computation and zeroes its RESULT for the four
// families this step ports (`base`, `skin`, `cloth`, `hair` - the families the Goo project gives a rim to through
// `PBRToonBase` / `PBRToonBaseHair`), and leaves it alone for `face` and for every family this step does not
// port. The FACE is excluded on the reference's own evidence and not for convenience: `PBRToonBaseFace` has a
// `Rim_Color` socket and NO rim-width or attenuation ports, and where that colour goes is the spec's §9-U7
// (undetermined) - so the face's only rim is the old one, and it keeps it.

// ============================ THE REFERENCE'S GROUP-INTERNAL CONSTANTS ============================
//
// Every number below is a socket's value or a factor in `gooblender/nodes.json`'s
// `Arknights: Endfield_PBRToon_irisBase`, and the node's name is given so the value can be re-read rather than
// trusted. THE WRITE-UP'S ROUNDED FORMS ARE NOT USED: `MAT_OVERRIDES.md` renders the layer weight as `0.567`,
// while the .blend stores `0.5666666626930237`, and the stored value is what the reference computes with.

/// `运算.Value` - the constant the azimuth is subtracted FROM. THE GOO PROJECT'S VALUE: `Chen.blend` states `1.1`
/// here, and that difference is one of the two this group has between the two dumps.
static const float goo_iris_angle_center = 1.0;
/// `钳制.Min` / `钳制.Max` - the window the albedo's weight is clamped into. Both dumps agree on these two.
static const float goo_iris_window_min = 0.5;
static const float goo_iris_window_max = 1.0;
/// `混合.002.Factor_Float` - the second layer's weight in an ADD mix.
static const float goo_iris_ball_weight = 0.5666666626930237;
/// `运算.004.Value_001` - the divisor that turns the azimuth into a fraction of a half turn. The reference's own
/// float literal for pi (`3.141592502593994`, pi to float precision) rather than a platform constant, so the two
/// sides agree to the bit.
static const float goo_iris_half_turn = 3.141592502593994;
/// `值(明度)`'s output default inside `calculateAngel` - the sign the forward axis is multiplied by. `-1.0` in
/// both dumps of that group.
static const float goo_iris_forward_sign = -1.0;
/// The unconnected default of `自发光.Strength` on the DEAD second layer (see the header): recorded because the
/// number is part of what makes the branch dead, NOT applied, because the layer it multiplies is black.
/// (no constant: the branch is not implemented - the note above is the record.)

/// @brief the reference's `calculateAngel.AngleThreshold` - the sun's azimuth around the head's up axis
///
/// @param light_direction WORLD-space surface-to-light direction (the reference's `LightDirection` attribute,
///        which its own geometry-node tree `日光方向传递` writes; the port reads the engine's sun)
/// @param head_up / @p head_right / @p head_forward the head's own world-space basis (the reference's `headUp` /
///        `headRight` / `headForward` attributes; the port reads `heap_slots_scene_head`)
/// @return `1 - |azimuth| / pi`, in [0,1]: 0 when the sun is IN FRONT of the face, 1 when it is directly behind
/// @note THE RETURN IS NOT CLAMPED HERE and the reference does not clamp it either: the caller's `钳制` is what
///       bounds the albedo weight, and the brightness branch deliberately reads the UNCLAMPED value.
static float goo_iris_angle_threshold(const float3 light_direction, const float3 head_up, const float3 head_right, const float3 head_forward) {
    // the light direction with the head's up component removed - Blender's DOT_PRODUCT / MULTIPLY / SUBTRACT chain
    const float3 projected = light_direction - dot(light_direction, head_up) * head_up;
    // Blender's NORMALIZE answers (0,0,0) for a degenerate input and Slang's answers a NaN (see the header)
    const float projected_length = length(projected);
    const float3 axis = projected_length > 1e-6 ? projected / projected_length : float3(0.0, 0.0, 0.0);
    const float right_component = dot(axis, head_right);
    const float forward_component = dot(axis, head_forward) * goo_iris_forward_sign;
    // ---- `ATAN2` OF TWO ZEROES, SPELLED OUT RATHER THAN LEFT TO THE DEVICE ----
    //
    // The reference's `运算.003` is Blender's `ARCTAN2`, i.e. C's `atan2`, and this degenerate input is
    // REACHABLE: it is the sun lying exactly along the head's up axis, where the projection above is zero (the
    // guard) and both components are therefore zero. C answers that call DETERMINISTICALLY - `atan2(+0, -0)` is
    // `+pi`, and the negative zero comes from `值(明度)`'s `MULTIPLY(headForward, -1.0)` - so the reference's own
    // answer is a full half turn: `turns = +1`, and with the MIX's branch selection below that is
    // `AngleThreshold = 0`, a fully lit iris.
    //
    // SPIR-V'S `Atan2` DOES NOT ANSWER IT: its result is UNDEFINED when both operands are zero, so the same
    // expression on the device may produce any number - including a NaN, which the caller's `clamp` cannot clean
    // up and which would reach the framebuffer. The reference's answer is therefore written out here. This is not
    // a choice between two plausible values: it is the one the reference computes, stated where the platform
    // declines to.
    const float azimuth_magnitude = abs(right_component) + abs(forward_component);
    const float azimuth_turns = azimuth_magnitude > 1e-12 ? atan2(right_component, forward_component) / goo_iris_half_turn : 1.0;
    return 1.0 - abs(azimuth_turns);
}

/// @brief the reference group's emission 1 for the iris: the albedo layer plus the matcap layer, scaled
///
/// @param albedo the material's base colour - `混合.A_Color` / `混合.003.B_Color`, which the material tree feeds
///        from its albedo texture. The reference applies no `_BaseColor` tint in this group, and the port's glTF
///        base colour factor is what carries that tint here; on every iris in this repository the factor is
///        white, so the two are the same value rather than two readings of it.
/// @param albedo_alpha the same texture's alpha - the reference's `D_Alpha`, the MIX factor between the two
///        brightnesses. NOT the same channel as a coverage alpha: it is the material's own authored number and it
///        is read as a weight, so it is passed explicitly rather than taken from the surface's output coverage.
/// @param shading_normal the WORLD-space shading normal the sphere map is built from
/// @param matcap_index the `toon_slot::goo_matcap05` lane's texture-array index, `0` for "this material states
///        none" (see the header - the answer is black, not this renderer's white fallback)
/// @param eyes the `toon_colour_lane::goo_eye_brightness` lane: `.x` = `Eyes brightness`, `.y` = `Eyes HightLight
///        brightness`, either `< 0` for "the asset states no such row" (see the header for the fallback)
static float3 goo_iris_emission(const float3 albedo,
                                const float albedo_alpha,
                                const float3 shading_normal,
                                const uint matcap_index,
                                const float2 eyes) {
    // ---- the sphere map: `n_view.xy * 0.5 + 0.5`, with V negated for this renderer's texture convention ----
    // `mul(view, float4(n, 0.0))` IS the reference's `矢量变换(WORLD -> CAMERA)`: this renderer's camera matrix
    // has the camera's basis in its rows, which is the same space Blender's CAMERA conversion produces (see the
    // header's GOO-CONVENTION note, and `toon_matcap_uv` for the independent confirmation).
    const float3 normal_view = mul(camera_at(heap_camera_slot).view, float4(shading_normal, 0.0)).xyz;
    const float2 matcap_uv = float2(normal_view.x, -normal_view.y) * 0.5 + 0.5;
    const float3 ball = matcap_index != 0u
                            ? heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + matcap_index), heap_sampler_texture, matcap_uv).rgb
                            : float3(0.0, 0.0, 0.0);

    // ---- the two brightnesses, with the reference's own interface default behind the sentinel ----
    const float2 brightness = float2(eyes.x >= 0.0 ? eyes.x : 0.0, eyes.y >= 0.0 ? eyes.y : 0.0);

    // `混合` : `albedo * clamp(1.0 - angle, 0.5, 1.0)`
    const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 head_forward = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float3 light_direction = light_at(heap_light_slot).light_dir.xyz;
    const float angle = goo_iris_angle_threshold(light_direction, head_up, head_right, head_forward);
    const float albedo_weight = clamp(goo_iris_angle_center - angle, goo_iris_window_min, goo_iris_window_max);
    const float3 albedo_layer = albedo * albedo_weight;
    // `混合.003` then `混合.002`: `albedo_layer + 0.5666667 * (ball * albedo)`
    const float3 colour = albedo_layer + goo_iris_ball_weight * (ball * albedo);
    // `混合.001`: `lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`, on the UNCLAMPED
    // angle - `运算`'s `use_clamp` is false, so this is not the same number the albedo weight above was clamped to.
    const float strength = lerp(brightness.x, (goo_iris_angle_center - angle) * brightness.y, albedo_alpha);
    return colour * strength;
}

// ============================================================================
// STEP 2'S EDGE LIGHT HAS MOVED OUT OF THIS FILE (STEP 3)
// ============================================================================
//
// The whole object-space rim - `Rim_Color · Rim_ColorStrength`, `Directional light attenuation`,
// `Fresnel attenuation`, `Vertical attenuation`, the hair's `Use Rimlimitation?` layer AND the screen-space
// `DepthRim` the step-2 header below used to defer - now lives in `shaders/goo_rim.slang`, drawn by the
// fullscreen stage `vulkan/pass/goo_rim.cpp` with an additive blend.
//
// WHY IT COULD NOT STAY HERE, in one sentence: `character_forward_targets = {scene_color, gbuffer_depth}` makes
// the depth an ATTACHMENT of this very pass, so this fragment cannot sample it - and the reference's composition
// is a PRODUCT (`rim_term = C ⊙ (D · F · V · depth_factor)`) that cannot be split across two passes. The long
// argument, the source citations and the per-node derivation are in `shaders/goo_rim.slang`'s header and
// `zmd-ab/goo_step3_result.md`; this file keeps only the iris.
//
// WHAT THIS FILE STILL OWNS, and it is unchanged: the iris' two matcap layers, the `VR_GOO_TOON_CHAIN` guard
// that switches the OLD chain's rim off for the four families the Goo reference gives a rim to (a define that
// still has to be set here, because this is the module that knows which shading model it draws), and the
// non-eye branch's call into `toon_diffuse`. The old rim stays ON for `face` and `eye` - the face keeps it
// outright, and the eye never had one.

// ============================ THE ENTRY POINT: ONE BRANCH, AND EVERYTHING ELSE UNCHANGED ============================
//
// THE FAMILY IS THE GATE, and it is the reference's own partition rather than a heuristic: the Goo project attaches
// `Arknights: Endfield_PBRToon_irisBase` to exactly four materials - `M_actor_{chen,laevat,_pelica,yvonne}_iris_01`
// - and `gltf::toon_family_of` classifies exactly the names that contain `iris` (among its eye tokens) as
// `toon_family::eye`. Measured over every character glb in this repository: one eye-classified material per
// character (two on `tangtang`), each an `*_iris_0*`, and no other material of any of them matches. So "family ==
// eye" and "the reference uses the iris group" are the same set on every asset here, and the enumeration is in
// `zmd-ab/goo_step1_result.md`.
//
// NOTHING ELSE IN THIS ENTRY POINT DIFFERS FROM THE OLD ONE - same `gather_surface`, same family table, same
// lane reads, same emissive term, same coverage rule, and for a non-eye material the same `toon_diffuse` call with
// the same arguments. The ONE thing the eye branch skips is the old chain's own eye block inside `toon_diffuse`,
// which is the point: the two chains must not both shade the iris.
[shader("fragment")]
float4 goo_toon_frag_main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    bool v_is_front : SV_IsFrontFace) : SV_Target {
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);
    const toon_params params = toon_params_for(s.toon_family);

    // ---- THE IRIS: THE REFERENCE GROUP, WHOSE WHOLE OUTPUT IS EMISSION ----
    //
    // The two lanes it reads are the ones this step added: the Goo ball
    // (`toon_slot::goo_matcap05`, the second lane of the record's SECOND block) and the material's pair of
    // brightnesses (`toon_colour_lane::goo_eye_brightness`). The albedo and its alpha come from `gather_surface`,
    // i.e. from the same glTF base-colour texture the reference's material tree feeds the group from.
    if (s.toon_family == VR_FAMILY_EYE) {
        const uint4 lane_block_2 = toon_lanes2_at(heap_slots_toon_lanes, push.material_index);
        const float4 eyes = toon_colour_at(heap_slots_toon_colours, (push.material_index * character_toon_colour_lanes) + 6u);
        // THE ENGINE'S OWN EMISSIVE TERM IS STILL ADDED, on the same terms as the old entry point and for the same
        // reason: the G-buffer pass added it into the scene colour and this pass overwrites that pixel, so
        // dropping it would lose a term the frame already had. ON THIS MATERIAL IT IS EXACTLY ZERO - the iris'
        // glTF material states no `emissiveFactor` and no emissive map - which is why it changes nothing here; it
        // is carried because a second eye material in another asset may state one.
        const float3 lit = goo_iris_emission(s.albedo, s.alpha, normalize(s.normal), lane_block_2.y, eyes.xy) + s.emissive;
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // ---- EVERY OTHER FAMILY: THE OLD BODY, WITH THE REFERENCE'S RIM LEFT TO ITS OWN PASS ----
    //
    // The lane reads and the argument list are copied from `character_forward.slang`'s own entry point, so a
    // non-eye pixel is the same computation in both modules - WITH ONE DELIBERATE DIFFERENCE, which is the
    // rewrite's: the body's own rim term is switched off for the four families the reference gives a rim to (see
    // the `#ifndef VR_GOO_TOON_CHAIN` note near the top of this file), and the reference's rim is NOT added here.
    //
    // WHERE THE RIM IS ADDED INSTEAD, and why it is not a composition choice: `混合.019` adds it to the colour
    // this function computes, but the factor it is added WITH (`运算.028`'s `min(DepthRim, 0.5)`) needs a depth
    // sample this pass cannot take - the depth is this pass's own attachment. So the whole product is composed by
    // `shaders/goo_rim.slang`, which reads the frame this stage wrote and ADDs the term to it with a ONE/ONE
    // blend. ON THE PIXELS THIS STAGE SHADES THE TWO ARE THE SAME SUM: `混合.019`'s ADD at factor 1.0 on the same
    // linear values, one stage later. The one thing this file must therefore NOT do is compute the rim a second
    // time, which is why the step-2 call that stood here is gone rather than multiplied by zero.
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    // STEP 6's SATURATION COMES BACK OUT OF THE SHADING AND IS APPLIED TO THE PIXEL, exactly as
    // `character_forward.slang`'s `main` does it and for the same reason (`色相/饱和度/明度` is the reference's LAST
    // node, after its rim AND after its emission). The rim this pass cannot draw is added one stage later by
    // `goo_rim.slang`, which ADDs to what this writes - so this line's output is the pixel the desaturation
    // belongs on, and the rim that lands on top of it keeps its own saturation, which is the reference's own
    // arrangement. The value is the identity for every family this arm does not take.
    float desaturation = 0.0;
    // STEP 8'S DECODE stands here rather than inside `toon_diffuse`, because the shading normal is an ARGUMENT and
    // the reference decodes it where the normal map is sampled. `goo_toon_shading_normal` (defined in
    // `character_forward.slang`, which this file defines `VR_GOO_TOON_CHAIN` for before including it) answers the
    // `normalize(s.normal)` this call used to pass unless the material reads a normal map and its lane 24 row states
    // a strength, so a material with no row shades exactly as before.
    const float3 colour = toon_diffuse(s.albedo, s.alpha, s.toon_family, goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal)), v_world_pos, s.ao, params, s.toon_indices, v_uv, toon_lanes, push.material_index, desaturation);
    const float3 lit = goo_hsv_desaturate(colour + s.emissive, desaturation);
    const bool toon_transparent = (s.flags & 256u) != 0u;
    const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
    return float4(lit, out_alpha);
}
