// Headless unit tests: toon_material_sidecar (pure CPU) ==========================
// The `.toon.tsv` a model may carry beside it: the tab-separated format, the two rules the module exists to
// keep (a feature is switched on by an explicit `_Use` flag; a MISSING sidecar is not an error), and the
// failure cases - because a reader whose failure cases are not asserted is a reader whose failures are
// discovered by a character losing its ramp and looking slightly wrong.
//
// THE FAILURE CASES ARE THE TEST, as in test_render_resources: most of the checks below assert that something
// is REJECTED, and each one is a disagreement between this reader and the asset pipeline that would otherwise
// pass silently.
//
// THE LAST CHECK IS NOT ABOUT THE SIDECAR READER AT ALL, and it is here deliberately: the host bakes the ramp
// that fills a sidecar's `_DiffRampMap` LANE, and the stage that reads that lane inverts the bake using a
// constant of its own. The two numbers are written in two languages in two files, so their agreement is a
// contract nothing else can check - see `test_the_baked_ramp_and_the_shader_agree_on_its_width`.
#include "vk_test.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

import gltf_loader;
import toon_material_sidecar;

namespace {

    /// a well-formed sidecar, inline so the parser's own cases need no file on disk
    constexpr std::string_view good_text =
        "material\tkind\tname\tvalue\n"
        "M_body\tslot\t_DiffRampMap\tT_body_RD\n"
        "M_body\tfloat\t_UseDiffRampMap\t1.0\n"
        "M_body\tfloat\t_UseSpecRampMap\t0.0\n"
        "M_body\tfloat\t_OutlineWidth\t0.6\n"
        "M_body\tcolor\t_OutlineColor\t0.1,0.1,0.1,1.0\n"
        "M_hair\tslot\t_LineMap\tT_hair_line\n"
        "M_hair\tfloat\t_UseLineMap\t1.0\n";

    void test_well_formed() {
        auto const parsed = toon::parse_sidecar(good_text);
        CHECK(parsed.has_value());
        if (!parsed.has_value()) {
            return;
        }
        toon::sidecar const& sidecar = *parsed;
        CHECK(sidecar.materials.size() == 2);
        if (sidecar.materials.size() != 2) {
            return;
        }
        // THE FILE'S ORDER IS KEPT, and the names are the asset pipeline's
        CHECK(sidecar.materials[0].name == "M_body");
        CHECK(sidecar.materials[1].name == "M_hair");

        toon::material_sidecar const* const body = sidecar.find("M_body");
        CHECK(body != nullptr);
        if (body == nullptr) {
            return;
        }
        // a slot's value is the TEXTURE NAME as written, not a path and not an index
        CHECK(body->slot("_DiffRampMap") == "T_body_RD");
        // an absent slot is empty rather than a guess
        CHECK(body->slot("_SpecRampMap").empty());
        // a scalar reads back, and an absent one falls back
        CHECK(body->scalar("_OutlineWidth", -1.0f) == 0.6f);
        CHECK(body->scalar("_ShadowLutTex", -1.0f) == -1.0f);
        // a `color` row is KEPT rather than dropped, so an unknown kind reaches a consumer as data
        CHECK(body->others.size() == 1);
        CHECK(body->others.contains("_OutlineColor"));

        // THE FIRST RULE: the explicit flag decides, not the slot's presence.
        CHECK(body->enabled("_DiffRampMap"));  // _UseDiffRampMap = 1.0
        CHECK(!body->enabled("_SpecRampMap")); // _UseSpecRampMap = 0.0
        // ... and a flag that is ABSENT is OFF, which is the safe answer: an artist's switch is off unless
        // it was switched on, so a consumer cannot turn on a feature by finding a slot.
        CHECK(!body->enabled("_ShadowLutTex"));

        toon::material_sidecar const* const hair = sidecar.find("M_hair");
        CHECK(hair != nullptr);
        CHECK(hair != nullptr && hair->enabled("_LineMap"));
        // the name prefix resolves the same way for a slot that has no leading underscore
        CHECK(body->enabled("OutlineWidth") == false); // there is no _UseOutlineWidth, so: off
        CHECK(sidecar.find("M_absent") == nullptr);
    }

    void test_crlf_and_blank_lines() {
        // CRLF because the file is written on Windows, and blank lines because a hand-edited one has them
        constexpr std::string_view text =
            "material\tkind\tname\tvalue\r\n"
            "\r\n"
            "M_a\tslot\t_BaseMap\tT_a\r\n";
        auto const parsed = toon::parse_sidecar(text);
        CHECK(parsed.has_value());
        if (!parsed.has_value()) {
            return;
        }
        CHECK(parsed->materials.size() == 1);
        // THE \r IS STRIPPED FROM THE LAST FIELD TOO, which a naive split leaves attached and which would
        // make every texture name end in a carriage return
        CHECK(parsed->materials.size() == 1 && parsed->materials[0].slot("_BaseMap") == "T_a");
        CHECK(parsed->skipped_lines == 2); // the header and the blank line
    }

