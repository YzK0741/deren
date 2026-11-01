#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <utility>

import deren.utility.shared;
import deren.vstd;
import deren.utility;
import deren.engine.runtime;
import deren.gltf_loader;
import deren.toon_material_sidecar;

#include "toon_material.hpp"

namespace deren::app::toon_material {
    namespace {
        // ---- THE ASSET'S OWN `extras` ROWS, WHICH THE LOADER CARRIES AND THE TOON LOOKUP CONSULTS FIRST ----
        //
        // THE THIRD DATA SOURCE, and the ordering between the three is the whole content of this helper. A toon
        // material's per-material facts can come from:
        //   1. the ASSET'S `extras` block (`deren::gltf::material::extras_floats`, read by the loader from the game's own
        //      exported table that ships inside the .glb) - the ORIGINAL;
        //   2. the `.toon.tsv` SIDECAR beside the model - this port's TRANSCRIPTION of a subset of the same tables;
        //   3. the family table in `source/shaders/toon/toon_params.slang` - the port's own fallback.
        // EXTRAS WINS, and the reason is what the two files ARE rather than which is newer: extras is written by the
        // game's own material pipeline and travels inside the asset, while the sidecar is a copy this repository made
        // from the game's JSON tables - a copy can be incomplete (it carries 109 of the 194 names) and can be stale,
        // and a copy must not outrank the thing it was copied from. The sidecar stays the fallback rather than being
        // dropped because it carries facts the extras do not (the sidecar's own slot vocabulary, and every model
        // whose .glb has no `extras` block at all), and the family table stays the last resort because some of its
        // numbers are the port's own (see its note).
        //
        // MEASURED, AND THE HONEST STATEMENT IS THAT THIS ORDER IS NOT SETTLED BY chen's FRAMES: on `chars\chen.glb`
        // the two sources never disagree - every name they share carries the same value in both - so no frame can
        // tell the two rules apart. What the frames DO settle is that the extras value reaches the GPU at all (see
        // `deren-ab/extras_dump.md` and the probe arms in `remaining_port_spec.md`'s "extras 数据源"), and the order
        // above is therefore a rule stated where it is applied and PROVABLE BY ASSET: a sidecar row that contradicts
        // extras is ignored, and the probe in that section is exactly such a row.
        [[nodiscard]] std::optional<float> extras_float_of(deren::gltf::scenes const* const scenes, std::string_view const material_name, std::string_view const row) {
            if (scenes == nullptr) {
                return std::nullopt;
            }
            deren::gltf::material const* const material = scenes->material_by_name(material_name);
            if (material == nullptr) {
                return std::nullopt;
            }
            auto const found = material->extras_floats.find(deren::utility::vessel::string(row));
            if (found == material->extras_floats.end()) {
                return std::nullopt; // this asset states no such row: the caller's next source answers
            }
            return found->second;
        }
        /// @brief the `_RD` image name a material's `RampIndex` selects, i.e. the reference's `RampSelect` resolved
        ///
        /// READ OUT OF `gooblender/nodes.json`, NOT GUESSED (spec §2.3/§2.4): the selector is
        /// `运算.004 = idx < 0.9900000095367432`, `运算.005 = idx < 2.0`, `运算.006 = idx > 0.9900000095367432`,
        /// `运算.007 = 运算.005 < 运算.006`, `运算.008 = idx > 2.990000009536743`, and three MIXes whose `f = 0`
        /// takes their **A** side. Its four outcomes, in the order the cascade is written below (the LAST comparison is
        /// the FIRST test, because it is the outermost MIX):
        ///
        ///     idx >  2.990000009536743  -> `图像纹理.006`  `TPLK_actor_common_cloth_03_RD`
        ///     idx >= 2.0                -> `图像纹理.005`  `T_actor_common_cloth_04_RD`
        ///     idx >= 0.9900000095367432 -> `图像纹理.001`  `T_actor_common_body_01_RD`
        ///     otherwise (negative too)  -> `图像纹理`      `T_actor_common_cloth_04_RD`
        ///
        /// THE THIRD AND FOURTH ARMS NAME THE SAME IMAGE, which is the spec's A8 rather than a shortcut: slots 1 and 3
        /// are byte-identical (`sha256 ff12009a...`) and were exported from two different characters' folders.
        /// THE FIRST TWO A'S ARE WHAT MAKES THE BOUNDARIES WHAT THEY ARE: at `idx` exactly `0.9900000095367432` the
        /// first comparison is FALSE and the MIX takes its A side, which is the BODY ramp - writing "`f` true takes
        /// cloth" flips that boundary, and it is the boundary every laevatain material sits one step away from.
        ///
        /// THE RETURN IS A `string_view` INTO THE FILE, so a material that states no `RampIndex` (every other family,
        /// and every material of every other character in this repository) gets an EMPTY view - which the lane resolver
        /// turns into `invalid`, i.e. "no base ramp", i.e. the old chain's diffuse.
        [[nodiscard]] std::string_view toon_base_ramp_name(deren::toon::material_sidecar const& material) {
            float const index = material.scalar("_GooRampIndex", 0.0f);
            if (index > 2.990000009536743f) {
                return std::string_view{"TPLK_actor_common_cloth_03_RD"};
            }
            if (index >= 2.0f) {
                return std::string_view{"T_actor_common_cloth_04_RD"};
            }
            if (index >= 0.9900000095367432f) {
                return std::string_view{"T_actor_common_body_01_RD"};
            }
            return std::string_view{"T_actor_common_cloth_04_RD"};
        }

        /// THE `RS_Index` ROW'S NAME, which is the sidecar's contract rather than this table's: it is written here
        /// because `toon_colour_row` (which carries the same name for lane 29) is declared BELOW this point, and the
        /// parse below has to be the same parse that lane gets - see `toon_rs_sheet_of`.
        static constexpr std::string_view toon_rs_arm0_row = "_GooRSArm0";

        /// THE REFERENCE'S SECOND `_RS` SHEET SLOT - the one `RS_Index = 1` selects - AND IT IS NOT A LANE, nor may it
        /// become one. `toon_lane` below is sized by `deren::engine::toon_slot::count`, so a second sheet slot AS A LANE
        /// would cost a vocabulary entry, another lane block (in the SAME heap slot:
        /// `source/backends/vulkan/core/core.declarations.cppm`'s `heap_slots::toon_lanes`) and another accessor beside
        /// the ones that exist (`source/shaders/common/heap_access.slang`'s `toon_lanes_at`/`toon_lanes2_at`/
        /// `toon_lanes3_at`/`toon_lanes4_at`, one `uint4` column each) - a fourth DESCRIPTOR SET is NOT the cost and was
        /// never the proposal (`goo_step15_lane_rs_index_spec.md` §3.5/§9.4: the rejected `Rb`).
        /// THAT PRICE WAS PAID IN STEP 17 FOR A LANE THAT CANNOT BE ANSWERED HERE (`_OutlineMask`, which a mesh stage
        /// samples per vertex), and it does not make this one cheaper: `RS_Index` is a per-material CONSTANT (§1.3 - no
        /// material link in either dump), so the choice is resolved here at registration time and the shader keeps
        /// reading ONE slot, exactly like `RampSelect`/`RampIndex` above and for the same reason.
        static constexpr std::string_view toon_rs_sheet_b = "_GooRSSheet1";

        /// @brief what the `armA` sheet lane should resolve: a sidecar slot NAME, and whether it is the second sheet
        struct toon_rs_sheet_choice {
            /// the name to look up in the model; empty means this material names no sheet at all
            std::string_view name = {};
            /// true when `RS_Index` selected `_GooRSSheet1` AND that sheet's own `_UseGooRSSheet1` switch is on
            bool second = false;
        };

