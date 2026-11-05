// ============================================================================
// module: deren.engine.primitive:toon  (the GameToon material vocabulary of
//         deren.engine.primitive: texture slots, colour lanes and the
//         per-material toon inputs)
//
// This partition is one of the three domains of deren.engine.primitive; the
// top-level facade re-exports it, so consumers keep importing
// deren.engine.primitive.
// ============================================================================

module;

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

export module deren.engine.primitive:toon;

import deren.utility.shared;
import deren.vstd;

import :records;

namespace deren::engine {
    export enum class toon_slot : uint32_t {
        diffuse_ramp = 0,   // `_DiffRampMap`: what the lit/shadow ramp is looked up in
        shadow_lut = 1,     // `_ShadowLutTex`: the colour the shadow side is tinted toward
        specular_ramp = 2,  // `_SpecRampMap`: the highlight's shape and strength
        matcap = 3,         // `_MatcapTex`: the eye's reflection map
        sdf_lightmap = 4,   // `_SDFLightmap`: the FACE's shadow terminator, as a 2D distance field
        metallic_gloss = 5, // `_MetallicGlossMap`: metallic / reflectivity / ambient occlusion / smoothness
        sdf_mask = 6,       // `_SDFMask`: the face's own mask - SSS region, neck blend, SDF normal, rim region
        emotion = 7,        // `_EmotionMap`: the 2x2 expression atlas the face's own uv is read into
        split_normal = 8,   // `_SplitNormalMap` (`_UseSpecBumpMap`): TWO tangent-space normals packed in one map
        /**
         * `_GooMatcap05`: THE REWRITTEN TOON CHAIN's iris ball (see `source/shaders/toon/goo_toon.slang`).
         *
         * WHY IT IS NOT `matcap` ABOVE, which is the same KIND of map and the obvious lane to reuse: the two are
         * read by two different chains, and the old one GATES A WHOLE SHADING PATH on its lane -
         * `character_forward.slang` takes its eye block whenever `toon_indices.w != 0`, i.e. whenever the legacy
         * matcap lane is declared. Handing the Goo chain's ball to that lane would therefore switch the OLD eye
         * path on for a material that had none, and "with `goo_toon` off the frame is what it was" is the A/B this
         * rewrite is verified by. A lane of its own keeps the two chains' assets independent, and the cost is one
         * component of a buffer this enum's own note already says is cheap to extend.
         *
         * IT IS A PER-FAMILY TEXTURE IN THE REFERENCE AND A PER-MATERIAL LANE HERE: the Goo iris group samples
         * `T_actor_common_matcap_05_D.png` from INSIDE itself, so no material states it - but a texture reaches a
         * shader in this renderer only through a named glTF image joined by the model's sidecar, and the lane IS
         * that join. Every material that uses the Goo iris group states the row; a material that does not read no
         * ball at all (see the shader's `!= 0u` guard).
         *
         * `0` MEANS "DO NOT READ", the same contract as lanes 0..3 and the rest of this block.
         */
        goo_matcap05 = 9, // `_GooMatcap05`: the Goo iris group's internal ball (see source/shaders/toon/goo_toon.slang)
        /**
         * `_GooBaseRamp`: THE REWRITTEN TOON CHAIN's BASE / SKIN / CLOTH RAMP - the `_RD` image the Goo
         * reference's `RampSelect` resolves to for this material (spec `goo_step4_diffuse_spec.md` §2.4/§2.6).
         *
         * WHY IT IS A LANE OF ITS OWN RATHER THAN THE EXISTING `diffuse_ramp`, which names the same KIND of map:
         * the two are read by two different chains at two different coordinates. `diffuse_ramp` is the article's
         * `_DiffRampMap`, indexed by `ramp_u` (a back-light-compensated light term) and read as a TINT whose
         * ALPHA is a layer-selection gate (`character_forward.slang`'s `ramp_colour` / `ramp_weight`); this one is
         * the reference's, indexed by `min(two SigmoidSharp curves)` and read as the DIRECT DIFFUSE ITSELF, with
         * its alpha feeding only `smoothstep(GlobalShadowBrightnessAdjustment, 1, ·)`. Handing the Goo chain the
         * legacy lane would therefore make it read the article's ramp, and - worse - would make the ASSET's own
         * `_DiffRampMap` decide a term the reference computes from a different image.
         *
         * IT IS ALSO NOT A PER-FAMILY TEXTURE IN THE REFERENCE AND IS ONE HERE: the reference's `RampSelect` holds
         * four `_RD` images INSIDE itself and picks one by the material's `RampIndex`, and its own §2.6/§A8 show
         * that the four slots hold only TWO distinct images and that every laevatain material states
         * `RampIndex ∈ {0.0, 1.0}`. The selection is therefore resolved ON THE HOST, in the sidecar read, which is
         * where `RampIndex` is known and where the `_RD` images arrive by name - so the shader sees one lane and
         * one fetch, exactly as §2.6 argues it may.
         *
         * `0` MEANS "DO NOT READ", the same contract as every lane above; the shader's answer for it is the OLD
         * chain's diffuse (see `toon_diffuse`), which is what keeps a material that states no such row - and the
         * whole asset under `[render] goo_toon = false` - exactly where it was.
         *
         * LANE 10 IS INSIDE THE RECORD'S SECOND BLOCK (8..11), so it costs no third block and no third accessor -
         * which is why it is here and not at 12. It filled that block; THE STEP-7 LANES BELOW ARE THE ONES THAT
         * NEEDED THE THIRD BLOCK, and they are the reason `toon_lane_blocks` is now 3 (see the assertion below).
         */
        goo_base_ramp = 10, // `_GooBaseRamp`: the `_RD` image `RampSelect` resolves to (see source/shaders/toon/goo_toon.slang)
        /**
         * `_GooFaceSDF`: THE FACE CONTAINER's OWN SHADOW TERMINATOR - `T_actor_common_female_face_01_SDF.png`.
         *
         * WHY THIS IS NOT `sdf_lightmap` ABOVE, which is the same KIND of map and the obvious lane to reuse: THE
         * TWO ARE DIFFERENT IMAGES WITH DIFFERENT CHANNEL ROLES, measured rather than argued. Lane 4 names what
         * the SIDECAR's `_SDFLightmap` row resolves to and on this repository's characters that is the `_03` family
         * (`face_shader_compare.md` §5-1), while the Goo reference's Face container samples
         * `T_actor_common_female_face_01_SDF.png` from INSIDE itself - and it reads that image as `(R + G) / 2`
         * (spec `goo_step7_face_spec.md` §4.1), where the article's chain reads one channel of its own atlas and
         * treats the third as a normal. Handing the Goo chain lane 4 would therefore make it read the article's
         * face, and handing the article's chain this lane would make it read the reference's - two mistakes that
         * cancel into a plausible face rather than into an error, which is this file's whole subject.
         *
         * `0` MEANS "DO NOT READ", the same contract as every lane above, and the shader's answer for it is the
         * old chain's face shading (see `toon_diffuse`).
         */
        goo_face_sdf = 11, // `_GooFaceSDF`: the Face container's distance field, read as `(R + G) / 2`
        /**
         * `_GooFaceCmM`: the Face container's MASK - `T_actor_common_female_face_01_cm_M.png.001`, whose three
         * channels are three different things (spec §4.2, read from `gooblender/nodes.json`):
         *
         *   * `G` - THE LAYER SELECTOR, and it is the busiest channel in the container: it is the factor of
         *     `混合.005` (is the cast shadow itself used, or the constant 1), of `混合.002` (the SDF branch or the
         *     chin branch of the ramp coordinate) and of `混合.019` (the flattened or the raw probe irradiance),
         *     and it is `Recalculate normal`'s `ChinMask`;
         *   * `R` - `Front transparent red`'s `D_R`, the cheek's own forward-scatter weight;
         *   * `A` - the gate on `运算.010`, i.e. on the whole "front transparent red" term that multiplies the
         *     albedo.
         *
         * IT IS NOT `sdf_mask` ABOVE, for lane 11's reason in the other direction: lane 6's `_SDFMask` is the
         * ARTICLE's mask (`refine.y` the neck seam, `.z` the shadow ring, `.w` the rim region) and this is the
         * REFERENCE's, with `.y` read as a unit-less selector. A shared lane would give one image two channel
         * dictionaries.
         */
        goo_face_cm = 12, // `_GooFaceCmM`: `G` = the layer selector, `R` = `Front R`'s D_R, `A` = its gate
        /**
         * `_GooFaceCsutm`: `CsutmMask`, whose `G` is the Face container's EMISSION-BRIGHTNESS SWITCH.
         *
         * `混合.020 = MIX(Face Final brightness, Eyes white Final brightness, f = 图像纹理.004.Color.G > 0.5)`, and
         * `图像纹理.004` is this image (`CsutmMask`, 512x512) with an UNCONNECTED `Vector`. The spec's §11-U8 read
         * that as "texel (0,0) for every fragment"; the parent's ruling is Blender's actual semantics - an
         * unconnected `Vector` on a `ShaderNodeTexImage` samples the DEFAULT UV MAP - so the factor is a per-pixel
         * `step(0.5, G(uv))`. Measured on this asset: 1.809% of the IMAGE has `G > 0.5` (mean 4.6/255), so the
         * 1.5 branch is a small minority of the atlas; the rendered consequence is in `goo_step7_result.md`.
         *
         * THE OTHER TWO IMAGES THE CONTAINER NAMES ARE NOT LANES HERE, and each for its own measured reason:
         * `T_actor_common_face_01_hl_M.png` (the lips mask, read as `.R`) multiplies `Lips highlight color`, which
         * `M_actor_laevat_face_01` leaves at the group's interface default - BLACK - so the term is zero whatever
         * the mask is; and `图像纹理.003` (`CsutmMask` again) feeds a screen-space "shadow proxy" whose other
         * input is a depth sample this stage cannot take (the depth is its own attachment), so its chain is not
         * evaluated at all (see `toon_diffuse`'s face arm).
         */
        goo_face_csumt = 13, // `_GooFaceCsutm`: `CsutmMask`, whose `G` selects the Face container's brightness
        /**
         * `_GooRSMask`: the `_M（非色彩）` mask of the reference's mechanism table #14 (`RS EFF`), i.e. the LEFT
         * input of `混合.038 = _M ⊙ RS ColorTint` (`deren-ab/goo_step13_rs_eff_spec_s.md` §2.1/§3.2, F4).
         *
         * IT IS THE RAW IMAGE AND THE TRANSFER FUNCTION IS THE SHADER'S, which is the one thing about this lane
         * that cannot be read off its name: `_M` is NOT the texel on any material this port has measured. The two
         * materials that switch the branch on (`M_actor_laevat_cloth_02`, `M_actor_laevat_cloth_05`) both feed
         * their `_M` from an `Arknights: Endfield_SmoothStep` subgroup instantiated with `min = 0` and a
         * PER-MATERIAL `max` (`0.9900000095367432` and `1.0` respectively), and its `x` is
         * `float_from_vec4(图像纹理.Color)` = `dot(rgb, (0.2126, 0.7152, 0.0722))` - Rec.709 luminance, not the
         * `(r+g+b)/3` average (`float_from_vec3` is that one, and it is not the node here). The `max` rides
         * `toon_colour_lane::goo_rs_tint`'s `.w`, so no part of the mapping is baked into the upload either.
         *
         * UNORM, AND THAT IS A COLOUR-SPACE DECISION RATHER THAN A FORMAT DETAIL: the image's own colorspace is
         * Non-Color, its channels are a MASK, and the reference reads them through no sRGB decode - a `_SRGB`
         * upload would bend every texel by 2.2 gamma before the luminance dot, which is the quantity the
         * smoothstep thresholds. See `runtime.constructor.cppm`'s format table, where this lane sits beside
         * `goo_face_sdf` / `goo_face_cm` / `goo_face_csumt` for exactly that reason.
         *
         * A LANE OF 0 MEANS "DO NOT READ", the same contract every other lane uses, and here it is load-bearing
         * for a second reason: index 0 is the WHITE fallback texture, so a material with no `_M` would otherwise
         * read a pure-white mask, i.e. `_M = 1` everywhere, i.e. the strongest possible statement about a branch
         * the material never switched on (spec F5).
         *
         * SLOT 15 IS `armA`'s SHEET, AND IT CLOSED THE THIRD LANE BLOCK: the reference's `RS EFF` selects between
         * two arms (`RS Model`: 0 = `armA`, which samples the two 256x1 `_RS` character sheets; 1 = `armB`,
         * `_M ⊙ RS ColorTint`), and step 15 ports `armA`. `toon_colour_lane`'s sibling note recorded the lane
         * ceiling as `toon_record_lanes + toon_lane_blocks * 4` = 16 at the time, so it was the last lane that fit.
         * THE SECOND SHEET IS STILL NOT A LANE, and its reason is now its OWN rather than the ceiling: this arm
         * still carries ONE sheet slot, and the reference's other `_RS` sheet rides the material record's NAME plus
         * that sheet's OWN switch (`_GooRSSheet1` + `_UseGooRSSheet1`), which the HOST resolves from `RS_Index`
         * when the texture is registered - `main.cpp`'s `toon_texture` picks one of the two names and the stage
         * keeps sampling one slot, the same split `toon_colour_lane::goo_rs_arm0` describes for `.x`. (The ceiling
         * itself moved after this lane: `_OutlineMask` below is lane 16 and raised `toon_lane_blocks` to 4 - see
         * that lane's note - so "no free lane" is no longer part of this argument.)
         *
         * SRGB RATHER THAN UNORM, AND THE DIFFERENCE IS THE SAMPLER AND NOT THE SHADER: an `_RS` sheet is a
         * COLOUR image (the reference multiplies it into a tint), so `::deren::promise::rhi::image_format::rgba8_srgb` hands the stage
         * texels the sampler has already decoded - a second decode in the shader would double-apply the curve.
         * `_GooRSMask` above is UNORM for the opposite reason: its channels are a mask and are read through no
         * decode at all.
         */
        goo_rs_mask = 14,  // `_GooRSMask`: the Non-Color `_M` mask of mechanism table #14 (`RS EFF`), UNORM
        goo_rs_sheet = 15, // `_GooRSSheet`: `armA`'s 256x1 `_RS` colour sheet (mechanism table #14), SRGB
        /**
         * `_OutlineMask`: THE INVERTED-HULL OUTLINE's WIDTH / DEPTH-OFFSET MASK - the captured viewer's `Q121`
         * texture, sampled only when its own `#ifdef Q326` is defined, i.e. when `_EnableOutlineMask === 1` AND
         * the material names an `_OutlineMask` image (`source/shaders/toon/game_toon/captured/outline.slang`'s
         * header transcribes `vert:56-62`).
         *
         * WHAT THE SHADER DOES WITH IT, and it is why the image must be staged UNORM: the vertex stage samples it at
         * the hull's own uv and uses TWO CHANNELS as MULTIPLIERS - `R18 *= mask.r` scales the hull's screen-space
         * WIDTH, and `R136 *= mask.g` scales its VIEW-DEPTH offset (`vert:60-61`). Both channels are NUMBERS; an
         * sRGB decode would bend every one of them, exactly as it would bend `sdf_mask`'s or `goo_rs_mask`'s.
         *
         * WHY IT IS A LANE RATHER THAN A CONSTANT IN THE SHADER: this was the LAST unbridged input of the outline
         * port. The sidecar has staged the rows since the reference asset was attached (`slot _OutlineMask` ->
         * `WF_T_actor_common_female_face_01_ST` for the face, `WF_T_actor_laevat_hair_01_ST` for the hair, both with
         * `_EnableOutlineMask = 1.0`; body/cloth state the flag 0.0 and name no image), and `main.cpp` resolves a
         * texture `slot` row only through this enum's vocabulary - so without a lane the Q326 arm could only ever be
         * the "absent" branch: a uniform-width, uniform-offset hull on exactly the two families that asked for the
         * modulation.
         *
         * IT IS THE FIRST LANE OF THE FOURTH BLOCK, and taking it is a deliberate reversal of the "raising
         * `toon_lane_blocks` to 4 is the rejected `Rb`" ruling that the Goo spec made for a DIFFERENT question (a
         * second `_RS` sheet, which is a host-side name choice and needs no lane - see `goo_rs_sheet`'s note). This
         * lane cannot be answered on the host: the mask modulates a PER-VERTEX quantity inside the hull's geometry
         * stage, so the shader must sample it. The cost is exactly the one the block contract spells out - a fourth
         * `uvec4` per material in the same heap slot, `toon_lane_blocks` 3 -> 4, and a fourth accessor
         * (`toon_lanes4_at`) beside the other three - and it is paid here rather than left as a recorded deviation.
         *
         * `0` MEANS "DO NOT READ", the same contract every lane above uses, and it is the correct answer for
         * body/cloth (whose `_EnableOutlineMask` is 0) and for every material that states no image: the outline
         * stage then keeps the captured Q326-ABSENT arm, i.e. the uniform hull it drew before this lane existed.
         */
        outline_mask = 16, // `_OutlineMask`: r = hull-width multiplier, g = hull-depth-offset multiplier (UNORM)
        count = 17,
    };
    // THE LANES SPLIT INTO TWO GROUPS, and the split is a fact about the material record rather than a
    // convenience: lanes 0..3 ride `material_record::toon_indices`, and every lane from 4 on is carried BESIDE
    // the record in `core::heap_slots::toon_lanes` - because the record is INLINE in the per-draw push block
    // and a word added to it moves every offset in `surface.glsl` and `shadow.slang` (see that slot's note).
    // A lane added at or after `sdf_lightmap` therefore costs a component of that buffer and NOTHING here but
    // an entry in the enum, the format table in `register_material`, and the application's vocabulary table.
    inline constexpr uint32_t toon_record_lanes = 4;
    static_assert(static_cast<uint32_t>(toon_slot::sdf_lightmap) == toon_record_lanes,
                  "the first lane beside the record is the one the split is named for");
    // AND THE LANES BESIDE THE RECORD COME IN BLOCKS OF FOUR, because they are carried as `uvec4`s: lanes 4..7 in
    // the first block, 8..11 in the second and 12..15 in the third. The shader addresses the buffer by block, so
    // this number is half of a contract (see the stage's `character_toon_lane_blocks`), and a lane added past a
    // block boundary needs the block count raised rather than the lane appended.
    // `export` BECAUSE THE RUNTIME NEEDS IT FOR A TYPE: the material dedup key is a `data_block` whose size is
    // `sizeof(record) + toon_lane_blocks * sizeof(uvec4) + toon_colour_lane::count * sizeof(vec4)`, and that key is
    // declared in `runtime.declarations.cppm` - a different module. A non-exported constant is not visible there
    // (measured: "declaration of 'toon_lane_blocks' must be imported from module 'deren.engine.primitive' before it is
    // required"), which is why the sibling above it is module-private and this one is not.
    //
    // THE KEY HAS A THIRD TERM AND IT IS THE COLOUR LANES - the `toon_colour_lane::count` `vec4`s below, which
    // live outside the record for the same reason these blocks do and are written after the dedup's early return.
    // They are spelled through that enum rather than as a number, so adding a lane widens the key by itself; see
    // `toon_lane_blocks`'s consumers in `runtime.declarations.cppm` and the "材质去重键补上 colour lanes" section of `remaining_port_spec.md`.
    //
    // ---- 3, AND THAT IS THE DECISION THIS NUMBER IS: STEP 7's THIRD LANE DID NOT FIT THE SECOND BLOCK ----
    //
    // THE SAME FILE HAD WRITTEN "the next lane to be added will need `toon_lane_blocks` raised and both accessors
    // widened", and step 7 is that lane: the second block (8..11) was full (`split_normal`, `goo_matcap05`,
    // `goo_base_ramp` and no spare), and the FACE needs THREE beside it - its SDF, its `cm_M` and `CsutmMask`. The
    // arithmetic that made this the cheap answer rather than a redesign: everything downstream of this constant is
    // spelled THROUGH it (`material_capacity * toon_lane_blocks * sizeof(glm::uvec4)`, the dedup key's second term,
    // `register_material`'s block writes), so raising it widens the buffer, the key and the write loop together and
    // the only hand-edited places are the allocation and the two accessors' strides. A lane that reads another
    // material's block is the failure this whole arrangement exists to prevent, and it is silent - see the
    // three-round bug in `heap_access.slang`'s note on `toon_lanes_at`.
    //
    // ---- 4: `_OutlineMask` (lane 16) DID NOT FIT THE THIRD BLOCK, AND IT CANNOT BE ANSWERED ON THE HOST ----
    //
    // The third block (12..15) was full (`goo_face_cm`, `goo_face_csumt`, `goo_rs_mask`, `goo_rs_sheet`), and the
    // outline mesh stage needs a PER-VERTEX texture read - the captured `#ifdef Q326` multiplies the hull's width
    // by the mask's `.r` and its depth offset by `.g` INSIDE the geometry stage, so the answer has to reach a
    // shader and cannot be baked into the material's record. That made lane 16 the first lane of a FOURTH block
    // and this constant 4. The cost is the one the paragraph above already prices: the same heap slot grows to four
    // `uvec4`s per material, the dedup key follows the constant, and `heap_access.slang` gains the block-3 accessor
    // (`toon_lanes4_at`) beside `toon_lanes_at` / `toon_lanes2_at` / `toon_lanes3_at`. The Goo spec's rejected `Rb`
    // was a lane for a SECOND `_RS` SHEET, which is a host-side name choice and still needs no lane; it is not this.
    export inline constexpr uint32_t toon_lane_blocks = 4;
    static_assert(static_cast<uint32_t>(toon_slot::split_normal) == toon_record_lanes + 4u,
                  "split_normal is the first lane of the SECOND block, which is the one after the record's");
    // AND A LANE PAST THE LAST BLOCK IS THE FAILURE THIS CATCHES, which is a real one rather than a formality: the
    // shader addresses the table by BLOCK (`toon_lanes_at` block 0, `toon_lanes2_at` block 1, `toon_lanes3_at`
    // block 2 and `toon_lanes4_at` block 3), so a lane added past `toon_lane_blocks * 4` would be written by the
    // host into a block no accessor names - a lane that silently reads nothing rather than a compile error. Raising
    // `toon_lane_blocks` needs its accessor widened in the same move (see the note above), so the assertion is
    // where that decision is forced; the ceiling is now 4 + 4*4 = 20.
    static_assert(static_cast<uint32_t>(toon_slot::count) <= toon_record_lanes + toon_lane_blocks * 4u,
                  "a toon lane past the last block needs toon_lane_blocks raised AND its accessors widened");