    void test_header_is_recognised_by_its_column_not_its_position() {
        // a file with NO header is still readable, which is what "recognised by its first column" buys
        constexpr std::string_view text = "M_a\tslot\t_BaseMap\tT_a\n";
        auto const parsed = toon::parse_sidecar(text);
        CHECK(parsed.has_value());
        CHECK(parsed.has_value() && parsed->materials.size() == 1 && parsed->materials[0].name == "M_a");
    }

    void test_a_malformed_row_is_an_error_with_its_line_number() {
        constexpr std::string_view text =
            "material\tkind\tname\tvalue\n"
            "M_a\tslot\t_BaseMap\tT_a\n"
            "M_a\tslot\t_TooFewColumns\n";
        auto const parsed = toon::parse_sidecar(text);
        CHECK(!parsed.has_value()); // THE POINT OF THE TEST: it is rejected, not skipped
        if (parsed.has_value()) {
            return;
        }
        // the message names the LINE, which is what makes a broken asset pipeline one line to read
        CHECK(parsed.error().find("line 3") != std::string::npos);
    }

    void test_a_non_numeric_float_is_an_error() {
        constexpr std::string_view text =
            "material\tkind\tname\tvalue\n"
            "M_a\tfloat\t_OutlineWidth\twide\n";
        auto const parsed = toon::parse_sidecar(text);
        CHECK(!parsed.has_value());
        if (parsed.has_value()) {
            return;
        }
        CHECK(parsed.error().find("_OutlineWidth") != std::string::npos);
        // ... and a number with trailing junk is not a number either
        auto const trailing = toon::parse_sidecar("M_a\tfloat\t_X\t1.0x\n");
        CHECK(!trailing.has_value());
    }

    void test_rows_appended_for_an_existing_material_merge_into_its_entry() {
        // THE FILE SHAPE THIS EXISTS FOR, and it is not hypothetical: a sidecar extended by APPENDING rows rather
        // than inserting them beside the material's others - the natural way to add one, and how the rewritten
        // chain's own rows arrived. It used to give one material TWO entries, and `find` returns the first, so the
        // consumer saw NONE of the appended rows while the file plainly contained them. The failure is invisible in
        // both directions: the log that walks the sidecar lists every row (twice), and the lookup that matters
        // silently falls back to the material's older state.
        constexpr std::string_view text =
            "material\tkind\tname\tvalue\n"
            "M_a\tslot\t_BaseMap\tT_a\n"
            "M_b\tslot\t_BaseMap\tT_b\n"
            "M_a\tfloat\t_UseNewThing\t1.0\n"; // appended, i.e. NOT contiguous with M_a's first row
        auto const parsed = toon::parse_sidecar(text);
        CHECK(parsed.has_value());
        if (!parsed.has_value()) {
            return;
        }
        CHECK(parsed->materials.size() == 2); // one entry per NAME, not one per contiguous block
        CHECK(parsed->merged_rows == 1);      // and the merge is COUNTED, so a caller can report the shape
        toon::material_sidecar const* const a = parsed->find("M_a");
        CHECK(a != nullptr);
        // BOTH rows have to be in the SAME entry: the first block's slot, and the appended flag
        CHECK(a != nullptr && a->slot("_BaseMap") == "T_a");
        CHECK(a != nullptr && a->enabled_by_flag("_UseNewThing"));
        // the entries still keep the file's FIRST-appearance order, which is what the reader's contract says
        CHECK(parsed->materials.size() == 2 && parsed->materials[0].name == "M_a" && parsed->materials[1].name == "M_b");
    }

    void test_the_path_convention_appends_rather_than_replaces() {
        // `x.glb` -> `x.glb.toon.tsv`, NOT `x.toon.tsv`: the model's own extension stays
        std::filesystem::path const path = toon::sidecar_path_for("models/hero.glb");
        CHECK(path.filename() == "hero.glb.toon.tsv");
        CHECK(path.parent_path() == "models");
    }

    void test_a_missing_file_is_an_empty_sidecar_and_not_an_error() {
        // THE SECOND RULE: every non-character model has no sidecar, so this is the normal path rather than a
        // failure to handle - and the check is that it does NOT report one.
        auto const loaded = toon::load_sidecar(VR_TEST_SOURCE_DIR "/tests/fixtures/toon/absent.gltf");
        CHECK(loaded.has_value());
        CHECK(loaded.has_value() && loaded->empty());
        // an empty sidecar answers "no" to every lookup rather than asserting
        CHECK(loaded.has_value() && loaded->find("M_anything") == nullptr);
    }