        /// @brief which of the reference's two `_RS` sheets @p material reads, given its first sheet's name @p sheet_a
        ///
        /// THE RULE IS A CHOICE AND NOT A REPRODUCTION, and this paragraph is the whole reason the route is named the
        /// way it is: the reference MIXES its two sheets CONTINUOUSLY - `mix(A, B, clamp(RS_Index, 0, 1))` in
        /// `混合.032` (spec §1.1) - while this answers one of two names. An `RS_Index` strictly between the endpoints
        /// is therefore QUANTISED TO AN ENDPOINT and is NOT the reference's blend. That is a named limitation of this
        /// route (§9.3 (z)(1)) rather than an oversight, and it must not be written up as a faithful implementation.
        ///
        /// WHY `0.5`, AND WHY THE TIE GOES TO THE SECOND SHEET: the two dumps carry only the two endpoints (§1.3).
        /// The count is 27 `PBRToonBase`-group instances in total, NOT 23: the `gooblender` dump has 23 (22 at `0.0`
        /// plus the single `1.0`, `M_actor_yvonne_cloth_03`, which is in no captured asset) and `chen_dump` adds 4
        /// more under `PBRToonBase.001`, ALL of them `0.0`. The union of those values is still `{0.0, 1.0}`, so ANY
        /// threshold in `(0, 1)` agrees exactly; `0.5` is the reference's own 50/50 blend, i.e. the one value at which
        /// neither sheet is closer than the other; and `>=` rather than `>` fixes the tie's direction here instead of
        /// leaving it to an operator accident (§9.2).
        ///
        /// THE SECOND SHEET NEEDS ITS OWN SWITCH AS WELL AS THE LANE'S: `_UseGooRSSheet` was already asked by the
        /// caller before this runs (see the flag gate at the top of `toon_texture`), and `_UseGooRSSheet1` is asked
        /// here - absent means off, the sidecar module's contract for every optional slot. So a material that names
        /// `_GooRSSheet1` without switching it on reads the FIRST sheet, and so does one that switches it on without
        /// naming anything: `name` carries the answer and an empty `name` means the first sheet.
        [[nodiscard]] toon_rs_sheet_choice toon_rs_sheet_of(deren::toon::material_sidecar const& material, std::string_view const sheet_a) {
            toon_rs_sheet_choice choice = {};
            choice.name = sheet_a;
            // `RS_Index` IS THE `.x` OF A `color` ROW, so it sits in `others` VERBATIM and `scalar()` cannot see it
            // (the module keeps a `color` row as text on purpose). THE PARSE IS LANE 29's OWN RULE - comma separated
            // components read with `from_chars` - but this function answers only the FIRST component (`.x`, the
            // `substr` below), because the row's other components (`RS ColorTint`) are a different question. A missing
            // or unparseable component keeps the NEUTRAL, which for this lane is 0.0, i.e. the first sheet. Reading the
            // same row with a different rule would let this choice and the lane-29 colour lane disagree about one
            // material.
            // `from_chars` ALSO ACCEPTS `inf` AND `nan`: `nan` fails the `>= 0.5` test and keeps the first sheet, while
            // `inf` WOULD select the second - neither is reachable from the reference data, whose two dumps carry only
            // `0.0` and `1.0` (§1.3), so both are pinned here rather than measured away.
            float rs_index = 0.0f;
            if (auto const row = material.others.find(deren::utility::vessel::string(toon_rs_arm0_row)); row != material.others.end()) {
                std::string_view const row_text = row->second;
                std::string_view const first_component = row_text.substr(0, row_text.find(','));
                float value = 0.0f;
                if (auto const result = std::from_chars(first_component.data(), first_component.data() + first_component.size(), value);
                    result.ec == std::errc{}) {
                    rs_index = value;
                }
            }
            // WRITTEN AS A NEGATED COMPARISON because that is also the NaN-safe direction: an `RS_Index` that is
            // somehow not a number (a row this parser cannot produce, but a `nan` row it CAN) is NOT `>= 0.5`, so it
            // answers the first sheet rather than silently selecting the second one.
            if (!(rs_index >= 0.5f)) {
                return choice;
            }
            // THE SECOND SWITCH, asked by the flag's own name: `enabled("_GooRSSheet1")` is `_UseGooRSSheet1`, absent
            // meaning off (see the sidecar module's second rule).
            if (!material.enabled(toon_rs_sheet_b)) {
                return choice;
            }
            std::string_view const sheet_b = material.slot(toon_rs_sheet_b);
            if (sheet_b.empty()) {
                return choice;
            }
            choice.name = sheet_b;
            choice.second = true;
            return choice;
        }
        // THE ASSET PIPELINE'S VOCABULARY FOR THE FOUR TOON LANES, and the only place it appears: one entry per
        // `deren::engine::toon_slot`, in lane order, carrying BOTH names the pipeline uses for it.
        //
        // TWO NAMES RATHER THAN ONE, and the second is not derivable from the first: the ramp and LUT lanes are
        // switched on by `_Use<Slot>`, but the MATCAP lane `_MatcapTex` is switched on by `_UseMatcap` - the slot's
        // `Tex` suffix is not in the flag, which is a fact about the pipeline rather than a rule. Deriving the flag
        // from the slot answered "off" for every matcap in the file, silently, which is the failure the sidecar
        // module exists to prevent and precisely the one its own convention could not see: it is right for three
        // lanes and wrong for the fourth.
        //
        // IT SITS HERE, ABOVE THE DIAGNOSTIC, because the diagnostic prints every slot in the file and has to answer
        // the same question the lookup does. It did not, once: the lookup was taught the real flag names while the log
        // went on calling the convention, and the result was a log line reading `| off` about a lane the renderer was
        // reading - a diagnostic that contradicts the renderer is worse than no diagnostic, so both now ask
        // `toon_flag_for`.
        struct toon_lane_names {
            std::string_view slot;
            std::string_view flag;
        };
        static constexpr deren::utility::vessel::array<toon_lane_names, static_cast<std::size_t>(deren::engine::toon_slot::count)> toon_lane = {{
            {"_DiffRampMap", "_UseDiffRampMap"},
            {"_ShadowLutTex", "_UseShadowLutTex"},
            {"_SpecRampMap", "_UseSpecRampMap"},
            {"_MatcapTex", "_UseMatcap"},
            {"_SDFLightmap", "_UseSDFLightmap"},
            // THE METALLIC/GLOSS LANE FOLLOWS THE `_Use<Slot>` CONVENTION, unlike the matcap above: `_UseMetallicGlossMap`
            // is what the material files carry (24 of the 45 materials across the five characters export one).
            {"_MetallicGlossMap", "_UseMetallicGlossMap"},
            // THE FACE MASK IS SWITCHED ON BY THE SDF'S OWN FLAG, which is a fact about the material files rather than a
            // convention: there is no `_UseSDFMask` in any of them (the census over the five characters has
            // `_UseSDFLightmap` on 10 materials and `_UseSDFMask` on none), and the mask is meaningless without the SDF
            // it belongs to - the two are authored for one face and read at one coordinate.
            {"_SDFMask", "_UseSDFLightmap"},
            // THE EMOTION ATLAS HAS A FLAG OF ITS OWN (`_UseEmotionMap`, on the face and on nothing else in chen).
            {"_EmotionMap", "_UseEmotionMap"},
            // THE SPLIT NORMAL'S FLAG IS `_UseSpecBumpMap`, not `_UseSplitNormalMap` - the second slot in this table
            // whose flag does not follow the convention (the matcap is the first), and found the same way: by asking
            // the files rather than by deriving the name.
            {"_SplitNormalMap", "_UseSpecBumpMap"},
            // THE GOO IRIS BALL'S FLAG FOLLOWS THE CONVENTION (`_UseGooMatcap05`), which is the only kind of name it
            // could have: the reference has no per-material switch for this texture at all (it is sampled from inside
            // the group), so the flag is the port's own sidecar vocabulary rather than a row copied from an asset.
            // Keeping it in the `_Use<Slot>` shape is what lets the diagnostic above walk it like every other lane.
            {"_GooMatcap05", "_UseGooMatcap05"},
            // THE GOO BASE RAMP'S FLAG FOLLOWS THE CONVENTION (`_UseGooBaseRamp`), for the matcap lane's own reason:
            // the reference resolves this image INSIDE its `RampSelect` group and no material states a per-material
            // switch for it, so the flag is this port's sidecar vocabulary rather than a row copied from an asset -
            // and the `_Use<Slot>` shape is what lets the diagnostic above walk it like every other lane. The
            // reference's own switch is `RampIndex`, which is not a flag but a SELECTOR, and it is resolved here (see
            // `toon_base_ramp_name` below): the four slots hold two distinct images and every material this asset
            // gives the group to states `0.0` or `1.0`, which is the spec's §2.6 + its A8.
            {"_GooBaseRamp", "_UseGooBaseRamp"},
            // ---- STEP 7: THE FACE CONTAINER'S THREE MASKS, ALL `_Use<Slot>` ----
            //
            // THE CONVENTION HOLDS FOR ALL THREE, and that is a fact about this port's vocabulary rather than about an
            // asset: the reference samples every one of them from INSIDE `PBRToonBaseFace` (they are not material
            // inputs at all), so there is no game-side `_Use...` row to copy and the flag is this sidecar's own switch
            // - exactly as `_GooMatcap05` and `_GooBaseRamp` above are. Keeping the shape is what lets the diagnostic
            // walk them like every other lane.
            {"_GooFaceSDF", "_UseGooFaceSDF"},
            {"_GooFaceCmM", "_UseGooFaceCmM"},
            {"_GooFaceCsutm", "_UseGooFaceCsutm"},
            // ---- STEP 13: THE `RS EFF` MASK ----
            //
            // THE CONVENTION HOLDS, for the matcap-lane reason rather than an asset's: the reference samples `_M`
            // INSIDE its `Arknights: Endfield_PBRToonBase` group - it is a `组输入` socket, not a `_`-prefixed game
            // property - so there is no game-side `_Use...` row to copy and `_UseGooRSMask` is this sidecar's own
            // switch. `_M` is the LEFT input of `混合.038 = _M ⊙ RS ColorTint` (mechanism table #14); its texel is a
            // Non-Color mask (uploaded UNORM) and the smoothstep that turns it into a factor is the shader's, because
            // its `max` is per material and rides `_GooRSTint.w`. See `toon_slot::goo_rs_mask`.
            //
            // IT MUST BE IN THIS TABLE RATHER THAN ONLY IN THE SIDECAR: the lookup below resolves a lane's name here
            // first and returns the WHITE fallback before the flag is even asked, so a lane missing from this table
            // reads `slot_name` as empty, resolves no texture, and the branch is silently off - no warning, no
            // compile error, only a frame that does not move.
            {"_GooRSMask", "_UseGooRSMask"},
            // `_GooRSSheet` IS `armA`'s 256x1 `_RS` COLOUR SHEET, THE SIBLING OF THE MASK ABOVE AND THE OTHER HALF OF
            // MECHANISM TABLE #14's FIRST ARM. It is a texture lane rather than a colour lane because it is an image,
            // it is uploaded SRGB because it is a colour (the sampler decodes it, the shader does not), and it is
            // `_GooRSSheet`/`_UseGooRSSheet` rather than a game property name for the mask's own reason: the reference
            // reads it through a `组输入` socket, so there is no `_Use...` row to copy. A material with no row here and
            // `RS Model = 0` gets `arm0 = 0` - the branch must NOT fall back to the white texture at index 0. See
            // `toon_slot::goo_rs_sheet` and the `armA` block in `source/shaders/toon/goo_toon.slang`.
            //
            // AND IT IS ONE OF THE REFERENCE'S TWO SHEETS, WHICH IS WHY THERE IS NO SECOND ENTRY HERE: `RS_Index`
            // (`_GooRSArm0`'s `.x`, a `color` row) chooses between this sheet and the second one, and the host
            // resolves that choice BY NAME in `toon_texture` (`toon_rs_sheet_of` above) instead of growing this
            // table - a second texture lane would need `toon_slot::count` to move and another lane block (the cost
            // step 17 paid for `_OutlineMask`, which has no host answer at all), while THIS choice is a per-material
            // constant with one slot to name. The shader therefore still reads ONE slot and still never looks at
            // `rs_arm0_lane.x`: the second sheet is a MATERIAL's choice, not a frame's, which is the whole reason it
            // can be answered here. See `goo_step15_lane_rs_index_spec.md` §9 and `toon_rs_sheet_b`.
            {"_GooRSSheet", "_UseGooRSSheet"},
            // ---- STEP 17: THE INVERTED-HULL OUTLINE'S WIDTH/DEPTH MASK ----
            //
            // `_OutlineMask` IS A GAME PROPERTY NAME AND ITS FLAG IS `_EnableOutlineMask`, which is the one entry in this
            // table whose flag is not `_Use<Slot>` for an ASSET reason rather than a port reason: the captured viewer
            // emits its `#ifdef Q326` from `_EnableOutlineMask === 1 && _OutlineMask`, and the staged reference asset
            // states the flag as a `float` row (`1.0` on face/hair, `0.0` on body/cloth) beside the `slot` row. A
            // material with the flag 0 therefore names no readable mask here even when the file carries a texture row,
            // which is exactly the viewer's condition - and the lane stays 0, i.e. the Q326-absent uniform hull.
            //
            // IT IS THE LANE THAT RAISED `toon_lane_blocks` TO 4 and added `toon_lanes4_at`; it cannot be answered on
            // the host because the mask modulates a PER-VERTEX hull quantity inside the outline's geometry stage.
            {"_OutlineMask", "_EnableOutlineMask"},
        }};
        // THE MATERIAL COLOUR VOCABULARY, one `color` row name per `deren::engine::toon_colour_lane`, in lane order - the
        // same arrangement the texture table above uses and for the same reason: the asset pipeline's spelling belongs
        // where the sidecar is read, and the ORDER is the contract with the shader's lane indices.
        static constexpr deren::utility::vessel::array<std::string_view, static_cast<std::size_t>(deren::engine::toon_colour_lane::count)> toon_colour_row = {{
            "_SDFRimColor",
            "_EyeHighLightColor",
            "_EyeScatteringColor",
            // THE OUTLINE LANE IS THE ONE LANE WHOSE ROW CARRIES ONLY PART OF THE VALUE: `.rgb` is this row
            // (`color _OutlineTintColor`, the game's name for the author's `_OutlineColor`), and `.w` is the SEPARATE
            // `float _OutlineWidth`, read as a scalar below - see `toon_colour_lane::outline_edge`.
            "_OutlineTintColor",
            // THE SPECULAR LANE'S ROW IS A `float` AND NOT A `color`, which is why this table's name is used only as
            // the SIDECAR FALLBACK for it: the value the port prefers is the ASSET'S (`extras_float_of`), and this
            // row is consulted second - see `toon_colour_lane::specular_strength` and `extras_float_of`.
            "_Specular",
            // THE PARALLAX DEPTH'S ROW IS A `float` TOO, AND ON THIS ASSET THE SIDECAR CARRIES IT NOWHERE:
            // `chars\chen.glb.toon.tsv` has no `_ParallaxScale` row at all, so the asset's `extras` block is the only
            // source that speaks for it here and the stage's own constant is the answer for every material it does
            // not speak for. The name still belongs in this table for the other direction: a model whose sidecar
            // holds the row is a model this lane can answer for without any `extras` block at all.
            //
            // WHAT "EVERY MATERIAL IT DOES NOT SPEAK FOR" TURNED OUT TO MEAN, MEASURED RATHER THAN EXPECTED: on chen
            // the asset states the row for three materials (iris 0.03, brow 0.5, cloth_01 0.5) and the ONE consumer -
            // the stage's parallax block - is gated on the material's matcap, which only the iris has. So the lane is
            // routed correctly and this character's frame does not move by a pixel; the probe that proves the lane is
            // live (the iris re-stated as 0.3) is in `remaining_port_spec.md`'s "extras 数据源" section.
            "_ParallaxScale",
            // THE GOO IRIS BRIGHTNESS LANE'S ROW IS A `color` AND NOT TWO `float`s, which is the shape the lane
            // forces rather than a choice: one lane is one `vec4` and the reference states its two numbers as two
            // SIBLING sockets of one group (`Eyes brightness` / `Eyes HightLight brightness`), read at the same two
            // lines of it. The row's name is the port's own - the reference's sockets are neither `_`-prefixed game
            // properties nor sidecar rows - and `.x` / `.y` are the two sockets IN THE REFERENCE'S OWN ORDER.
            "_GooEyeBrightness",
            // THE REWRITTEN CHAIN'S RIM LANES, and BOTH rows are `color` - which is the shape the reference forces
            // rather than a choice: `Rim_Color` is an RGBA socket, and `Rim_ColorStrength` / `Rim_DirLightAtten` /
            // `ToonfresnelPow` / `Use Rimlimitation?` are FOUR SIBLING sockets of the same group instance, read at
            // one composition, so one `vec4` row carries them in the reference's own order. `.w` is a BOOLEAN's
            // 0/1, which a `color` row states as a fourth float without loss.
            //
            // THE NAMES ARE THE PORT'S OWN, on `_GooEyeBrightness`'s terms: the reference's sockets are neither
            // `_`-prefixed game properties nor sidecar rows. They are spelled with the reference's own capitalisation
            // of `Rim` so a reader can find the socket they came from.
            "_GooRimColour",
            "_GooRimScalars",
            // THE SCREEN-SPACE RIM'S TWO WIDTHS, and they are THEIR OWN ROW rather than two more floats on
            // `_GooRimScalars` because they belong to the OTHER group the same material instantiates: the container
            // reads `Rim_ColorStrength` / `Rim_DirLightAtten` from its own interface, while `Rim_width_X` /
            // `Rim_width_Y` are `DepthRim`'s two `组输入` sockets, reached through `群组.016`. `.z` / `.w` are
            // reserved. See `toon_colour_lane::goo_rim_widths`.
            "_GooRimWidths",
            // ---- STEP 4: THE BASE / SKIN / CLOTH DIRECT-DIFFUSE REPLACEMENT'S SIX LANES ----
            //
            // ITS OWN `BaseColor` FIRST, and the row name is the REFERENCE'S OWN - `组输入.BaseColor` of
            // `Arknights: Endfield_PBRToonBase`. It is a `color` row in the material tree (`[1.1628, 0.9888, 1.0280]`
            // on `body_01`), so `.x` / `.y` / `.z` are the tint and nothing else is read; the reason the port needs it
            // at all is that its own albedo carries no `baseColorFactor` (see `toon_colour_lane::goo_base_colour`).
            "_GooBaseColour",
            // THE FOUR `SigmoidSharp` ARGUMENTS, in the order the reference's two call sites state them, and the
            // ordering is the lane's contract rather than a preference: `.x` / `.y` are the half-Lambert curve's
            // `center` / `sharp` and `.z` / `.w` the cast-shadow curve's.
            "_GooDiffuseA",
            // THE GATE'S LOWER EDGE AND ITS TWO NEIGHBOURS ON THE SAME MATERIAL ROW. `.y` HAS NO CONSUMER THIS STEP
            // and the row is still named here, because this table's job is to be the ONE place the asset pipeline's
            // spelling lives - see `toon_colour_lane::goo_diffuse_b` for why the value is recorded rather than applied.
            "_GooDiffuseB",
            // THE TOONFRESNEL PAIR: each row is a `color` whose `.rgb` is one side of the reference's `混合.006` MIX and
            // whose `.w` is one end of the window its factor is a `smoothstep` of. `ToonfresnelPow` - the exponent
            // between them - is NOT here: it rides `_GooRimScalars.z` already, and one socket must have one carrier.
            "_GooFresnelInside",
            "_GooFresnelOutside",
            // AND THE DIRECT-OCCLUSION COLOUR, whose neutral is BLACK rather than white because black is what the
            // reference's `lerp(black, white, AO)` starts from (see `toon_colour_lane::goo_direct_occlusion`).
            "_GooDirectOcclusion",
            // ---- STEP 5: THE REFERENCE'S DIRECT-SPECULAR / IBL-DIFFUSE / IBL-SPECULAR TERMS' FOUR LANES ----
            //
            // THE FOUR PER-MATERIAL NUMBERS THOSE TERMS CONSUME AND THE PORT COULD NOT CARRY, each one the
            // REFERENCE'S OWN socket name (spec `goo_step5_specular_spec.md` §5.2/§7.3):
            //
            //   * `specularFGD Strength` - the IBL specular's strength (`0.8` on `body_01`/`body_02`, the group's
            //     `1.0` on the cloth, and NO SOCKET AT ALL on the Face and Hair containers);
            //   * `dirLight_lightColor` - THE LIGHT COLOUR OF THE DIRECT TERMS, which the spec's §5.4 makes
            //     authoritative over Goo's `Shader Info` and which closes step 4's own recorded substitution (its
            //     result document §4 item 2). It is a `color` row with a 4.2% blue/green pull on every material;
            //   * `AmbientLightColorTint` - what the IBL diffuse multiplies the engine's probe irradiance by
            //     (`[1.5121498107910156] x3` on the body, white on the cloth).
            //
            // ... AND A FOURTH, `SpecularColor`, whose absence would leave the direct specular 4.2x too dark on
            // `body_01`/`body_02` (spec §7.3 item 5). It is NOT `BaseColor` reused: the two are independent sockets of
            // the same group instance and disagree by 3.6x on the body, and `cloth_02`'s specular colour is darker than
            // its base colour while the body's is brighter - see `toon_colour_lane::goo_specular_color`.
            //
            // THE ROW NAMES ARE THE REFERENCE'S SOCKET NAMES, which is why they have no leading underscore: every other
            // entry above is a GAME property (`_BaseColor`, `_Specular`) or a name this port minted for a reference
            // socket it had to name itself (`_GooRimColour`). These four are socket names the reference's own group
            // carries verbatim, so a reader can grep `nodes.json` for them - and `_GooBaseColour` above set that
            // precedent for `BaseColor`'s row.
            "_GooSpecularFGD",
            "_GooLightColor",
            "_GooAmbientTint",
            "_GooSpecularColor",
            // ---- STEP 7: THE FACE CONTAINER'S FOUR LANES ----
            //
            // THE ROW NAMES ARE THE PORT'S OWN (the reference's sockets are neither `_`-prefixed game properties nor
            // sidecar rows), on `_GooEyeBrightness`'s terms - and the two scalar rows carry FOUR SIBLING SOCKETS each,
            // packed in the reference's own order, which is the shape one `vec4` forces and the same packing
            // `_GooDiffuseA` / `_GooDiffuseB` already use:
            //
            //   * `_GooFaceScalarsA` = `chin_RemaphalfLambert_center`[x] / `_sharp`[y] / `sphereNormal_Strength`[z] /
            //     `SmoothnessMax`[w] - the chin `SigmoidSharp` pair, the sphere-normal blend and the roughness source;
            //   * `_GooFaceScalarsB` = `Face Final brightness`[x] / `Eyes white Final brightness`[y] / `Front R
            //     Pow`[z] / `Front R Smo`[w] - the emission-brightness switch's two arms and `Front transparent
            //     red`'s shape;
            //   * `_GooFaceNoseShadow` = `nose_shadow_Color` and `_GooFaceFrontR` = `Front R Color`, both `color` rows
            //     whose NEUTRAL IS BLACK because black is the socket's own `interface[]` default (see the lane's note
            //     in `deren::engine::toon_colour_lane`).
            //
            // THE AUDIT THAT KEPT THIS AT FOUR ROWS: eight more of the container's numbers already have carriers,
            // filled from the SAME SOCKET NAMES by the same materials - `BaseColor` on `_GooBaseColour`, the two
            // `SigmoidSharp` pairs and `MetallicMax` on `_GooDiffuseA` / `_GooDiffuseB`, `dirLight_lightColor` /
            // `AmbientLightColorTint` / `SpecularColor` on step 5's three, `Rim_Color` on `_GooRimColour`, and
            // `Color desaturation in shaded areas attenuation` on `_GooDiffuseB.y` (which step 6 taught to
            // desaturate). Minting rows for those would give one socket two carriers.
            "_GooFaceScalarsA",
            "_GooFaceScalarsB",
            "_GooFaceNoseShadow",
            "_GooFaceFrontR",
            // ---- STEP 8: THE NORMAL DECODE'S STRENGTH ----
            //
            // ONE ROW, ONE SCALAR, and it is the reference's `DecodeNormal :: 组输入.NormalStrength`: the weight between
            // the FLAT tangent normal and the one the map's `xy` decodes to. It is a `float` row rather than a `color`
            // one (the port's rule for a lane whose payload is `.x` alone), so it does NOT go through the generic
            // `others` path below - its `float` kind lands in `material_sidecar::scalars`, which only `scalar()` reads,
            // and that is why `toon_colour` has a branch of its own for it, the way `_Specular` and `_ParallaxScale` do.
            //
            // THE VALUES ARE THE REFERENCE'S, NOT THE GLTF'S: `normalTexture.scale` is `1.0` on all 11 materials of
            // `chars\laevatain_goo.glb`, so it cannot be the source (the `_Specular` lane's `extras` precedence exists
            // for a property the asset really states; this one it does not).
            "_GooNormalStrength",
            // ---- STEP 10: THE ANISOTROPY GATE ----
            //
            // ONE ROW FOR THREE SIBLING SOCKETS OF `PBRToonBase`, in the reference's own order, which is the shape
            // `_GooRimScalars` and `_GooEyeBrightness` already use and the shape one `vec4` forces: `.x = Use
            // anisotropy?`, `.y = Anisotropic mask`, `.z = Use Toonaniso?`, `.w` reserved. `混合.016` (the direct
            // specular's arm selector) reads the first, `混合.017` multiplies by the second, and `混合.020` - the
            // anisotropic lobe - switches on the third, which step 12 implemented (the lobe's own roughnesses ride
            // the lane below); the mask is `0.0` on every material of this asset, so none of it is reachable in a
            // frame this repository renders.
            //
            // IT IS A `color` ROW AND NOT A `float` ONE, so it needs NO branch in `toon_colour`: unlike step 8's
            // `_GooNormalStrength` (whose `float` kind lands in `material_sidecar::scalars` and is therefore invisible to
            // the generic `others` path below), this row parses through that path like every other colour lane, and its
            // four comma-separated components arrive component by component.
            "_GooAnisoGate",
            // ---- STEP 12: THE ANISOTROPIC LOBE'S TWO ROUGHNESSES ----
            //
            // ONE `color` ROW FOR THE TWO SIBLING SOCKETS `roughnessT` / `roughnessB` ARE BUILT FROM, spelled
            // `Aniso_SmoothnessMaxT` / `Aniso_SmoothnessMaxB` in the reference's own graph (`.x` / `.y`, in that
            // order; `.z` / `.w` reserved). The port's lobe computes `rT = (1 - lane.x)^2`, `rB = (1 - lane.y)^2`,
            // which is the two `Power` nodes the snapshot calls `roughnessT` / `roughnessB`.
            //
            // IT IS THE SAME KIND OF ROW AS `_GooAnisoGate` ABOVE AND TAKES THE SAME PATH, so it needs no branch in
            // `toon_colour`: a `color` row lands in `material_sidecar::others`, which is what the generic tail of that
            // lambda reads, and the four comma-separated components arrive one at a time. Its neutral is `(0,0,0,0)`
            // - the reference's own group default rather than a sentinel - see `toon_colour_lane::goo_aniso_rough`.
            "_GooAnisoRough",
            // ---- STEP 13: THE TWO `RS EFF` LANES ----
            //
            // `_GooRSScalars` is `.x Use RS_Eff?` / `.y RS Multiply Value` / `.z RS Model` and `_GooRSTint` is
            // `.rgb RS ColorTint` / `.w SmoothStep.max` (mechanism table #14). BOTH ARE `color` ROWS AND BOTH TAKE THE
            // GENERIC TAIL OF `toon_colour`, so this table is the whole host-side change: they land in
            // `material_sidecar::others` and their four comma-separated components are parsed one at a time, exactly
            // as `_GooAnisoGate`'s and `_GooAnisoRough`'s are - no branch, no sentinel, no new sidecar grammar.
            //
            // THEY ARE IN THIS TABLE BECAUSE THE TABLE *IS* THE BINDING: the lambda derives the row name from the LANE
            // and the lane from the enum, so an enum lane with no name here makes every `_GooRSScalars` /
            // `_GooRSTint` row in every sidecar unreachable - the lookup finds nothing, falls back to
            // `toon_colour_neutral` (zero), and the RS branch is off with no warning and no compile error.
            //
            // THE NEUTRAL OF BOTH IS `(0,0,0,0)`, which is a value and not a placeholder: `.x` of lane 27 is the
            // branch's switch, and zero there is what makes the stage's identity BITWISE (see the `float3
            // rs_final = lit;` / `if (rs_use > 0.0f && ...)` pair in `source/shaders/toon/goo_toon.slang`, which does no
            // floating-point work at all when the gate is shut).
            "_GooRSScalars",
            "_GooRSTint",
            // `_GooRSArm0` IS THE SAME MECHANISM'S OTHER ARM, and it is a COLOUR row rather than four `float` rows for
            // the reason `_GooRSScalars` is one: it is four numbers (`RS_Index`, `RS Strength`, `Layer weight Value`,
            // `Layer weight Value Offset`) off four `组输入` sockets of the same group, and the sidecar vocabulary has a
            // four-component form for exactly that. Its neutral is `(0,0,0,0)`, which is ALSO the arm's off switch
            // (`RS Strength = 0` zeroes the product), so a material that states no row gets an `armA` that changes
            // nothing - but note it is not the same as `Use RS_Eff? = 0`: a zeroed `armA` still passes through the
            // LIGHTEN below. See `toon_colour_lane::goo_rs_arm0` and the `armA` block in `source/shaders/toon/goo_toon.slang`.
            "_GooRSArm0",
            "_GameToon0",
            "_GameToon1",
            "_GameToon2",
            "_GameToon3",
            "_GameToon4",
            "_GameToon5",
            "_GameToon6",
            "_GameToon7",
            "_GameToon8",
            "_GameToon9",
            "_GameToon10",
            "_GameToon11",
            "_GameToon12",
            "_GameToon13",
            "_GameToon14",
            "_GameToon15",

        }};
        // The declared flag for a toon lane; the `_Use<Slot>` convention for every OTHER slot, which the diagnostic
        // needs because it walks the whole file (`_BaseMap`, `_BumpMap`, the outline and SDF masks and the rest).
        auto const toon_flag_for = [](std::string_view const slot_name) -> deren::utility::vessel::string {
            for (toon_lane_names const& lane : toon_lane) {
                if (lane.slot == slot_name) {
                    return deren::utility::vessel::string(lane.flag);
                }
            }
            deren::utility::vessel::string flag{deren::toon::enable_flag_prefix};
            flag.append(slot_name.starts_with('_') ? slot_name.substr(1) : slot_name);
            return flag;
        };