    /**
     * @brief the MATERIAL COLOURS the game's own parameter table carries, in `toon_inputs::colours` order
     *
     * A SECOND KIND OF VALUE BESIDE THE TEXTURES, and it needs its own lanes for a reason the texture lanes do not
     * have: a `color` row is four floats - `_EyeHighLightColor` is (2.399, 1.885, 2.038) on chen's iris - so it
     * cannot ride the `uvec4` of texture indices, and the material record has no room for it (see
     * `core::heap_slots::toon_colours` for why growing the record is not an option here).
     *
     * ONLY THE LANES SOMETHING READS ARE HERE. The parameter table carries more colours than this (an eye tint, an
     * outline tint, a matcap tint on chen alone); a lane nobody consumes would be a second source of truth for a
     * value that has none, which is the arrangement this repository keeps refusing.
     *
     * THE OUTLINE LANE IS THE ONE THE LIST USED TO NAME AS UNCLAIMED, and it carries TWO numbers in its four
     * floats because the article's outline needs two and they are stated by the SAME material rows: `.rgb` is the
     * game's `_OutlineTintColor` - the author's `_OutlineColor` (`MyZmdOutlineShader.shader:15`) - and `.w` is its
     * `_OutlineWidth` (`:7`), which the outline hull's own geometry stage reads as its `> 0` gate. A separate
     * lane for the second number would be a second carrier for one material's one statement, and the material
     * record cannot take either (see the note above).
     *
     * EVERY LANE HERE IS ALSO A TERM OF THE MATERIAL DEDUP KEY, and that is a property of the ENUM rather than
     * of the caller: the key's third component is `toon_colour_lane::count` `vec4`s (see `toon_lane_blocks`), so
     * a lane added below widens the key by itself and nothing has to be remembered at the key. The failure that
     * made this a term is worth knowing before adding the next one: the lanes are written AFTER the dedup's early
     * return, so a lane OUTSIDE the key is a lane the second of two record-identical materials reads from the
     * first - a wrong value rather than a missing one, and invisible in the frame unless some other scene happens
     * to contain such a pair (see the "材质去重键补上 colour lanes" section of `remaining_port_spec.md`).
     */
    export enum class toon_colour_lane : uint32_t {
        sdf_rim = 0,        // `_SDFRimColor`: the FACE's own rim colour, read where the SDF drives the rim
        eye_highlight = 1,  // `_EyeHighLightColor`: the iris' highlight tint
        eye_scattering = 2, // `_EyeScatteringColor`: the iris' scattering tint
        outline_edge = 3,   // `_OutlineTintColor` (rgb) and `_OutlineWidth` (w): the toon outline's tint and width
        /**
         * `_Specular` in `.x`: the material's own SPECULAR STRENGTH, which the family table could only
         * approximate - see below. `.y` / `.z` / `.w` are unused and reserved.
         *
         * THE ONE LANE THAT IS NOT A COLOUR, and it is here for the reason the lane above carries a width: the
         * value is PER MATERIAL, it is a scalar, and the material record cannot take it (the note at the top of
         * this enum says why - 80 bytes, inline in the per-draw push block, and it is already full). The four
         * floats of a lane are the only per-material storage this renderer has, so a scalar that must be
         * per-material rides one, exactly as `_OutlineWidth` does.
         *
         * WHY IT EXISTS AT ALL: `source/shaders/toon/toon_params.slang` holds the toon parameters PER FAMILY, and
         * `spec_strength` was that table's reading of the game's `_Specular` (skin 0.454, cloth/face/hair 1.0,
         * eye 0.0). The game states the value PER MATERIAL, and on chen one material disagrees with its family:
         * `M_actor_chen_brow_01` is classified `face` and states `_Specular = 0.0` against the face family's
         * 1.0, so the brow wore a highlight no one authored. The value arrives from the asset's own `extras`
         * block (`deren::gltf::claimed_extras_floats`) rather than from the sidecar, which carries no `_Specular` row
         * at all - it is the first property of a third source, and it is connected one property at a time.
         *
         * `.x < 0` MEANS "THE ASSET STATES NOTHING", and that sentinel is the honest form rather than a
         * default: `0.0` is a VALUE the game states (the iris, the brow), so it cannot double as "absent", and
         * filling an absent lane with a number here would mean this side had resolved a fallback that belongs
         * to the family table in the shader - a second source of truth for exactly the constants this lane is
         * meant to let the asset override. `toon_inputs::colours` therefore starts this lane at `-1`, and the
         * stage reads `lane.x >= 0 ? lane.x : params.spec_strength`.
         *
         * IT IS NO LONGER THE ONLY LANE OF THIS KIND: `parallax_scale` below is the second, from the same source
         * and with the same sentinel contract, and the two are the pair this table's per-material scalars ride.
         */
        specular_strength = 4, // `_Specular`: the material's own specular strength (`.x`; <0 = the family's)
        /**
         * `_ParallaxScale` in `.x`: the material's own PARALLAX DEPTH - the distance the iris' albedo lookup
         * slides behind the cornea (`.y` / `.z` / `.w` are unused and reserved).
         *
         * THE SECOND SCALAR TO RIDE A LANE FOR THE SAME REASON AS THE FIRST: it is PER MATERIAL, the material
         * record cannot take it, and the asset's `extras` block is the only source that states it per material
         * (`deren::gltf::claimed_extras_floats`). What it replaces is `source/shaders/toon/character_forward.slang`'s own
         * `character_eye_parallax_depth` constant - the value of ONE material (`M_actor_chen_iris_01`, 0.03)
         * that the stage used to apply to every material the eye path runs on, with a note saying the per-material
         * value could not be carried. The asset states 0.03 on the iris and 0.5 on the brow, so the constant was
         * right for the iris and wrong for the brow - and the two are the whole visible difference of this lane.
         *
         * `.x < 0` MEANS "THE ASSET STATES NOTHING", the same sentinel contract as the lane above and for the
         * same reason: `0.0` is a legitimate authored value here (a material that wants no parallax at all would
         * state it), so it cannot double as "absent". No material in `deren-ab/extras_dump.md` states 0 (the domain
         * there is 0.03 / 0.5), but that is not what makes the sentinel right - the value's own domain does. The
         * stage reads `lane.x >= 0 ? lane.x : character_eye_parallax_depth`, i.e. a material that states no such
         * row keeps the frame it had before this lane existed.
         */
        parallax_scale = 5, // `_ParallaxScale`: the material's own parallax depth (`.x`; <0 = the stage's constant)
        /**
         * `_GooEyeBrightness` in `.x` / `.y`: THE REWRITTEN TOON CHAIN's two iris BRIGHTNESSES - the reference
         * group `Arknights: Endfield_PBRToon_irisBase`'s `Eyes brightness` and `Eyes HightLight brightness`
         * sockets (`source/shaders/toon/goo_toon.slang`), which are per MATERIAL in the Goo project (`1.5` and `10.0` on
         * Laevatain's iris) and are the factor its whole emission is scaled by. `.z` / `.w` are unused and
         * reserved.
         *
         * WHY A LANE RATHER THAN TWO FIELDS SOMEWHERE: the same reason as the two scalars above - they are
         * per-material numbers and the material record is inline in the per-draw push block, so a lane is the only
         * per-material storage this renderer has. WHY BOTH IN ONE LANE: they are ONE mechanism's two numbers,
         * stated by the SAME material and consumed at the SAME two lines of the same group; two lanes would be
         * two carriers for one material's one statement, which is the arrangement this enum's notes refuse.
         *
         * `.x < 0` MEANS "THE ASSET STATES NOTHING", the same sentinel contract as the two lanes above and for the
         * same reason: `0.0` is a legitimate authored brightness (it is in fact the value the group's own
         * interface defaults BOTH sockets to), so it cannot double as "absent". The stage then answers with that
         * interface default rather than with a number invented here, and the two components are tested
         * independently (`>= 0.0` each) because a material may state one and not the other.
         */
        goo_eye_brightness = 6, // `_GooEyeBrightness`: the Goo iris' `Eyes`[x] / `Eyes HightLight`[y] brightness
        /**
         * `_GooRimColour` in `.rgb`: THE REWRITTEN TOON CHAIN's OBJECT-SPACE RIM COLOUR - the reference group
         * `Arknights: Endfield_PBRToonBase`'s `Rim_Color` socket, which reaches the rim through the
         * `Rim_Color` sub-group's `混合.017 = Rim_Color · Rim_ColorStrength` (see `source/shaders/toon/goo_toon.slang`).
         * `.a` is unused and reserved.
         *
         * IT IS PER MATERIAL IN THE REFERENCE (`materials[...] :: 群组.00N.inputs[Rim_Color]`), it is a `color`
         * row, and the material record cannot take it - the same three facts that put every lane above here.
         * `.rgb` ALONE IS THE TINT AND `.a` IS NOT READ, and that is a fact about the reference rather than a
         * saving: `混合.017` broadcasts `Rim_ColorStrength` (a VALUE socket) into a RGBA as `(s,s,s,1.0)`, so the
         * product's alpha is 1.0 for every material in both dumps and cannot carry a statement.
         *
         * THE NEUTRAL IS WHITE, and it is the REFERENCE'S OWN INTERFACE DEFAULT rather than a number chosen
         * here: `::- Rim_Color :: 组输入.Rim_Color = [1.0, 1.0, 1.0, 1.0]`, so a material whose sources state
         * no row gets exactly what a material that calls the group without stating it gets in Goo. TEN of the
         * eleven `M_actor_laevat_*` materials are at that default; `M_actor_laevat_face_01` is the only
         * override, and it belongs to the FACE family, which this step does not port (see
         * `source/shaders/toon/goo_toon.slang`'s scope note and the spec's §5.10/§9-U7).
         */
        goo_rim_colour = 7, // `_GooRimColour`: the Goo rim group's `Rim_Color` (`.rgb`; white = the group's default)
        /**
         * `_GooRimScalars`: THE REWRITTEN TOON CHAIN's three rim SCALARS plus the hair's rim-limitation gate, in
         * the order the reference states them - `.x` = `Rim_ColorStrength`, `.y` = `Rim_DirLightAtten`,
         * `.z` = `ToonfresnelPow`, `.w` = `Use Rimlimitation?`.
         *
         * WHY FOUR IN ONE LANE, and the reason is the enum's own rule rather than this step's convenience: they
         * are ONE mechanism's numbers, stated by the SAME material row (`群组.00N.inputs[...]` of one group
         * instance) and consumed within one composition (`source/shaders/toon/goo_toon.slang`'s rim term). A lane each would
         * be four carriers for one material's one statement, which is the arrangement this enum keeps refusing.
         *
         * `.x` / `.y` / `.z` ARE SCALARS WHOSE ZERO IS A REAL STATEMENT - `Rim_ColorStrength = 0.0` is how the
         * reference's own author SWITCHES A MATERIAL'S RIM OFF (`M_actor_laevat_cloth_03` states exactly that),
         * and `Rim_DirLightAtten` / `ToonfresnelPow` are exponents and attenuations that are meaningful at any
         * sign-free value - so each carries the SAME `< 0` sentinel as the lanes above and falls back to the
         * reference group's OWN interface default (`1.0` / `0.8999999761581421` / `2.0`). `.w` IS A 0/1 GATE
         * (`Use Rimlimitation?`, BOOLEAN in Blender), so its sentinel is the same `< 0` and its fallback is the
         * group's `0.0` - i.e. 0.0 is a statement (limitation off) and -1 is "not stated".
         *
         * `.w` EXISTS FOR ONE MATERIAL IN THIS REPOSITORY (`M_actor_laevat_hair_01`, 1.0, the only material that
         * overrides it in either dump) and is READ BY ONE BRANCH (the `PBRToonBaseHair` composition). It is a
         * component rather than a lane for the reason above; it is not a Base-family switch, because the Goo
         * `PBRToonBase` has no such socket at all (spec §5.12).
         */
        goo_rim_scalars = 8, // `_GooRimScalars`: `Rim_ColorStrength`[x] / `Rim_DirLightAtten`[y] / `ToonfresnelPow`[z] / `Use Rimlimitation?`[w]
        /**
         * `_GooRimWidths` in `.x` / `.y`: THE REWRITTEN CHAIN's SCREEN-SPACE RIM WIDTHS - the reference group
         * `DepthRim`'s `Rim_width_X` / `Rim_width_Y` sockets, which is the pair the offset sample's camera-space
         * displacement is built from (`source/shaders/toon/goo_rim.slang`). `.z` / `.w` are unused and reserved.
         *
         * IT IS A LANE OF ITS OWN RATHER THAN TWO MORE COMPONENTS ON `goo_rim_scalars`, and the reason is the
         * enum's own rule read the other way round: that lane's four components are the four scalars ONE
         * COMPOSITION reads (the `PBRToonBase` container's rim term), while these two belong to the OTHER group
         * the same material instantiates - `DepthRim`, whose two `组输入` sockets are the only thing it takes and
         * which the container reaches through `群组.016`. A material states them in its own row because they are
         * its own numbers, and the two groups are separate statements in the reference's graph.
         *
         * THE `DepthRim` GROUP HAS NO OTHER SOURCE AND NO FALLBACK INSIDE THE MATERIAL, which is why this lane is
         * the difference between the reference's rim and a rim of some other width: `Rim_width_X` = `0.5` (the
         * group's own interface default) puts the offset sample 10x further across the screen than the `0.041847`
         * every laevatain material states. THE SENTINEL IS THEREFORE PER COMPONENT AND THE FALLBACK IS `0.5` -
         * the group's `interface[]` value, read out of `gooblender/nodes.json`, not a number chosen here.
         *
         * `.x < 0` MEANS "THE ASSET STATES NOTHING", the same `< 0` contract as every lane above: `0.0` is a
         * legitimate authored width (it makes the offset sample the pixel's own depth, i.e. `dz = 0`, i.e. no
         * rim - which is a statement), so it cannot double as "absent".
         */
        goo_rim_widths = 9, // `_GooRimWidths`: `Rim_width_X`[x] / `Rim_width_Y`[y] (`.z`/`.w` reserved)
        /**
         * `_GooBaseColour` in `.rgb`: THE REWRITTEN CHAIN's `PBRToonBase`'s `BaseColor` - the reference's albedo
         * TINT, which its own graph multiplies into the albedo before every term that reads it.
         *
         * WHY THE PORT NEEDS A LANE FOR IT AT ALL, and this is the uncomfortable half: `BaseColor` is non-white on
         * every material this step covers (`body_01` `[1.1628, 0.9888, 1.0280]`, every cloth
         * `[1.2425] x3` - see the spec's §4.2 table), and THE PORT HAS NO ROW FOR IT ANYWHERE: its albedo is
         * `baseColorFactor x base-colour map`, and `laevatain_goo.glb`'s eleven materials state NO
         * `baseColorFactor` at all (the spec's §9-U9). Without this lane the cloth's direct diffuse would be
         * `1/1.2425 = 0.805` of the reference's - a 24% error on five of the seven materials, and one that no
         * family table could express because the value is per material.
         *
         * THE NEUTRAL IS WHITE, and it is the reference's OWN interface default (`::- Arknights:
         * Endfield_PBRToonBase :: 组输入.BaseColor = [1.0, 1.0, 1.0, 1.0]`), so a material that states no such row
         * gets exactly what a material calling the group without stating it gets in Goo. `.a` IS NOT READ: the
         * reference's tint is `混合 = MULTIPLY(A = 混合.035.Result, B = 组输入.BaseColor)`, i.e. the whole RGBA, but
         * the albedo the port tints is a `float3` and its alpha is the coverage the pass blends with - a second
         * owner of that channel would be a second statement about coverage.
         */
        goo_base_colour = 10, // `_GooBaseColour`: `PBRToonBase`'s `BaseColor` (`.rgb`; white = the group's default)
        /**
         * `_GooDiffuseA` - the reference's FOUR `SigmoidSharp` arguments for the BASE / SKIN / CLOTH families, in
         * the order its two call sites state them: `.x` = `RemaphalfLambert_center`, `.y` =
         * `RemaphalfLambert_sharp`, `.z` = `CastShadow_center`, `.w` = `CastShadow_sharp`.
         *
         * ONE LANE FOR FOUR NUMBERS, and the reason is the enum's own rule: they are ONE mechanism's arguments,
         * stated by ONE material row (one group instance's four sibling sockets) and consumed at ONE composition -
         * `u = min(SigmoidSharp(CastShadows, .z, .w), SigmoidSharp(0.5*NoL+0.5, .x, .y))`. `.x` / `.y` move the
         * TERMINATOR of the half-Lambert curve and `.z` / `.w` the one of the cast-shadow curve, and it is `.x`
         * that is the per-material terminator position the acceptance test checks against the frame: `0.1` on
         * `body_01`, `0.4` on `cloth_01/02`, `0.57` on `cloth_03/04/05`.
         *
         * `.x` / `.y` CARRY THE SAME `< 0` SENTINEL AS THE LANES ABOVE, and the fallback is the reference's own
         * interface default - `0.5699999928474426` and `0.1599999964237213` (spec §4.1). It is a SENTINEL rather
         * than a no-op because every one of the four numbers is meaningful at zero AND at a negative value:
         * `CastShadow_center` is `-0.1` on both body materials, i.e. the reference's own author states a negative
         * one, so a lane left at 0.0 could not be told from "not stated". `.z` / `.w` use the same sentinel and the
         * same kind of fallback (`0.0` / `0.0`, which IS the group's default for both).
         */
        goo_diffuse_a = 11, // `_GooDiffuseA`: `RemaphalfLambert_center`[x] / `_sharp`[y] / `CastShadow_center`[z] / `_sharp`[w]
        /**
         * `_GooDiffuseB` - three more of this step's per-material numbers: `.x` =
         * `GlobalShadowBrightnessAdjustment`, `.y` = `Color desaturation in shaded areas attenuation`, `.z` =
         * `MetallicMax`. `.w` IS RESERVED.
         *
         * `.x` IS THE GATE'S OWN LOWER EDGE and the one number this step's second new factor turns on:
         * `gate = smoothstep(GSBA, 1.0, RampAlpha)` - so on the cloth, whose GSBA is `-1.8`, a ramp texel of alpha
         * 0 still lifts the shadow side to `0.708`, while on the body, whose GSBA is `0.0318`, the same texel is
         * left at 0. THAT IS THE SPEC'S §10.4 ITEM 5: the `cloth_04_RD` alpha has a NON-MONOTONE notch at
         * `u ~= 0.6`, and this gate is what makes it observable. `< 0` is the sentinel and `0.0` is the fallback
         * (`::- ... :: 组输入.GlobalShadowBrightnessAdjustment = 0.0`); a sentinel rather than a no-op because
         * `0.0` is a value the reference really states, and one the body materials do NOT state (`0.0318`).
         *
         * `.y` HAS NO CONSUMER IN THIS STEP and is carried anyway, which needs the reason stated rather than
         * assumed: the reference reads it a SECOND time, as
         * `钳制.004 = clamp(luma(RampColor) + this, 0, 1) -> 色相/饱和度/明度.Saturation`, i.e. as the final
         * image's desaturation - a POST-rim stage this port has no equivalent of (spec §9-U1), so the value is
         * recorded where it belongs rather than applied where the port cannot honour it. It is in THIS lane and not
         * one of its own because it comes off the same material row and the same group as `.x` and `.z`.
         *
         * `.z` IS THE OTHER HALF OF A SPECIES OF DOUBLE COUNTING this step has to be careful about: the reference's
         * `metallic = lerp(0, MetallicMax, _P.R)` and its `diffuseColor = albedo * (1 - metallic)` are one
         * mechanism, and the PORT already reads `metallic` off the metallic/gloss map with the FAMILY's own
         * `MetallicMax` folded into `toon_params`... except that the port's `toon_params` has no `metallic_max`
         * field at all (it reads `orm.x` raw). This lane is therefore the reference's value, read where the
         * reference reads it, and the shader uses it for the new `1 - metallic` factor only - the port's own
         * `0.96 - 0.96*metallic` on the OLD three-layer path is left exactly as it was, because that path is not
         * what this step replaces. `.z` defaults to `1.0` (the group's own value) and carries the same `< 0`
         * sentinel.
         */
        goo_diffuse_b = 12, // `_GooDiffuseB`: `GlobalShadowBrightnessAdjustment`[x] / `Color desat...`[y] / `MetallicMax`[z]
        /**
         * `_GooFresnelInside`: `PBRToonBase`'s `fresnelInsideColor` in `.rgb` and `_ToonfresnelSMO_L` in `.w`.
         *
         * THE REFERENCE'S WHOLE SECOND FACTOR is `混合.006 = MIX(f = smoothstep(SMO_L, SMO_H, clamp(N.V)^Pow),
         * A = fresnelOutsideColor, B = fresnelInsideColor)`, and this lane is that MIX's B side plus the low end of
         * its window. BOTH ARE NON-WHITE ON EVERY MATERIAL THIS STEP COVERS (`[1.5443] x3` on `body_01`,
         * `[1.5234] x3` on every cloth), so the factor is a real LIFT of up to 1.54 and not a detail.
         *
         * WHY IT IS ONE LANE WITH THE OUTSIDE COLOUR'S SIBLING RATHER THAN A LANE WITH THE FOURTH COMPONENT FREE:
         * the two colours and the two window edges are the FOUR sockets of ONE MIX chain in ONE group, and the
         * window edge rides the colour because they are read at the same line - `smoothstep(L, H, x)` - so a
         * material that states one and not the other is a state the lanes can still express component by
         * component. `.w`'s sentinel is `< 0` with the group's own default behind it (`0.0`); the colour's neutral
         * behind the sentinel is the group's own `[1,1,1,1]`.
         */
        goo_fresnel_inside = 13, // `_GooFresnelInside`: `fresnelInsideColor`[rgb] + `_ToonfresnelSMO_L`[w]
        /**
         * `_GooFresnelOutside`: `PBRToonBase`'s `fresnelOutsideColor` in `.rgb` and `_ToonfresnelSMO_H` in `.w`.
         *
         * THE SHADED END OF THE SAME MIX, and the reason it is a LANE OF ITS OWN rather than the same lane's
         * reserved components: `.rgb` is the A side of a `MIX` whose B side is the lane above, so the two are
         * different TERMS of one lerp rather than two components of one value, and the reference states them as
         * two sibling sockets of two different names. `body_01`'s outside colour is `[0.6035, 0.5052, 0.4115]` - a
         * warm 0.4-0.6, i.e. this factor DARKENS the rim of the object where the surface turns away from the
         * camera, which is why it cannot be folded into the inside one.
         *
         * `_ToonfresnelSMO_H` IS THE WINDOW'S HIGH END and is `1.07` on `body_01/02` and `0.5` on every cloth - so
         * the cloth's fresnel saturates at `N.V^Pow = 0.5` while the body's never quite saturates. Its sentinel is
         * `< 0` with the group's default `1.0` behind it.
         *
         * `ToonfresnelPow` IS ALREADY CARRIED - `goo_rim_scalars.z` - and IS NOT DUPLICATED HERE (spec §10.3
         * item 3 says the same): one material's one socket must have one carrier.
         */
        goo_fresnel_outside = 14, // `_GooFresnelOutside`: `fresnelOutsideColor`[rgb] + `_ToonfresnelSMO_H`[w]
        /**
         * `_GooDirectOcclusion` in `.rgb`: `PBRToonBase`'s `directOcclusionColor`, the colour
         * `directOcclusion = lerp(directOcclusionColor, white, AO)` walks FROM as the occlusion rises.
         *
         * ITS NEUTRAL IS BLACK AND IT IS THE GROUP'S OWN DEFAULT (`[0.0, 0.0, 0.0, 1.0]`), which is also the only
         * value any of the seven materials states - but the lane exists because the port must be able to read the
         * OVERRIDE the moment another asset states one (`M_actor_laevat_hair_01` states
         * `[0.0811, 0.0109, 0.0187]`, and the hair is out of this step's scope exactly because its composition is
         * a different one - see the shader's family note). `.a` is not read: the reference's mix has an alpha, but
         * the port's occlusion is a scalar and the coverage alpha has one owner already.
         *
         * BLACK IS NOT A NO-OP HERE, which is why the sentinel is NOT used: `lerp(black, white, AO)` is a valid
         * `directOcclusion` and the value the reference's own assets produce, so `0.0` is a STATEMENT and cannot
         * double as "absent". The fallback behind a `< 0` component is therefore that same group default.
         */
        goo_direct_occlusion = 15, // `_GooDirectOcclusion`: `directOcclusionColor` (`.rgb`; black = the group's default)
        /**
         * `_GooSpecularFGD` in `.x`: `PBRToonBase`'s `specularFGD Strength` - THE ROUGHNESS-INDEPENDENT STRENGTH
         * the reference multiplies its whole IBL-specular term by (`混合.025 = specularFGD ⊙ this`, then
         * `混合.010` multiplies that by the energy compensation). `.y` / `.z` / `.w` are unused and reserved.
         *
         * IT IS ONE OF THE FOUR PER-MATERIAL NUMBERS STEP 5'S THREE TERMS CONSUME, and it is a SENTINELED SCALAR
         * because it is a STRENGTH: `body_01` / `body_02` state `0.7999999523162842` and every cloth inherits the
         * group's `1.0`, while `face_01` / `hair_01` DO NOT HAVE THE SOCKET AT ALL (their containers are
         * `PBRToonBaseFace` / `PBRToonBaseHair`, which have no `specularFGD Strength` in their `interface[]` -
         * spec §5.1). So `< 0` means "this material's container states nothing", and the stage answers the
         * reference's own group default `1.0` (`::- Arknights: Endfield_PBRToonBase :: 组输入.specularFGD Strength
         * = 1.0`) - which is also why a `-1` sentinel is honest here and a `0.0` would not be: `0.0` would switch a
         * material's entire IBL specular off, and it is not a value the group defaults the socket to.
         *
         * A LANE RATHER THAN A NUMBER IN `toon_params.slang`, for the reason every lane here exists: the value is
         * PER MATERIAL (`0.8` against the cloth's `1.0`) and the material record is inline in the per-draw push
         * block, so a `vec4` lane is the only per-material storage this renderer has.
         */
        goo_specular_fgd = 16, // `_GooSpecularFGD`: `specularFGD Strength` (`.x`; <0 = the group's own 1.0)
        /**
         * `_GooLightColor` in `.rgb`: `PBRToonBase`'s `dirLight_lightColor`, THE LIGHT COLOUR THE REFERENCE'S OWN
         * DIRECT SPECULAR MULTIPLIES ITSELF BY (`Vector Math.006 = Vector Math.005 * 转接点.042`, and `转接点.042`
         * is this socket - spec §5.4/§6.2). `.a` is unused and reserved.
         *
         * THE SPEC'S §5.4 CLOSES `goo_toon_plan.md` §1.4 item 6 AND STEP 4'S U7: this socket - a PER-MATERIAL
         * constant, `[1.0, 0.9580051302909851, 0.9580051302909851]` on `body_01` and
         * `[1.0, 0.9577637910842896, 0.9577637910842896]` on every cloth - is authoritative for the DIRECT terms,
         * and Goo's `Shader Info.Ambient Lighting` is the PROBE DIFFUSE IRRADIANCE the IBL-diffuse term is built
         * from (it has exactly one consumer in the whole container, `混合.013.A_Color`). So one lane carries the
         * sun's colour and the IBL keeps reading the engine's irradiance, which is the arrangement the reference
         * itself has.
         *
         * STEP 4 RECORDED THE ABSENCE OF THIS LANE AS A DELIBERATE SUBSTITUTION (its result document §4 item 2:
         * "`dirLight_lightColor` is not on a lane, the 4.2% blue pull is not applied"). STEP 5 LANDS IT, because
         * the reference multiplies `directLighting_specular` by it as well - and the two direct terms each carry it
         * ONCE, not one standing in for the other.
         *
         * THE NEUTRAL IS WHITE AND THE SENTINEL IS `< 0`: `1.0` is the group's own `interface[]` default for the
         * socket (`::- ... :: 组输入.dirLight_lightColor = [1.0, 1.0, 1.0, 1.0]`), i.e. a material that states no
         * row gets exactly what a material calling the group without stating it gets in Goo - while a material whose
         * CONTAINER has no such socket at all (`face_01`, whose `PBRToonBaseFace` states one;
         * `hair_01`, whose `PBRToonBaseHair` does not) is the "no statement" case the sentinel is for.
         */
        goo_light_color = 17, // `_GooLightColor`: `dirLight_lightColor` (`.rgb`; white = the group's default)
        /**
         * `_GooAmbientTint` in `.rgb`: `PBRToonBase`'s `AmbientLightColorTint` - what the reference's IBL DIFFUSE
         * multiplies the engine's probe irradiance by (`混合.013 = Shader Info.Ambient Lighting ⊙ this`).
         * `.a` is unused and reserved.
         *
         * IT IS A REAL PER-MATERIAL TINT ON THIS ASSET AND NOT A FORMALITY: `body_01` / `body_02` state
         * `[1.5121498107910156] x3` - a 51% LIFT of their whole ambient - while every cloth inherits the group's
         * white, so a single family number could not express it (spec §5.2). `face_01` overrides it too
         * (`[1.5509, 1.2822, 1.2712]`) and `hair_01`'s container has no such socket - the same two sentinel cases
         * the lane above has, and the same answer: white, which is the group's own `interface[]` default.
         *
         * `G9` OF THE SPEC'S §7.2 IS THE REASON IT IS READ HERE AND NOT FOLDED INTO THE OLD `ambient`: the port's
         * `irradiance_sample(n) * albedo * ao_used * params.ambient_strength` is THREE mechanisms the reference's
         * `混合.008` does not have (the family strength, the AO gate and the raw albedo), and this lane is one of
         * the two per-material replacements for them - the other being `directOcclusion`'s own AO on the direct
         * side, which step 4 already carries.
         */
        goo_ambient_tint = 18, // `_GooAmbientTint`: `AmbientLightColorTint` (`.rgb`; white = the group's default)
        /**
         * `_GooSpecularColor` in `.rgb`: `PBRToonBase`'s `SpecularColor` - THE DIRECT SPECULAR'S OWN PER-MATERIAL
         * TINT, the fourth factor of the reference's `directLighting_specular`
         * (`Vector Math.016 = 混合.016.Result ⊙ 转接点.077`, and `转接点.077 <- 组输入.SpecularColor`). `.a` is
         * unused and reserved.
         *
         * STEP 5 CANNOT OMIT IT AND THE SPEC SAYS SO IN ONE LINE (§7.3 item 5: "必须新增一条颜色 lane，否则直接高光
         * 少一个逐材质乘子（`body_01` 会暗 4.2 倍）"). These are NOT tints: they are HDR MULTIPLIERS well above 1 -
         * `body_01`/`body_02` state `[4.2092814445495605, 3.7652196884155273, 3.7652196884155273]`, `cloth_01`
         * `[1.600000023841858, 1.4312067031860352, 1.4312067031860352]`, `cloth_02` `[1.0, 0.8945042490959167,
         * 0.8945042490959167]`, `cloth_03/04/05` `[4.2092814445495605, 3.7652199268341064, 3.7652199268341064]`.
         *
         * WHY IT IS NOT `goo_base_colour` REUSED, which is the tempting economy and was this step's first attempt:
         * `BaseColor` and `SpecularColor` are two INDEPENDENT per-material sockets of the same group instance
         * (`Input_4` and `Input_13` of `群组.002`), and on this asset they disagree by 3.6x on the body
         * (`[1.1628, 0.9888, 1.0280]` against `[4.2093, 3.7652, 3.7652]`). Substituting one for the other would be
         * a magnitude invented at the substitution site, which is the one thing this project's discipline forbids -
         * and the port would then have no way to be right about `cloth_02`, whose specular colour is DARKER than
         * its base colour while the body's is brighter. The spec's §9-U7 records that the value's DIMENSION is
         * unconfirmed (a linear HDR colour or a strength); this lane carries it as the RGB it is and lets it
         * multiply, exactly as the reference's `Vector Math.016` does.
         *
         * THE NEUTRAL IS WHITE (the group's own `interface[]` default, so a material that states no row gets what a
         * caller that states nothing gets in Goo) and the sentinel is `< 0`, for the reason every colour lane above
         * has: no component of a multiplier the reference's author wrote is negative.
         */
        goo_specular_color = 19, // `_GooSpecularColor`: `SpecularColor` (`.rgb`; white = the group's default)
        // ---- STEP 7: THE FACE CONTAINER'S OWN NUMBERS, WHICH NO LANE ABOVE CARRIES ----
        //
        // FOUR LANES AND NOT SIX, because the audit came first and four of the FACE's twenty-three stated numbers
        // already have carriers the SAME socket name fills: `BaseColor` rides `goo_base_colour` (10),
        // `GlobalShadowBrightnessAdjustment` / `Color desaturation in shaded areas attenuation` / `MetallicMax` ride
        // `goo_diffuse_b` (12), `SDF_RemaphalfLambert_center` / `_sharp` and `CastShadow_center` / `_sharp` ride
        // `goo_diffuse_a` (11 - the four slots are the same four sockets, in the same order, from a different
        // container), and `dirLight_lightColor` / `AmbientLightColorTint` / `SpecularColor` / `Rim_Color` ride
        // 17/18/19/7. The spec's §13.2 says exactly that ("先审计每个数是否已经在某条 lane 里 ... 不要为已经在 lane 里的数新增行"), and
        // the audit is the reason this is four lanes rather than the fourteen the container's socket list suggests.
        /**
         * `_GooFaceScalarsA`: the FACE's four remaining SCALARS that are not a tint - `chin_RemaphalfLambert_center`
         * [x] / `chin_RemaphalfLambert_sharp` [y] / `sphereNormal_Strength` [z] / `SmoothnessMax` [w].
         *
         * ALL FOUR ARE MATERIAL OVERRIDES ON `M_actor_laevat_face_01` EXCEPT THE LAST (`0.5`, `0.10000000149011612`,
         * `1.0`, and `1.0` which is the group's own default), and each one is load-bearing:
         *
         *   * THE CHIN PAIR is the SECOND `SigmoidSharp` call site's `center` / `sharp` (`群组.021`), i.e. the
         *     half-Lambert branch of the ramp coordinate - the branch `cm_M`'s `G` selects over the SDF's. It is a
         *     DIFFERENT pair from the SDF's (which ride lane 11) and the two are not interchangeable: on this
         *     material they are `(0.5, 0.1)` against `(0.1, 0.5)`, i.e. the same two numbers with the roles swapped.
         *   * `sphereNormal_Strength` is `Recalculate normal`'s factor, and it is `1.0` here: the face's shading
         *     normal is the SPHERE normal about `headCenter` blended with the geometric one by `cm_M.G`. Without the
         *     lane the port would have to pick a number, and `0.0` (the group's default, and the port's own
         *     behaviour before this step) is the opposite behaviour rather than a neutral one.
         *   * `SmoothnessMax` is `PerceptualSmoothnessToPerceptualRoughness`'s input, and it is the ONE place the
         *     FACE disagrees with the port's family table by construction: the reference's face is `1.0` (perfectly
         *     smooth, so the GGX lobe is a delta and `directLighting_specular` collapses to zero), while
         *     `toon_params.slang`'s face entry carries `roughness = 0.7` for the ARTICLE's chain. Reading the
         *     family's number here would give the reference's face a broad specular lobe it does not have.
         */
        goo_face_scalars_a = 20, // `_GooFaceScalarsA`: chin `_center`[x] / `_sharp`[y] / `sphereNormal_Strength`[z] / `SmoothnessMax`[w]
        /**
         * `_GooFaceScalarsB`: the FACE's two EMISSION BRIGHTNESSES and `Front transparent red`'s two shape numbers -
         * `Face Final brightness` [x] / `Eyes white Final brightness` [y] / `Front R Pow` [z] / `Front R Smo` [w].
         *
         * `混合.020` SELECTS BETWEEN THE FIRST TWO per pixel (`Cm_M`'s sibling mask's `G > 0.5`, see
         * `toon_slot::goo_face_csumt`) and multiplies the face's finished colour by the winner - so on this material
         * the face is `1.15x` its shaded value over 98.2% of the mask's atlas and `1.5x` over the rest. THEY ARE NOT
         * A TINT OR A STRENGTH the port already has: the reference's face output is an EMISSION node whose colour is
         * `色相/饱和度/明度.Color` and whose STRENGTH is this number, and no other socket of the container carries it.
         */
        goo_face_scalars_b = 21, // `_GooFaceScalarsB`: `Face Final brightness`[x] / `Eyes white Final brightness`[y] / `Front R Pow`[z] / `Front R Smo`[w]
        /**
         * `_GooFaceNoseShadow`: the FACE's `nose_shadow_Color`, a MIX's A side rather than a multiply.
         *
         * `混合.017 = MIX(A = this, B = white, f = _D(sRGB).A)`, so the nose shadow's whole strength is the ALBEDO
         * TEXTURE'S ALPHA - and on this asset that alpha is NOT the constant 1 the spec's §2.4/A7 assumed: measured
         * over `T_actor_laevat_face_01_D.png` it is `min 202 / mean 254.978 / max 255`, so `混合.017` is white over
         * most of the plate and as low as `0.792*1 + 0.208*0.3084 = 0.856` where the alpha dips - a 14% darkening
         * that the spec's A7 says does not exist. THE NEUTRAL IS BLACK, which is the socket's own `interface[]`
         * default, so a material that states no row gets a fully shadowed nose at alpha 0 and white at alpha 1 -
         * the reference's own answer rather than a number chosen here.
         */
        goo_face_nose_shadow = 22, // `_GooFaceNoseShadow`: `nose_shadow_Color` (`.rgb`; BLACK = the group's default)
        /**
         * `_GooFaceFrontR`: the FACE's `Front R Color` - `Front transparent red`'s `sideColor`, i.e. the tint the
         * cheek takes where the mask says the light passes through it.
         *
         * IT IS THE ONE COLOUR OF THAT SUB-GROUP THAT IS NOT ALREADY CARRIED, and the sub-group is `users == 1`
         * (only the FACE container instantiates it). `Front R Pow` / `Front R Smo` ride lane 21, `D_R` rides
         * `toon_slot::goo_face_cm`'s `R` and `Positive attenuation` is `dot(headForward, V)` - so this lane
         * completes the sub-group's five arguments with no sixth lane.
         *
         * THE NEUTRAL IS BLACK, the socket's own `interface[]` default: `Front transparent red` ends in a MIX whose
         * B side is this colour, so black leaves the albedo as the albedo.
         */
        goo_face_front_r = 23, // `_GooFaceFrontR`: `Front R Color` (`.rgb`; BLACK = the group's default)
        /**
         * `_GooNormalStrength`: the `DecodeNormal` group's own `NormalStrength` input, i.e. the strength the reference
         *'s normal decode applies to the normal map's `xy` before it is turned into a tangent-space normal.
         *
         * IT IS NOT A PROPERTY THE GLTF CARRIES, and that is why it needs a lane: `normalTexture.scale` is `1.0` on
         * every material of every character in this repository, while the values the reference states for the same
         * materials are `1.25` (the body), `1.4458599090576172` (the cloth) and `1.0` (everything else). It is a
         * socket of a `ShaderNodeGroup` instance inside each material's tree (`DecodeNormal :: 组输入.NormalStrength`),
         * so the sidecar is the only channel that can carry it.
         *
         * `.x` IS THE STRENGTH AND THE OTHER THREE COMPONENTS ARE UNUSED - the port's scalar-lane convention (see
         * `specular_strength` and `parallax_scale`). THE SENTINEL IS `-1000` ON EVERY COMPONENT rather than `-1` on
         * `.x` alone, because `-1` is a strength the group's own range does not contain but `0` IS a value the
         * reference really states (`M_actor_chen_body_01.001` carries `1.3184714317321777` with `Use NormalTex? = 0`,
         * so a decode is switched off there by the material's own gate rather than by a zero strength): see
         * `goo_lane_absent` in `source/shaders/toon/character_forward.slang` for the audit rule this follows.
         *
         * A MATERIAL WITH NO ROW KEEPS THE PREVIOUS NORMAL RATHER THAN GETTING THE REFERENCE'S GROUP DEFAULT `1.0`,
         * which is a deliberate deviation from "the reference's default" and the reason is measured: the shipped
         * normal maps store `x` and `y` with a CONSTANT blue channel (uniq(B) = 1 on the body and the cloth), so the
         * old `rgb*2-1` decode is inward-pointing on every one of them - i.e. the previous normal is not "the
         * reference at strength 1", it is a different (wrong) picture. Switching every material with no measured row
         * onto the decode would move models this step did not measure (chen's cloth has a `NormalStrength` in the
         * reference but no row here), so a row is what turns the decode on. The FACE, BROW and IRIS materials have no
         * normal texture at all, so for them "no row" and "the reference's default" are the same frame.
         */
        goo_normal_strength = 24, // `_GooNormalStrength`: `DecodeNormal`'s `NormalStrength` (`.x`; `-1000` = no row)
        // ---- STEP 10: THE TWO SWITCHES THAT DECIDE WHICH ARM OF `混合.016` THE DIRECT SPECULAR TAKES ----
        // The sockets are `Use anisotropy?` / `Anisotropic mask` / `Use Toonaniso?` of `Arknights:
        // Endfield_PBRToonBase` (`ng[2]`), all three plain `组输入` inputs with an `interface[]` default of `0.0`
        // (`interface[3]`, `[45]`, `[4]`). They were constant in this port until step 10 (`goo_use_anisotropy_default`
        // / `goo_anisotropic_mask_default` in `source/shaders/toon/character_forward.slang`), which made the stage return the
        // WRONG ARM for the one material of this asset that states `Use anisotropy? = 1`. The census (all 23 `ng[2]`
        // instances, `goo_step9_verify.md` §7.6 / `deren-ab/_s9_aniso3.py` §C) is what the lane's values in
        // `chars/laevatain_goo.glb.toon.tsv` are filed against.
        /**
         * `_GooAnisoGate`: the TWO switches of `PBRToonBase`'s `混合.016` / `混合.017`, plus the third one that
         * selects the lobe `混合.020` mixes in.
         *
         * `混合.016 = MIX(f = 组输入.Use anisotropy?, A = DV_SmithJointGGX_Aniso.original × F_Schlick, B =
         * 混合.017)`, `混合.017 = MULTIPLY(A = 混合.020, B = 组输入.Anisotropic mask)` (its `MULTIPLY` factor is the
         * unlinked-but-`enabled` `1.0`, so it is exactly `A*B`) and `混合.020 = MIX(f = 组输入.Use Toonaniso?, A =
         * 群组.007.anisotropy × F_Schlick, B = 钳制.Result)`. All three sockets are plain `组输入` inputs of `ng[2]`
         * - there is no texture channel and no other node on the factor chain (verified: `混合.016.Factor`'s only
         * `from` is `组输入.Use anisotropy?`, so there is NO polarity node either).
         *
         * `.x = Use anisotropy?`, `.y = Anisotropic mask`, `.z = Use Toonaniso?`, `.w` RESERVED AND UNUSED. `.z` IS
         * RECORDED HERE AND READ AT THE ARM: `混合.020` mixes the anisotropic lobe against its `钳制` clamp arm, and
         * BOTH the lobe (step 12's lane 26) and the clamp arm are implemented - but the whole of `混合.020` is
         * multiplied by `Anisotropic mask` in `混合.017`, and this asset's mask is `0.0` on every material, so no
         * pixel of any frame this repository renders can reach either arm (see `混合.016`'s block in
         * `source/shaders/toon/character_forward.slang`). It was recorded without a reader from step 10 until step 12.
         *
         * THE NEUTRAL IS `(0, 0, 0, 0)` AND IT IS THE REFERENCE'S OWN GROUP DEFAULT, not a chosen number:
         * `ng[2].interface[3]` (`Use anisotropy?`) `default = 0.0`, `interface[45]` (`Anisotropic mask`)
         * `default = 0.0` and `interface[4]` (`Use Toonaniso?`) `default = 0.0`. A material that states no row
         * therefore gets the group's own answer, which - since `混合.016` returns its A arm at 0 - is the isotropic
         * product. THE GPU TABLE'S DEFAULT IS `glm::vec4(1.0f)` FOR EVERY LANE, so this lane MUST be overridden in
         * `runtime.constructor.cppm`; with no override a material with no row would read flag 1 and get the masked
         * arm instead. The census behind the values this asset states: all 23 `ng[2]` instances, flag `1.0` only on
         * `M_actor_laevat_cloth_05` and `M_actor_chen_cloth_01.001`, mask non-zero only on `M_actor_chen_cloth_01.001`
         * (linked) and `M_actor_yvonne_cloth_03` (`goo_step9_verify.md` §7.6; `deren-ab/_s9_aniso3.py` §C reprints it).
         */
        goo_aniso_gate = 25, // `_GooAnisoGate`: `Use anisotropy?` [x] / `Anisotropic mask` [y] / `Use Toonaniso?` [z] (w reserved)
        /**
         * `_GooAnisoRough` in `.x` / `.y`: THE ANISOTROPIC LOBE's TWO ROUGHNESSES - `Aniso_SmoothnessMaxT` and
         * `Aniso_SmoothnessMaxB`, the two `PBRToonBase` sockets `roughnessT` and `roughnessB` are built from
         * (`rT = (1 - Aniso_SmoothnessMaxT)^2`, `rB = (1 - Aniso_SmoothnessMaxB)^2`). `.z` / `.w` are unused and
         * reserved.
         *
         * WHY IT IS A LANE AND NOT A CONSTANT: the two sockets are PER MATERIAL and nothing else in this renderer
         * can carry them - the material record is full (see the note at the top of this enum) and both are plain
         * `组输入` numbers rather than textures or colours. The values this asset states are NOT the group's
         * defaults: `Aniso_SmoothnessMaxT = 0.2197451889514923` and `Aniso_SmoothnessMaxB = 0.668789803981781` on
         * all five `M_actor_laevat_cloth_*` materials, against `ng[2].interface[20]` / `[21]` = `0.0` / `0.0`. A
         * constant here would therefore be wrong for every material the reference's own author tuned, in both
         * directions (a default of 0 gives `rT = rB = 1`, a plausible-looking but unauthored answer).
         *
         * THE NEUTRAL IS `(0.0, 0.0, 0.0, 0.0)` AND IT IS THE REFERENCE'S OWN GROUP DEFAULT, not a chosen number -
         * the same shape lane 25 above uses and for the same reason. `Aniso_SmoothnessMaxT = 0` makes `rT = 1`,
         * i.e. a fully rough anisotropic lobe, which is exactly what `ng[2]` returns for a caller that states
         * nothing; and because `0.0` here is a stated value the reference really has, it cannot double as "absent",
         * so NO sentinel is needed and the lookup's generic `color` path answers `toon_colour_neutral` unchanged
         * (see `main.cpp`'s `toon_colour`, which needs no branch for this lane). THE GPU TABLE'S DEFAULT IS
         * `glm::vec4(1.0f)` FOR EVERY LANE, so this lane MUST be overridden in `runtime.constructor.cppm` exactly
         * as lane 25 is; with no override a material with no row would read `Aniso_SmoothnessMaxT = 1.0`, i.e.
         * `rT = 0`, a mirror the reference never describes.
         */
        goo_aniso_rough = 26, // `_GooAnisoRough`: `Aniso_SmoothnessMaxT` [x] / `Aniso_SmoothnessMaxB` [y] (zw reserved)
        /**
         * `_GooRSScalars`: the three scalars of the reference's mechanism table #14 (`RS EFF`) that the branch's
         * own gates and its LIGHTEN factor are built from, in the reference's socket order - `.x = Use RS_Eff?`,
         * `.y = RS Multiply Value`, `.z = RS Model`. `.w` is unused and reserved (`0.0`).
         *
         * IT IS A `color` ROW AND TAKES THE GENERIC PATH, so `main.cpp`'s `toon_colour` needs no branch for it:
         * the four comma-separated components arrive one at a time through `material_sidecar::others`, exactly as
         * `_GooAnisoGate`'s and `_GooAnisoRough`'s do. The three sockets are BOOLEANS AND NUMBERS rather than a
         * colour, which is why the row is spelled as a raw list (`1.0,1.0,1.0,0.0`) and not as an RGBA tint.
         *
         * THE BOOLEANS TRAVEL AS `0.0` / `1.0` AND THE STAGE TESTS `> 0.0f` RATHER THAN `== 1.0`, because a
         * boolean socket's value is a float in this sidecar and any positive value that is not exactly one would
         * be read as "off" by an equality test (`goo_step13_rs_eff_spec_s.md` §2.1).
         *
         * `.z` IS `RS Model`, AND SINCE STEP 15 IT IS HONOURED: the reference's `RS EFF` is a two-arm selector
         * (`RS Model`: 0 = `armA`, the 256x1 `_RS` sheet sampled at the Layer Weight's `u`; 1 = `armB`,
         * `_M ⊙ RS ColorTint`) and the stage takes the arm this component names. Before step 15 it was CARRIED
         * WITHOUT BEING HONOURED - the stage kept the base unchanged when `.z == 0.0` - which was the deliberate
         * infidelity the `armA` port removes; the port site and its three stated limits are in
         * `source/shaders/toon/goo_toon.slang`'s RS block and `toon_colour_lane::goo_rs_arm0`.
         *
         * WHY IT IS A LANE AND NOT A CONSTANT: all three are per material. `Use RS_Eff?` is `1` on exactly three
         * of the 23 `PBRToonBase` instances in `gooblender/nodes.json`, and `RS Model` is `0` on one of those
         * three (`M_actor_yvonne_cloth_03`, which no captured asset carries) - so a constant would either switch
         * the branch on for 20 materials that never asked for it or off for the two this asset ships.
         */
        goo_rs_scalars = 27, // `_GooRSScalars`: `Use RS_Eff?` [x] / `RS Multiply Value` [y] / `RS Model` [z] (w reserved)
        /**
         * `_GooRSTint`: `.rgb = RS ColorTint` (the RIGHT input of `混合.038 = _M ⊙ RS ColorTint`) and
         * `.w = SmoothStep.max`, the upper edge of the `Arknights: Endfield_SmoothStep` subgroup that produces
         * `_M` itself.
         *
         * THE TWO HALVES ARE ONE LANE BECAUSE THEY ARE ONE MATERIAL'S ONE STATEMENT, which is the shape
         * `_GooEyeBrightness` and `_GooRimScalars` already use: a `color` row is four floats, and the port has
         * exactly two numbers to carry here.
         *
         * `.w` IS NOT PART OF THE TINT AND MUST NOT BE READ AS ONE. It is the second argument of the smoothstep
         * that maps the mask's Rec.709 luminance to a factor, and it is PER MATERIAL: the two materials this
         * asset switches the branch on for state `0.9900000095367432` (`M_actor_laevat_cloth_02`) and `1.0`
         * (`M_actor_laevat_cloth_05`). A single constant would be wrong for one of them, and the two are close
         * enough that the error would be invisible in a log. The smoothstep's `min` is `0.0` on both and is NOT
         * carried (spec U11) - if a third material with a non-zero `min` ever lands, this lane has no component
         * left and the mapping needs a lane of its own.
         *
         * `.w <= 0` IS READ AS `1.0` BY THE STAGE, which is a deviation from the reference and is deliberate: the
         * GPU table's default for an unstated lane is `(0,0,0,0)`, and a literal `max = 0` would divide by zero in
         * the smoothstep's `(luma - min) / (max - min)` (spec U7). It is stated at the port site.
         *
         * THE TWO MATERIALS' TINTS DIFFER IN `.y` BY 3.6e-6 (`1.4143484830856323` vs `1.4143449068069458`), and
         * that is a real difference between two authored values rather than a rounding of one: they must not be
         * unified, and a test pins both. `M_actor_laevat_cloth_05`'s `_M` also comes from an image named
         * `T_actor_laevat_cloth_03_M` - the image name is NOT the material name (spec §4.4 trap 1).
         */
        goo_rs_tint = 28, // `_GooRSTint`: `RS ColorTint` [rgb] / `SmoothStep.max` [w]
        /**
         * `_GooRSArm0`: the four `armA` sockets that are not a colour, one lane for the reason
         * `_GooRSScalars` is one - a `color` row is four floats and this arm has exactly four numbers.
         *
         * `.x = RS_Index` (which of the reference's two `_RS` character sheets `混合.032` mixes towards),
         * `.y = RS Strength` (`s023 = s036 * RS Strength`, unclamped in the reference),
         * `.z = Layer weight Value` and `.w = Layer weight Value Offset`, the two that build the sheet's `u`
         * coordinate: `u = clamp(1 - |V·n_rs|^remap(.z) + .w, 0, 1)`. The sheet's `v` is a constant `0.5` because
         * every `_RS` sheet in the dumps is 256x1.
         *
         * `.x` IS CARRIED AND ANSWERED - BY THE HOST, NOT BY THE SHADER, and the split is the lane ceiling above
         * rather than an oversight: the port has ONE sheet slot, so `RS_Index` cannot add a second sampler and
         * cannot be a shader-side choice between two bound textures. `main.cpp`'s `toon_texture` therefore
         * resolves which sheet the slot holds (`RS_Index >= 0.5` and an enabled, named, resolvable `_GooRSSheet1`
         * read that sheet; every other case reads the first) and the shader samples one slot as it always did,
         * never looking at `.x`. THE RULE IS A CHOICE AND NOT A REPRODUCTION: the reference blends its two sheets
         * CONTINUOUSLY (`mix(A, B, clamp(RS_Index, 0, 1))`, `混合.032`), so an `RS_Index` between the endpoints is
         * quantised to one - a named limitation of this route (`goo_step15_lane_rs_index_spec.md` §9.3 (z)(1)).
         * The dumps put `RS_Index = 1` on exactly one material (`M_actor_yvonne_cloth_03`, in no captured asset)
         * and `0` on the two this asset ships, so the deviation is stated rather than measured away.
         *
         * THE NEUTRAL IS `(0,0,0,0)` AND IT MAKES `armA` A NO-OP: `RS Strength = 0` zeroes `s023` and with it
         * `arm0`, which is what a material whose sidecar states no `_GooRSArm0` row must get. Note that is NOT
         * the same as the lane being absent: the lane exists and is written for every material, and a zeroed
         * `arm0` still passes through `armB`'s own LIGHTEN (`max(base, 0)`), which is why the outer gate stays
         * `Use RS_Eff?` and only that - see the RS block's note in `source/shaders/toon/goo_toon.slang`.
         */
        goo_rs_arm0 = 29, // `_GooRSArm0`: `RS_Index` [x] / `RS Strength` [y] / `Layer weight Value` [z] / `Offset` [w]
        // Captured viewer profile: sixteen explicitly packed vec4 values; zero disables it.
        game_toon_0 = 30,
        game_toon_1 = 31,
        game_toon_2 = 32,
        game_toon_3 = 33,
        game_toon_4 = 34,
        game_toon_5 = 35,
        game_toon_6 = 36,
        game_toon_7 = 37,
        game_toon_8 = 38,
        game_toon_9 = 39,
        game_toon_10 = 40,
        game_toon_11 = 41,
        game_toon_12 = 42,
        game_toon_13 = 43,
        game_toon_14 = 44,
        game_toon_15 = 45,
        count = 46,
    };