    void test_a_file_on_disk_is_read_through_the_convention() {
        auto const loaded = toon::load_sidecar(VR_TEST_SOURCE_DIR "/tests/fixtures/toon/minimal.gltf");
        CHECK(loaded.has_value());
        if (!loaded.has_value()) {
            return;
        }
        CHECK(loaded->materials.size() == 2);
        toon::material_sidecar const* const body = loaded->find("M_actor_test_body_01");
        CHECK(body != nullptr);
        if (body == nullptr) {
            return;
        }
        CHECK(body->slot("_BaseMap") == "T_actor_test_body_01_D");
        CHECK(body->slot("_DiffRampMap") == "T_actor_common_body_01_RD");
        // THE CASE A CONSUMER THAT INFERRED WOULD GET WRONG: `_BumpMap` IS DECLARED and `_UseBumpMap` is 0.0,
        // so the slot is present while the feature is OFF. "Declared" and "enabled" are two different
        // questions, which is the first rule of the module's header and the reason the flag exists at all.
        CHECK(!body->slot("_BumpMap").empty());
        CHECK(!body->enabled("_BumpMap"));
        CHECK(body->enabled("_DiffRampMap"));
        // ... and `_SpecRampMap` is neither declared nor enabled, so the two really do vary independently
        CHECK(body->slot("_SpecRampMap").empty());
        CHECK(!body->enabled("_SpecRampMap"));
    }

    void test_a_malformed_file_on_disk_reports_its_path() {
        auto const loaded = toon::load_sidecar(VR_TEST_SOURCE_DIR "/tests/fixtures/toon/broken.gltf");
        CHECK(!loaded.has_value());
        if (loaded.has_value()) {
            return;
        }
        // the path is in the message, because a sidecar that cannot be parsed is a file to go and look at
        CHECK(loaded.error().find("broken.gltf.toon.tsv") != std::string::npos);
    }

    void test_the_sidecar_and_the_model_join_by_texture_name() {
        // THE JOIN THE TWO MODULES EXIST FOR, and it is the whole reason the loader carries image names: the
        // sidecar refers to a toon map by bare asset name, and the model's IMAGE NAMES are what turn that into
        // something loadable. Nothing else connects the two files.
        auto const model = gltf::load_model(VR_TEST_SOURCE_DIR "/tests/fixtures/toon/named_texture.gltf");
        CHECK(model.has_value());
        if (!model.has_value()) {
            return;
        }
        auto const sidecar = toon::load_sidecar(VR_TEST_SOURCE_DIR "/tests/fixtures/toon/named_texture.gltf");
        CHECK(sidecar.has_value());
        if (!sidecar.has_value()) {
            return;
        }
        toon::material_sidecar const* const body = sidecar->find("M_actor_test_body_01");
        CHECK(body != nullptr);
        if (body == nullptr) {
            return;
        }
        CHECK(model->textures.size() == 2);
        std::optional<uint16_t> const base = model->texture_index_by_name(body->slot("_BaseMap"));
        std::optional<uint16_t> const ramp = model->texture_index_by_name(body->slot("_DiffRampMap"));
        CHECK(base.has_value());
        CHECK(ramp.has_value());
        // two DIFFERENT images, so the join really resolved names rather than returning a constant
        CHECK(base.has_value() && ramp.has_value() && *base != *ramp);

        // A NAME THE MODEL DOES NOT HAVE MISSES, AND THAT IS A STATE RATHER THAN AN ERROR: the sidecar turns a
        // feature ON for a texture the artist did not export, which happens whenever an optional map is absent.
        // The consumer's safe answer is to leave the feature off - which is the same rule as the `_Use` flag,
        // arriving from the other side.
        CHECK(body->enabled("_SpecRampMap"));
        CHECK(!model->texture_index_by_name(body->slot("_SpecRampMap")).has_value());

        // the round trip: a texture that HAS a name is found by it
        CHECK(model->texture_index_by_name(model->textures[0].name).has_value());
        CHECK(model->texture_index_by_name(model->textures[0].name) == std::optional<uint16_t>{0});
        // AN EMPTY NAME MATCHES NOTHING, which is what keeps a model whose images are all unnamed (every other
        // model in this repository) from resolving every query to its first texture
        CHECK(!model->texture_index_by_name("").has_value());
    }

    /// the value of the first `<name> = <number>` in @p text, or nothing - a five-line parser, because a regex
    /// or a build dependency would be more machinery than one shared constant is worth
    std::optional<float> float_constant_of(std::string_view const text, std::string_view const name) {
        std::string const needle = std::string(name) + " =";
        std::size_t const at = text.find(needle);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        char const* const begin = text.data() + at + needle.size();
        char* end = nullptr;
        float const value = std::strtof(begin, &end);
        if (end == begin) {
            return std::nullopt;
        }
        return value;
    }