        // ---- THE PROCEDURAL RAMP, which is what replaces the game's own ramps on the read path ----
        //
        // WHY IT IS BAKED RATHER THAN SAMPLED FROM THE MODEL: the ramps a character ships with are the GAME's
        // textures, and `ASSET_LICENSE_BOUNDARY_CN.md` excludes those from redistribution - so a read path that
        // depends on them is a read path this repository cannot carry. This bakes an equivalent SHAPE instead: a flat
        // shadow side, a step, a flat lit side, which is what a toon ramp IS (see character_forward.slang's note on
        // why a ramp replaces the procedural threshold rather than layering with it).
        //
        // ONE ASSET SERVES BOTH LANES, the diffuse ramp and the specular ramp. The reference keeps them in two V
        // bands of one atlas for exactly this reason - they are the same step read with two different coordinates,
        // one from the shadow-gated half-Lambert and one from the half-vector angle - and a neutral step is the
        // shape both of them need. Two files would be two things to keep in step for no gain.
        //
        // THE BAKE IS A NEUTRAL UNIT STEP, NOT ANY ONE FAMILY'S RAMP, and that is what lets a SINGLE redistributable
        // asset serve every family without copying the family table into this file. The texture holds the SHAPE -
        // zero through the shadow side, one through the lit side, a step at x = 0.5 - and the SHADER maps the
        // family's own threshold and edge width onto that step when it builds the coordinate it reads at (see
        // `character_ramp_half_width` in character_forward.slang). The family's numbers therefore stay in ONE place,
        // in the shader, where the procedural branch and the rim stage already read them; what this file supplies is
        // the shape, and what the shader supplies is where along it this material's terminator sits.
        //
        // THAT IS NOT A WORKAROUND FOR THE TABLE BEING IN THE SHADER, it is the structure the reference uses: its
        // ramp atlas is shared across materials and each material's own `_ShadowCenter` / `_ShadowSmoothness` decide
        // the UV it is read at. A per-family BAKE would be the copy - and it would be a worse one, because the
        // family's edge width would be frozen into pixels at load time instead of staying a number the shader can
        // evaluate the procedural branch with.
        //
        // `baked_ramp_half_width` IS HALF OF A SHARED CONTRACT; the other half is the shader's
        // `character_ramp_half_width`. The shader's remap inverts this bake exactly when the two agree, which is
        // what makes the texture branch and the procedural branch produce the same tint for the same family rather
        // than merely similar ones. `source/tests/test_toon_material_sidecar.cpp` reads both files and fails on drift -
        // and it is there rather than in a test of its own because the contract is a sidecar-lane contract.
        constexpr uint32_t baked_ramp_width = 256;
        constexpr uint32_t baked_ramp_height = 8;
        constexpr float baked_ramp_half_width = 0.035f;
        static auto const bake_unit_ramp = []() {
            // `smoothstep(0.5 - w, 0.5 + w, x)`: the same Hermite step the shader's procedural branch builds, with
            // the step moved to the ramp's centre and its width normalised so the remap can undo it.
            auto const srgb_encode = [](float const linear) {
                return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            };
            auto const smoothstep = [](float const edge0, float const edge1, float const x) {
                float const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            deren::utility::vessel::vector<uint8_t> pixels(static_cast<std::size_t>(baked_ramp_width) * baked_ramp_height * 4u, 255u);
            for (uint32_t v = 0; v < baked_ramp_height; ++v) {
                for (uint32_t u = 0; u < baked_ramp_width; ++u) {
                    float const x = static_cast<float>(u) / static_cast<float>(baked_ramp_width - 1u);
                    float const step = smoothstep(0.5f - baked_ramp_half_width, 0.5f + baked_ramp_half_width, x);
                    uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(v) * baked_ramp_width + u) * 4u;
                    // GREY, and the SHADER READS `.r`: the ramp carries the step and nothing else, so the family's
                    // own `shadow_tint` is what colours the dark side rather than a bake that could only ever hold
                    // one family's tint. Writing the same value into R, G and B keeps the asset readable as a ramp
                    // by eye.
                    //
                    // THE UPLOAD IS SRGB (see register_material's slot table), so the texel holds the ENCODED value:
                    // what the sampler hands the shader is then the LINEAR step, which is the number the procedural
                    // branch's `smoothstep` produced. Encoding is what makes the two branches agree on the VALUE and
                    // not just on the shape.
                    uint8_t const encoded = static_cast<uint8_t>(std::clamp(srgb_encode(step), 0.0f, 1.0f) * 255.0f + 0.5f);
                    texel[0] = encoded;
                    texel[1] = encoded;
                    texel[2] = encoded;
                    texel[3] = 255u;
                }
            }
            return pixels;
        };