    /**
     * @ingroup engine_primitive
     * @brief one primitive's TOON TEXTURE INPUTS, in `toon_slot` order
     *
     * These come from the model's toon material SIDECAR rather than from glTF, and the runtime does not read
     * that file: it asks a `toon_lookup` the application installs (see `runtime::set_toon_lookup`), because
     * `deren_engine` deliberately does not depend on `gltf_loader` and the sidecar reader lives there. An
     * INVALID input means "this material has no such map" - and `flags` is what says whether the artist wanted
     * it at all, which is a different question: a map can exist while its `_Use` flag is off.
     */
    export struct toon_inputs {
        deren::utility::vessel::array<texture_input, static_cast<std::size_t>(toon_slot::count)> slots = {};
        /**
         * THE MATERIAL'S OWN COLOURS (see `toon_colour_lane`), each one NEUTRAL BY DEFAULT.
         *
         * WHITE IS THE NEUTRAL, and it is chosen rather than left zero for the reason every other default here is
         * chosen: a missing row must leave the shading exactly where it was. White does that for a multiply-tint
         * (the eye's two region colours) and for a rim colour, whereas (0,0,0,0) would black them out - which is
         * the failure mode of a lane whose absence is indistinguishable from a black value.
         *
         * THE OUTLINE LANE IS THE EXCEPTION, AND ITS `.w` IS 0.0 FOR THE SAME REASON THE OTHERS ARE WHITE: the
         * four floats of that lane are not all the same kind of value. `.rgb` is a tint, where white is the no-op,
         * and `.w` is a WIDTH, where 0 is the no-op - a material with no `_OutlineWidth` row must not be outlined
         * at all, and the shader's gate is exactly `w > 0`. White in `.w` would draw an outline of width 1.0
         * around every material that states no outline, which is the whole character.
         *
         * ... AND THE TWO SCALAR LANES' NEUTRALS ARE NEITHER, WHICH ARE THE PLACES THIS TABLE CARRIES A SENTINEL:
         * their `.x` is `-1.0`, meaning "the asset states no such row for this material". A "no-op" number does
         * not exist for either - the value IS the term's strength (`_Specular`) or its depth (`_ParallaxScale`),
         * so any number in the value's own range would be one the port invented (and `0.0`, the tempting one, is
         * a value the game really states on the iris and the brow for `_Specular`, and a legitimate authored
         * "no parallax" for `_ParallaxScale`). `-1` is outside either value's range and therefore cannot be
         * confused with a statement; the stage's test is `>= 0.0` on both (see
         * `toon_colour_lane::specular_strength` and `toon_colour_lane::parallax_scale`).
         *
         * THE RIM LANES ADD A THIRD SHAPE AND IT IS WORTH NAMING BECAUSE IT IS NOT A THIRD CONVENTION: they use
         * the SAME `< 0` sentinel as the two above, and their FALLBACKS differ per component because the
         * REFERENCE's defaults differ per component. `goo_rim_colour` starts WHITE (the `Rim_Color` sub-group's
         * own interface default - a multiply-tint, so white is the no-op), and `goo_rim_scalars` /
         * `goo_rim_widths` start `(-1, -1, -1, -1)` with the STAGE resolving each component to that socket's
         * reference default (`1.0` / `0.8999999761581421` / `2.0` / `0.0` for the scalars, `0.5` for each width).
         * A single number here could not express that, and a number in a component's own range could not mean
         * "absent" (`Rim_ColorStrength = 0.0` is how the reference's author switches a rim OFF). See
         * `toon_colour_lane::goo_rim_colour` / `goo_rim_scalars` / `goo_rim_widths`.
         *
         * ... AND STEP 4'S SIX LANES USE BOTH SHAPES AT ONCE, which is the same statement one level further out:
         * `goo_base_colour` starts WHITE and `goo_direct_occlusion` starts BLACK, because those two ARE the
         * reference's own interface defaults and each is a real value in its own right; the other four start
         * `(-1, -1, -1, -1)`, because each of their eight per-material numbers is one the reference or its own
         * author states at zero or below - `CastShadow_center` is `-0.1` on both `body_01` and `body_02` - and
         * their fallbacks are that same group's interface defaults, resolved component by component in the stage.
         * See `toon_colour_lane::goo_base_colour` .. `goo_direct_occlusion`.
         *
         * THE INITIALISER BELOW IS A POSITIONAL AGGREGATE DEFAULT AND IT MUST STATE EXACTLY
         * `toon_colour_lane::count` ELEMENTS - one per lane, in `toon_colour_lane` order, none repeated,
         * none reordered, none omitted. A SHORT LIST IS NOT A COMPILE ERROR AND NOT A WARNING: the elements
         * that are not written are VALUE-INITIALISED, so omitting one silently renumbers the defaults of
         * every lane after it. This table once stated 23 of the 29, and lanes 23..28 read plain zero instead
         * of the sentinels and the opaque black their own lanes document - a defect that was invisible in
         * every frame and visible only in the text. `source/tests/test_toon_material_sidecar.cpp` now counts these
         * elements for exactly that reason.
         *
         * `main.cpp`'s `toon_colour_neutral` IS THE AUTHORITATIVE COPY of this table - the application
         * answers a material's missing row from it, lane by lane, through the `toon_lookup::colour` callback
         * - and this member is its mirror. The two must agree element for element; the runtime keeps a third
         * copy in `runtime.constructor.cppm`.
         *
         * CHANGING THIS TABLE CHANGES NO FRAME, WHICH IS NOT AN EXCUSE FOR IT TO BE WRONG: the runtime
         * overwrites EVERY lane of every registered material from the callback (the loop
         * `lane < static_cast<uint32_t>(toon_colour_lane::count)` in `runtime.declarations.cppm`), so what
         * the stage reads is the callback's answer and never this default. This default is what a consumer
         * with no callback installed would read.
         */
        deren::utility::vessel::array<glm::vec4, static_cast<std::size_t>(toon_colour_lane::count)> colours = {glm::vec4(1.0f),
                                                                                                               glm::vec4(1.0f),
                                                                                                               glm::vec4(1.0f),
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
                                                                                                               glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f),
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f),
                                                                                                               glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f),
                                                                                                               // ---- STEP 4'S SIX LANES (`toon_colour_lane::goo_base_colour` .. `goo_direct_occlusion`) ----
                                                                                                               //
                                                                                                               // THE FIRST TWO ARE THE REFERENCE'S OWN INTERFACE DEFAULTS RATHER THAN A
                                                                                                               // CONVENTION - `BaseColor` is `[1,1,1,1]` and `directOcclusionColor` is
                                                                                                               // `[0,0,0,1]` in `gooblender/nodes.json` - and the other four are the SAME
                                                                                                               // `-1000` SENTINEL (`goo_lane_absent` in the shader), and it is NOT `< 0`
                                                                                                               // BECAUSE TWO OF THE EIGHT ARE AUTHORED NEGATIVES - `CastShadow_center` is
                                                                                                               // `-0.1` on both body materials and `GlobalShadowBrightnessAdjustment` is
                                                                                                               // `-1.8` on the cloth - so a neutral inside the range would swallow them. The two
                                                                                                               // fresnel lanes' `.rgb` is white behind the sentinel - the group's own
                                                                                                               // `fresnel{Inside,Outside}Color` default - and their `.w` is the window
                                                                                                               // edge's default (`0.0` / `1.0`), resolved by the stage component by
                                                                                                               // component exactly as `goo_rim_scalars`' four are.
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                                                                                               // ---- STEP 5'S FOUR LANES (`toon_colour_lane::goo_specular_fgd` .. `goo_specular_color`) ----
                                                                                                               //
                                                                                                               // ALL FOUR ARE THE REFERENCE'S OWN `interface[]` DEFAULTS, which is
                                                                                                               // the same shape step 4's first two lanes use and for the same reason:
                                                                                                               // each one is a value the graph really uses (a strength of 1.0, a white
                                                                                                               // light, a white multiplier) rather than a sentinel standing in for a missing mechanism.
                                                                                                               // `.x` CARRIES THE SENTINEL AND `.yzw` THE FALLBACK, because the three
                                                                                                               // components of the one scalar lane must be able to say "this material's
                                                                                                               // container has no such socket" while the three colour lanes have no
                                                                                                               // in-range value that could mean it: `specularFGD Strength = 0.0` would
                                                                                                               // be a material with no IBL specular at all, and black is a light
                                                                                                               // colour the reference's own author never wrote. So -1 in `.x` is
                                                                                                               // the scalar's absence and the STAGE resolves it (exactly as
                                                                                                               // `goo_rim_scalars`' four components are resolved), while the two
                                                                                                               // colour lanes fall back on their own four components.
                                                                                                               glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               // ... AND `SpecularColor` IS WHITE FOR THE OUTLINE LANE'S REASON READ THE
                                                                                                               // OTHER WAY: it is a MULTIPLIER, so 1.0 is the no-op, and a lane left at
                                                                                                               // 0 would DELETE the direct specular of every material that states no
                                                                                                               // row. White is also the reference's own `interface[]` default for
                                                                                                               // the socket, so "not stated" and "stated as white" agree.
                                                                                                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                                                                                                               // ---- STEP 7'S FOUR LANES (`toon_colour_lane::goo_face_scalars_a` .. `goo_face_front_r`) ----
                                                                                                               //
                                                                                                               // THE TWO SCALAR LANES ARE `-1000` SENTINELS AND THE TWO COLOUR LANES ARE
                                                                                                               // OPAQUE BLACK, each for its own lane's reason: the scalars' zero is a value the
                                                                                                               // graph really uses (`SmoothnessMax = 0` is "perfectly rough", and the two
                                                                                                               // brightnesses are multiplied into the pixel), while the two colours ARE the
                                                                                                               // reference's own `interface[]` defaults - white would be the strongest possible
                                                                                                               // statement about a nose shadow (`混合.017`'s A side) and about `Front transparent
                                                                                                               // red`'s tint. See `toon_colour_lane::goo_face_scalars_a` .. `goo_face_front_r`.
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                                                                                               // ---- STEP 8'S ONE LANE (`toon_colour_lane::goo_normal_strength`) ----
                                                                                                               //
                                                                                                               // A `-1000` SENTINEL RATHER THAN THE GROUP'S OWN `1.0`, because an absent row
                                                                                                               // has to leave the chain's previous normal in place instead of switching the
                                                                                                               // decode on for a material the port carried no measured value for - and not a
                                                                                                               // `0`, because `NormalStrength = 0` is a value the reference's own materials
                                                                                                               // state. See `toon_colour_lane::goo_normal_strength`.
                                                                                                               glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
                                                                                                               // ---- STEP 10'S ONE LANE (`toon_colour_lane::goo_aniso_gate`) ----
                                                                                                               //
                                                                                                               // `(0, 0, 0, 0)` IS THE REFERENCE'S OWN GROUP DEFAULT AND IS NOT A SENTINEL:
                                                                                                               // `Use anisotropy?`, `Anisotropic mask` and `Use Toonaniso?` are all `0.0` in
                                                                                                               // the dump, so a material that states no `_GooAnisoGate` row gets the arm the
                                                                                                               // graph gives a caller that states nothing. See
                                                                                                               // `toon_colour_lane::goo_aniso_gate`.
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               // ---- STEP 12'S ONE LANE (`toon_colour_lane::goo_aniso_rough`) ----
                                                                                                               //
                                                                                                               // `(0, 0, 0, 0)` IS THE REFERENCE'S OWN `interface[]` DEFAULT,
                                                                                                               // exactly as lane 25's is, and NOT the `-1` / `-1000` sentinel shape:
                                                                                                               // `Aniso_SmoothnessMaxT = 0` is a value the graph really uses (it is
                                                                                                               // what a caller that states nothing gets, and `rT = 1` follows from it),
                                                                                                               // so there is no state it could stand in for. See
                                                                                                               // `toon_colour_lane::goo_aniso_rough`.
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               // ---- STEP 13'S TWO LANES (`toon_colour_lane::goo_rs_scalars` / `goo_rs_tint`) ----
                                                                                                               //
                                                                                                               // `(0, 0, 0, 0)` IS THE POINT HERE, not a placeholder: `.x` of lane 27 is `Use RS_Eff?`, and a material
                                                                                                               // whose sidecar states no `_GooRSScalars` row must read `Use = 0` so the stage's gate leaves its colour
                                                                                                               // UNTOUCHED. The stage defaults to zero for exactly this reason - see the `float3 rs_final = lit;` /
                                                                                                               // `if (rs_use > 0.0f && ...)` pair in `source/shaders/toon/goo_toon.slang`, which performs NO floating-point
                                                                                                               // operation at all when `Use` is zero, so this value is what makes that identity BITWISE rather than
                                                                                                               // approximate.
                                                                                                               //
                                                                                                               // IT IS ONE OF THE `count` STATED ENTRIES RATHER THAN PART OF AN INITIALIZER TAIL, and
                                                                                                               // that is not tidiness either: what a value-initialised `deren::utility::vessel::array` tail holds depends on
                                                                                                               // GLM's `GLM_FORCE_CTOR_INIT`, so a table that stopped short of `count` would be leaving
                                                                                                               // the values of its last lanes to a build configuration. A stated zero cannot drift.
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               // Lane 28's `.w` is the mask's `SmoothStep.max`, so zero here also has to mean "unstated" rather than a
                                                                                                               // literal `max = 0`: the stage reads `.w <= 0` as `1.0`, which is the divide-by-zero guard the port
                                                                                                               // needs precisely because this neutral is `(0,0,0,0)` (see `toon_colour_lane::goo_rs_tint`, spec U7).
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               // ---- STEP 15'S ONE LANE (`toon_colour_lane::goo_rs_arm0`) ----
                                                                                                               //
                                                                                                               // `(0, 0, 0, 0)` IS THE REFERENCE'S OWN GROUP DEFAULT for the
                                                                                                               // `armA` sockets AND it is the arm's off switch: `.y` is `RS Strength`,
                                                                                                               // and `s023 = s036 * RS Strength` zeroes the whole arm when it is zero.
                                                                                                               // A material with no `_GooRSArm0` row therefore gets an `arm0` of zero
                                                                                                               // rather than a guess. See `toon_colour_lane::goo_rs_arm0`.
                                                                                                               glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                                                                                                               glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)};
        /**
         * THE AUTHOR'S TRANSPARENT VARIANT (`_TRANSPARENT_ON`), which its sidecar selects with the PAIR
         * `_SrcBlend 5` / `_DstBlend 10` - Unity's `SrcAlpha` / `OneMinusSrcAlpha`.
         *
         * WHY IT IS A FLAG AND NOT A BLEND STATE: the pipeline is per-PASS in this renderer (the pass binds one
         * pipeline and every leaf draws with it), so a per-material blend cannot be a pipeline property. What it
         * CAN be is a per-material DEPTH-WRITE plus a per-material OUTPUT ALPHA, because both of those are
         * already dynamic state and shader output: the pass turns standard alpha blending on for the whole
         * group - which for an alpha of 1 is bit-identical to the overwrite it replaces - and this flag says
         * which materials hand the shader a real coverage instead.
         *
         * FALSE BY DEFAULT, and that is the contract every other field here keeps: a material the sidecar says
         * nothing about is drawn exactly as it was.
         */
        bool alpha_blend = false;
    };
} // namespace deren::engine