    void test_the_baked_ramp_and_the_shader_agree_on_its_width() {
        // THE ONE NUMBER THE BAKE AND THE READ SHARE, and why it needs a test rather than a comment on each
        // side: the host bakes a neutral step at x = 0.5 with half width `w`, and the shader inverts that bake
        // by reading at `0.5 + (gated - center) * (w' / softness)`. When `w != w'` the ramp STILL LOOKS LIKE A
        // RAMP - a step is a step - so nothing about the frame announces that the contract broke; what happens
        // is that the texture branch and the procedural branch stop agreeing and each family's terminator sits
        // at the wrong place. That is the silent-drift shape this project writes tests for.
        //
        // IT LIVES HERE rather than in a test of its own because it is a contract between the host's sidecar
        // LANE bake and the stage that reads that lane, which is what this test is about - and because a test
        // target of its own would have to be mirrored into VR_TEST_TARGETS, the workflow and the docs.
        std::ifstream host_file{VR_TEST_SOURCE_DIR "/main.cpp"};
        std::ifstream shader_file{VR_TEST_SOURCE_DIR "/shaders/character_forward.slang"};
        CHECK(host_file.good());
        CHECK(shader_file.good());
        if (!host_file.good() || !shader_file.good()) {
            return;
        }
        std::string const host{std::istreambuf_iterator<char>{host_file}, std::istreambuf_iterator<char>{}};
        std::string const shader{std::istreambuf_iterator<char>{shader_file}, std::istreambuf_iterator<char>{}};

        std::optional<float> const baked = float_constant_of(host, "baked_ramp_half_width");
        std::optional<float> const read_at = float_constant_of(shader, "character_ramp_half_width");
        CHECK(baked.has_value());
        CHECK(read_at.has_value());
        CHECK(baked == read_at);

        // AND THE BAKE PUTS THE STEP AT THE CENTRE THE SHADER'S REMAP ASSUMES. `0.5` is written into both the
        // bake's `smoothstep` call and the shader's remap, so asserting the two SPELLINGS is what keeps a bake
        // whose step sat anywhere else from shifting the family's highlight by the difference while both
        // constants above still matched.
        CHECK(host.find("smoothstep(0.5f - baked_ramp_half_width, 0.5f + baked_ramp_half_width") != std::string::npos);

        // ONLY THE SPECULAR LANE IS STILL ON THIS CONTRACT, and the diffusive one's departure is the 终末地 port's
        // change - so it is asserted here rather than left to the reader. The diffuse lane now reads the GAME'S
        // OWN `_DiffRampMap` as a colour mapping indexed by the light term, and its fallback is
        // `bake_neutral_ramp`, whose content (white RGB, identity lightness) inverts NOTHING: there is no neutral
        // step for it to undo and no half-width for it to divide by. The unit step still serves the specular lane,
        // whose `.r` is read as a THRESHOLD - and the host still bakes ONE unit-step asset for it, so this lane's
        // remap has to divide by the SAME `character_ramp_half_width` the bake was written at.
        CHECK(shader.find("saturate(0.5 + (no_h - params.spec_center) * (character_ramp_half_width") != std::string::npos);

        // THE SHADOW LUT'S TILE COUNT IS THE SAME KIND OF CONTRACT, and it fails the same silent way: the bake
        // lays a `32^3` cube into a `1024x32` strip of `32x32` tiles and the shader's `toon_shadow_lut` inverts
        // that layout, so a bake for a DIFFERENT tile count reads back as a different COLOUR - the lane still
        // answers, it just answers with the wrong cube - and nothing about the frame says which side moved.
        std::optional<float> const lut_tiles = float_constant_of(host, "baked_lut_tiles");
        std::optional<float> const lut_tiles_read = float_constant_of(shader, "character_shadow_lut_tiles");
        CHECK(lut_tiles.has_value());
        CHECK(lut_tiles_read.has_value());
        CHECK(lut_tiles == lut_tiles_read);

        // THE COLOUR TABLE'S STRIDE IS A THIRD CONTRACT OF THE SAME KIND, and it fails the worst of the three: the
        // shader addresses that table FLAT - `material_index * lanes + lane` - so a lane count that drifts does not
        // fail, it reads ANOTHER MATERIAL's colour, one material away per lane of drift. The host's number is an
        // enum (`vulkan::toon_colour_lane::count`) rather than a float, so the check is on the two SPELLINGS, the
        // way the ramp's `0.5` is checked above: the shader's constant and the enum's last line.
        std::ifstream primitive_file{VR_TEST_SOURCE_DIR "/vulkan/primitive/primitive.cppm"};
        CHECK(primitive_file.good());
        if (primitive_file.good()) {
            std::string const primitive{std::istreambuf_iterator<char>{primitive_file}, std::istreambuf_iterator<char>{}};
            // STEP 8 RAISED ALL FOUR OF THESE FROM 24 TO 25 (lane 24 is `_GooNormalStrength`); STEP 10 RAISED THEM
            // AGAIN TO 26 (lane 25 is `_GooAnisoGate`); STEP 12 RAISED THEM ONCE MORE TO 27 (lane 26 is
            // `_GooAnisoRough`, the anisotropic lobe's two roughnesses). Each time the numbers moved together with
            // the enum, the host's two tables and the shaders' reads.
            CHECK(shader.find("character_toon_colour_lanes = 27u") != std::string::npos);
            CHECK(primitive.find("count = 14,") != std::string::npos);
            // ... AND THE OTHER READER OF THE SAME TABLE, which carries its OWN copy of the stride because a
            // stage cannot include `character_forward.slang` without inheriting its entry point: the outline's
            // geometry stage. It reads ONE lane of the table and still needs the whole stride - a copy left at an
            // older lane count addresses a neighbouring MATERIAL's lane, which is why it is asserted here too.
            std::ifstream pbr_file{VR_TEST_SOURCE_DIR "/shaders/pbr.slang"};
            CHECK(pbr_file.good());
            if (pbr_file.good()) {
                std::string const pbr{std::istreambuf_iterator<char>{pbr_file}, std::istreambuf_iterator<char>{}};
                CHECK(pbr.find("pbr_toon_colour_lanes = 27u") != std::string::npos);
            }
            // ... AND THE THIRD READER, which step 3 added: the REWRITTEN chain's rim is a fullscreen stage of its
            // own (`shaders/goo_rim.slang`) and it reads FIVE lanes of this table, so a copy left at the old count
            // would give every material another material's rim colour, rim scalars AND rim widths - the loudest
            // form of the silent failure this check exists for. The file is spelled here rather than reached
            // through `character_forward.slang` because it is not part of that include chain at all.
            std::ifstream goo_rim_file{VR_TEST_SOURCE_DIR "/shaders/goo_rim.slang"};
            CHECK(goo_rim_file.good());
            if (goo_rim_file.good()) {
                std::string const goo_rim{std::istreambuf_iterator<char>{goo_rim_file}, std::istreambuf_iterator<char>{}};
                CHECK(goo_rim.find("goo_rim_colour_lanes = 27u") != std::string::npos);
            }
            // AND THE LANE BLOCK COUNT, the same shape one level down: lanes 8..11 ride a SECOND `uvec4` of the same
            // table, addressed as `material * blocks + 1`, so a block count that drifts reads a neighbouring
            // material's lanes exactly the way a colour-lane drift reads its colours. STEP 7 RAISED BOTH OF THESE
            // FROM 2 TO 3, because the FACE container's three masks did not fit the second block; the numbers below
            // moved with the enum, the host's `toon_lane_blocks`, the allocation and all three accessors.
            CHECK(shader.find("character_toon_lane_blocks = 3u") != std::string::npos);
            CHECK(primitive.find("toon_lane_blocks = 3") != std::string::npos);
            // AND THE STRIDE ITSELF, on ALL THREE accessors - because the failure this check exists for already
            // happened: the host moved to a two-block stride while `toon_lanes_at` still read `[material]`, so every
            // material read another material's lanes and the frame merely looked like a character. The spellings
            // below are what the two sides must agree on, and they are asserted rather than measured on a frame
            // because a wrong lane is invisible in most of them.
            std::ifstream heap_access_file{VR_TEST_SOURCE_DIR "/shaders/heap_access.slang"};
            CHECK(heap_access_file.good());
            if (heap_access_file.good()) {
                std::string const access{std::istreambuf_iterator<char>{heap_access_file}, std::istreambuf_iterator<char>{}};
                CHECK(access.find("#define toon_lanes_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u]") != std::string::npos);
                CHECK(access.find("#define toon_lanes2_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u + 1u]") != std::string::npos);
                CHECK(access.find("#define toon_lanes3_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u + 2u]") != std::string::npos);
            }
        }
    }