        // ---- THE DIFFUSE RAMP'S NEUTRAL CONTENT, which is a DIFFERENT SHAPE from the unit step above ----
        //
        // THE TWO LANES READ THEIR RAMP DIFFERENTLY, so one bake can no longer serve both. The specular lane reads
        // `.r` as a THRESHOLD, and the unit step above is exactly that. The diffuse lane reads RGB as a COLOUR mapping
        // and A as a LIGHTNESS (see the shading stage's `toon_diffuse`), and a grey step is wrong in BOTH channels for
        // that use: its alpha is a constant 1, which claims every fragment is fully lit, and its RGB would tint the
        // lit side as though the material's ramp had said so.
        //
        // SO THIS BAKE HOLDS THE ONLY CONTENT THAT MEANS "NO COLOUR MAPPING, AND AS LIT AS THE LIGHT TERM SAYS": WHITE
        // in RGB and the identity lightness in A. A material whose model ships no ramp is then shaded by its own light
        // term rather than by an invented band, which is the same neutrality the other bakes keep - and the real map
        // takes precedence whenever the model has one (`_DiffRampMap` is on every character material in this
        // repository, so the fallback is for fixtures and for models exported without it).
        constexpr uint32_t baked_neutral_ramp_width = 256;
        constexpr uint32_t baked_neutral_ramp_height = 8;
        static auto const bake_neutral_ramp = []() {
            auto const srgb_encode = [](float const linear) {
                return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            };
            // WHITE RGB, filled in here and never overwritten: the lane is uploaded as sRGB, so 255 is the encoded
            // form of the linear 1.0 the shading stage needs back - a colour mapping that changes nothing.
            deren::utility::vessel::vector<uint8_t> pixels(static_cast<std::size_t>(baked_neutral_ramp_width) * baked_neutral_ramp_height * 4u, 255u);
            for (uint32_t v = 0; v < baked_neutral_ramp_height; ++v) {
                for (uint32_t u = 0; u < baked_neutral_ramp_width; ++u) {
                    float const x = static_cast<float>(u) / static_cast<float>(baked_neutral_ramp_width - 1u);
                    uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(v) * baked_neutral_ramp_width + u) * 4u;
                    // ENCODED for the same reason the unit step is: the lane is an sRGB texture, so the shader must
                    // get the LINEAR lightness back, and writing `x` raw would hand it the encoded one.
                    texel[3] = static_cast<uint8_t>(std::clamp(srgb_encode(x), 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            return pixels;
        };

        // ---- THE SHADOW LUT, WHICH IS A CUBE RATHER THAN A STEP ----
        //
        // IT IS THE ONE LANE THAT CANNOT REUSE THE UNIT RAMP, and the shape is the reason: a ramp answers "how deep
        // is the shadow here", a shadow LUT answers "what is THIS MATERIAL's colour in shadow" and is indexed by the
        // material's own albedo. That is not a variation on the ramp - it is a function of a different variable -
        // and it is why the reference ships skin and cloth a `1024x32` cube at all rather than another band.
        //
        // THE LAYOUT IS THE REFERENCE'S, TO THE TILE COUNT: 32 horizontal `32x32` tiles holding a `32^3` cube, the
        // tile chosen by the X channel and a bilinear hop between adjacent tiles because X is continuous. The
        // shader's `toon_shadow_lut` inverts this exactly, and `baked_lut_tiles` is HALF OF THAT CONTRACT - the other
        // half is `character_shadow_lut_tiles` - because a bake laid out for a different tile count reads back as a
        // different colour with no other symptom. `source/tests/test_toon_material_sidecar.cpp` compares the two.
        //
        // THE CONTENT IS THE IDENTITY CUBE, which is the same choice the ramps make and for the same reason: a
        // neutral bake reproduces the look the procedural branch already had (`albedo` in, `albedo` out), so the
        // lane is EXERCISED without the frame being changed by a guess at what the artist meant. What the lane buys
        // is that an AUTHORED cube would be honoured - a real skin palette makes the shadow a function of the skin
        // tone, which no per-family constant can be - and that is not something a neutral bake can substitute for.
        //
        // Note the axes, because they are the ones that invert: X picks the TILE, the in-tile X is the GREEN channel,
        // the in-tile Y is the BLUE channel FLIPPED, and each channel's stored value is `index / (tiles - 1)`. The
        // half-texel offsets are in the SHADER rather than here - the bake writes texel centres, and a reader that
        // sampled corners would be off by half a texel in every direction.
        constexpr uint32_t baked_lut_tiles = 32;
        constexpr uint32_t baked_lut_width = baked_lut_tiles * baked_lut_tiles;
        constexpr uint32_t baked_lut_height = baked_lut_tiles;
        static auto const bake_shadow_lut = []() {
            deren::utility::vessel::vector<uint8_t> pixels(static_cast<std::size_t>(baked_lut_width) * baked_lut_height * 4u, 255u);
            for (uint32_t row = 0; row < baked_lut_height; ++row) {
                for (uint32_t column = 0; column < baked_lut_width; ++column) {
                    // The identity cube at this texel's coordinate: the X channel from which tile it is in, the Y
                    // channel from where it sits inside that tile, the Z channel from the row - flipped, because the
                    // shader reads the cube's Z from the BOTTOM of the strip upward.
                    float const x = static_cast<float>(column / baked_lut_tiles) / static_cast<float>(baked_lut_tiles - 1u);
                    float const y = static_cast<float>(column % baked_lut_tiles) / static_cast<float>(baked_lut_tiles - 1u);
                    float const z = static_cast<float>(baked_lut_tiles - 1u - row) / static_cast<float>(baked_lut_tiles - 1u);
                    float const cube[3] = {x, y, z};
                    uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(row) * baked_lut_width + column) * 4u;
                    for (int32_t c = 0; c < 3; ++c) {
                        // STORED AS THE DISPLAY-SPACE COORDINATE, which is what keeps the identity true now that the
                        // shader indexes this lane by the DISPLAY-SPACE albedo (the game's own domain - see
                        // character_forward.slang's toon_shadow_lut). The lane is uploaded as an sRGB texture, so the
                        // sampler DECODES whatever is stored: storing the encoded coordinate makes the shader receive
                        // the LINEAR value that coordinate means, which is exactly the albedo it looked up. Encoding the
                        // linear coordinate instead - what this bake did before the domain changed - would hand back
                        // `srgb(albedo)` decoded, i.e. a brighter albedo, on every material with no LUT.
                        texel[c] = static_cast<uint8_t>(std::clamp(cube[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    texel[3] = 255u;
                }
            }
            return pixels;
        };

        // ---- THE MATCAP, WHICH IS BLACK, AND THAT IS THE WHOLE OF ITS NEUTRALITY ----
        //
        // THE THIRD SHAPE AND THE THIRD VARIABLE: a ramp is indexed by the shading term, the shadow LUT by the
        // material's albedo, and a matcap by the VIEW-SPACE NORMAL - `normalVS.xy * 0.5 + 0.5`, which is a sphere
        // map's own UV and the reason a matcap image is a picture of a ball.
        //
        // BLACK IS NOT A PLACEHOLDER, IT IS THE ONLY CONTENT THAT IS NEUTRAL HERE, and the difference from the other
        // two lanes is worth stating because it looks like a weaker bake and is not. The ramps and the cube have a
        // procedural branch to reproduce, so their neutral bake is a SHAPE the shader already computed (`smoothstep`,
        // the identity). This stage had NO matcap term at all before this lane existed, so "the look without the
        // artist's matcap" is the reference's own expression with the lookup at zero - `1 + 0 * strength` - and any
        // other content would be this repository inventing art direction and calling it a substitute. The lane is
        // real, it is read, and what it is FOR is honouring an authored ball.
        constexpr uint32_t baked_matcap_size = 256;
        static auto const bake_matcap = []() {
            // A matcap is an sRGB reference image (the reference's `EfClothSampleMatcap` says so explicitly), so the
            // lane's upload format applies here as it does to the ramps: zero is zero in both encodings, which is the
            // one value where the encode step cannot disagree with itself.
            return deren::utility::vessel::vector<uint8_t>(static_cast<std::size_t>(baked_matcap_size) * baked_matcap_size * 4u, 0u);
        };

        // ---- THE METALLIC/GLOSS NEUTRAL IS GONE, AND ITS ABSENCE IS THE CONTRACT ----
        //
        // A material with no `_MetallicGlossMap` is shaded by its FAMILY's `roughness` / `reflectivity` (see
        // toon_params.slang), because those four channels are the material's own numbers rather than art this project
        // would be reproducing - and a family default is the article's own answer for skin, which ships no such map.
        // The lane therefore answers "there is none" and the shading stage decides.

        // THE LOOKUP, whose lane vocabulary lives with the diagnostic above so that the two cannot disagree about
        // which flag switches a lane on.
        // THE COLOUR LOOKUP, whose lanes are the same sidecar rows read as four floats instead of as a texture name.
        //
        // A `color` ROW IS TEXT UNTIL SOMEBODY PARSES IT (`2.3985064,1.885226,2.0379374,1.0`), and the parsing lives
        // here rather than in the sidecar module because the *meaning* of the value is the consumer's: the module
        // keeps the row verbatim so that a consumer which does not know what `_EyeHighLightColor` is cannot damage it.
        // A ROW THAT IS ABSENT, OR UNPARSEABLE, ANSWERS THE LANE'S NEUTRAL - white for the three multiply-tints, and
        // for lane 3 white in `.rgb` with a WIDTH OF 0 in `.w` (see the table below), and for the two scalar lanes
        // (4 and 5) the sentinel `-1` (see them, too) - so a material whose sources say nothing tints nothing, is not
        // outlined, and keeps the fallback its consumer already had.
        //
        // THE NEUTRAL IS PER LANE AND NOT ONE WHITE, AND THAT IS A CORRECTNESS MATTER RATHER THAN A REFINEMENT: three
        // of the outline lane's four floats are a tint, whose neutral is white, and the fourth is `_OutlineWidth`,
        // whose neutral is ZERO. A missing row answered with `vec4(1.0f)` would therefore mean a width of 1.0 - an
        // outline around EVERY material that states none, which is the whole model (`toon_colour_lane`'s own note
        // states the same thing from the other side).
        //
        // THE SPECULAR LANE'S NEUTRAL IS `-1.0` AND IT IS A SENTINEL RATHER THAN A NO-OP, which is the first of the
        // two places this table says "the asset states nothing" instead of "multiply by one": the lane's `.x` is a
        // STRENGTH, so `0` would mean "no highlight at all" - a value the game really states on the iris and the
        // brow - and `1` would mean a full one. The stage tests `>= 0.0` and falls back to the family table (see
        // `toon_colour_lane::specular_strength`).
        //
        // THE PARALLAX LANE'S NEUTRAL IS THE SAME `-1.0` FOR THE SAME KIND OF REASON: its `.x` is a DEPTH rather than a
        // tint, so there is no no-op number - `0` is a legitimate authored "no parallax", and any other in-range value
        // would be a depth the port invented. The stage tests `>= 0.0` and falls back to its own
        // `character_eye_parallax_depth` constant (see `toon_colour_lane::parallax_scale`).
        static constexpr deren::utility::vessel::array<glm::vec4, static_cast<std::size_t>(deren::engine::toon_colour_lane::count)> toon_colour_neutral = {
            {glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f), glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f),
             // STEP 4'S SIX. THE FOUR SENTINELED LANES START AT `-1000.0f` AND NOT AT `-1.0f`, and the reason is
             // the whole point of a sentinel: two of their eight per-material numbers are AUTHORED NEGATIVES -
             // `CastShadow_center` is `-0.10000000149011612` on both body materials and
             // `GlobalShadowBrightnessAdjustment` is `-1.7999999523162842` on the cloth - so a neutral inside the
             // values' own range would make the stage read an authored number as "not stated". See
             // `goo_lane_absent` in `source/shaders/toon/character_forward.slang` and each lane's note in
             // `deren::engine::toon_colour_lane`.
             glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
             // STEP 5'S FOUR, WHOSE NEUTRALS ARE THE REFERENCE'S OWN `interface[]` DEFAULTS rather than the `-1000`
             // sentinel four of step 4's lanes need - and the reason is the SOCKETS' RANGES, which is the check step 4's
             // own root cause ("two of MY sockets are legitimately negative") says to make before reusing a sentinel.
             // None of these four numbers is negative in the reference's asset: `specularFGD Strength` is `0.8` or
             // `1.0`, `dirLight_lightColor` is `(1, 0.958..., 0.958...)`, `AmbientLightColorTint` is white or
             // `(1.512...)`, and `SpecularColor` is a positive HDR multiplier (`[4.2093, 3.7652, 3.7652]`). So `< 0`
             // lies outside every one of their domains and is enough, while step 4's lanes had
             // `CastShadow_center = -0.1` and `GlobalShadowBrightnessAdjustment = -1.8` - authored negatives that a `-1`
             // neutral would have swallowed. The scalar lane carries its sentinel in `.x` alone (the stage resolves
             // that component to the reference's own `1.0`), and the three colour lanes fall back on their own four
             // components, because a `-1` component of a light or a multiplier is not a state the reference has.
             glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
             // STEP 7'S FOUR. THE TWO SCALAR LANES ARE `-1000` SENTINELS and the two colours are BLACK, each for the
             // reason its own note gives: the scalars' zero is a meaningful value (`SmoothnessMax = 0` is "perfectly
             // rough", and the two brightnesses are multiplied into the pixel), while the two colours' socket defaults
             // in the reference's own interface ARE black - white would be the strongest possible statement about a
             // nose shadow (`混合.017`'s A side) and about `Front transparent red`'s tint.
             glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
             glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
             // STEP 8'S ONE. A `-1000` SENTINEL RATHER THAN THE REFERENCE'S GROUP DEFAULT `1.0`, and that is the whole
             // of this lane's contract: an absent row leaves the chain's previous normal in place instead of switching
             // the decode on for a material the port carried no measured value for. `NormalStrength = 0` is a value the
             // reference's own materials state (`chen_body_01.001` is `1.3184...` with `Use NormalTex? = 0`), so a `0`
             // neutral would be a statement; `-1000` is outside the socket's range and passes only as "not stated".
             glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
             // STEP 10'S ONE IS `(0, 0, 0, 0)` AND IT IS THE REFERENCE'S OWN GROUP DEFAULT, not a sentinel and not a
             // chosen number: `ng[2].interface[3]` (`Use anisotropy?`) is `0.0`, `interface[45]` (`Anisotropic mask`) is
             // `0.0` and `interface[4]` (`Use Toonaniso?`) is `0.0`. So a material that states no `_GooAnisoGate` row
             // gets the answer the graph gives a caller that states nothing - and because `混合.016` returns its A arm
             // at a factor of 0, that answer is the isotropic product (`.y = 0` also makes the masked arm zero, so the
             // two agree here as they do in the dump). The ZERO IS ALSO LOAD-BEARING FOR THE OTHER DIRECTION: the GPU
             // table starts every lane at `glm::vec4(1.0f)`, so without this entry a material with no row would read
             // `Use anisotropy? = 1` and take the arm the reference does not. See `toon_colour_lane::goo_aniso_gate`.
             //
             // IT NEEDS NO BRANCH IN `toon_colour` BELOW AND GETS NO START-UP DIAGNOSTIC, and both follow from the same
             // fact: it is a `color` row with three components the reference states as plain numbers, so it carries no
             // sentinel - step 8's lane needed its own branch only because a `float` row cannot travel the generic
             // `others` path, and its `-1000` needed the `>= 0.0f` test because a sentinel has to be read as "not
             // stated". Here `0.0` IS a stated value (it is the reference's own default), there is nothing to test for,
             // and the generic path at the end of `toon_colour` carries the row as it carries `_GooSpecularColor`.
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
             // STEP 12'S ONE IS ALSO `(0, 0, 0, 0)`, AND FOR STEP 10'S REASON RATHER THAN BY IMITATION: `ng[2].interface[20]`
             // (`Aniso_SmoothnessMaxT`) is `0.0` and `interface[21]` (`Aniso_SmoothnessMaxB`) is `0.0`, so a material
             // that states no `_GooAnisoRough` row reads the roughnesses - `rT = rB = 1` - that the graph itself gives
             // a caller that states nothing. It too is a `color` row, so it too needs no branch below, and the value
             // `0.0` is a real one the reference uses, so it is not a sentinel. The GPU-side override is
             // load-bearing in the same way: that table starts every lane at `glm::vec4(1.0f)`, and an unoverridden
             // lane 26 would give every rowless material `rT = (1 - 1)^2 = 0`. See
             // `toon_colour_lane::goo_aniso_rough`.
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
             // STEP 13'S TWO ARE `(0, 0, 0, 0)` AS WELL, AND HERE THE ZERO IS NOT A GROUP DEFAULT BUT THE SWITCH
             // ITSELF: `_GooRSScalars.x` is `Use RS_Eff?`, so a material whose sidecar states no such row must read
             // `Use = 0` - the reference's own `interface[]` default for that socket - and the stage's gate then
             // leaves the material's colour UNTOUCHED. Lane 28's `.w` is the mask's `SmoothStep.max` and zero there
             // means "unstated" rather than a literal zero-sized window, which the stage turns into `1.0` precisely so
             // that this neutral cannot divide by zero (see `toon_colour_lane::goo_rs_tint`, spec U7).
             //
             // THESE TWO ARE WRITTEN OUT RATHER THAN LEFT TO THE ARRAY'S TAIL, and that is not style: this array's
             // length is now `toon_colour_lane::count`, and what a VALUE-INITIALISED tail holds depends on GLM's
             // `GLM_FORCE_CTOR_INIT` - so "27 entries plus a tail that happens to be zero" is a claim about a build
             // configuration rather than about this table. Stated zeros cannot drift with a define.
             //
             // AND THEY ARE NOT WHAT KEEPS THE TWENTY `Use RS_Eff? = 0` MATERIALS UNCHANGED. That is a property of
             // the three mirror tables' common stride (`material_index * toon_colour_lane::count + lane`), which
             // `register_material` overwrites lane by lane for every material it registers. These entries are the
             // honest statement of the neutral, and the guard against a future consumer that reads the table before
             // registration - not the mechanism of the identity gate.
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
             // STEP 15'S ONE IS `(0, 0, 0, 0)`, WHICH IS BOTH THE REFERENCE'S GROUP DEFAULTS AND THE ARM'S OFF
             // SWITCH: all four `armA` sockets default to `0.0` in `ng[2].interface[]`, `RS Strength = 0` zeroes the
             // whole arm's product, and `RS_Index = 0` names the first sheet - so a material that states no
             // `_GooRSArm0` row reads what the graph gives a caller that states nothing. It is a `color` row, so the
             // generic path at the end of `toon_colour` carries it and no branch below is needed.
             //
             // IT IS NOT THE SAME AS `Use RS_Eff? = 0`, and the difference is stated rather than hidden: this lane's
             // zero makes `armA`'s colour zero, and a zero colour still passes through `混合.029` (LIGHTEN), whose
             // `max(base, 0)` is only bitwise `base` where `base >= 0`. The outer gate is what keeps a material with no
             // RS row unchanged. See `toon_colour_lane::goo_rs_arm0`.
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f),
             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)}};
        // THE SCALAR LOOKUP, for the one toon fact that is neither a map nor a colour: the article's TRANSPARENT
        // variant, which `M_actor_chen_cloth_02` selects with `_SrcBlend 5` / `_DstBlend 10` (Unity's `SrcAlpha` /
        // `OneMinusSrcAlpha`) while its glTF `alphaMode` stays OPAQUE. The two rows are read by NAME through this
        // callback so the runtime never has to know what a sidecar is (see `toon_lookup::scalar`), and the pair is
        // mapped to `toon_inputs::alpha_blend` only when BOTH say the standard alpha equation - 5/10 is the only
        // pair any material in this repository states, and any other pair is left alone rather than guessed at.

    } // namespace

    lookup::lookup(deren::gltf::scenes const& scenes, deren::toon::sidecar const* sidecar) {
        this->sidecar = sidecar;
        this->scenes = &scenes;
        baked_unit_ramp = bake_unit_ramp();
        ramp_width = baked_ramp_width;
        ramp_height = baked_ramp_height;
        baked_neutral_ramp = bake_neutral_ramp();
        neutral_ramp_width = baked_neutral_ramp_width;
        neutral_ramp_height = baked_neutral_ramp_height;
        baked_shadow_lut = bake_shadow_lut();
        lut_width = baked_lut_width;
        lut_height = baked_lut_height;
        baked_matcap = bake_matcap();
        matcap_size = baked_matcap_size;
    }

    deren::engine::runtime::toon_lookup lookup::runtime_lookup() const noexcept {
        return deren::engine::runtime::toon_lookup{.owner = const_cast<lookup*>(this), .texture = texture_callback, .colour = colour_callback, .scalar = scalar_callback};
    }

    deren::engine::texture_input lookup::texture_callback(void* const owner, std::string_view const material_name, deren::engine::toon_slot const lane) {
        lookup const& self = *static_cast<lookup const*>(owner);
        deren::engine::texture_input out = {};
        if (self.sidecar == nullptr || self.scenes == nullptr) {
            return out;
        }
        deren::toon::material_sidecar const* const material = self.sidecar->find(material_name);
        if (material == nullptr) {
            return out;
        }
        toon_lane_names const& names = toon_lane[static_cast<std::size_t>(lane)];
        std::string_view const slot_name = names.slot;
        // white fallback - i.e. "do not read" (see material_record::toon_indices). A map that exists while its
        // flag is off must NOT be read: that is the first rule the sidecar module exists to keep. Asked BY NAME
        // rather than by slot, because that is the only spelling that is true for all four lanes.
        if (!material->enabled_by_flag(names.flag)) {
            return out;
        }
        // ---- WHICH LANES READ THE MODEL'S IMAGE AND WHICH READ A NEUTRAL BAKE ----
        //
        // THE SPLIT IS THE ASSET BOUNDARY RATHER THAN A PREFERENCE, and the two sides are different in kind. Three
        // lanes stand in for maps whose content is a SHAPE the shading could describe itself (see bake_unit_ramp /
        // bake_shadow_lut / bake_matcap), so they take the bake whatever the model carries - the artist's switch
        // above still decides WHETHER there is a ramp or a LUT at all. The METALLIC/GLOSS lane is the other case:
        // its four channels are the material's own NUMBERS rather than art this project would be reproducing, so
        // it READS THE MODEL'S MAP and falls back to a neutral texel when the model has none.
        //
        // THE MODEL'S IMAGE FOR THIS SLOT IS RESOLVED ONCE, here, for every lane below: the sidecar names a texture
        // asset and the glTF carries images by name, so the join is one lookup - and a lane that resolved it may
        // still decline it.
        std::string_view const texture_name = material->slot(slot_name);
        deren::gltf::texture_data const* model_tex = nullptr;
        if (!texture_name.empty()) {
            if (std::optional<uint16_t> const index = self.scenes->texture_index_by_name(texture_name); index.has_value()) {
                deren::gltf::texture_data const& candidate = self.scenes->textures[*index];
                if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                    model_tex = &candidate; // present and usable; whether it is READ is the lane's decision
                }
            }
        }
        // ---- THE METALLIC/GLOSS LANE READS THE MODEL'S OWN MAP, OR ANSWERS "THERE IS NONE" ----
        //
        // IT DOES NOT GET A NEUTRAL TEXEL, unlike the ramps and the LUT below, and that is a change the SKIN forced:
        // the four channels are the material's own numbers, and a material with no map for them is not a material
        // whose numbers are neutral - it is one whose numbers the FAMILY states (`toon_params`' `roughness` /
        // `reflectivity`, which is exactly where the article keeps its skin's). So a lane of 0 here means "ask the
        // family", and handing it a baked texel instead would override that with one invented surface.
        if (lane == deren::engine::toon_slot::metallic_gloss) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE DIFFUSE RAMP READS THE MODEL'S OWN MAP, WHICH IS THE CHANGE THIS LANE WAS WAITING FOR ----
        //
        // THE GAME'S RAMP IS THE ONE TOON ASSET WHOSE CONTENT IS NOT A SHAPE this stage could reconstruct: its
        // RGB is the artist's colour mapping along the light axis and its A the artist's lightness along the same
        // axis, so a neutral bake can only ever stand in for a model that has none. Every character in this
        // repository ships one (`_DiffRampMap` is on 45 of the 45 materials across the five characters), so the
        // lane reads it and `bake_neutral_ramp` covers the rest.
        if (lane == deren::engine::toon_slot::diffuse_ramp) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!self.baked_neutral_ramp.empty()) {
                out.data = std::span<uint8_t const>(self.baked_neutral_ramp.data(), self.baked_neutral_ramp.size());
                out.width = self.neutral_ramp_width;
                out.height = self.neutral_ramp_height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // THE SPECULAR RAMP LANE STILL GETS THE BAKED UNIT STEP, NOT THE MODEL'S IMAGE - see bake_unit_ramp for
        // why a path that read the game's own ramps is a path this repository cannot carry, and note that the
        // artist's switch above is still what decides WHETHER there is a ramp at all. Nothing has taught THIS
        // lane to read the game's own atlas yet, and it is the one that still reads `.r` as a threshold.
        if (lane == deren::engine::toon_slot::specular_ramp && !self.baked_unit_ramp.empty()) {
            out.data = std::span<uint8_t const>(self.baked_unit_ramp.data(), self.baked_unit_ramp.size());
            out.width = self.ramp_width;
            out.height = self.ramp_height;
            out.mip_levels = 1;
            out.valid = true;
            return out;
        }
        // ---- THE SHADOW LUT READS THE MODEL'S OWN PALETTE, AND THIS IS THE SKIN'S BIGGEST LEVER ----
        //
        // THE ONE TOON LANE WHOSE CONTENT IS A COLOUR THE ARTIST CHOSE per skin tone: the game ships
        // `T_actor_common_femaleskincolorNN_lut_D` - a `1024x32` strip holding a `32^3` cube - and the shading reads
        // it as "what is THIS albedo when it is in shadow". Its earlier treatment here was the identity bake, which
        // is the right FALLBACK for a model that ships none and the wrong answer for one that does: a baked identity
        // reproduces the albedo, so the whole dark side of every skin in the game was being replaced by the lit
        // one. The artist's switch above still decides whether there is a LUT at all, which is why hair - whose
        // `_UseShadowLutTex` is off - is unaffected by any of this.
        if (lane == deren::engine::toon_slot::shadow_lut) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!self.baked_shadow_lut.empty()) {
                out.data = std::span<uint8_t const>(self.baked_shadow_lut.data(), self.baked_shadow_lut.size());
                out.width = self.lut_width;
                out.height = self.lut_height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE MATCAP LANE READS THE MODEL'S OWN BALL, WITH THE BLACK BAKE AS ITS FALLBACK ----
        //
        // THE EYE'S HIGHLIGHT IS A MATCAP AND NOT A LOBE, which is the article's own arrangement for that family
        // (`EF_EYE_IRIS_MATCAP05_STRENGTH` and a separate overlay material), and the game ships the ball as
        // `T_actor_common_matcap_06_D`. Reading it is what turns an iris from a flat dark disc into a surface with a
        // catchlight in it - and the bake below still covers a model whose material switches a matcap on without
        // shipping one, where black remains the only neutral content (see bake_matcap).
        //
        // THIS IS ALSO THE LANE THE FLAG TABLE ABOVE WAS FIXED FOR: until the flag was asked by name, `_MatcapTex`
        // resolved to "off" for the very material that ships it, so this branch was unreachable and the game's ball
        // was neither read nor replaced - the lane was simply absent, the quietest possible version of wrong.
        if (lane == deren::engine::toon_slot::matcap) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!self.baked_matcap.empty()) {
                out.data = std::span<uint8_t const>(self.baked_matcap.data(), self.baked_matcap.size());
                out.width = self.matcap_size;
                out.height = self.matcap_size;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE GOO BASE RAMP RESOLVES THE REFERENCE'S `RampSelect` ON THE HOST, AND THAT IS THE WHOLE POINT ----
        //
        // THE REFERENCE'S SELECTOR IS NOT A TEXTURE SLOT: `RampSelect` instantiates four `ShaderNodeTexImage`
        // nodes inside itself and picks one with the material's `RampIndex` through five comparators and three
        // MIX nodes (`goo_step4_diffuse_spec.md` §2.3/§2.4). The port may resolve it here for two reasons the spec
        // proves rather than assumes (§2.6 + its A8):
        //
        //   * `RampIndex` HAS NO MATERIAL LINK in either dump - it is a per-material CONSTANT, so the choice cannot
        //     change inside a frame;
        //   * THE FOUR SLOTS HOLD ONLY TWO DISTINCT IMAGES (sha256 `ff12009a...` for slots 1 and 3,
        //     `f92de330...` for slot 2 and the dangling `...001`), and every laevatain material states
        //     `RampIndex` 0.0 or 1.0 - so on this asset the whole 41-node selector answers one of two names.
        //
        // THE SHADER THEREFORE SEES ONE LANE AND ONE FETCH, and this branch is where the arm it lands in stops
        // being the shader's business. THE NAMES ARE THE `_RD` IMAGES' glTF NAMES, appended to the model by
        // `deren-ab/attach_toon_images.py` - without that step the lane resolves to nothing and the material keeps
        // the old chain's diffuse, which is the correct fallback and an invisible one in a log.
        if (lane == deren::engine::toon_slot::goo_base_ramp) {
            // TWO SOURCES, AND THE ORDER IS THE STATEMENT RATHER THAN A CONVENIENCE. `_GooBaseRamp` is the
            // sidecar's own slot row and carries the name the reference's selector resolves to, already applied
            // where the file was written; `_GooRampIndex` is the raw socket and is read only when the slot row is
            // absent, which is the case for a material whose sidecar states the index and no name. Both answer the
            // SAME name on every material this asset gives the group to, and `source/tests/test_goo_toon_math.cpp` asserts
            // that agreement against the reference's four thresholds.
            //
            // THE MODEL IS THE JUDGE OF PRESENCE: a name the model does not carry leaves the lane `invalid`, so a
            // material with no ramp in the file keeps the old chain's diffuse rather than sampling the white
            // fallback as if it were a ramp.
            std::string_view const ramp_name = !texture_name.empty() ? texture_name : toon_base_ramp_name(*material);
            deren::gltf::texture_data const* ramp_tex = nullptr;
            if (!ramp_name.empty()) {
                if (std::optional<uint16_t> const index = self.scenes->texture_index_by_name(ramp_name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = self.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        ramp_tex = &candidate;
                    }
                }
            }
            if (ramp_tex != nullptr) {
                out.data = std::span<uint8_t const>(ramp_tex->data.data(), ramp_tex->data.size());
                out.width = ramp_tex->width;
                out.height = ramp_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE `armA` SHEET LANE HONOURS `RS_Index`: TWO NAMED SHEETS, AND THE HOST IS WHAT CHOOSES ----
        //
        // THE REFERENCE MIXES TWO PER-CHARACTER `_RS` SHEETS by `RS_Index` (`混合.032`; spec §1.1) and this port
        // reads ONE sheet per material, so the choice has to be resolved HERE, before the shader sees a lane -
        // which is what `toon_rs_sheet_of` above does. It may be resolved on the host for the same reason
        // `RampSelect` may be (see the branch above): `RS_Index` has NO MATERIAL LINK in either dump, so it is a
        // per-material CONSTANT and the answer cannot change inside a frame (spec §1.3, §9.1).
        //
        // WHY THIS IS A HOST RULE AND NOT A SHADER ONE: `rs_arm0_lane.x` stays UNREAD in `source/shaders/toon/goo_toon.slang`
        // (see the L1 note there) - the lane carries the reference's socket, the HOST answers it, and the shader's
        // `u` derivation and its single `heap_texel` fetch are untouched by this step. THE `.spv` FILES ARE THE
        // EVIDENCE: this branch must not move a single one of them.
        //
        // THE FALLBACK IS THE FIRST SHEET, IN EVERY DIRECTION. A name that is absent, or whose image this model
        // does not carry, leaves the lane reading `_GooRSSheet` exactly as it read before this branch existed -
        // which is also what keeps every existing fixture and every shipped material (`RS_Index = 0`, no
        // `_GooRSSheet1` row at all) byte-identical. `_GooRSSheet1`'s own `_UseGooRSSheet1` switch is asked
        // inside `toon_rs_sheet_of`, so "named but switched off" is the first sheet too.
        if (lane == deren::engine::toon_slot::goo_rs_sheet) {
            toon_rs_sheet_choice const choice = toon_rs_sheet_of(*material, texture_name);
            deren::gltf::texture_data const* sheet_tex = nullptr;
            if (!choice.name.empty()) {
                if (std::optional<uint16_t> const index = self.scenes->texture_index_by_name(choice.name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = self.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        sheet_tex = &candidate;
                    }
                }
            }
            // THE SECOND SHEET WAS NAMED, SWITCHED ON, AND STILL DID NOT RESOLVE: this model does not carry that
            // image, so the material reads the first sheet instead - the same answer as every other miss, and the
            // same distinction the tail below draws between "the sidecar names a map" and "the model has it". The
            // alternative (an invalid lane) would silently punch a hole in the shading rather than degrade to the
            // sheet the material would have read without `RS_Index` at all.
            if (sheet_tex == nullptr && choice.second) {
                if (std::optional<uint16_t> const index = self.scenes->texture_index_by_name(texture_name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = self.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        sheet_tex = &candidate;
                    }
                }
            }
            if (sheet_tex != nullptr) {
                out.data = std::span<uint8_t const>(sheet_tex->data.data(), sheet_tex->data.size());
                out.width = sheet_tex->width;
                out.height = sheet_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }

        if (model_tex == nullptr) {
            return out; // the sidecar names a map this model does not have, or one that is present but unusable
        }
        out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
        out.width = model_tex->width;
        out.height = model_tex->height;
        out.mip_levels = 1;
        out.valid = true;
        return out;
    }

    glm::vec4 lookup::colour_callback(void* const owner, std::string_view const material_name, deren::engine::toon_colour_lane const lane) {
        std::size_t const lane_index = static_cast<std::size_t>(lane);
        lookup const& self = *static_cast<lookup const*>(owner);
        // ---- THE SPECULAR STRENGTH IS READ BEFORE THE SIDECAR IS EVEN LOOKED FOR, AND THAT ORDER IS THE RULE ----
        //
        // Its value is the ASSET'S (`extras_float_of`), the sidecar is only the fallback, and the FAMILY TABLE in
        // the shader is the fallback behind that - so this lane must answer even for a model that has extras and
        // NO sidecar at all, which is why it does not sit behind the two early returns below. `-1` is the third
        // answer (neither source speaks), and the stage reads it as "the family's number stands".
        //
        // THE PARALLAX DEPTH TAKES EXACTLY THE SAME PATH IN THE SAME ORDER, which is why the two share one branch:
        // both are per-material SCALARS whose preferred source is the asset (`-1` = nothing stated), and the only
        // thing that differs is what the stage falls back TO - `params.spec_strength` for one,
        // `character_eye_parallax_depth` for the other. On chen the sidecar holds no `_ParallaxScale` row at all,
        // so for that property the asset is the ONLY source that can speak and this lane answers the sentinel for
        // every material but the three that state the row - which is the state the frame had before this lane
        // existed, material by material.
        if (lane == deren::engine::toon_colour_lane::specular_strength || lane == deren::engine::toon_colour_lane::parallax_scale) {
            glm::vec4 scalar = toon_colour_neutral[lane_index]; // -1 in .x until a source states a value
            if (std::optional<float> const from_asset = extras_float_of(self.scenes, material_name, toon_colour_row[lane_index])) {
                scalar.x = *from_asset;
                return scalar;
            }
            if (self.sidecar != nullptr) {
                if (deren::toon::material_sidecar const* const from_sidecar = self.sidecar->find(material_name)) {
                    // The same "states nothing" contract as the generic path below: `scalar` answers the fallback
                    // when the row is absent, and the fallback here IS the sentinel.
                    float const value = from_sidecar->scalar(toon_colour_row[lane_index], scalar.x);
                    // A SIDECAR ROW OUTSIDE THE SENTINEL'S SIDE OF ZERO IS IGNORED RATHER THAN TRUSTED, because
                    // `-1` is not a strength (nor a depth) and a negative one would shade nothing: a row that
                    // parsed as a negative number is a row this lane cannot express, so the stage's own fallback
                    // answers instead.
                    if (value >= 0.0f) {
                        scalar.x = value;
                    }
                }
            }
            return scalar;
        }
        if (self.sidecar == nullptr) {
            return toon_colour_neutral[lane_index];
        }
        deren::toon::material_sidecar const* const material = self.sidecar->find(material_name);
        if (material == nullptr) {
            return toon_colour_neutral[lane_index];
        }
        // ---- STEP 8'S NORMAL STRENGTH, WHICH IS A `float` ROW AND THEREFORE NEEDS THIS BRANCH ----
        //
        // The generic path at the bottom of this lambda reads `material->others`, and a `float`-kind row does not
        // land there: `toon_material_sidecar.cpp` routes `slot` to `slots`, `float` to `scalars` and every other
        // kind to `others`, and only `scalar()` reads `scalars`. So a `_GooNormalStrength` row written as a `color`
        // row would be parsed by the generic path and a row written as the `float` row it is would be invisible -
        // hence this branch, and hence it sits before the outline branch rather than inside the generic one.
        //
        // THE `>= 0.0f` TEST IS THE SAME ONE THE SPECULAR LANE MAKES, for the same reason: the value is a positive
        // strength on every material either dump states, so a row that parsed negative is a row this lane cannot
        // express and the neutral (the sentinel, i.e. "not stated") answers instead of a strength that would flip
        // the normal's `xy`.
        if (lane == deren::engine::toon_colour_lane::goo_normal_strength) {
            glm::vec4 strength = toon_colour_neutral[lane_index]; // -1000 in every component until a row says otherwise
            float const value = material->scalar(toon_colour_row[lane_index], strength.x);
            if (value >= 0.0f) {
                strength.x = value;
            }
            return strength;
        }
        // `specularFGD Strength` IS A `float` ROW TOO, so it needs the same branch step 8's lane needed: a
        // `float`-kind row lands in `material_sidecar::scalars` (see `toon_material_sidecar.cpp`), and the
        // generic path below reads `material->others`, so without this branch the row is unreachable and the
        // lane answers its neutral for every material - which is what shipped: both body materials state
        // `0.7999999523162842` and every cloth states `1.0`, and the shader read `-1.0` for all of them.
        //
        // THE NEUTRAL'S `.x` IS THE SENTINEL (`-1.0`, "this material's container states nothing") and the
        // shader resolves it to the reference's own group default `1.0` - see
        // `deren::engine::toon_colour_lane::goo_specular_fgd`, whose contract is `< 0` and NOT the `-1000` sentinel
        // step 4's four lanes use.
        if (lane == deren::engine::toon_colour_lane::goo_specular_fgd) {
            glm::vec4 fgd = toon_colour_neutral[lane_index]; // -1 in `.x` until a row says otherwise
            float const value = material->scalar(toon_colour_row[lane_index], fgd.x);
            if (value >= 0.0f) {
                fgd.x = value;
            }
            return fgd;
        }
        // THE OUTLINE LANE IS THE ONE LANE FED BY TWO ROWS OF TWO DIFFERENT KINDS, so it does not go through the
        // single-row path below: `.rgb` is the `color _OutlineTintColor` row (the game's name for the author's
        // `_OutlineColor`) and `.w` is the SEPARATE `float _OutlineWidth` row - see `toon_colour_lane::outline_edge`.
        //
        // THE TWO ROWS ARE INDEPENDENT, WHICH IS MEASURED RATHER THAN ASSUMED: in `chars\chen.glb.toon.tsv` the
        // tint row exists for `M_actor_chen_cloth_01` ALONE (1,1,1,1), while `_OutlineWidth` exists for all five
        // materials (body 0.6 / cloth_01 0.6 / cloth_02 0.0 / face 0.5 / hair 0.5). Reading the width only when
        // the tint row is present would therefore outline one material out of five and silently drop the rest, so
        // the width is read unconditionally and a missing tint answers white - the article's own `_OutlineColor`
        // default is white, and white is what "the game states no tint" has to look like for a multiply.
        if (lane == deren::engine::toon_colour_lane::outline_edge) {
            glm::vec4 outline = toon_colour_neutral[lane_index]; // white tint, width 0 until a row says otherwise
            if (auto const tint = material->others.find(deren::utility::vessel::string(toon_colour_row[lane_index])); tint != material->others.end()) {
                std::string_view rest = tint->second;
                for (int32_t component = 0; component < 4 && !rest.empty(); ++component) {
                    std::size_t const comma = rest.find(',');
                    std::string_view const field = rest.substr(0, comma);
                    float value = 0.0f;
                    if (auto const result = std::from_chars(field.data(), field.data() + field.size(), value); result.ec == std::errc{}) {
                        outline[component] = value;
                    }
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
            }
            // ... AND THE WIDTH, which is a `float` row rather than part of the colour row. A material with no
            // `_OutlineWidth` row gets 0.0, i.e. no outline - the only answer that leaves the frame where it was.
            outline.w = material->scalar("_OutlineWidth", 0.0f);
            return outline;
        }
        auto const row = material->others.find(deren::utility::vessel::string(toon_colour_row[lane_index]));
        if (row == material->others.end()) {
            return toon_colour_neutral[lane_index];
        }
        glm::vec4 parsed = toon_colour_neutral[lane_index];
        std::string_view rest = row->second;
        for (int32_t component = 0; component < 4 && !rest.empty(); ++component) {
            std::size_t const comma = rest.find(',');
            std::string_view const field = rest.substr(0, comma);
            float value = 0.0f;
            if (auto const result = std::from_chars(field.data(), field.data() + field.size(), value); result.ec == std::errc{}) {
                parsed[component] = value;
            }
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        return parsed;
    }

    float lookup::scalar_callback(void* const owner, std::string_view const material_name, std::string_view const row, float const fallback) {
        lookup const& self = *static_cast<lookup const*>(owner);
        if (self.sidecar == nullptr) {
            return fallback;
        }
        deren::toon::material_sidecar const* const material = self.sidecar->find(material_name);
        if (material == nullptr) {
            return fallback;
        }
        return material->scalar(row, fallback);
    }

    // ... AND THE ONE `float` ROW THAT IS NOT A LANE SWITCH AND NOT A COLOUR, named here because the diagnostic
    // below walks the SIDECAR rather than the lane table and will therefore report it: `_GooRampIndex` is the
    // reference's `RampIndex` socket, written beside its resolved name (`_GooBaseRamp`) so that the choice the
    // host made can be re-derived from the file. Its synthetic flag is `_UseGooRampIndex`, which no asset states,
    // so the diagnostic prints it `off` - which is TRUE of the flag and says nothing about the ramp (the ramp's
    // switch is `_UseGooBaseRamp`). It has no colour row either, so `toon_colour` below never parses it.
    void log_sidecar(deren::gltf::scenes const& scenes, deren::toon::sidecar const& sidecar) {
        deren::utility::log("toon sidecar: {} material(s) described ({} line(s) skipped)", sidecar.materials.size(), sidecar.skipped_lines);
        // A NON-ZERO MERGE COUNT IS A FACT ABOUT THE FILE AND NOT A COMPLAINT: it says some material's rows
        // arrived in more than one block, which the reader merges by name (see `sidecar::merged_rows`). It is
        // logged because the OPPOSITE reading - "this material states nothing" - is what a reader that did not
        // merge would silently report, and the two are indistinguishable in every other line of this output.
        if (sidecar.merged_rows > 0) {
            deren::utility::log("toon sidecar: {} row(s) belong to a material whose rows are NOT CONTIGUOUS - merged into its entry by name", sidecar.merged_rows);
        }
        for (deren::toon::material_sidecar const& material : sidecar.materials) {
            // THE FAMILY COMES FROM THE LOADER'S CLASSIFIER over the SAME name, so the sidecar (which
            // supplies the parameters) and the renderer (which selects them) cannot disagree about which
            // family a material is: there is one classifier and both sides ask it.
            deren::utility::log("  '{}' -> family {} | {} slot(s), {} scalar(s)", material.name, static_cast<uint32_t>(deren::gltf::toon_family_of(material.name)), material.slots.size(), material.scalars.size());
            // STEP 8'S `float` ROW IS PRINTED BY NAME, because the loop below walks `material.slots` ONLY: a
            // `float` property is counted in the `{} scalar(s)` above and never named, so a human reading this
            // log could not tell "the file states nothing" from "the reader dropped the row" - the two look
            // identical in every other line. `ABSENT` means the chain's previous normal stands for this
            // material; a value means lane 24 carries it into the Goo chain's decode (see
            // `toon_colour_lane::goo_normal_strength`).
            if (auto const normal_strength = material.scalars.find("_GooNormalStrength"); normal_strength != material.scalars.end()) {
                deren::utility::log("      _GooNormalStrength = {:.10g} | lane 24 (the reference's DecodeNormal strength)", static_cast<double>(normal_strength->second));
            } else {
                deren::utility::log("      _GooNormalStrength = ABSENT | lane 24 keeps the chain's previous normal");
            }
            // LANE 16'S `float` ROW IS PRINTED BY NAME FOR THE SAME REASON, and here the distinction is the
            // whole of debt (u): `ABSENT` makes the stage answer the reference's own group default `1.0`, while
            // a value is what this lane exists to carry. The two body materials state `0.7999999523162842` and
            // every cloth states `1.0`, so a reader that sees `ABSENT` for them is looking at D1.
            if (auto const specular_fgd = material.scalars.find("_GooSpecularFGD"); specular_fgd != material.scalars.end()) {
                deren::utility::log("      _GooSpecularFGD = {:.10g} | lane 16 (the reference's `specularFGD Strength`)", static_cast<double>(specular_fgd->second));
            } else {
                deren::utility::log("      _GooSpecularFGD = ABSENT | lane 16 answers the reference's group default 1.0");
            }
            for (auto const& [slot_name, texture_name] : material.slots) {
                std::optional<uint16_t> const index = scenes.texture_index_by_name(texture_name);
                deren::utility::log("      {} = '{}' -> {} | {}", slot_name, texture_name, index.has_value() ? deren::utility::vessel::format("texture #{}", *index) : deren::utility::vessel::string("ABSENT from this model"), material.enabled_by_flag(toon_flag_for(slot_name)) ? "ON" : "off");
            }
        }
    }
} // namespace deren::app::toon_material