    void test_the_matcap_slot_does_not_follow_the_use_slot_rule() {
        // THE ONE SLOT WHOSE FLAG IS NOT `_Use<Slot>`, and it is a test because the failure is silent AND
        // one-sided: the iris in this repository's own test model switches its matcap on with `_UseMatcap`, so a
        // consumer asking `enabled("_MatcapTex")` builds `_UseMatcapTex`, finds nothing, and answers "off" -
        // correctly by this module's rule, and completely wrongly by the asset pipeline's. What that looks like
        // downstream is not an error: it is one material quietly missing a feature it asked for.
        constexpr std::string_view text =
            "material\tkind\tname\tvalue\n"
            "M_iris\tslot\t_MatcapTex\tT_matcap_10_D\n"
            "M_iris\tfloat\t_UseMatcap\t1.0\n";
        auto const parsed = toon::parse_sidecar(text);
        CHECK(parsed.has_value());
        if (!parsed.has_value()) {
            return;
        }
        toon::material_sidecar const* const iris = parsed->find("M_iris");
        CHECK(iris != nullptr);
        if (iris == nullptr) {
            return;
        }
        // the convention misses
        CHECK(!iris->enabled("_MatcapTex"));
        // the explicit name hits, and that is what the application's lane table asks with
        CHECK(iris->enabled_by_flag("_UseMatcap"));
        // A SLOT WITH NO FLAG AT ALL IS STILL OFF, whichever way it is asked - the module's first rule survives
        // the second entry point
        CHECK(!iris->enabled_by_flag("_UseSpecRampMap"));
        CHECK(!iris->enabled("_SpecRampMap"));
        // and the three lanes that DO follow the convention answer the same through both entry points
        // THE EXPECTED HAS TO BE NAMED, and this is not style. `parse_sidecar(...)->find(...)` returns a
        // pointer INTO A TEMPORARY `sidecar`, which is destroyed at the end of that statement, so `body`
        // dangles and the `enabled()` calls below read a freed `material_sidecar` - ASan reported exactly
        // that (heap-use-after-free in `scalar()`, freed by `__libcpp_deallocate<material_sidecar>`), while
        // the unsanitized run read the freed block and happened to pass. Every pointer `find()` hands out
        // borrows from the object it was called on, so the object outlives the pointer or neither is used.
        auto const parsed_good = toon::parse_sidecar(good_text);
        CHECK(parsed_good.has_value());
        if (!parsed_good.has_value()) {
            return;
        }
        toon::material_sidecar const* const body = parsed_good->find("M_body");
        CHECK(body != nullptr);
        if (body == nullptr) {
            return;
        }
        CHECK(body->enabled("_DiffRampMap"));
        CHECK(body->enabled_by_flag("_UseDiffRampMap"));
    }

    /// THE TWO ASSET-SOURCED SCALAR LANES' CONTRACT: `specular_strength` (`_Specular`) and `parallax_scale`
    /// (`_ParallaxScale`), the third data source's first two properties and the two lanes whose neutral is a
    /// SENTINEL rather than a no-op.
    ///
    /// WHY IT NEEDS A TEST AT ALL, given the stride check in the test above: the stride only says the two sides
    /// agree that the table has SIX lanes. What can still drift, silently, is (a) WHICH lane the shader reads -
    /// a wrong index reads a neighbouring material's colour and the frame merely looks odd - and (b) what "the
    /// asset states nothing" LOOKS LIKE, which is the one value on these lanes that cannot be a number in the
    /// value's own range. A host that left the lane at the white neutral would shade every material that states
    /// no `_Specular` with a full-strength highlight, and a host that sent `0` there would silently delete the
    /// highlight from every material - neither of which is visible in a log line. The parallax lane's failure is
    /// the same one lane over: white in `.x` is a depth of 1.0, i.e. an offset thirty-three times the stage's
    /// own constant.
    ///
    /// THE TWO LANES ARE CHECKED TOGETHER BECAUSE THEY ARE RESOLVED BY ONE BRANCH in the application's lookup and
    /// by one shape of read in the stage: two properties, one contract, and an edit to that shared branch is
    /// exactly the change that could break the other one.
    ///
    /// THE THREE HOST SITES ARE ALL CHECKED because all three write the neutral: the `toon_inputs` default, the
    /// table's initial fill in the runtime (which is what an unvisited lane actually holds on the GPU), and the
    /// lookup's fallback in the application.
    void test_the_two_asset_scalar_lanes_hold_on_both_sides() {
        std::ifstream shader_file{VR_TEST_SOURCE_DIR "/shaders/character_forward.slang"};
        std::ifstream primitive_file{VR_TEST_SOURCE_DIR "/vulkan/primitive/primitive.cppm"};
        std::ifstream runtime_file{VR_TEST_SOURCE_DIR "/vulkan/runtime/runtime.constructor.cppm"};
        std::ifstream host_file{VR_TEST_SOURCE_DIR "/main.cpp"};
        CHECK(shader_file.good());
        CHECK(primitive_file.good());
        CHECK(runtime_file.good());
        CHECK(host_file.good());
        if (!shader_file.good() || !primitive_file.good() || !runtime_file.good() || !host_file.good()) {
            return;
        }
        auto const slurp = [](std::ifstream& file) { return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}}; };
        std::string const shader = slurp(shader_file);
        std::string const primitive = slurp(primitive_file);
        std::string const runtime = slurp(runtime_file);
        std::string const host = slurp(host_file);

        // (a) THE LANE INDICES, spelled once in the host's enum and once in each stage read: 4 and 5 on both
        // sides, and each pair is the same number by these assertions rather than by review.
        CHECK(primitive.find("specular_strength = 4,") != std::string::npos);
        CHECK(shader.find("colour_base + 4u") != std::string::npos);
        CHECK(primitive.find("parallax_scale = 5,") != std::string::npos);
        CHECK(shader.find("+ 5u).x") != std::string::npos);
        // ... AND THE FALLBACK THE SHADER APPLIES WHEN THAT READ SAYS "NOTHING STATED". The family table must
        // still be reachable or the lane would have replaced a per-material value with a per-family default of
        // its own - the exact inversion of what it is for. The parallax lane's fallback is the stage's own
        // constant rather than a family number (the eye path is its only consumer), and that constant has to
        // still be the number the iris states or the material this lane was measured against would move too.
        CHECK(shader.find("extras_specular_strength >= 0.0 ? extras_specular_strength : params.spec_strength") != std::string::npos);
        CHECK(shader.find("extras_parallax_scale >= 0.0 ? extras_parallax_scale : character_eye_parallax_depth") != std::string::npos);
        CHECK(shader.find("character_eye_parallax_depth = 0.03") != std::string::npos);
        // ... AND THE CONSUMER IS THE CONSUMER: the one line the eye's parallax offset is built on. A lane that
        // reaches the GPU while this line still reads the constant changes nothing at all - the shape this
        // repository records for a lane whose reader was not wired - and it is invisible in a frame because it
        // looks exactly like "no bug".
        CHECK(shader.find("parallax_offset = offset_dir * parallax_depth * float2(1.0, 0.25)") != std::string::npos);

        // (b) WHAT "NOTHING STATED" IS, at every site that writes it. Each search starts at the site's own
        // symbol and looks forward, so an unrelated `-1` elsewhere in the file cannot satisfy it.
        std::size_t const toon_inputs_at = primitive.find("std::array<glm::vec4, static_cast<std::size_t>(toon_colour_lane::count)> colours");
        CHECK(toon_inputs_at != std::string::npos);
        if (toon_inputs_at != std::string::npos) {
            CHECK(primitive.find("glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)", toon_inputs_at) != std::string::npos);
        }
        std::size_t const fill_at = runtime.find("toon_colour_lane::specular_strength");
        CHECK(fill_at != std::string::npos);
        if (fill_at != std::string::npos) {
            CHECK(runtime.find("glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)", fill_at) != std::string::npos);
        }
        // the parallax lane's own initial fill in the runtime's table
        std::size_t const parallax_fill_at = runtime.find("toon_colour_lane::parallax_scale");
        CHECK(parallax_fill_at != std::string::npos);
        if (parallax_fill_at != std::string::npos) {
            CHECK(runtime.find("glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)", parallax_fill_at) != std::string::npos);
        }
        std::size_t const neutral_at = host.find("toon_colour_neutral = {");
        CHECK(neutral_at != std::string::npos);
        if (neutral_at != std::string::npos) {
            // TWO sentinels in that table now, one per scalar lane - so the search has to find a SECOND one:
            // a table that kept only the specular lane's would leave the parallax lane at white, i.e. 1.0.
            std::size_t const first_sentinel = host.find("glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)", neutral_at);
            CHECK(first_sentinel != std::string::npos);
            if (first_sentinel != std::string::npos) {
                CHECK(host.find("glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)", first_sentinel + 1) != std::string::npos);
            }
        }

        // (c) THE ASSET-SIDE SPELLING, which is what the host asks the loader for: the claimed table names the
        // rows the shader's lanes carry. A rename on either side is a material that silently keeps the value it
        // had before the lane existed - the state before this lane - so the spellings are pinned together.
        std::ifstream loader_file{VR_TEST_SOURCE_DIR "/gltf_loader/gltf_loader.cppm"};
        CHECK(loader_file.good());
        if (loader_file.good()) {
            std::string const loader = slurp(loader_file);
            CHECK(loader.find("claimed_extras_floats = {\"_Specular\", \"_ParallaxScale\"}") != std::string::npos);
        }
        // ... AND THE HOST'S OWN ROW TABLE, which is the name the lookup asks the loader with: `_ParallaxScale`
        // reaching the whitelist but not this table would import the row and never ask for it.
        CHECK(host.find("\"_ParallaxScale\",") != std::string::npos);
        CHECK(host.find("extras_float_of(state.scenes, material_name, toon_colour_row[lane_index])") != std::string::npos);
    }

    /// THE MATERIAL DEDUP KEY CARRIES EVERYTHING THE MATERIAL RECORD DOES NOT - the two texture lane blocks AND
    /// the six colour lanes.
    ///
    /// WHY THE KEY AT ALL, since the third contract above already pins the colour TABLE's stride: the stride says
    /// the two sides agree on how WIDE a row is. It says nothing about which materials are allowed to SHARE one,
    /// and that is this key's job: `register_material` looks the key up BEFORE it writes anything, and returns the
    /// existing index when it hits. A per-material thing that is not in the key is therefore not a missing feature
    /// for the second material - it is the FIRST material's value, silently, and the frame looks like a rendering
    /// result rather than like a mistake.
    ///
    /// THE FAILURE THIS PINS IS INVISIBLE ON EVERY ASSET IN THIS REPOSITORY (chen's seven materials all have
    /// different records, so the `toon: material N` lines are all present and correct), which is exactly why the
    /// check is on the KEY'S COMPOSITION rather than on a frame: the probe that makes it visible needs a second
    /// material with a byte-identical record and identical texture lanes, and that asset is built, measured and
    /// deleted in `remaining_port_spec.md`'s "材质去重键补上 colour lanes" section. MEASURED THERE, on the pre-fix tree: a copy of
    /// `M_actor_chen_hair_01` stating `_Specular = 0.0`, registered one node earlier than the hair, made the
    /// hair render BYTE-FOR-BYTE like an asset whose hair states 0.0 - the hair's own 1.0 discarded.
    void test_the_material_dedup_key_carries_the_colour_lanes() {
        std::ifstream declarations_file{VR_TEST_SOURCE_DIR "/vulkan/runtime/runtime.declarations.cppm"};
        std::ifstream runtime_file{VR_TEST_SOURCE_DIR "/vulkan/runtime/runtime.constructor.cppm"};
        std::ifstream primitive_file{VR_TEST_SOURCE_DIR "/vulkan/primitive/primitive.cppm"};
        CHECK(declarations_file.good());
        CHECK(runtime_file.good());
        CHECK(primitive_file.good());
        if (!declarations_file.good() || !runtime_file.good() || !primitive_file.good()) {
            return;
        }
        auto const slurp = [](std::ifstream& file) { return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}}; };
        std::string const declarations = slurp(declarations_file);
        std::string const runtime = slurp(runtime_file);
        std::string const primitive = slurp(primitive_file);

        // (a) THE THREE COMPONENTS, in ONE spelling: the key's TYPE (declared beside `material_slot_cache`) and
        // the local key the constructor fills are asserted to be the SAME TEXT, not merely the same size - a size
        // that happened to match while the bytes meant something else is the way this kind of key goes wrong. The
        // compiler already forces the two sizes to agree (a mismatch is a type error at `find`), so what is left
        // for a test is the reading: record, then the texture lane blocks, then the colour lanes.
        std::string const key_type =
            "utility::data_block<sizeof(vulkan::material_record) + vulkan::toon_lane_blocks * sizeof(glm::uvec4) + static_cast<std::size_t>(vulkan::toon_colour_lane::count) * sizeof(glm::vec4)>";
        CHECK(declarations.find(key_type) != std::string::npos);
        CHECK(runtime.find(key_type) != std::string::npos);
        // ... AND IT IS THE ENUM RATHER THAN A NUMBER, which is the drift this repository has already paid for
        // once: `count = 10` today, and a hand-written 4 or 5 would leave the NEWEST lanes - the two the asset's
        // own `extras` block speaks for, the rewritten chain's iris brightnesses, its two rim lanes and its
        // screen-space rim widths - out of the key while every existing asset continued to look right.
        CHECK(primitive.find("count = 14,") != std::string::npos);
        CHECK(primitive.find("toon_lane_blocks = 3") != std::string::npos);

        // (b) THE BYTES ACTUALLY GO IN, from the array the table is filled from: keying anything else (the
        // neutral, a copy taken before the lookup resolved, a differently ordered row) would dedup on the wrong
        // set of lanes and split or merge the wrong pairs.
        CHECK(runtime.find("info.toon.colours.data(),") != std::string::npos);
        CHECK(runtime.find("static_cast<std::size_t>(vulkan::toon_colour_lane::count) * sizeof(glm::vec4));") != std::string::npos);

        // (c) THE ORDER THAT MAKES THE COLOUR TERM NECESSARY, asserted because it is the reason and not an
        // accident: the key is COMPLETE before the lookup, and the table write sits AFTER it. If a later change
        // moved the write above the early return, the key would no longer need the lanes and this check is the
        // line that says so out loud - revisit the term and this test together, do not just delete the check.
        std::size_t const key_at = runtime.find("material_key = {}");
        std::size_t const colours_at = runtime.find("info.toon.colours.data(),");
        std::size_t const lookup_at = runtime.find("this->material_slot_cache.find(material_key)");
        std::size_t const write_at = runtime.find("colours[lane] = info.toon.colours[lane];");
        CHECK(key_at != std::string::npos);
        CHECK(colours_at != std::string::npos);
        CHECK(lookup_at != std::string::npos);
        CHECK(write_at != std::string::npos);
        if (key_at != std::string::npos && colours_at != std::string::npos && lookup_at != std::string::npos && write_at != std::string::npos) {
            CHECK(key_at < colours_at);    // the colour bytes are part of the key...
            CHECK(colours_at < lookup_at); // ...and are in it BY THE TIME it is looked up...
            CHECK(lookup_at < write_at);   // ...because the table write happens after the early return
        }
    }

} // namespace

int32_t main() {
    test_well_formed();
    test_crlf_and_blank_lines();
    test_header_is_recognised_by_its_column_not_its_position();
    test_a_malformed_row_is_an_error_with_its_line_number();
    test_a_non_numeric_float_is_an_error();
    test_rows_appended_for_an_existing_material_merge_into_its_entry();
    test_the_path_convention_appends_rather_than_replaces();
    test_a_missing_file_is_an_empty_sidecar_and_not_an_error();
    test_a_file_on_disk_is_read_through_the_convention();
    test_a_malformed_file_on_disk_reports_its_path();
    test_the_sidecar_and_the_model_join_by_texture_name();
    test_the_baked_ramp_and_the_shader_agree_on_its_width();
    test_the_matcap_slot_does_not_follow_the_use_slot_rule();
    test_the_two_asset_scalar_lanes_hold_on_both_sides();
    test_the_material_dedup_key_carries_the_colour_lanes();
    return vk_test::finish("test_toon_material_sidecar");
}
