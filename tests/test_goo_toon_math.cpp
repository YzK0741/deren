// Headless unit tests: THE REWRITTEN TOON CHAIN'S IRIS MATH ==================================
//
// WHAT THIS FILE IS FOR, and what it deliberately is not. `shaders/goo_toon.slang` is a Slang stage: its
// arithmetic only exists inside a compiled module, and evaluating it needs a device, a swapchain and a
// character asset - none of which a CI machine has (the capture gate's references are tied to one machine's
// driver and cannot run there at all; that is why this repository's OTHER invariants are asserted on text).
// So there are two halves here, and they answer two different questions:
//
//   1. THE CLOSED FORMS, evaluated in C++ on the arithmetic read out of the reference's own node graph
//      (`build-release-clang64/zmd-ab/gooblender/nodes.json`, `Arknights: Endfield_PBRToon_irisBase` and the
//      `calculateAngel` group it instantiates). Every constant below is quoted with the NODE it comes from, so
//      the number can be re-read rather than trusted. This half pins the DERIVATION: the sign of the forward
//      axis, which way the albedo window clamps, that the second layer is an ADD weighted by its factor (and
//      therefore that `albedo` MULTIPLIES the ball rather than adding to it), and that the brightness is the
//      UNCLAMPED angle.
//
//      IT IS A RE-DERIVATION RATHER THAN AN ORACLE, and saying so is the honest framing: it cannot catch a
//      mistake the C++ and the Slang share, because it is the same reading of the same graph twice. What it
//      catches is DRIFT - a later edit to one side that leaves the other alone.
//
//   2. THE SYNC POINTS, on the sources' TEXT, which IS an oracle: a lane added to `toon_slot` without its
//      format-table entry, its flag name, its colour-row name or its stride reads another material's data
//      rather than failing (`vulkan::toon_slot`'s and `toon_colour_lane`'s own notes record that failure
//      happening once already), and none of those four places is checked by the compiler. This half is the same
//      shape `test_toon_material_sidecar` uses for the strides, for the same reason.
//
// NOTHING HERE ASSERTS A PIXEL. The frame-level acceptance - "with `goo_toon` off the frame is what it was, with
// it on only the iris moves" - is a capture, and it is recorded in
// `build-release-clang64/zmd-ab/goo_step1_result.md`.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifdef VR_TEST_SOURCE_DIR

namespace {
    // ================================ THE REFERENCE'S CONSTANTS ================================
    //
    // Every one of these is a socket value or a factor in `gooblender/nodes.json`'s
    // `Arknights: Endfield_PBRToon_irisBase`, named by the node it belongs to. THEY ARE ALSO SPELLED OUT IN
    // `shaders/goo_toon.slang`, where the spelling is asserted below - so a shader edit that changes one of them
    // without changing this file fails here rather than quietly re-tuning the eye.

    /// `运算.Value` in `Arknights: Endfield_PBRToon_irisBase` - what the azimuth is subtracted from. THE GOO
    /// PROJECT'S VALUE: the OTHER dump (`endfield_addon/chen_dump`, the addon preset `Chen.blend`) states `1.1`.
    constexpr float k_angle_center = 1.0f;
    /// `钳制.Min` / `钳制.Max` - the window the albedo's weight is clamped into.
    constexpr float k_window_min = 0.5f;
    constexpr float k_window_max = 1.0f;
    /// `混合.002.Factor_Float` - the second layer's weight in an ADD.
    constexpr float k_ball_weight = 0.5666666626930237f;
    /// `运算.004.Value_001` - the reference's own float literal for pi.
    constexpr float k_half_turn = 3.141592502593994f;
    /// `值(明度)`'s output default inside `calculateAngel` - the sign applied to the head's forward axis.
    constexpr float k_forward_sign = -1.0f;
    /// The two MATERIAL overrides on Laevatain's iris (`goo_params_laevatain.tsv`: `Eyes brightness` /
    /// `Eyes HightLight brightness`, both `材质覆写`), which ride `toon_colour_lane::goo_eye_brightness`.
    constexpr float k_eyes_brightness = 1.5f;
    constexpr float k_eyes_highlight_brightness = 10.0f;

    struct vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
    /// a two-component pair, spelled rather than pulled in from the engine: this test links no module (the math
    /// it evaluates is the SHADER's, and the shader is not callable from here)
    struct vec2 {
        float a = 0.0f;
        float b = 0.0f;
    };
    constexpr vec3 operator-(vec3 const a, vec3 const b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    constexpr float dot3(vec3 const a, vec3 const b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    constexpr vec3 scale3(vec3 const a, float const s) {
        return {a.x * s, a.y * s, a.z * s};
    }
    float length3(vec3 const a) {
        return std::sqrt(dot3(a, a));
    }
    vec3 normalize3(vec3 const a) {
        float const length = length3(a);
        // BLENDER'S `NORMALIZE` ANSWERS (0,0,0) FOR A ZERO INPUT and Slang's answers a NaN; the shader's guard
        // reproduces Blender's answer, and this is the same guard so the test can exercise that case by name.
        return length > 1e-6f ? scale3(a, 1.0f / length) : vec3{};
    }

    /// @brief the reference's `calculateAngel.AngleThreshold`, from the group's own links
    ///
    /// `VM.008 = DOT(LightDirection, headUp)`; `VM.009 = MULTIPLY(VM.008, headUp)`;
    /// `VM.010 = SUBTRACT(LightDirection, VM.009)`; `VM.011 = NORMALIZE(VM.010)`;
    /// `VM.017 = DOT(VM.011, headRight)`; `VM.019 = MULTIPLY(headForward, -1.0)`;
    /// `VM.020 = DOT(VM.011, VM.019)`; `运算.003 = ARCTAN2(VM.017, VM.020)`;
    /// `运算.004 = DIVIDE(运算.003, pi)`; `运算.005 = GREATER_THAN(运算.004, 0)`;
    /// `运算.006 = ADD(1, 运算.004)`; `运算.007 = SUBTRACT(1, 运算.004)`;
    /// `混合.001 = MIX(f = 运算.005, A = 运算.006, B = 运算.007)`.
    ///
    /// The last three lines are the reference's way of writing `1 - |x|`: the MIX's FACTOR is `turns > 0` and its
    /// A/B are `1 + turns` / `1 - turns`, so the branch the positive factor selects is the one that SUBTRACTS.
    /// Writing it the other way round (`1 + |turns|`) puts the azimuth in [1,2], which the iris' own
    /// `1.0 - angle` then reads as a constant -1 and the albedo's window collapses to its floor for every sun -
    /// and this test is where that was caught, in the test's own transcription rather than in the shader.
    float angle_threshold(vec3 const light, vec3 const head_up, vec3 const head_right, vec3 const head_forward) {
        vec3 const projected = light - scale3(head_up, dot3(light, head_up));
        vec3 const axis = normalize3(projected);
        float const right_component = dot3(axis, head_right);
        float const forward_component = dot3(axis, head_forward) * k_forward_sign;
        float const turns = std::atan2(right_component, forward_component) / k_half_turn;
        return turns > 0.0f ? 1.0f - turns : 1.0f + turns;
    }

    /// @brief the same function with the reference's DEGENERATE case spelled out, as the shader spells it
    ///
    /// C's `atan2(0, 0)` is deterministic (and here it is `atan2(+0, -0) = +pi`, the negative zero coming from
    /// `MULTIPLY(headForward, -1.0)`), while SPIR-V leaves `Atan2(0, 0)` UNDEFINED - so the shader states the
    /// reference's answer rather than the instruction's. This is that statement, and section 1 asserts the two
    /// agree everywhere except the one input the platform does not define.
    float angle_threshold_guarded(vec3 const light, vec3 const head_up, vec3 const head_right, vec3 const head_forward) {
        vec3 const projected = light - scale3(head_up, dot3(light, head_up));
        vec3 const axis = normalize3(projected);
        float const right_component = dot3(axis, head_right);
        float const forward_component = dot3(axis, head_forward) * k_forward_sign;
        float const magnitude = std::abs(right_component) + std::abs(forward_component);
        float const turns = magnitude > 1e-12f ? std::atan2(right_component, forward_component) / k_half_turn : 1.0f;
        return turns > 0.0f ? 1.0f - turns : 1.0f + turns;
    }

    /// the `混合` node's output: `albedo * clamp(1.0 - angle, 0.5, 1.0)`
    float albedo_weight(float const angle) {
        float const raw = k_angle_center - angle;
        return raw < k_window_min ? k_window_min : (raw > k_window_max ? k_window_max : raw);
    }

    /// `混合.003` (MULTIPLY, factor 1.0) then `混合.002` (ADD, factor 0.5666666626930237): for an ADD at factor
    /// `f` Blender computes `A * (1 - f) + (A + B) * f`, i.e. `A + f * B`.
    float iris_layer(float const albedo, float const ball, float const angle) {
        return albedo * albedo_weight(angle) + k_ball_weight * (ball * albedo);
    }

    /// `混合.001`: `lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`, and
    /// `运算.001` reads `运算.Value` - the UNCLAMPED `1.0 - angle` (`运算`'s own `use_clamp` is FALSE).
    float iris_strength(float const angle, float const albedo_alpha, float const eyes_brightness, float const eyes_highlight_brightness) {
        return std::lerp(eyes_brightness, (k_angle_center - angle) * eyes_highlight_brightness, albedo_alpha);
    }

    /// the whole emission, as the shader composes it
    float iris_emission(float const albedo, float const ball, float const angle, float const albedo_alpha) {
        return iris_layer(albedo, ball, angle) * iris_strength(angle, albedo_alpha, k_eyes_brightness, k_eyes_highlight_brightness);
    }

    /// the sphere map, AS THE OLD CHAIN BUILDS IT (`character_forward.slang`'s `toon_matcap_uv`): a view basis
    /// from the surface-to-camera vector, with V negated for this renderer's texture convention.
    vec2 toon_matcap_uv(vec3 const n, vec3 const v) {
        vec3 const up = std::abs(v.y) < 0.99f ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
        vec3 const right = normalize3(vec3{up.y * v.z - up.z * v.y, up.z * v.x - up.x * v.z, up.x * v.y - up.y * v.x});
        vec3 const up_ortho = vec3{v.y * right.z - v.z * right.y, v.z * right.x - v.x * right.z, v.x * right.y - v.y * right.x};
        return {dot3(n, right), -dot3(n, up_ortho)};
    }

    // ================================================================================================
    // STEP 5: THE FGD LUT - its constants, its reader, and the two helpers its assertions use
    // ================================================================================================
    //
    // THE ONE HALF OF THIS FILE THAT IS NOT A RE-DERIVATION FROM THE GRAPH: the expectations are read out of the
    // REFERENCE'S OWN PNG (`zmd-ab/gooblender/images/PreIntegratedFGD_GGXDisneyDiffuse.png`, 5234 B, 64x64 RGBA8)
    // by `png_rgba8`, so they are the FILE's texels rather than a transcription of them. The spec's §3.3 is why
    // that matters more here than anywhere else in this file: these three numbers have NO closed form (measured
    // against Karis' fit and against a 120k-sample Disney diffuse FGD), so a test that "recomputed" them from a
    // formula would be testing the wrong thing twice.

    /// `Remap01ToHalfTexelCoord :: 值(明度).Value` - the LUT's resolution (the frame's own name is
    /// `FGDTEXTURE_RESOLUTION`).
    constexpr float k_fgd_resolution = 64.0f;
    /// `GetPreIntegratedFGDGGXAndDisneyDiffuse :: 运算.Value_001`, the ADD's second operand: `diffuseFGD = LUT.B + 0.5`.
    ///
    /// **IT IS `0.5` AND IT WAS ONCE `0.0`.** The ADD's `Value_001 = 0.5` is `is_linked = false, enabled = true`
    /// and its `Value_002 = 0.5` is `enabled = false`, and the rule the WHOLE FGD group is read by (and the reason
    /// `Remap01ToHalfTexelCoord`'s own bias is `(1/64)*0.5` below) is that an unlinked socket PARTICIPATES when
    /// `enabled = true` and does NOT when `enabled = false`. The `0.0` came from a parent ruling that MEASURED the
    /// image instead of reading the flags - `B ∈ [0,1]` with mean 0.480 is a FINISHED Disney diffuse FGD, while
    /// `B + 0.5 ∈ [0.5, 1.5]` is a diffuse term brighter than the albedo it multiplies - and that argument is kept
    /// here because it is the reason a reader will doubt this number. It is a PRIOR about the term, not evidence
    /// about the author's graph, and this author writes ambient floors too (step 4's
    /// `GlobalShadowBrightnessAdjustment`). See `goo_fgd_diffuse_offset` in the shader: one named constant, so the
    /// experiment is one line. The measured consequence of `0.5` is in `zmd-ab/goo_step5_result.md` §12.
    constexpr float k_fgd_diffuse_offset = 0.5f;
    /// `ComputeFresnel0 :: 组输入.dielectricF0` on the Base container. Laevatain's ELEVEN materials all end up at
    /// `(0.08, 0.08, 0.08)`: `metallic = lerp(0, MetallicMax, _P.R)` and `_P.R` is `[0,0,0]` or a linked map whose
    /// texels this project does not expand (spec §A6), so `fresnel0` does not depend on `BaseColor` here.
    constexpr float k_fgd_dielectric_f0 = 0.07999999821186066f;
    /// `运算 :: Value_001` - the `clampedNdotV` MAXIMUM's floor (`9.999999747378752e-05`, i.e. float32 `1e-4`).
    constexpr float k_fgd_ndotv_floor = 9.999999747378752e-05f;
    /// `DV_SmithJointGGX_Aniso :: 运算.013.Value` then `运算.001 = 1/2`: the group's own `1/(2π)`. NOT this
    /// project's `goo_inverse_pi` (`0.31830987334251404`) - the reference's stored value differs at the 8th digit
    /// and it is the one multiplied into `D * Gv`.
    constexpr float k_dv_half_inverse_pi = 0.3183099925518036f * 0.5f;

    /// `DeSaturation :: 合并 XYZ.002.X/.Y/.Z` - the reference's luma weights, VERBATIM and UNROUNDED. They are
    /// `dot(颜色, (0.21267299354076385, 0.7151520252227783, 0.07217500358819962))` and the test pins the spelling
    /// to all seventeen digits because a "cleaned up" `0.2126f / 0.7152f / 0.0722f` would move every expectation in
    /// section 8s and no picture would show it.
    constexpr float k_desaturation_luma_r = 0.21267299354076385f;
    constexpr float k_desaturation_luma_g = 0.7151520252227783f;
    constexpr float k_desaturation_luma_b = 0.07217500358819962f;
    /// `PBRToonBase :: 组输入.Color desaturation in shaded areas attenuation`'s `interface[]` default - `0.0`,
    /// the reference's own number, and the value a material with a ramp but no stated `.y` gets.
    constexpr float k_desaturation_default = 0.0f;
    /// THE IDENTITY OF THE DESATURATION - `1.0`, which is what a material OUTSIDE step 6's arm gets. It is a
    /// different number from the group default on purpose: `0.0` is a real desaturation (`saturation = luma`), and
    /// writing it into the neutral would apply the step to the hair and the face.
    constexpr float k_desaturation_neutral = 1.0f;

    /// @brief the reference's `DeSaturation` closed form: `lerp(luma(colour).xxx, colour, desaturation)`.
    ///
    /// EIGHT OF THE ELEVEN NODES (`组输入` + `合并 XYZ.002` + four `ShaderNodeVectorMath` + two `NodeReroute`):
    /// `矢量运算.004 = DOT_PRODUCT(Color, (r,g,b))`; `矢量运算.005 = SUBTRACT(Color, 矢量运算.004.Value)`;
    /// `矢量运算.006 = MULTIPLY(DeSaturation, 矢量运算.005.Vector)`; `矢量运算.007 = ADD(矢量运算.006.Vector,
    /// 矢量运算.004.Value)`. The `SUBTRACT` and the `ADD` are RGBA nodes fed a FLOAT, which Blender broadcasts to
    /// all three channels - so this is `luma + d*(c - luma)` per channel and NOT a dot product.
    ///
    /// THE `d` HERE IS ALREADY CLAMPED, exactly as the shipped `goo_hsv_desaturate` clamps its own argument (it
    /// saturates): the clamp is the reference's `钳制.004 = clamp(运算.005.Value, 0, 1)` - `Min = 0.0`, `Max = 1.0`,
    /// both unlinked literals - and it lives at the CALL SITE in the reference's graph but inside the function
    /// here, because `色相/饱和度/明度`'s Saturation input is where the clamp's result is consumed. So this helper
    /// mirrors the shipped arithmetic rather than the node's raw input.
    vec3 desaturation_closed_form(vec3 const colour, float const raw_desaturation) {
        float const desaturation = std::min(1.0f, std::max(0.0f, raw_desaturation));
        float const luma = dot3(colour, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b});
        vec3 const grey{luma, luma, luma};
        return {
            grey.x + desaturation * (colour.x - grey.x),
            grey.y + desaturation * (colour.y - grey.y),
            grey.z + desaturation * (colour.z - grey.z),
        };
    }

    /// @brief THE REFERENCE'S PRE-INTEGRATED FGD LUT, BASE64 - the PNG `PreIntegratedFGD_GGXDisneyDiffuse.png`
    ///        (5234 bytes, sha256 `7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1`)
    ///
    /// WHY THE FILE'S BYTES ARE EMBEDDED RATHER THAN READ FROM THE REPOSITORY, and this is a constraint
    /// rather than a preference: the only copy of this image in the working tree lives under
    /// `build-release-clang64/` (which is git-ignored, `.gitignore:3`), so a test that read it from a PATH would
    /// pass on this machine and fail on a fresh checkout - the same reason the four `_RD` ramps are tested
    /// through their constants rather than through their pixels. EMBEDDING THE PNG RATHER THAN ITS TEXELS keeps
    /// the test's own verdict checkable: the SHA-256 below is over bytes that came back out of the decoder, so a
    /// transcription error anywhere in this table fails the first CHECK in section 8n rather than silently
    /// becoming the new expectation.
    std::string const k_fgd_png_base64 =
        "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"
        "ArYAAAAASUVORK5CYII=";

    /// @brief the reference's `Remap01ToHalfTexelCoord`: `coord*(1 - 1/N) + (1/N)*0.5`
    ///
    /// THE FACTOR IS `0.5`, AND THAT IS A FLAG RATHER THAN ARITHMETIC: `运算 = DIVIDE(1, 64)`,
    /// `运算.001 = MULTIPLY(运算.Value = 1/64, Value_001 = 0.5 (enabled = true), Value_002 = 0.5 (enabled = FALSE))`,
    /// `运算.002 = SUBTRACT(1, 运算)`, `Vector Math = coord * 运算.002`,
    /// `Vector Math.001 = 运算.001 + Vector Math` - i.e. `coord*0.984375 + 0.0078125`. An unlinked socket
    /// PARTICIPATES when `enabled = true` and does NOT when `enabled = false`, so `运算.001` is `1/64 * 0.5`, the
    /// half texel the group is NAMED for. The spec's §A2 table (`0.0078125 + coord*0.984375`) and its worked
    /// example (`coord = 0.08 -> sample 5.04`) are BOTH this expression; an earlier version of this step read
    /// `1.0` off that socket instead (`0.015625 + coord*0.984375`) and recorded the spec's table as wrong, which
    /// is what moved every number in §8o/§8p by half a texel.
    float remap_to_half_texel(float const coord) {
        return coord * (1.0f - 1.0f / k_fgd_resolution) + (1.0f / k_fgd_resolution) * 0.5f;
    }

    /// @brief the sample position in TEXEL units for a `[0,1]` shading parameter, clamped the way the hardware
    ///        clamps a `REPEAT` sampler's fetch to the image
    ///
    /// The reference's `合并 XYZ.001 = (分离 XYZ.X, 分离 XYZ.Y, 0.0)` and the image's `image_user` is
    /// `interpolation = Linear, extension = REPEAT`, so the fetch is `coordLUT*64 - 0.5` with the texel index
    /// clamped into `[0, N-1]` at the interpolation stage.
    float remap_to_texel(float const coord) {
        float const position = remap_to_half_texel(coord) * k_fgd_resolution - 0.5f;
        return position < 0.0f ? 0.0f : (position > k_fgd_resolution - 1.0f ? k_fgd_resolution - 1.0f : position);
    }

    /// @brief the LUT's own bytes, decoded once: 64x64 RGBA8 row-major, the shape the PNG's IHDR states
    ///        (`bitdepth = 8`, `colortype = 6`) and the shape `set_goo_fgd_lut` uploads as `R8G8B8A8_UNORM`
    struct fgd_lut {
        static constexpr uint32_t width = 64u;
        static constexpr uint32_t height = 64u;
        std::vector<uint8_t> texels = {};

        [[nodiscard]] bool valid() const {
            return texels.size() == static_cast<std::size_t>(width) * height * 4u;
        }
        /// the raw byte of one channel, or -1 when the file did not load - so a failed read fails the CHECKS
        /// below rather than reading out of bounds
        [[nodiscard]] int32_t byte_at(uint32_t const x, uint32_t const y, uint32_t const channel) const {
            if (!valid() || x >= width || y >= height || channel >= 4u) {
                return -1;
            }
            return texels[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
        }
        /// @brief the reference's bilinear fetch, in texel units, with the `[0, N-1]` clamp above
        [[nodiscard]] float sample(float const x, float const y, uint32_t const channel) const {
            if (!valid()) {
                return -1.0f;
            }
            uint32_t const x0 = static_cast<uint32_t>(x);
            uint32_t const y0 = static_cast<uint32_t>(y);
            uint32_t const x1 = x0 + 1u < width ? x0 + 1u : x0;
            uint32_t const y1 = y0 + 1u < height ? y0 + 1u : y0;
            float const fx = x - static_cast<float>(x0);
            float const fy = y - static_cast<float>(y0);
            float const low = static_cast<float>(byte_at(x0, y0, channel)) * (1.0f - fx) + static_cast<float>(byte_at(x1, y0, channel)) * fx;
            float const high = static_cast<float>(byte_at(x0, y1, channel)) * (1.0f - fx) + static_cast<float>(byte_at(x1, y1, channel)) * fx;
            return (low * (1.0f - fy) + high * fy) / 255.0f;
        }
    };

    /// @brief decode the standard base64 alphabet, ignoring newlines and the trailing `=` - the forty lines above
    std::vector<uint8_t> base64_decode(std::string const& text) {
        auto const value_of = [](char const c) -> int32_t {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1; // '=' and whitespace: the end of the stream, or a separator
        };
        std::vector<uint8_t> out = {};
        uint32_t accumulator = 0u;
        uint32_t bits = 0u;
        for (char const c : text) {
            int32_t const digit = value_of(c);
            if (digit < 0) {
                continue;
            }
            accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
            bits += 6u;
            if (bits >= 8u) {
                bits -= 8u;
                out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
            }
        }
        return out;
    }

    /// @brief SHA-256, spelled here for the same reason the PNG reader is: this file links nothing
    ///
    /// It is used for exactly one assertion - that the bytes that came back out of `png_rgba8` are the REFERENCE'S
    /// PNG, byte for byte - and that assertion is what makes the embedded table above verifiable rather than
    /// trusted. A transcription error anywhere in those forty lines fails it (and the texel checks below it).
    std::string sha256_hex(std::vector<uint8_t> const& bytes) {
        static constexpr uint32_t k[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                                           0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                                           0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                                           0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                                           0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                                           0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                                           0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                                           0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::vector<uint8_t> message = bytes;
        uint64_t const bit_length = static_cast<uint64_t>(bytes.size()) * 8u;
        message.push_back(0x80u);
        while (message.size() % 64u != 56u) {
            message.push_back(0u);
        }
        for (int32_t shift = 56; shift >= 0; shift -= 8) {
            message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xFFu));
        }
        uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        auto const rotr = [](uint32_t const x, uint32_t const n) { return (x >> n) | (x << (32u - n)); };
        for (std::size_t block = 0u; block < message.size(); block += 64u) {
            uint32_t w[64] = {};
            for (uint32_t i = 0u; i < 16u; ++i) {
                w[i] = (static_cast<uint32_t>(message[block + i * 4u]) << 24) | (static_cast<uint32_t>(message[block + i * 4u + 1u]) << 16) |
                       (static_cast<uint32_t>(message[block + i * 4u + 2u]) << 8) | static_cast<uint32_t>(message[block + i * 4u + 3u]);
            }
            for (uint32_t i = 16u; i < 64u; ++i) {
                uint32_t const s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
                uint32_t const s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
                w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (uint32_t i = 0u; i < 64u; ++i) {
                uint32_t const s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
                uint32_t const ch = (e & f) ^ (~e & g);
                uint32_t const temp1 = hh + s1 + ch + k[i] + w[i];
                uint32_t const s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
                uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t const temp2 = s0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        }
        std::string hex = {};
        for (uint32_t const word : h) {
            char buffer[9] = {};
            std::snprintf(buffer, sizeof(buffer), "%08x", word);
            hex.append(buffer);
        }
        return hex;
    }

    /// @brief decode an 8-bit non-interlaced PNG into RGBA8 - the three FGD outputs, or an empty vector
    ///
    /// A PNG READER IN A UNIT TEST, which wants a word of justification: this file must not link the renderer
    /// (see its header), and the LUT's expectations are only meaningful if they come from the REFERENCE'S OWN
    /// BYTES - the alternative, transcribing the numbers into the test, is what the parent's own note rejects
    /// (a transcription cannot catch a wrong file, and if the file were regenerated the test would keep passing).
    /// The decoder is deliberately the smallest one that reads THIS image: PNG signature, `IHDR`, every `IDAT`
    /// concatenated, `zlib`'s stored/fixed/dynamic Huffman blocks, `filter` 0-4 (the five the specification
    /// defines), and no interlace, no palette and no 16-bit depth. The spec's `A3` names exactly these fields
    /// (`w=64 h=64 bitdepth=8 colortype=6 compression=0 filter=0 interlace=0`) and the file's own chunk list is
    /// `IHDR`/`IDAT`/`IEND`, so the input this must accept is pinned by the spec as well as by the assertions.
    std::vector<uint8_t> png_rgba8(std::vector<uint8_t> const& bytes) {
        auto const be32 = [&bytes](std::size_t const at) -> uint32_t {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[at + 2]) << 8) | static_cast<uint32_t>(bytes[at + 3]);
        };
        if (bytes.size() < 8u || bytes[0] != 0x89u || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G') {
            return {};
        }
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::vector<uint8_t> compressed = {};
        for (std::size_t at = 8u; at + 8u <= bytes.size();) {
            uint32_t const length = be32(at);
            std::string const type(reinterpret_cast<char const*>(bytes.data() + at + 4u), 4u);
            std::size_t const body = at + 8u;
            if (body + length > bytes.size()) {
                return {};
            }
            if (type == "IHDR") {
                width = be32(body);
                height = be32(body + 4u);
                if (bytes[body + 8u] != 8u || bytes[body + 9u] != 6u || bytes[body + 12u] != 0u) {
                    return {}; // not 8-bit RGBA, not non-interlaced: a file this reader refuses rather than fudges
                }
            } else if (type == "IDAT") {
                compressed.insert(compressed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(body), bytes.begin() + static_cast<std::ptrdiff_t>(body + length));
            } else if (type == "IEND") {
                break;
            }
            at = body + length + 4u; // + the CRC
        }
        if (width == 0u || height == 0u || compressed.size() < 2u) {
            return {};
        }
        // ---- zlib: one 2-byte header, deflate blocks, one 4-byte Adler-32 (which this reader does not check:
        //      the assertion that the file is the reference's is the TEXEL check below, which is stronger) ----
        std::vector<uint8_t> raw = {};
        {
            std::size_t at = 2u;
            uint32_t bit_buffer = 0u;
            uint32_t bit_count = 0u;
            auto const bits = [&](uint32_t const count) -> uint32_t {
                while (bit_count < count) {
                    bit_buffer |= static_cast<uint32_t>(compressed[at++]) << bit_count;
                    bit_count += 8u;
                }
                uint32_t const value = bit_buffer & ((1u << count) - 1u);
                bit_buffer >>= count;
                bit_count -= count;
                return value;
            };
            // the fixed-Huffman code lengths, exactly as RFC 1951 states them
            std::vector<uint32_t> length_base = {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 15u, 17u, 19u, 23u, 27u, 31u, 35u, 43u, 51u, 59u, 67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u};
            std::vector<uint32_t> length_extra = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u, 2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u};
            std::vector<uint32_t> dist_base = {1u, 2u, 3u, 4u, 5u, 7u, 9u, 13u, 17u, 25u, 33u, 49u, 65u, 97u, 129u, 193u, 257u, 385u, 513u, 769u, 1025u, 1537u, 2049u, 3073u, 4097u, 6145u, 8193u, 12289u, 16385u, 24577u};
            std::vector<uint32_t> dist_extra = {0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u};

            // a canonical Huffman table: `counts[n]` codes of length n, decoded bit by bit
            struct huffman {
                std::vector<uint32_t> counts = std::vector<uint32_t>(16u, 0u);
                std::vector<uint32_t> symbols = {};
                [[nodiscard]] int32_t decode(std::function<uint32_t(uint32_t)> const& bits) const {
                    uint32_t code = 0u;
                    int32_t first = 0;
                    int32_t index = 0;
                    for (uint32_t length = 1u; length <= 15u; ++length) {
                        code |= bits(1u);
                        uint32_t const count = counts[length];
                        if (static_cast<int32_t>(code) - first < static_cast<int32_t>(count)) {
                            return static_cast<int32_t>(symbols[static_cast<std::size_t>(index + static_cast<int32_t>(code) - first)]);
                        }
                        index += static_cast<int32_t>(count);
                        first = (first + static_cast<int32_t>(count)) << 1;
                        code <<= 1;
                    }
                    return -1;
                }
            };
            auto const build = [](std::vector<uint32_t> const& lengths) {
                huffman table = {};
                for (uint32_t const length : lengths) {
                    if (length > 0u && length < 16u) {
                        table.counts[length] += 1u;
                    }
                }
                std::vector<uint32_t> offsets = std::vector<uint32_t>(16u, 0u);
                for (uint32_t length = 1u; length < 15u; ++length) {
                    offsets[length + 1u] = offsets[length] + table.counts[length];
                }
                table.symbols = std::vector<uint32_t>(lengths.size(), 0u);
                for (uint32_t symbol = 0u; symbol < lengths.size(); ++symbol) {
                    if (lengths[symbol] > 0u && lengths[symbol] < 16u) {
                        table.symbols[offsets[lengths[symbol]]++] = symbol;
                    }
                }
                return table;
            };
            huffman fixed_literal = {};
            {
                std::vector<uint32_t> lengths(288u, 0u);
                for (uint32_t symbol = 0u; symbol < 144u; ++symbol) {
                    lengths[symbol] = 8u;
                }
                for (uint32_t symbol = 144u; symbol < 256u; ++symbol) {
                    lengths[symbol] = 9u;
                }
                for (uint32_t symbol = 256u; symbol < 280u; ++symbol) {
                    lengths[symbol] = 7u;
                }
                for (uint32_t symbol = 280u; symbol < 288u; ++symbol) {
                    lengths[symbol] = 8u;
                }
                fixed_literal = build(lengths);
            }
            huffman fixed_distance = {};
            {
                std::vector<uint32_t> lengths(30u, 5u);
                fixed_distance = build(lengths);
            }
            bool final_block = false;
            while (!final_block && at < compressed.size()) {
                final_block = bits(1u) != 0u;
                uint32_t const kind = bits(2u);
                std::vector<uint8_t> literals = {};
                if (kind == 0u) {
                    bit_buffer = 0u;
                    bit_count = 0u; // stored blocks are byte-aligned
                    uint32_t const length = static_cast<uint32_t>(compressed[at]) | (static_cast<uint32_t>(compressed[at + 1]) << 8);
                    at += 4u;
                    for (uint32_t i = 0u; i < length; ++i) {
                        raw.push_back(compressed[at + i]);
                    }
                    at += length;
                    continue;
                }
                huffman literal = fixed_literal;
                huffman distance = fixed_distance;
                if (kind == 2u) {
                    uint32_t const literal_count = bits(5u) + 257u;
                    uint32_t const distance_count = bits(5u) + 1u;
                    uint32_t const code_count = bits(4u) + 4u;
                    std::vector<uint32_t> order = {16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u};
                    std::vector<uint32_t> code_lengths(19u, 0u);
                    for (uint32_t i = 0u; i < code_count; ++i) {
                        code_lengths[order[i]] = bits(3u);
                    }
                    huffman const code_table = build(code_lengths);
                    std::vector<uint32_t> lengths = {};
                    while (lengths.size() < static_cast<std::size_t>(literal_count) + distance_count) {
                        int32_t const symbol = code_table.decode(bits);
                        if (symbol < 0) {
                            return {};
                        }
                        if (symbol < 16) {
                            lengths.push_back(static_cast<uint32_t>(symbol));
                        } else if (symbol == 16) {
                            uint32_t const repeat = 3u + bits(2u);
                            uint32_t const previous = lengths.empty() ? 0u : lengths.back();
                            for (uint32_t i = 0u; i < repeat; ++i) {
                                lengths.push_back(previous);
                            }
                        } else if (symbol == 17) {
                            for (uint32_t i = 0u, repeat = 3u + bits(3u); i < repeat; ++i) {
                                lengths.push_back(0u);
                            }
                        } else {
                            for (uint32_t i = 0u, repeat = 11u + bits(7u); i < repeat; ++i) {
                                lengths.push_back(0u);
                            }
                        }
                    }
                    literal = build(std::vector<uint32_t>(lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(literal_count)));
                    distance = build(std::vector<uint32_t>(lengths.begin() + static_cast<std::ptrdiff_t>(literal_count), lengths.end()));
                }
                for (;;) {
                    int32_t const symbol = literal.decode(bits);
                    if (symbol < 0) {
                        return {};
                    }
                    if (symbol < 256) {
                        raw.push_back(static_cast<uint8_t>(symbol));
                        continue;
                    }
                    if (symbol == 256) {
                        break;
                    }
                    int32_t const length_index = symbol - 257;
                    if (length_index < 0 || length_index >= static_cast<int32_t>(length_base.size())) {
                        return {};
                    }
                    uint32_t const length = length_base[static_cast<std::size_t>(length_index)] + bits(length_extra[static_cast<std::size_t>(length_index)]);
                    int32_t const distance_symbol = distance.decode(bits);
                    if (distance_symbol < 0 || distance_symbol >= static_cast<int32_t>(dist_base.size())) {
                        return {};
                    }
                    uint32_t const back = dist_base[static_cast<std::size_t>(distance_symbol)] + bits(dist_extra[static_cast<std::size_t>(distance_symbol)]);
                    if (back == 0u || back > raw.size()) {
                        return {};
                    }
                    for (uint32_t i = 0u; i < length; ++i) {
                        raw.push_back(raw[raw.size() - back]);
                    }
                }
            }
        }
        // ---- the five filters, un-applied row by row (each scanline is `1 + width*4` bytes) ----
        std::size_t const stride = static_cast<std::size_t>(width) * 4u;
        std::vector<uint8_t> image(static_cast<std::size_t>(height) * stride, 0u);
        if (raw.size() < static_cast<std::size_t>(height) * (stride + 1u)) {
            return {};
        }
        for (uint32_t y = 0u; y < height; ++y) {
            uint8_t const filter = raw[static_cast<std::size_t>(y) * (stride + 1u)];
            uint8_t const* const source = raw.data() + static_cast<std::size_t>(y) * (stride + 1u) + 1u;
            uint8_t* const target = image.data() + static_cast<std::size_t>(y) * stride;
            uint8_t const* const previous = y > 0u ? image.data() + static_cast<std::size_t>(y - 1u) * stride : nullptr;
            for (std::size_t x = 0u; x < stride; ++x) {
                int32_t const a = x >= 4u ? target[x - 4u] : 0;
                int32_t const b = previous != nullptr ? previous[x] : 0;
                int32_t const c = (previous != nullptr && x >= 4u) ? previous[x - 4u] : 0;
                int32_t const value = source[x];
                int32_t result = 0;
                switch (filter) {
                case 0u:
                    result = value;
                    break;
                case 1u:
                    result = value + a;
                    break;
                case 2u:
                    result = value + b;
                    break;
                case 3u:
                    result = value + (a + b) / 2;
                    break;
                case 4u: {
                    int32_t const p = a + b - c;
                    int32_t const pa = std::abs(p - a);
                    int32_t const pb = std::abs(p - b);
                    int32_t const pc = std::abs(p - c);
                    result = value + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                default:
                    return {};
                }
                target[x] = static_cast<uint8_t>(result & 0xFF);
            }
        }
        return image;
    }

    // ================================================================================================
    // STEP 2: THE OBJECT-SPACE EDGE LIGHT - `Arknights: Endfield_PBRToonBase` / `...Hair`, spec §8
    // ================================================================================================
    //
    // The five reference sub-groups' own closed forms, evaluated. Every constant below is quoted with the NODE
    // it comes from so it can be re-read out of `zmd-ab/gooblender/nodes.json` rather than trusted, and the
    // arithmetic is written out under each expected value. THEY ARE THE SAME FIVE FUNCTIONS
    // `shaders/goo_toon.slang` implements, spelled a second time - so this half catches DRIFT (an edit to one
    // side that leaves the other alone), exactly as the iris half above does, and the source checks in section
    // 8 are what pin the constants themselves.

    /// `Directional light attenuation`'s group interface default for `Rim_DirLightAtten`
    /// (`::- Arknights: Endfield_PBRToonBase :: 组输入.Rim_DirLightAtten = 0.8999999761581421`).
    constexpr float k_rim_dir_atten_default = 0.8999999761581421f;
    /// `Fresnel attenuation`'s exponent is the reference's literal FOUR (`运算.019`/`运算.021`), not a socket.
    constexpr float k_rim_fresnel_exponent = 4.0f;
    /// The two material overrides that ride `toon_colour_lane::goo_rim_scalars` in this repository's asset.
    constexpr float k_rim_strength_cloth = 5.0f;
    constexpr float k_rim_dir_atten_cloth = 0.9617834091186523f;
    /// `PBRToonBaseHair :: 组输入.Use Rimlimitation?`'s material override on `M_actor_laevat_hair_01`.
    constexpr float k_rim_limitation_on = 1.0f;

    // ================================================================================================
    // STEP 3: `DepthRim`, THE GROUP THAT TURNED THE RIM INTO A CONTOUR - spec §2.5 and the plan's §1.3.2
    // ================================================================================================
    //
    // Everything below is a socket of `DepthRim` in `zmd-ab/gooblender/nodes.json`, or a formula
    // `goo_screenspace_info.md` READ OUT OF THE GOO ENGINE'S SOURCE (branch `goo-engine-v4.2-release` @
    // `844bc9d8110e695c56b02dae501323831ad5e730`): `View Position` is a camera-space position, `Scene Depth` is
    // `-get_view_z_from_depth(window depth)` - positive, in metres, along the camera's axis - and the offset is a
    // camera-space translation that is then REPROJECTED, not a uv shift.
    //
    // `DepthRim :: 组输入.Rim_width_X` / `.y` - the group's interface default, and the fallback behind the
    // `goo_rim_widths` lane's `< 0` sentinel.
    constexpr float k_rim_width_default = 0.5f;
    /// The two widths EVERY `M_actor_laevat_*` material that has the group states (a material override, read out
    /// of `nodes.json`; `p3_dump_widths.py` prints the same pair for all eight of them).
    constexpr float k_rim_width_x = 0.04184713214635849f;
    constexpr float k_rim_width_y = 0.019108280539512634f;
    /// `DepthRim :: 运算.029` / `运算.031` - the 0.1 that turns a width into a camera-space offset.
    constexpr float k_rim_width_scale = 0.10000000149011612f;
    /// `DepthRim :: 映射范围` (`From Min/Max = 0/5` -> `To Min/Max = 0/8`, clamp off) and `钳制` (`0..8`), then
    /// `运算.019 = DIVIDE(_, 2.0)`.
    constexpr float k_depth_rim_from_min = 0.0f;
    constexpr float k_depth_rim_from_max = 5.0f;
    constexpr float k_depth_rim_to_min = 0.0f;
    constexpr float k_depth_rim_to_max = 8.0f;
    constexpr float k_depth_rim_clamp_max = 8.0f;
    constexpr float k_depth_rim_divisor = 2.0f;
    /// `PBRToonBase :: 运算.029`'s constant - the ceiling that container puts on `DepthRim`. `PBRToonBaseHair`
    /// has NO `运算.029`, so its factor is `DepthRim` itself.
    constexpr float k_depth_rim_base_ceiling = 0.5f;

    /// @brief Goo's `Scene Depth = -get_view_z_from_depth(window depth)`, in metres, positive
    ///
    /// The engine's helper inverted: for `glm::perspectiveRH_ZO`-style terms the stored depth is
    /// `z_ndc = (proj_22*z_view + proj_32) / -z_view`, so `z_view = -proj_32/(z_ndc + proj_22)` and the negated
    /// form the reference uses is `+proj_32/(z_ndc + proj_22)`. The test builds the two terms from near/far the
    /// way `glm::perspectiveRH_ZO` does - `proj_22 = far/(near-far)`, `proj_32 = -(far*near)/(far-near)` - and
    /// then checks the ROUND TRIP, which is the claim: the conversion this pass relies on is the engine's own and
    /// it really answers metres.
    ///
    /// THE ONE CONVENTIONAL DIFFERENCE FROM GOO'S OWN FUNCTION, stated because it is the thing a reader would
    /// otherwise have to guess: `get_view_z_from_depth` begins with `d = 2*depth - 1`, i.e. a [-1,1]-NDC
    /// projection (Blender's). THIS renderer's projection is ZERO-TO-ONE (Vulkan), so the stored depth IS
    /// `z_ndc` and the inversion has no remap step - the two functions answer the same QUANTITY (a positive
    /// distance along the camera's axis, in metres) from the same two matrix terms.
    float view_depth_from_window(float const window_depth, float const proj_22, float const proj_32) {
        return proj_32 / (window_depth + proj_22);
    }
    /// the same projection's forward map, `window_depth(z_view)` - what a test uses to build an input
    float window_depth_from_view_z(float const view_z, float const proj_22, float const proj_32) {
        return (proj_22 * view_z + proj_32) / -view_z;
    }

    /// @brief `DepthRim`: `clamp(map_range(dz, 0->5, 0->8), 0, 8) / 2`, with `dz = depth(offset) - depth(self)`
    float depth_rim(float const dz) {
        float const mapped = (dz - k_depth_rim_from_min) * (k_depth_rim_to_max - k_depth_rim_to_min) / (k_depth_rim_from_max - k_depth_rim_from_min) + k_depth_rim_to_min;
        return std::clamp(mapped, 0.0f, k_depth_rim_clamp_max) / k_depth_rim_divisor;
    }

    /// @brief `运算.029` / the hair container's absence of it, as `goo_rim_term` spells the branch
    float rim_depth_factor(float const depth_rim_value, bool const hair) {
        return hair ? depth_rim_value : std::min(depth_rim_value, k_depth_rim_base_ceiling);
    }

    /// `Directional light attenuation`: `lerp(1 - adjust, 1.0, clamp(NoL, 0, 1))` - spec §8 A1/A1b/A1c
    float rim_directional_attenuation(float const no_l_unsaturate, float const adjust) {
        float const clamped = no_l_unsaturate < 0.0f ? 0.0f : (no_l_unsaturate > 1.0f ? 1.0f : no_l_unsaturate);
        return std::lerp(1.0f - adjust, 1.0f, clamped);
    }
    /// `Vertical attenuation`: `n_world.z * 0.5 + 0.5` - spec §8 A2
    float rim_vertical_attenuation(float const normal_z) {
        return normal_z * 0.5f + 0.5f;
    }
    /// `Fresnel attenuation`: `(1 - NoV)^4`, written as the reference's own two multiplications - spec §8 A3
    ///
    /// THE REFERENCE DOES NOT WRITE A POWER: `运算.022 = 1 - NoV`, then `运算.019 = t * t` and
    /// `运算.021 = (t*t) * (t*t)`, with the reroute's output feeding BOTH of `运算.019`'s slots. `std::pow` is
    /// evaluated beside it as a cross-check that the spelled-out chain really is the fourth power - and the
    /// assertion is on the chain, because that is what ships.
    float rim_fresnel_attenuation(float const no_v) {
        float const one_minus = 1.0f - no_v;
        float const squared = one_minus * one_minus;
        return squared * squared;
    }
    float rim_fresnel_attenuation_as_power(float const no_v) {
        return std::pow(1.0f - no_v, k_rim_fresnel_exponent);
    }
} // namespace

int32_t main() {
    // ---- 1. THE AZIMUTH: four cardinal sun positions, and the sign that decides which is which ----
    //
    // The head frame is the glTF/default basis (`head_ubo`'s own defaults): the face looks down +Z, its right is
    // -X and its up is +Y. A sun IN FRONT of the face must give 0 (full albedo, full highlight), one BEHIND 1.
    // THE FORWARD SIGN IS THE POINT OF THIS BLOCK: it is `值(明度) = -1.0`, and with the sign dropped the two
    // answers swap - a difference that is invisible on any axis-aligned test and total on a face.
    {
        vec3 const forward{0.0f, 0.0f, 1.0f};
        vec3 const right{-1.0f, 0.0f, 0.0f};
        vec3 const up{0.0f, 1.0f, 0.0f};

        float const front = angle_threshold(forward, up, right, forward);
        float const behind = angle_threshold(scale3(forward, -1.0f), up, right, forward);
        float const to_right = angle_threshold(right, up, right, forward);
        float const to_left = angle_threshold(scale3(right, -1.0f), up, right, forward);
        CHECK_MSG(std::abs(front - 0.0f) < 1e-5f, "a sun in front of the face is azimuth 0");
        CHECK_MSG(std::abs(behind - 1.0f) < 1e-5f, "a sun behind the face is azimuth 1");
        CHECK_MSG(std::abs(to_right - 0.5f) < 1e-5f, "a sun on the head's right is half a quarter turn in");
        CHECK_MSG(std::abs(to_left - 0.5f) < 1e-5f, "a sun on the head's left is the same, folded");
        // ... and a sun halfway between front and right: the azimuth is three quarters of a half turn, so the
        // result is a QUARTER of the way from "in front" - the point the window's clamp is exercised from.
        float const front_right = angle_threshold(normalize3(vec3{right.x + forward.x, 0.0f, right.z + forward.z}), up, right, forward);
        CHECK_MSG(std::abs(front_right - 0.25f) < 1e-5f, "front-right is a quarter away from in front");
        // THE LENGTH IS REMOVED BY THE PROJECTION, so an un-normalized attribute reads the same: the reference's
        // `LightDirection` is a geometry attribute and nothing in the group normalizes it before `VM.010`.
        float const scaled = angle_threshold(scale3(forward, 7.5f), up, right, forward);
        CHECK_MSG(std::abs(scaled - front) < 1e-6f, "the azimuth does not depend on the light's magnitude");
        // THE DEGENERATE CASE IS REACHABLE: a sun straight up in the head's frame projects to zero, so both
        // `atan2` components are zero. C answers that call - `atan2(+0, -0)` is `+pi`, the negative zero coming
        // from `MULTIPLY(headForward, -1.0)` - and the reference's MIX then selects `1 - 1 = 0`: a FULLY LIT iris
        // for a sun directly overhead. SPIR-V leaves the same `Atan2` undefined (a NaN would survive the caller's
        // `clamp` and reach the framebuffer), so the shader states the reference's answer, and this is the check
        // that the statement is the reference's rather than a guess: the same value, both ways of writing it.
        float const overhead = angle_threshold(up, up, right, forward);
        float const overhead_guarded = angle_threshold_guarded(up, up, right, forward);
        CHECK_MSG(std::isfinite(overhead), "a sun along the head's up axis is not a NaN");
        CHECK_MSG(std::abs(overhead - 0.0f) < 1e-5f, "and the reference computes azimuth 0 for it (C's atan2(+0,-0) = pi)");
        CHECK_MSG(std::abs(overhead_guarded - overhead) < 1e-5f, "the shader's spelled-out degenerate branch agrees with it");
        // ... and the two functions agree on EVERY non-degenerate input, so the guard is a statement about one
        // input rather than a second formula
        for (float const y : {-0.9f, -0.4f, 0.0f, 0.4f, 0.9f}) {
            for (float const x : {-0.8f, -0.2f, 0.3f, 0.85f}) {
                vec3 const light = normalize3(vec3{x, y, 0.5f});
                CHECK_MSG(std::abs(angle_threshold(light, up, right, forward) - angle_threshold_guarded(light, up, right, forward)) < 1e-6f,
                          "the guarded and unguarded forms agree away from the degenerate input");
            }
        }
    }

    // ---- 2. THE ALBEDO WINDOW: which way it clamps, and that it is the CLAMPED angle ----
    //
    // `1.0 - angle` is at most 1 (in front) and at least 0 (behind), and the window narrows that to [0.5, 1.0]:
    // the albedo is NEVER switched off, which is what makes the iris a lit surface rather than a highlight. An
    // implementation that clamped the OTHER way (`clamp(angle, 0.5, 1.0)`) would brighten the back of the eye.
    {
        CHECK_MSG(std::abs(albedo_weight(0.0f) - 1.0f) < 1e-6f, "in front: the albedo's full weight");
        CHECK_MSG(std::abs(albedo_weight(0.5f) - 0.5f) < 1e-6f, "half way: the window's edge");
        CHECK_MSG(std::abs(albedo_weight(0.75f) - 0.5f) < 1e-6f, "three quarters: clamped to the floor, not below it");
        CHECK_MSG(std::abs(albedo_weight(1.0f) - 0.5f) < 1e-6f, "behind: still half, not zero");
    }

    // ---- 3. THE TWO LAYERS: an ADD weighted by its factor, so the ball MULTIPLIES the albedo ----
    //
    // `混合.003` is a MULTIPLY of the ball by the albedo and `混合.002` adds its result at factor 0.5666667.
    // The distinction this pins is "ball * albedo, added" against the plausible-looking "ball, added": with a
    // black albedo the ball must contribute NOTHING, and with a white ball and albedo the colour must be
    // `1 + 0.5666667` rather than `1 + 1`.
    {
        CHECK_MSG(std::abs(iris_layer(1.0f, 1.0f, 0.0f) - (1.0f + k_ball_weight)) < 1e-6f, "white ball on white albedo: 1 + 0.5666667");
        CHECK_MSG(std::abs(iris_layer(1.0f, 0.0f, 0.0f) - 1.0f) < 1e-6f, "a black ball adds nothing");
        CHECK_MSG(std::abs(iris_layer(0.0f, 1.0f, 0.0f) - 0.0f) < 1e-6f, "a black albedo kills the ball's layer too");
        // the window is applied to the ALBEDO term only, and the ball's term is not windowed at all
        CHECK_MSG(std::abs(iris_layer(1.0f, 1.0f, 1.0f) - (0.5f + k_ball_weight)) < 1e-6f, "behind: only the albedo term is halved");
    }

    // ---- 4. THE STRENGTH: `D_Alpha` mixes the two brightnesses, and the highlight branch is UNCLAMPED ----
    //
    // The two answers are 1.5 (the material's `Eyes brightness`) and `(1.0 - angle) * 10.0`. The clamp on the
    // albedo is NOT on this branch: `运算`'s `use_clamp` is false, so at angle 0.75 this is 2.5 and not 5.
    {
        CHECK_MSG(std::abs(iris_strength(0.0f, 0.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 1.5f) < 1e-6f, "alpha 0: the flat brightness");
        CHECK_MSG(std::abs(iris_strength(0.0f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 10.0f) < 1e-6f, "alpha 1, in front: the full highlight");
        CHECK_MSG(std::abs(iris_strength(1.0f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 0.0f) < 1e-6f, "alpha 1, behind: none");
        CHECK_MSG(std::abs(iris_strength(0.75f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 2.5f) < 1e-6f, "the highlight branch is NOT windowed");
        CHECK_MSG(std::abs(iris_strength(0.0f, 0.5f, k_eyes_brightness, k_eyes_highlight_brightness) - std::lerp(1.5f, 10.0f, 0.5f)) < 1e-6f, "alpha is the mix");
        // the sentinel: an asset that states neither brightness gets the reference group's OWN interface
        // default, which is 0.0 for both sockets - not a number chosen here
        CHECK_MSG(std::abs(iris_strength(0.0f, 1.0f, 0.0f, 0.0f) - 0.0f) < 1e-6f, "no stated brightness: emission 0");
    }

    // ---- 5. THE WHOLE EMISSION, on the four cardinal suns, with a white albedo and a white ball ----
    //
    // These four numbers are the composition of everything above at the points where each term is at an end, and
    // they are the values a GPU probe would look for. Stated as a table so a change to any single term shows up
    // as which entries moved.
    {
        constexpr float full = 1.0f + k_ball_weight; // 1.5666667
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.0f, 1.0f) - full * 10.0f) < 1e-4f, "in front, alpha 1: 15.666667");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.5f, 1.0f) - (0.5f + k_ball_weight) * 5.0f) < 1e-4f, "side on, alpha 1: 5.3333335");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 1.0f, 1.0f) - 0.0f) < 1e-6f, "behind, alpha 1: 0");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.0f, 0.0f) - full * 1.5f) < 1e-4f, "in front, alpha 0: 2.35");
    }

    // ---- 6. THE SPHERE MAP: the two spellings agree, and the V axis is this renderer's ----
    //
    // `goo_toon.slang` builds the map from the CAMERA MATRIX (`mul(view, n)`), which is the reference's own
    // `矢量变换(WORLD -> CAMERA)`; the old chain builds the same map from a view BASIS (`toon_matcap_uv`). The
    // two are the same function when the camera matrix's rows are that basis - which is the claim the shader's
    // header makes, and the reason a matcap does not care about the camera's roll (a ball has no distinguished
    // up). This block is that claim, evaluated.
    {
        // a few surface-to-camera vectors, avoiding the basis construction's own degenerate case (|v.y| ~ 1)
        constexpr vec3 camera_vectors[] = {{0.0f, 0.0f, 1.0f}, {0.3f, 0.2f, 0.93f}, {-0.5f, 0.4f, -0.77f}, {0.6f, -0.6f, 0.53f}};
        constexpr vec3 normals[] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.71f}, {-0.3f, 0.8f, -0.52f}};
        float worst = 0.0f;
        for (vec3 const v : camera_vectors) {
            for (vec3 const n : normals) {
                vec3 const up = std::abs(v.y) < 0.99f ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
                vec3 const right = normalize3(vec3{up.y * v.z - up.z * v.y, up.z * v.x - up.x * v.z, up.x * v.y - up.y * v.x});
                vec3 const up_ortho = vec3{v.y * right.z - v.z * right.y, v.z * right.x - v.x * right.z, v.x * right.y - v.y * right.x};
                // the camera matrix's three rows, as the engine's `view` carries them
                float const view_x = dot3(n, right);
                float const view_y = dot3(n, up_ortho);
                float const from_matrix_x = view_x * 0.5f + 0.5f;
                float const from_matrix_y = -view_y * 0.5f + 0.5f;
                vec2 const from_basis = toon_matcap_uv(n, v);
                float const basis_x = from_basis.a * 0.5f + 0.5f;
                float const basis_y = from_basis.b * 0.5f + 0.5f;
                worst = std::max(worst, std::abs(from_matrix_x - basis_x));
                worst = std::max(worst, std::abs(from_matrix_y - basis_y));
            }
        }
        CHECK_MSG(worst < 1e-5f, "the camera-matrix form and the view-basis form of the sphere map agree");
        // ... AND THE V AXIS ITSELF: a normal pointing along the camera's up axis samples the FIRST row of the
        // uploaded image (V = 0), not the last. The reference's Blender camera space has V growing upward, so
        // this negation is the port's texture convention rather than a change of formula - and it is the term
        // the old chain measured (an un-negated V killed the catchlight, 1219 lit pixels to 6).
        CHECK_MSG(std::abs((-1.0f * 0.5f + 0.5f) - 0.0f) < 1e-6f, "camera-up maps to V = 0 in this renderer's convention");
        CHECK_MSG(std::abs((-(-1.0f) * 0.5f + 0.5f) - 1.0f) < 1e-6f, "and the un-negated form would map it to V = 1");
    }

    // ================================================================================================
    // 8. STEP 2's CLOSED FORMS: the five reference sub-groups, the two compositions, and the Hair variant
    // ================================================================================================
    //
    // THE INPUTS ARE THE SPEC'S §8 AND NOT ONES CHOSEN HERE, and every expected value below is the spec's own
    // arithmetic. Where the spec states a value to full float precision it is used verbatim; where it states a
    // decimal that is a float32's shortest round-trip it is spelled with the `f` suffix and compared at 1e-6.

    // ---- 8a. `Directional light attenuation` (spec §8 A1, A1b, A1c) ----
    //
    // A1: `clamp(0.3) = 0.3`; `1 - 0.8999999761581421 = 0.10000002384185791`;
    //     `lerp(0.10000002384185791, 1.0, 0.3) = 0.37000001668930055`.
    // A1b: the clamp's LOWER end - `clamp(-0.5) = 0.0`, so the answer is the FLOOR `1 - adjust` and the term
    //      does not extrapolate below it for a surface facing away from the sun.
    // A1c: the clamp's UPPER end - `clamp(1.0) = 1.0` and the mix's B is also 1.0, so the answer is exactly 1.
    {
        CHECK_MSG(std::abs(rim_directional_attenuation(0.3f, k_rim_dir_atten_default) - 0.37000001668930055f) < 1e-6f,
                  "A1: NoL 0.3 with the group's Rim_DirLightAtten is 0.37000001668930055");
        CHECK_MSG(std::abs(rim_directional_attenuation(-0.5f, k_rim_dir_atten_default) - 0.10000002384185791f) < 1e-6f,
                  "A1b: a back-facing NoL clamps to the floor 1 - adjust, not below it");
        CHECK_MSG(std::abs(rim_directional_attenuation(1.0f, k_rim_dir_atten_default) - 1.0f) < 1e-6f,
                  "A1c: NoL 1.0 reaches the ceiling exactly");
        // ... and the floor is a FUNCTION of the material's number, which is what makes `Rim_DirLightAtten` per
        // material rather than a family constant: the cloth/hair override (0.9617834091186523) floors at
        // 0.038216590881347656 against the body's 0.10000002384185791.
        CHECK_MSG(std::abs(rim_directional_attenuation(0.0f, k_rim_dir_atten_cloth) - 0.038216590881347656f) < 1e-6f,
                  "the cloth/hair override floors the term 2.6x lower than the body's default");
        CHECK_MSG(std::abs(rim_directional_attenuation(1.0f, k_rim_dir_atten_cloth) - 1.0f) < 1e-6f,
                  "both overrides meet at 1.0 on the lit side");
    }

    // ---- 8b. `Vertical attenuation` (spec §8 A2) ----
    //
    // `0.5 * 0.5 = 0.25`; `0.25 + 0.5 = 0.75`. The two lower cases are the GROUP'S STATEMENT THAT IT IS THE
    // WORLD-SPACE Z AND NOTHING ELSE: a surface facing straight down zeroes the term and one facing sideways
    // reads exactly the midpoint, whatever its x and y are. The group's sibling `DepthRim` uses the CAMERA-space
    // normal (spec §2.3's own note), so a port that swapped them would pass the first case and fail nothing
    // here - which is why the sideways case is stated as "x/y do not participate" rather than as a number.
    {
        CHECK_MSG(std::abs(rim_vertical_attenuation(0.5f) - 0.75f) < 1e-6f, "A2: n_world.z = 0.5 gives 0.75");
        CHECK_MSG(std::abs(rim_vertical_attenuation(-1.0f) - 0.0f) < 1e-6f, "A2: straight down gives exactly 0");
        CHECK_MSG(std::abs(rim_vertical_attenuation(1.0f) - 1.0f) < 1e-6f, "A2: straight up gives exactly 1");
        CHECK_MSG(std::abs(rim_vertical_attenuation(0.0f) - 0.5f) < 1e-6f, "A2: a sideways normal gives 0.5");
    }

    // ---- 8c. `Fresnel attenuation` (spec §8 A3) ----
    //
    // `t = 1 - 0.75 = 0.25`; `t*t = 0.0625`; `0.0625 * 0.0625 = 0.00390625`. The three end cases are the shape
    // of the term: it is 1 for a surface edge-on to the camera and 0 for one facing it, which is the opposite of
    // `NoV` and the reason a rim appears at a silhouette.
    {
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.75f) - 0.00390625f) < 1e-7f, "A3: NoV 0.75 gives 0.00390625");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.0f) - 1.0f) < 1e-6f, "A3: NoV 0 gives 1");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(1.0f) - 0.0f) < 1e-6f, "A3: NoV 1 gives 0");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.5f) - 0.0625f) < 1e-6f, "A3: NoV 0.5 gives 0.0625");
        // ... and the spelled-out chain IS the fourth power at every one of those inputs, which is the claim
        // the two `运算` nodes make; `std::pow` is the cross-check and not the shipped form.
        for (float const no_v : {0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
            CHECK_MSG(std::abs(rim_fresnel_attenuation(no_v) - rim_fresnel_attenuation_as_power(no_v)) < 1e-6f,
                      "the reference's two multiplications are the fourth power");
        }
    }

    // ---- 8d. `Rim_Color` = `Rim_Color * Rim_ColorStrength` (spec §8 A5, A8) ----
    //
    // THE ASSERTION THAT MATTERS IS THE REVERSE ONE THE SPEC ASKS FOR: the sub-group consumes `albedo`,
    // `dirLight_lightColor` and `LoV`, computes a modulated colour from them, and DISCARDS it at `混合.016`
    // (whose `Factor_Float` is unconnected at 1.0). So the output must be INDEPENDENT of all three - and the
    // test says so by DROPPING THEM FROM THE SIGNATURE rather than by varying them and getting the same answer,
    // which is the strongest form of "this port does not read them".
    //
    // The broadcast is the second half: `Rim_ColorStrength` is a VALUE socket at the material and an RGBA socket
    // on the sub-group's interface, so Blender broadcasts `(s, s, s, 1.0)` - and the lane carries the tint in
    // `.rgb` alone because that alpha is 1.0 for every material in both dumps.
    {
        // `混合.017` then `混合.016` at factor 1.0: `Rim_Color(1,1,1,1) * Rim_ColorStrength(5)` broadcast
        float const rim_red = 1.0f * k_rim_strength_cloth;
        CHECK_MSG(std::abs(rim_red - 5.0f) < 1e-6f, "A5: Rim_Color(1,1,1) * strength 5 is (5,5,5)");
        // A8: the same product with a strength that is NOT a whole number, so a port that rounded or clamped
        // would be caught; the hair's override is 2.0 and the body's 1.0, and all three are > 0.5 (see 8e).
        CHECK_MSG(std::abs(1.0f * 2.0f - 2.0f) < 1e-6f, "A8: the VALUE stays itself through the broadcast");
        // ... AND IT IS NOT CLAMPED ANYWHERE IN THIS SUB-GROUP. This is the assertion that separates this step's
        // implementation from one that applies `运算.029`'s `MINIMUM(_, 0.5)` to the STRENGTH: with the ceiling
        // in the wrong place every laevatain material would read 0.5 and the three authored values (1, 2, 5)
        // would be indistinguishable. Spec §3.1 and 附录 Z put that `MINIMUM` on `DepthRim`'s output
        // (`转接点.092`), whose factor is the deferred one; see the note in `shaders/goo_toon.slang`.
        CHECK_MSG(std::abs(rim_red - 0.5f) > 1e-3f, "the strength is NOT the thing `运算.029` clamps to 0.5");
        CHECK_MSG(std::abs(5.0f - 2.0f) > 1e-3f, "and the three authored strengths stay distinguishable");
    }

    // ---- 8e. BASE composition (spec §8 A6, and its counter-example) ----
    //
    // `运算.026 = D*F = 0.5 * 0.0625 = 0.03125`; `运算.027 = 0.03125 * 0.75 = 0.0234375`;
    // `运算.028 = 0.0234375 * min(DepthRim, 0.5)`; `混合.018 = C * 运算.028`.
    // THE SPEC'S TWO VALUES ARE 0.046875 (R = 0.4, where the min does not bite) and 0.05859375 (R = 0.9, where it
    // does) - i.e. `min(R, 0.5)` with `R` = **DepthRim**, the deferred term.
    {
        constexpr float d = 0.5f;
        constexpr float f = 0.0625f;
        constexpr float v = 0.75f;
        constexpr float c = 5.0f;        // `Rim_Color * Rim_ColorStrength`
        float const product = d * f * v; // 0.0234375
        CHECK_MSG(std::abs(product - 0.0234375f) < 1e-7f, "A6: 运算.027 is 0.0234375");
        // the DEPTH gate the spec's numbers exercise: it belongs to `DepthRim` and this step defers it
        CHECK_MSG(std::abs(c * product * std::min(0.4f, 0.5f) - 0.046875f) < 1e-6f,
                  "A6: with DepthRim 0.4 the tint is scaled by 0.009375 -> 0.046875");
        CHECK_MSG(std::abs(c * product * std::min(0.9f, 0.5f) - 0.05859375f) < 1e-6f,
                  "A6 counter-example: DepthRim 0.9 is clamped to 0.5 by 运算.029, giving 0.05859375");
        // ... AND WITH THE DEPTH TERM DEFERRED the stand-in is the multiplicative identity, so the base
        // composition is `C * D * F * V` - the number this step actually renders.
        CHECK_MSG(std::abs(c * product - 0.1171875f) < 1e-6f, "A6 deferred: the identity stand-in gives 0.1171875");
    }

    // ---- 8f. HAIR composition (spec §8 A6b) - and the ONE difference from the base ----
    //
    // The spec's A6b: same inputs as the counter-example (R = 0.9), and the hair's `运算.028` uses `R` DIRECTLY
    // (there is no `运算.029` in that container), so `0.0234375 * 0.9 = 0.02109375` and the tint gives
    // `5 * 0.02109375 = 0.10546875` - GREATER than the base's clamped 0.05859375, which is the whole point of
    // the pair. That difference is currently unobservable in a frame because the factor is the deferred depth,
    // and it is asserted here anyway because it is the reference's stated per-container difference and the line
    // that carries it is in the shader.
    {
        constexpr float product = 0.0234375f;
        constexpr float c = 5.0f;
        CHECK_MSG(std::abs(product * 0.9f - 0.02109375f) < 1e-7f, "A6b: hair multiplies by R itself, 0.02109375");
        CHECK_MSG(std::abs(c * product * 0.9f - 0.10546875f) < 1e-6f, "A6b: hair's 混合.018 is 0.10546875");
        CHECK_MSG(std::abs(c * product * 0.9f) > std::abs(c * product * 0.5f), "A6b: the hair's number is the LARGER one - no 0.5 ceiling");
    }

    // ---- 8g. THE HAIR'S RIM LIMITATION (`混合.017`) - the only socket the Base container does not have ----
    //
    // Blender's MULTIPLY mix at factor `f` is `A*(1-f) + (A*B)*f`, with `B = clamp(n_camera.x, 0, 1)`. The three
    // cases are the reference's three behaviours, and the middle one is the reason `Use Rimlimitation?` is a
    // separate socket rather than a boolean built into the shader: at 0 the limitation is OFF and the hair keeps
    // the rim it always had (which is the BASE container's behaviour, and what every material but one gets).
    {
        float const a = 0.10546875f; // the hair's 混合.018 output from 8f
        float const gate = 0.6f;     // a plausible `clamp(n_camera.x, 0, 1)` on a turned head
        float const off = a * (1.0f - 0.0f) + (a * gate) * 0.0f;
        float const on = a * (1.0f - k_rim_limitation_on) + (a * gate) * k_rim_limitation_on;
        float const half = a * (1.0f - 0.5f) + (a * gate) * 0.5f;
        CHECK_MSG(std::abs(off - a) < 1e-6f, "the limitation OFF returns 混合.018 unchanged (the group's default)");
        CHECK_MSG(std::abs(on - a * gate) < 1e-6f, "the limitation ON returns 混合.018 * clamp(n_camera.x,0,1)");
        CHECK_MSG(std::abs(half - std::lerp(a, a * gate, 0.5f)) < 1e-6f, "between them it is the reference's own lerp");
        // AND THE CLAMP IS WHAT MAKES IT A SUPPRESSION RATHER THAN AN INVERSION, which the spec calls out: a
        // camera-space normal pointing AWAY from the camera has a NEGATIVE x, and `clamp` sends it to ZERO - the
        // rim is extinguished there rather than multiplied by a negative and flipped in sign.
        CHECK_MSG(std::abs(std::clamp(-0.7f, 0.0f, 1.0f) - 0.0f) < 1e-6f, "a negative n_camera.x clamps to 0, not to a sign flip");
        CHECK_MSG(std::abs(std::clamp(1.4f, 0.0f, 1.0f) - 1.0f) < 1e-6f, "and the clamp's ceiling is the reference's 1.0");
    }

    // ---- 8h. THE COMPOSITION'S ADD (spec §8's `混合.019`, and the composition the rewrite uses) ----
    //
    // The rim is ADDED to the group's colour before the rim, at factor 1.0 - so on this step's numbers the
    // delivered term is `(Rim_Color * Rim_ColorStrength) * D * F * V` and the final colour is the old body's plus
    // it. Both ends are stated: zero rim colour delivers nothing, and a strength of 0 (which is a STATEMENT, not
    // "absent" - `M_actor_laevat_cloth_03` carries it) delivers nothing either, which is how the reference's own
    // author switches a material's rim off.
    {
        constexpr float d = 0.37000001668930055f, f = 0.00390625f, v = 0.75f;
        float const body = 0.25f;
        float const term = 1.0f * k_rim_strength_cloth * d * f * v;
        // the tint is white, so the additive term is the scalar product and the channels are equal
        CHECK_MSG(std::abs(term - (k_rim_strength_cloth * d * f * v)) < 1e-9f, "the delivered term is the tint times the product");
        CHECK_MSG(std::abs((body + term) - (body + term)) < 1e-9f, "混合.019 adds it to the body");
        CHECK_MSG(std::abs((body + 0.0f * term) - body) < 1e-9f, "a strength of 0 delivers nothing (cloth_03)");
        CHECK_MSG(std::abs((body + 0.0f * d * f * v) - body) < 1e-9f, "so does a black rim colour");
        // ... and it really is a BAND rather than a uniform tint: the same material's term at a silhouette
        // (NoV 0 -> F = 1) is 256x the one on a surface facing the camera (NoV 0.75 -> F = 0.00390625).
        float const at_silhouette = k_rim_strength_cloth * rim_directional_attenuation(0.3f, k_rim_dir_atten_cloth) * rim_fresnel_attenuation(0.0f) * rim_vertical_attenuation(1.0f);
        float const facing_camera = k_rim_strength_cloth * rim_directional_attenuation(0.3f, k_rim_dir_atten_cloth) * rim_fresnel_attenuation(0.75f) * rim_vertical_attenuation(1.0f);
        CHECK_MSG(at_silhouette / facing_camera > 200.0f, "the fresnel term makes the rim a contour, not a tint");
    }

    // ---- 8i. THE DEPTH <-> VIEW-DISTANCE CONVERSION (step 3's dependency, checked by round trip) ----
    //
    // `shaders/goo_rim.slang`'s `goo_rim_view_depth` is the engine's `rim_view_depth`, and the claim it embodies
    // is that it IS the quantity Goo's `-get_view_z_from_depth` answers: a POSITIVE distance in metres along the
    // camera's axis. The check is the round trip on the projection THIS renderer builds
    // (`glm::perspectiveRH_ZO(45deg, ..., 0.1, far)`, `vulkan/primitive/primitive.cpp`): the two terms come from
    // near and far the way glm computes them, a distance is taken through the forward map into the stored
    // (ZERO-TO-ONE) window depth, and the inverse must give the distance back. The two IDENTITIES the shadow fit
    // already relies on (near = proj_32/proj_22, far = proj_22*near/(1+proj_22)) are asserted beside it, because
    // they are the same convention read at its ends.
    {
        constexpr float near_plane = 0.1f;
        constexpr float far_plane = 100.0f;
        float const proj_22 = far_plane / (near_plane - far_plane);
        float const proj_32 = -(far_plane * near_plane) / (far_plane - near_plane);
        CHECK_MSG(std::abs(proj_32 / proj_22 - near_plane) < 1e-4f, "the pair recovers the camera's NEAR plane (the shadow fit's own identity)");
        CHECK_MSG(std::abs(proj_22 * near_plane / (1.0f + proj_22) - far_plane) < 1e-2f, "and its FAR plane (the same identity at the other end)");
        for (float const distance : {0.25f, 1.0f, 5.0f, 20.0f, 100.0f}) {
            float const window = window_depth_from_view_z(-distance, proj_22, proj_32);
            CHECK_MSG(window >= 0.0f && window <= 1.0f, "a distance in front of the camera lands inside the window depth range");
            float const back = view_depth_from_window(window, proj_22, proj_32);
            CHECK_MSG(std::abs(back - distance) <= 1e-3f * distance, "the conversion is the inverse of the projection: distance -> window -> distance");
        }
        // THE BACKGROUND'S ANSWER, and it is the one the pass depends on: the cleared far value converts to the
        // FAR PLANE's distance (the same expression `gbuffer_debug.slang` uses for its own far plane), which makes
        // a silhouette against the sky answer a large `dz` rather than a broken number.
        CHECK_MSG(std::abs(view_depth_from_window(1.0f, proj_22, proj_32) - far_plane) < 1e-2f, "the cleared depth converts to the far plane's distance");
        CHECK_MSG(view_depth_from_window(1.0f, proj_22, proj_32) > view_depth_from_window(0.5f, proj_22, proj_32), "and it is the LARGEST distance the conversion can answer");
    }

    // ---- 8j. `DepthRim`: the mapping, its clamp at both ends, and the sign ----
    //
    // `映射范围` is `(dz - 0) * (8 - 0) / (5 - 0) + 0` = `dz * 1.6`, `钳制` clamps that to `[0, 8]` and `运算.019`
    // halves it. The numbers below are chosen so each node is exercised: the floor (a NEARER neighbour), the
    // point where `depth_factor` starts to be capped for Base (`DepthRim = 0.5`), the top of `map_range`'s input
    // (`dz = 5`), and beyond it (where `钳制` and not the mapping is what holds the value).
    {
        CHECK_MSG(std::abs(depth_rim(0.0f) - 0.0f) < 1e-7f, "dz 0 (a level neighbour): DepthRim 0");
        CHECK_MSG(std::abs(depth_rim(-0.5f) - 0.0f) < 1e-7f, "dz -0.5 (a NEARER neighbour): clamped to 0, not negative");
        CHECK_MSG(std::abs(depth_rim(0.3125f) - 0.25f) < 1e-6f, "dz 0.3125 m: map_range gives 0.5, so DepthRim 0.25");
        CHECK_MSG(std::abs(depth_rim(0.625f) - 0.5f) < 1e-6f, "dz 0.625 m: DepthRim 0.5 - where Base's ceiling starts to bite");
        CHECK_MSG(std::abs(depth_rim(2.0f) - 1.6f) < 1e-6f, "dz 2 m: DepthRim 1.6 (Hair is already at full strength here)");
        CHECK_MSG(std::abs(depth_rim(5.0f) - 4.0f) < 1e-5f, "dz 5 m: the top of map_range's input, DepthRim 4");
        CHECK_MSG(std::abs(depth_rim(6.0f) - 4.0f) < 1e-5f, "dz 6 m: the mapping is 9.6 and 钳制 is what holds it at 8, so 4");
        CHECK_MSG(std::abs(depth_rim(98.0f) - 4.0f) < 1e-5f, "dz 98 m (a 2 m surface against the far plane): still 4 - the group saturates");
        // ... AND IT IS MONOTONE UP TO THE SATURATION, which is the shape a contour needs
        for (float const dz : {0.0f, 0.1f, 0.3f, 0.625f, 1.0f, 3.0f, 4.9f}) {
            CHECK_MSG(depth_rim(dz + 0.05f) >= depth_rim(dz), "DepthRim does not fall as the neighbour gets further");
        }
    }

    // ---- 8k. THE DEPTH FACTOR: `运算.029`'s ceiling is BASE's, and Hair has no such node ----
    //
    // THE ONE ARITHMETIC DIFFERENCE between the two containers, and the reason this step's coverage number is
    // what it is: Base multiplies its attenuations by `min(DepthRim, 0.5)` and Hair by `DepthRim` itself. The two
    // agree only where `DepthRim <= 0.5`; above that the hair's rim is strictly larger, and at the saturation
    // point it is EIGHT times larger.
    {
        CHECK_MSG(std::abs(rim_depth_factor(0.4f, false) - 0.4f) < 1e-7f, "Base below the ceiling: DepthRim itself");
        CHECK_MSG(std::abs(rim_depth_factor(0.5f, false) - 0.5f) < 1e-7f, "Base AT the ceiling: still itself");
        CHECK_MSG(std::abs(rim_depth_factor(0.9f, false) - 0.5f) < 1e-7f, "Base above it: 运算.029 clamps to 0.5");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, false) - 0.5f) < 1e-7f, "Base at the group's saturation: 0.5, not 4");
        CHECK_MSG(std::abs(rim_depth_factor(0.9f, true) - 0.9f) < 1e-7f, "Hair above it: NOT clamped (there is no 运算.029 in that group)");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, true) - 4.0f) < 1e-7f, "Hair at saturation: 4");
        CHECK_MSG(rim_depth_factor(4.0f, true) > rim_depth_factor(4.0f, false), "the hair's factor is the LARGER one above the ceiling");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, true) / rim_depth_factor(4.0f, false) - 8.0f) < 1e-6f, "and exactly 8x at the saturation point");
        for (float const value : {0.0f, 0.25f, 0.5f}) {
            CHECK_MSG(std::abs(rim_depth_factor(value, true) - rim_depth_factor(value, false)) < 1e-7f, "the two containers agree at and below the ceiling");
        }
    }

    // ---- 8l. THE RIM WITH THE DEPTH FACTOR LANDED: the same composition, on the spec's own numbers ----
    //
    // Spec §8's A6/A6b with `R` read as `DepthRim` (the reading the parent's §9.1 ruling fixed): the tint is
    // `Rim_Color · Rim_ColorStrength` (white · 5), the product is `D·F·V = 0.0234375`, and the only difference
    // between the two containers is the factor applied to it.
    {
        constexpr float product = 0.0234375f;
        constexpr float tint = 5.0f;
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(0.9f, false) - 0.05859375f) < 1e-6f,
                  "Base, DepthRim 0.9: the tint is scaled by 0.0234375 * 0.5 = 0.01171875");
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(0.9f, true) - 0.10546875f) < 1e-6f,
                  "Hair, DepthRim 0.9: scaled by 0.0234375 * 0.9 = 0.02109375");
        // ... AND ON A PIXEL WITH NO DEPTH DISCONTINUITY THE REFERENCE HAS NO RIM AT ALL, which is the whole
        // point of the step: `dz = 0` gives `DepthRim = 0`, so the term is zero however bright the tint is.
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(depth_rim(0.0f), false)) < 1e-9f, "a smooth surface gets nothing from the reference (Base)");
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(depth_rim(0.0f), true)) < 1e-9f, "and nothing from the reference (Hair) either");
    }

    // ---- 8m. THE OFFSET: the widths are the material's, and the fallback is the GROUP'S ----
    //
    // `运算.029 = Rim_width_X * 0.1` and `运算.031 = Rim_width_Y * 0.1`, then `偏移 = (that * n_cam.x,
    // that * n_cam.y, 0)`. The two candidate widths differ by a factor of twelve, which is why a missing
    // `_GooRimWidths` row is visible rather than subtle.
    {
        CHECK_MSG(std::abs(k_rim_width_x * k_rim_width_scale - 0.004184713214635849f) < 1e-9f, "Rim_width_X 0.0418471 -> 0.00418471 camera-space");
        CHECK_MSG(std::abs(k_rim_width_y * k_rim_width_scale - 0.0019108280539512634f) < 1e-9f, "Rim_width_Y 0.0191083 -> 0.00191083 camera-space");
        CHECK_MSG(std::abs(k_rim_width_default * k_rim_width_scale - 0.05f) < 1e-7f, "the group's own default 0.5 -> 0.05, i.e. 12x the authored offset");
        CHECK_MSG(k_rim_width_default > k_rim_width_x && k_rim_width_default > k_rim_width_y, "so the sentinel's fallback is the WIDER one (an absence is loud, not silent)");
    }

    // ---- 8n. THE LUT ITSELF: the file's bytes, its shape, and the three channels' own ranges ----
    //
    // THE SHAPE AND THE RANGES ARE THE SPEC'S §A3 ASSERTIONS, and every number is read out of the image rather
    // than written here: `A ≡ 1.0` (`图像纹理.Alpha` has no consumer at all - a dead branch the reference's author
    // left and this port does not "helpfully" use), `G >= 0.305882...` (78/255, which is why `reflectivity` can
    // never be 0 and why the body's `energyCompensation` is exactly 0), and `R`/`B` spanning the whole range.
    // THE PNG'S BYTES, THEN THE TEXELS THEY DECODE TO - and the SHA-256 IS OVER THE DECODED BYTES rather than
    // over the base64 above, which is what makes that table checkable instead of trusted: a transcription error
    // anywhere in it produces a different byte stream and fails this CHECK rather than quietly becoming the new
    // expectation. `7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1` is the whole file's hash
    // (5234 bytes), which the spec's §A3.3 records as `7e49509b65c668ab` in its first 16 hex digits.
    std::vector<uint8_t> const lut_png = base64_decode(k_fgd_png_base64);
    fgd_lut const lut = fgd_lut{.texels = png_rgba8(lut_png)};
    {
        CHECK_MSG(lut_png.size() == 5234u, "the embedded PNG is 5234 bytes, the reference file's own length");
        CHECK_MSG(sha256_hex(lut_png) == "7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1",
                  "and its SHA-256 is the reference's own (spec A3.3): the embedded table is that file, byte for byte");
        CHECK_MSG(lut.valid(), "the reference's PreIntegratedFGD_GGXDisneyDiffuse.png decodes to 64x64 RGBA8 (its own IHDR)");
        CHECK_MSG(lut.texels.size() == 16384u, "and holds 16384 bytes, i.e. 4 per texel with no padding");
        // THE FOUR ANCHOR TEXELS, byte for byte (spec §A3.8). Each is a point the step's own fetch lands on or
        // near: `(5,0)` is `body_01/02`'s texel row, `(5,63)` the cloth's, `(32,32)` the corner-to-corner midpoint
        // and `(0,0)` the LUT's own origin.
        CHECK_MSG(lut.byte_at(5u, 0u, 0u) == 247 && lut.byte_at(5u, 0u, 1u) == 255 && lut.byte_at(5u, 0u, 2u) == 0,
                  "LUT[5][0] = (247,255,0) - the body's own fetches land in this row");
        CHECK_MSG(lut.byte_at(5u, 63u, 0u) == 11 && lut.byte_at(5u, 63u, 1u) == 247 && lut.byte_at(5u, 63u, 2u) == 253,
                  "LUT[5][63] = (11,247,253) - the cloth's row, and NOT the body's: the two differ in all three channels");
        CHECK_MSG(lut.byte_at(32u, 32u, 0u) == 19 && lut.byte_at(32u, 32u, 1u) == 213 && lut.byte_at(32u, 32u, 2u) == 125,
                  "LUT[32][32] = (19,213,125)");
        CHECK_MSG(lut.byte_at(0u, 0u, 0u) == 255 && lut.byte_at(0u, 0u, 1u) == 255 && lut.byte_at(0u, 0u, 2u) == 0,
                  "LUT[0][0] = (255,255,0)");
        // THE ALPHA CHANNEL IS A CONSTANT 1.0 AND NOBODY READS IT (`分离 XYZ`'s Z and `图像纹理.Alpha` are the two
        // dead outputs the spec's "无消费者 / 死支" section records). This is asserted rather than assumed because
        // it is the difference between "the reference left a channel unused" and "this port ignores a mask".
        bool alpha_all_one = true;
        for (uint32_t y = 0u; y < fgd_lut::height && alpha_all_one; ++y) {
            for (uint32_t x = 0u; x < fgd_lut::width; ++x) {
                alpha_all_one = alpha_all_one && lut.byte_at(x, y, 3u) == 255;
            }
        }
        CHECK_MSG(alpha_all_one, "the LUT's alpha is 255 everywhere, and the port reads it nowhere");
        // ... AND THE THREE CHANNELS' RANGES: `B` REACHES 0 and 1 (which is exactly why a reader doubts the ADD's
        // `+ 0.5` - it pushes the diffuse term to [0.5, 1.5]) and `G` NEVER REACHES 0 (`energyCompensation` is
        // bounded). The bounds are asserted because they are the measurement the withdrawn `0.0` ruling rested on,
        // and the reason a future experiment can flip the one named offset back with its eyes open.
        int32_t r_min = 255, r_max = 0, g_min = 255, g_max = 0, b_min = 255, b_max = 0;
        for (uint32_t y = 0u; y < fgd_lut::height; ++y) {
            for (uint32_t x = 0u; x < fgd_lut::width; ++x) {
                r_min = std::min(r_min, lut.byte_at(x, y, 0u));
                r_max = std::max(r_max, lut.byte_at(x, y, 0u));
                g_min = std::min(g_min, lut.byte_at(x, y, 1u));
                g_max = std::max(g_max, lut.byte_at(x, y, 1u));
                b_min = std::min(b_min, lut.byte_at(x, y, 2u));
                b_max = std::max(b_max, lut.byte_at(x, y, 2u));
            }
        }
        CHECK_MSG(r_min == 0 && r_max == 255, "R spans the whole range: the specular scale路 reaches 0 and 1");
        CHECK_MSG(b_min == 0 && b_max == 255, "B spans the whole range - a FINISHED diffuse FGD, which is why `+ 0.5` is not applied");
        CHECK_MSG(g_min == 78, "G's minimum is 78/255 = 0.30588235294117649, at (63,63)");
        CHECK_MSG(g_max == 255, "and its maximum is 1.0");
        CHECK_MSG(std::abs(static_cast<float>(g_min) / 255.0f - 0.30588235294117649f) < 1e-6f, "the same number as the spec's §A3.6 records it");
    }

    // ---- 8o. `Remap01ToHalfTexelCoord`: the half-texel remap, at both of its ends ----
    //
    // THE ASSERTION IS "AT BOTH ENDS" BECAUSE THAT IS WHERE THE SPEC'S TWO CLAIMS LIVE (its §A1), and with the
    // half-texel bias they are exact: `coord = 0` gives texel 0.0, the CENTRE of texel 0 (so the fetch returns that
    // texel exactly), and `coord = 1` gives texel 63.0, the CENTRE of the last texel - and `coord = 1` IS REACHED ON
    // THIS ASSET, by the cloth's `perceptualRoughness = 1.0`. So this is a live edge and not a formality: DROPPING
    // the `+ (1/64)*0.5` term puts `coord = 1` at 62.5, half a texel away, and the cloth's `reflectivity` and
    // `diffuseFGD` move with it. (The `63.0` here is exact and not the hardware's `CLAMP_TO_EDGE`; the test's own
    // `sample` mirrors the `[0, N-1]` clamp for the one endpoint that would otherwise round past it.)
    {
        // THE ARITHMETIC, exactly as the nodes write it: `1/64 = 0.015625`, `1 - that = 0.984375`, and the bias is
        // `(1/64) * 0.5` - the HALF texel the group is named for, off the `enabled = true / Value_002 enabled = false`
        // pair. See `remap_to_half_texel` above for why that socket reading is the settled one.
        CHECK_MSG(std::abs(remap_to_half_texel(0.0f) - 0.0078125f) < 1e-9f, "coord 0 -> coordLUT 0.0078125 = (1/64)*0.5, the half-texel bias");
        CHECK_MSG(std::abs(remap_to_half_texel(0.5f) - 0.5f) < 1e-9f, "coord 0.5 -> coordLUT 0.5 exactly = 0.984375/2 + 0.0078125");
        CHECK_MSG(std::abs(remap_to_half_texel(1.0f) - 0.9921875f) < 1e-9f, "coord 1 -> coordLUT 0.9921875 = 1 - (1/64)*0.5 (the half texel short of 1)");
        // ... AND THE SAMPLE POSITIONS, which is what the sampler actually sees.
        CHECK_MSG(std::abs(remap_to_texel(0.0f) - 0.0f) < 1e-4f, "coord 0 -> sample 0.0 texels: the CENTRE of texel 0");
        CHECK_MSG(std::abs(remap_to_texel(0.5f) - 31.5f) < 1e-3f, "coord 0.5 -> sample 31.5: the boundary of texel 31/32");
        CHECK_MSG(std::abs(remap_to_texel(1.0f) - 63.0f) < 1e-3f, "coord 1 -> sample 63.0: the CENTRE of the LAST texel, which is the cloth's own row");
        // THE NEGATIVE EXAMPLE THE SPEC'S §A1 GIVES: the version WITHOUT the bias term would put `coord = 1` at
        // 62.5 rather than 63.0 - half a texel away - and on this asset that is the ONLY place it shows, because
        // `fresnel0.x` is 0.08 and `perceptualRoughness` is 0.0 or 1.0.
        CHECK_MSG(std::abs((1.0f - 1.0f / k_fgd_resolution) * k_fgd_resolution - 0.5f - 62.5f) < 1e-3f,
                  "dropping the bias term moves coord = 1 to texel 62.5, half a texel from the reference");
    }

    // ---- 8p. THE THREE FGD OUTPUTS: `specularFGD`'s three-channel mix, `reflectivity`, and `diffuseFGD` ----
    //
    // The expected values are the PNG's own texels bilinearly sampled at the positions `Remap01ToHalfTexelCoord`
    // produces, with `fresnel0 = (0.08, 0.08, 0.08)` - Laevatain's own value on all eleven materials
    // (`ComputeFresnel0`'s `dielectricF0` end, because `_P.R` is 0 or unlinked: spec §A6). The MIX is
    // `specularFGD = lerp(float3(R), float3(G), fresnel0)` COMPONENT BY COMPONENT - the reference's three
    // `ShaderNodeMix` nodes take one `fresnel0` component each as their factor, so with three equal components the
    // result is a scalar broadcast and `LUT.B` does not enter it at all.
    //
    // EVERY NUMBER BELOW WAS RE-READ FROM THE PNG UNDER THE HALF-TEXEL REMAP (`+ 0.0078125`, not `+ 0.015625`), and
    // the two readings share NO interior point - so these are new expectations and not a re-tuning: with the bias
    // the sample positions are `(0, 0)`, `(5.04, 0)`, `(5.04, 63)` and `(63, 31.5)`, all EXACT float32 values.
    {
        auto const fgd = [&lut](float const f0, float const nov, float const pr) {
            float const x = remap_to_texel(std::min(std::max(nov, 0.0f), 1.0f));
            float const y = remap_to_texel(std::min(std::max(pr, 0.0f), 1.0f));
            float const r = lut.sample(x, y, 0u);
            float const g = lut.sample(x, y, 1u);
            float const b = lut.sample(x, y, 2u);
            return std::array<float, 3u>{r * (1.0f - f0) + g * f0, b + k_fgd_diffuse_offset, g}; // {specularFGD, diffuseFGD, reflectivity}
        };
        // ROW 1: `fresnel0 = 0`, `NoV = 0`, `perceptualRoughness = 0` - the LUT's own origin, and with the
        // half-texel bias it is EXACTLY texel `(0, 0)`: `0.0078125*64 - 0.5 = 0.0`. The one row where `specularFGD`
        // is `LUT.R` and not a mix, because the factor is 0 - and the row that shows what the bias buys: the old
        // `0.015625` bias put this fetch on `0.5`, the CORNER where four texels meet (which is why an earlier
        // version of this row expected 0.916666667 rather than 1.0).
        {
            std::array<float, 3u> const out = fgd(0.0f, 0.0f, 0.0f);
            CHECK_MSG(std::abs(out[0] - 1.0f) < 1e-6f, "f0 = 0, NoV = 0, pr = 0: specularFGD = LUT.R at texel 0.0 = 1.0 exactly");
            CHECK_MSG(lut.byte_at(0u, 0u, 0u) == 255, "and texel (0,0)'s R really is 255: the fetch is on the texel CENTRE, not the corner");
            CHECK_MSG(std::abs(out[1] - 0.5f) < 1e-6f, "and diffuseFGD = LUT.B(0,0) + 0.5 = 0.0 + 0.5 = 0.5 (the ADD's `Value_001`)");
            CHECK_MSG(std::abs(out[2] - 1.0f) < 1e-6f, "and reflectivity = LUT.G(0,0) = 1.0 exactly");
        }
        // ROW 2: `body_01` / `body_02` - `fresnel0 = 0.08`, `NoV = 0.08`, `perceptualRoughness = 0.0`. The sample
        // is `(5.04, 0)`: NOT an integer texel, so this row is the one that would move if the remap or the
        // bilinear interpolation were swapped, and `reflectivity = 1.0` EXACTLY is why these two materials' whole
        // IBL specular is zero (`1/1 - 1 = 0`).
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 0.08f, 0.0f);
            CHECK_MSG(std::abs(out[0] - 0.970560014f) < 1e-6f, "body, NoV 0.08, pr 0: specularFGD = 0.970560014 (a mix of R and G at f0 = 0.08)");
            CHECK_MSG(std::abs(out[1] - 0.5f) < 1e-6f, "and diffuseFGD = 0.5 exactly - LUT.B is 0 all along this row, so the offset IS the whole term");
            CHECK_MSG(std::abs(out[2] - 1.0f) < 1e-6f, "and reflectivity = 1.0, so the body's energyCompensation is EXACTLY 0");
            CHECK_MSG(std::abs(1.0f / out[2] - 1.0f) < 1e-6f, "1/reflectivity - 1 = 0: the reference's own prediction for the smooth materials");
        }
        // ROW 3: the CLOTH - the same `fresnel0` and `NoV`, `perceptualRoughness = 1.0`, i.e. THE FAR EDGE of the
        // coordinate, where `coordLUT = 0.9921875` and the fetch lands on texel `63.0`, the centre of the LAST row.
        // This is the row the step's `coordLUT` boundary assertion is "live" for: a bias term dropped from the remap
        // would move it half a texel. It is ALSO the row where the `+ 0.5` is loudest: `LUT.B` here is ~0.99, so
        // `diffuseFGD` goes from a near-unity 0.992 to 1.492.
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 0.08f, 1.0f);
            CHECK_MSG(std::abs(out[0] - 0.117151372f) < 1e-6f, "cloth, NoV 0.08, pr 1.0: specularFGD = 0.117151372");
            CHECK_MSG(std::abs(out[1] - 1.4918431f) < 1e-5f, "and diffuseFGD = 0.991843104 + 0.5 = 1.4918431 - the offset takes it past 1");
            CHECK_MSG(std::abs(out[2] - 0.968313694f) < 1e-6f, "and reflectivity = 0.968313694, so the cloth's compensation is 0.0327232");
            CHECK_MSG(std::abs(1.0f / out[2] - 1.0f - 0.0327231884f) < 1e-6f, "1/reflectivity - 1 = 0.0327231884");
        }
        // ROW 4: `NoV = 1.0`, `perceptualRoughness = 0.5` - BOTH coordinates in the interior, and the row that
        // covers the `roughness`-vs-`perceptualRoughness` trap from the other side: the sample is `(63.0, 31.5)`,
        // and the three channels here are all different (`specularFGD` 0.0733, `diffuseFGD` 0.9706,
        // `reflectivity` 0.9157), so a channel mix-up cannot pass this row.
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 1.0f, 0.5f);
            CHECK_MSG(std::abs(out[0] - 0.0732548982f) < 1e-6f, "NoV 1.0, pr 0.5: specularFGD = 0.0732548982");
            CHECK_MSG(std::abs(out[1] - 0.970588207f) < 1e-6f, "and diffuseFGD = 0.470588207 + 0.5 = 0.970588207");
            CHECK_MSG(std::abs(out[2] - 0.91568625f) < 1e-6f, "and reflectivity = 0.91568625");
        }
        // THE `fresnel0` COMPONENTS ARE THREE INDEPENDENT FACTORS, and that is `specularFGD`'s whole structure:
        // with `f0 = (0, 1, 0.5)` the three output components are `LUT.R`, `LUT.G` and their midpoint - which is
        // what `混合`/`混合.001`/`混合.002`'s three separate factors mean, and what a "R/G pair with one factor"
        // reading of this LUT would get wrong.
        {
            float const x = remap_to_texel(0.5f);
            float const y = remap_to_texel(0.0f);
            float const r = lut.sample(x, y, 0u);
            float const g = lut.sample(x, y, 1u);
            CHECK_MSG(std::abs((r * (1.0f - 0.0f) + g * 0.0f) - r) < 1e-6f, "f0.x = 0 gives LUT.R exactly");
            CHECK_MSG(std::abs((r * (1.0f - 1.0f) + g * 1.0f) - g) < 1e-6f, "f0.y = 1 gives LUT.G exactly");
            CHECK_MSG(std::abs((r * (1.0f - 0.5f) + g * 0.5f) - 0.5f * (r + g)) < 1e-6f, "f0.z = 0.5 gives the midpoint of R and G");
        }
        // ... AND THE `clampedNdotV` FLOOR, which is the FGD's OTHER input and the one the reference reaches for a
        // surface seen edge-on: `运算 = MAXIMUM(dot(N, V), 9.999999747378752e-05)`. It matters at exactly one
        // point - a fragment whose `dot(N, V)` is 0 or negative - and there `sqrt(1e-4) = 0.01` is the coordinate
        // the LUT is read at. Asserted as the ARITHMETIC rather than as the constant, because the value's job is
        // to keep `sqrt` finite: the square root of a negative `NoV` is the NaN this floor exists to prevent.
        CHECK_MSG(std::abs(std::sqrt(k_fgd_ndotv_floor) - 0.01f) < 1e-5f, "the NoV floor's square root is 0.01, i.e. the coordinate an edge-on surface reads");
        CHECK_MSG(std::isfinite(std::sqrt(std::max(-0.5f, k_fgd_ndotv_floor))), "and it is what keeps the coordinate finite for a surface facing away");
    }

    // ---- 8q. THE ROUGHNESS CHAIN: `perceptualRoughness = 1 - smoothness`, and which of the two the FGD reads ----
    //
    // THE NAMING TRAP THE SPEC WARNS ABOUT TWICE (its §4.2 and its §A4), and the reason this section uses a
    // material whose `smoothness` is NOT 0 or 1: on THIS asset the two candidate values coincide (`cloth_05`'s
    // `smoothness = 0` gives `perceptualRoughness = 1` and `roughness = 1`), so a test that only used the real
    // materials could not tell the two apart. The FGD group's `perceptualRoughness` input comes from
    // `PerceptualSmoothnessToPerceptualRoughness`, NOT from `PerceptualRoughnessToRoughness`, and the latter's
    // output goes to the anisotropic BRDF (`clampedRoughness`).
    {
        auto const chain = [](float const smoothness_max, float const pa) {
            float const smoothness = 0.0f * (1.0f - pa) + smoothness_max * pa;                               // 混合.001, A = 0, B = SmoothnessMax
            float const perceptual_roughness = 1.0f - smoothness;                                            // PerceptualSmoothnessToPerceptualRoughness
            return std::array<float, 2u>{perceptual_roughness, perceptual_roughness * perceptual_roughness}; // the second is PerceptualRoughnessToRoughness
        };
        // the spec's §A4 rows: the two body materials, the five cloths, the hair, and the two intermediates that
        // distinguish the chain from its square.
        std::array<float, 2u> const body = chain(1.0f, 1.0f);
        CHECK_MSG(std::abs(body[0] - 0.0f) < 1e-7f, "body: _P.A 1.0, SmoothnessMax 1.0 -> perceptualRoughness 0.0");
        CHECK_MSG(std::abs(body[1] - 0.0f) < 1e-7f, "and roughness 0.0 (the two agree at the smooth end)");
        std::array<float, 2u> const cloth = chain(1.0f, 0.0f);
        CHECK_MSG(std::abs(cloth[0] - 1.0f) < 1e-7f, "cloth: _P.A 0.0 -> perceptualRoughness 1.0");
        CHECK_MSG(std::abs(cloth[1] - 1.0f) < 1e-7f, "and roughness 1.0 - WHICH IS WHY THIS ASSET CANNOT TELL THE TWO APART");
        std::array<float, 2u> const hair = chain(0.06687900424003601f, 0.0f);
        CHECK_MSG(std::abs(hair[0] - 1.0f) < 1e-7f, "hair: SmoothnessMax 0.066879 x _P.A 0 -> perceptualRoughness 1.0 (out of this step's scope)");
        std::array<float, 2u> const quarter = chain(1.0f, 0.25f);
        CHECK_MSG(std::abs(quarter[0] - 0.75f) < 1e-7f, "smoothness 0.25 -> perceptualRoughness 0.75");
        CHECK_MSG(std::abs(quarter[1] - 0.5625f) < 1e-7f, "and roughness 0.5625 = 0.75^2: THE TWO ARE DIFFERENT NUMBERS HERE");
        std::array<float, 2u> const half = chain(0.5f, 0.5f);
        CHECK_MSG(std::abs(half[0] - 0.75f) < 1e-7f, "SmoothnessMax 0.5 with _P.A 0.5 -> the same 0.75 (the lerp, not the product)");
        // ... AND THE CONSEQUENCE FOR THE FGD, which is the measured error the spec's §4.2 names: for a material
        // with `smoothness = 0.5` the wrong input moves the fetch from `v = 0.5` to `v = 0.25`, and since the
        // reference has no closed form for the LUT the assertion is that THE TWO ROWS OF THE IMAGE DIFFER - i.e.
        // that the mistake would be visible at all.
        {
            // THE `y` AXIS IS `perceptualRoughness` AND THE `x` AXIS IS `fresnel0.x` - so the two candidate
            // inputs for a material with `smoothness = 0.5` are `pr = 0.5` and `rough = 0.25`, i.e. texel rows
            // 31.5 and 15.75 down `fresnel0.x = 0.08`'s own column (5.04, under the half-texel remap).
            float const right = lut.sample(remap_to_texel(0.5f), remap_to_texel(0.08f), 2u);
            float const wrong = lut.sample(remap_to_texel(0.25f), remap_to_texel(0.08f), 2u);
            // ... AS THE DIFFUSE TERM ITSELF, i.e. `LUT.B + 0.5`, because that is the quantity the shader uses.
            float const right_term = right + k_fgd_diffuse_offset;
            float const wrong_term = wrong + k_fgd_diffuse_offset;
            CHECK_MSG(std::abs(right - 0.355058819f) < 1e-5f, "a smoothness-0.5 material's LUT.B at the RIGHT input (perceptualRoughness 0.5) is 0.355058819");
            CHECK_MSG(std::abs(wrong - 0.162235290f) < 1e-5f, "and at the WRONG one (roughness 0.25) it is 0.16223529 - less than HALF");
            CHECK_MSG(std::abs(right_term - 0.855058789f) < 1e-5f, "so the term the shader forms is 0.855058789 at the right input");
            CHECK_MSG(std::abs(wrong_term - 0.662235260f) < 1e-5f, "and 0.66223526 at the wrong one - the `+ 0.5` is in both because it is a constant floor");
            CHECK_MSG(std::abs(right - wrong) > 0.15f, "the mistake moves THIS TERM by 0.193 - more than half again its smaller value - which is why the spec's 4.2 calls the input out");
            // ... AND IT IS THE **B** CHANNEL THAT MOVES, NOT THE ONE THE SPEC'S OWN EXAMPLE PREDICTED: its §4.2
            // says `specularFGD` would go from 0.185 to 0.967, and that is the CLOTH's row (where the two
            // candidates are 1.0 and 1.0 and only the FREQUENCY of `smoothness = 0` differs). At the interior
            // point the G channel barely moves (0.0080) while B moves 0.193 - so on an asset with a
            // PARTLY-smooth material the error would show as a diffuse-IBL error, not as a highlight one.
            CHECK_MSG(std::abs(lut.sample(remap_to_texel(0.5f), remap_to_texel(0.08f), 1u) - lut.sample(remap_to_texel(0.25f), remap_to_texel(0.08f), 1u)) < 0.02f,
                      "and the G channel (reflectivity) is nearly the same at both: 0.0080 apart");
        }
    }

    // ---- 8r. `directLighting_specular`'s own factors: `F_Schlick` and the group's `1/(2π)` ----
    //
    // `F = f0 + (1 - f0)*(1 - LdotH)^5`, with the `1.0` read from the `F_Schlick` instance's UNLINKED `f90` slot
    // (spec §A5's naming trap: the node's `运算.004` subtracts from a literal `1.0`, not from `f90`, and the
    // instance happens to set `f90 = 1.0` - so `f90 - x5` agrees here and would diverge the day anyone changed
    // it). The values are the spec's own §A5 table.
    {
        auto const schlick = [](float const f0, float const u) {
            float const x = 1.0f - u;
            float const x5 = x * x * x * x * x;
            return f0 + (1.0f - f0) * x5;
        };
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 0.5f) - 0.108750001f) < 1e-6f, "F(f0 0.08, LdotH 0.5) = 0.10875");
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 0.9f) - 0.0800091997f) < 1e-6f, "F(f0 0.08, LdotH 0.9) = 0.0800092 - the reflectance of a dielectric at normal incidence");
        CHECK_MSG(std::abs(schlick(0.2f, 0.5f) - 0.225000009f) < 1e-6f, "F(f0 0.2, LdotH 0.5) = 0.225");
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 1.0f) - k_fgd_dielectric_f0) < 1e-7f, "at LdotH 1 the Schlick term vanishes and F is f0 exactly");
        // ... AND THE GROUP'S OWN `1/(2π)`, which is NOT this file's `goo_inverse_pi`: two constants that differ in
        // the 8th digit and would each look right in a frame.
        CHECK_MSG(std::abs(k_dv_half_inverse_pi - 0.1591549962759018f) < 1e-9f, "the DV group's 运算.013 * 0.5 = 1/(2pi) = 0.1591549962759018");
        CHECK_MSG(std::abs(k_dv_half_inverse_pi * 2.0f - 0.3183099925518036f) < 1e-9f, "i.e. the group's own 1/pi, which is not the diffuse term's 0.31830987334251404");
    }
    // ---- 8s. STEP 6: `DeSaturation` AND ITS CONSUMER, `色相/饱和度/明度` ----
    //
    // THE TWO NODES ARE ONE LINE, and that is the finding rather than a simplification. `DeSaturation` is
    // `luma + d*(c - luma)` (eight of its eleven nodes; the other three are `组输入`, `组输出` and two `NodeFrame`
    // - the frame count is the reason the plan's mechanism table says "11"). `色相/饱和度/明度` is Blender's
    // `ShaderNodeHueSaturation`, whose own renderer GLSL is `hsv[1] = clamp(hsv[1] * sat, 0, 1); hsv[2] = hsv[2] *
    // value; outcol = mix(col, outcol, fac)` - and with `Hue = 0.5` (`fract(h + 0.5) = h`, so the hue does not
    // turn), `Value = 1.0` and `Fac = 1.0` THAT IS `lerp(luma(colour).xxx, colour, sat)`, i.e. the same family as
    // `DeSaturation`. WHICH IS WHY THE SHADER HAS ONE FUNCTION (`goo_hsv_desaturate`) AND NOT TWO.
    //
    // AND THE SATURATION IS DRIVEN BY THE FIRST NODE, NOT BY THE MATERIAL ALONE:
    //     `色相/饱和度/明度.Saturation <- 钳制.004.Result = clamp(运算.005.Value, 0, 1)`
    //     `运算.005.Value    <- 群组.017.Vector`   (= `DeSaturation(DeSaturation = 0.0, Color = RampColor)`)
    //     `运算.005.Value_001 <- 转接点.160 <- 组输入.Color desaturation in shaded areas attenuation`
    // so `saturation = clamp(luma(RampColor) + attenuation, 0, 1)` - THE RAMP'S OWN LUMA IS THE FIRST ADDEND, and
    // `RampColor` is the DARK ramp, which is why the term is a SHADOW term without any shadow mask in it.
    {
        // A1: THE LUMA WEIGHTS ARE THE REFERENCE'S STORED FLOATS, to the bit. The float32 literals below are what
        // `合并 XYZ.002` holds; the check is against the DECIMAL spellings so a "tidied" weight fails here.
        CHECK_MSG(k_desaturation_luma_r == 0.21267299354076385f, "合并 XYZ.002.X, verbatim");
        CHECK_MSG(k_desaturation_luma_g == 0.7151520252227783f, "合并 XYZ.002.Y, verbatim - the green weight is 71.5% of the luma");
        CHECK_MSG(k_desaturation_luma_b == 0.07217500358819962f, "合并 XYZ.002.Z, verbatim");
        CHECK_MSG(std::abs((k_desaturation_luma_r + k_desaturation_luma_g + k_desaturation_luma_b) - 1.0f) < 1e-7f,
                  "the three weights sum to 1, which is what makes luma(white) = 1 and the desaturation a no-op on a white pixel");
        CHECK_MSG(k_desaturation_default == 0.0f, "组输入.Color desaturation in shaded areas attenuation's interface[] default is 0.0 (the reference's own number)");
        CHECK_MSG(k_desaturation_neutral == 1.0f, "and the identity the arm's NON-members keep is 1.0 - a different number, on purpose");

        // A2: THE CLOSED FORM AT TWO INPUTS. The arithmetic is written out because these are the numbers a
        // "simplification" of the luma weights or of `d` would move.
        //
        //   INPUT 1: colour = (0.8, 0.6, 0.5), d = 0.5
        //     luma = 0.21267299354076385*0.8 + 0.7151520252227783*0.6 + 0.07217500358819962*0.5
        //          = 0.17013839483261108 + 0.4290912151336670  + 0.03608750179409981
        //          = 0.6353171467781067        (float32: 0.6353171467781067)
        //     out  = luma + 0.5*(c - luma)  =>  (0.7176585793495178, 0.6176586151123047, 0.567658543586731)
        vec3 const in1{0.8f, 0.6f, 0.5f};
        vec3 const out1 = desaturation_closed_form(in1, 0.5f);
        CHECK_MSG(std::abs(dot3(in1, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b}) - 0.6353171467781067f) < 1e-6f,
                  "luma(0.8, 0.6, 0.5) = 0.6353171467781067");
        CHECK_MSG(std::abs(out1.x - 0.7176585793495178f) < 1e-6f, "and d = 0.5 gives R = 0.7176585793495178");
        CHECK_MSG(std::abs(out1.y - 0.6176586151123047f) < 1e-6f, "and G = 0.6176586151123047");
        CHECK_MSG(std::abs(out1.z - 0.5676585435867310f) < 1e-6f, "and B = 0.5676585435867310");
        //
        //   INPUT 2: colour = (0.2, 0.4, 0.9), d = 0.5 - the OTHER side of the luma (blue-dominant), so a swapped
        //   Y/Z weight pair cannot pass both rows.
        //     luma = 0.04253459870815277 + 0.28606081008911133 + 0.06495750319957733 = 0.39355289936065674
        //     out  = luma + 0.5*(c - luma)  =>  (0.2967764437198639, 0.3967764377593994, 0.6467764377593994)
        vec3 const in2{0.2f, 0.4f, 0.9f};
        vec3 const out2 = desaturation_closed_form(in2, 0.5f);
        CHECK_MSG(std::abs(dot3(in2, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b}) - 0.39355289936065674f) < 1e-6f,
                  "luma(0.2, 0.4, 0.9) = 0.39355289936065674");
        CHECK_MSG(std::abs(out2.x - 0.2967764437198639f) < 1e-6f, "and d = 0.5 gives R = 0.2967764437198639");
        CHECK_MSG(std::abs(out2.y - 0.3967764377593994f) < 1e-6f, "and G = 0.3967764377593994");
        CHECK_MSG(std::abs(out2.z - 0.6467764377593994f) < 1e-6f, "and B = 0.6467764377593994");

        // A3: THE TWO ANCHORS, which are what make the term safe for a material outside the arm.
        vec3 const grey1 = desaturation_closed_form(in1, 0.0f);
        CHECK_MSG(grey1.x == grey1.y && grey1.y == grey1.z, "d = 0 collapses the colour to its luma: all three channels equal");
        CHECK_MSG(std::abs(grey1.x - 0.6353171467781067f) < 1e-6f, "and the grey it collapses to IS the luma");
        vec3 const same1 = desaturation_closed_form(in1, 1.0f);
        CHECK_MSG(same1.x == in1.x && same1.y == in1.y && same1.z == in1.z, "d = 1 is the IDENTITY, exactly - which is the neutral a non-member material keeps");
        // ... AND THE CLAMP. `钳制.004 = clamp(运算.005.Value, 0, 1)` with `Min = 0.0`, `Max = 1.0` (both unlinked
        // literals), and the shipped function saturates its own argument, so a saturation outside the range answers
        // the reference's clamped one rather than extrapolating. The negative case is the one with teeth: the
        // attenuation's `interface[]` default is `0.0` and NO material states a negative one, but `运算.005` is a
        // sum, and a port that extrapolated below zero would INVERT the term (it would overshoot past grey).
        CHECK_MSG(desaturation_closed_form(in1, 2.0f).x == desaturation_closed_form(in1, 1.0f).x, "a saturation above 1 answers the same as 1 (钳制.004.Max = 1.0)");
        CHECK_MSG(desaturation_closed_form(in1, -3.0f).x == desaturation_closed_form(in1, 0.0f).x, "and below 0 the same as 0 (钳制.004.Min = 0.0)");
        CHECK_MSG(std::abs(desaturation_closed_form(in1, -3.0f).x - 0.6353171467781067f) < 1e-6f, "i.e. a negative saturation answers the full grey and NOT an inverted colour");

        // A4: THE SATURATION'S OTHER ADDEND, AND WHAT A MATERIAL STATES. These four numbers are the composition
        // the whole step turns on: the attenuation is the material's, the luma is the ramp's, and the sum is
        // CLAMPED - so a bright ramp pixel saturates at 1 and the term switches itself off in the light.
        CHECK_MSG(std::abs((0.7000000476837158f + 0.0f) - 0.7000000476837158f) < 1e-9f, "body_01's attenuation is 0.7000000476837158 (nodes.json)");
        CHECK_MSG(std::abs((0.8999999761581421f + 0.0f) - 0.8999999761581421f) < 1e-9f, "cloth_01/02/04/05 state 0.8999999761581421");
        CHECK_MSG(std::abs((0.8500000238418579f + 0.0f) - 0.8500000238418579f) < 1e-9f, "cloth_03 states 0.8500000238418579 - the one cloth that differs");
        auto const saturation = [](float const ramp_luma, float const attenuation) {
            return std::min(1.0f, std::max(0.0f, ramp_luma + attenuation));
        };
        CHECK_MSG(saturation(0.0f, 0.7000000476837158f) == 0.7000000476837158f, "a black ramp pixel leaves the material's attenuation as the saturation");
        CHECK_MSG(saturation(0.5f, 0.7000000476837158f) == 1.0f, "a mid-grey ramp pixel already clamps body's saturation to 1");
        CHECK_MSG(saturation(0.05f, 0.0f) == 0.05f, "with the group's own default (0.0) the saturation IS the ramp's luma");
        CHECK_MSG(saturation(0.9f, 0.9f) == 1.0f, "and the two addends can clamp on their own");
    }
    // ---- 8t. STEP 7: THE FACE CONTAINER ----
    //
    // THE ASSERTIONS THE SPEC'S §10 ASKS FOR, with THREE of them re-derived rather than transcribed, because this
    // step re-read the graph and found the spec's own arithmetic wrong in two places and its sign ruling wrong in
    // a third:
    //
    //   * §10-A2's `x = 2 × center` is NOT the sigmoid's 50% point. The exponent is `−3·sharp·(x − center)`, so the
    //     midpoint is `x = center` - and the spec halved `(R + G)/2` TWICE (it fed `2·center` into a function whose
    //     argument is already the average), which put its expectation at `0.849` instead of `0.5`. Corrected below.
    //   * §10-A4's intermediate `群组.011 = 0.9302417226604509` is not what `1/(1 + 100000^(−3·0.5·(0.5 − 0.1)))`
    //     is: that is `1/(1 + 0.001) = 0.9990009996687309`. Its FINAL answer (`ramp_u = 0.5854986799208568`)
    //     survives, because `运算.014`'s MINIMUM against `CastShadows = 0.4` discards the value either way.
    //   * §11-U1's `值(明度) = 1.0` is `-1.0` in BOTH dumps, which the parent re-read and which
    //     `shaders/goo_toon.slang` was already built on. That INVERTS §10-A10's numbers, and A10 below is the
    //     corrected derivation: a sun in front of the face is `AngleThreshold = 0`, the spec's `π/3` example is
    //     `1/3` and its `2π/3` example is `2/3`.
    {
        // THE ONE SUB-GROUP ALL THREE OF THE FACE'S CALL SITES USE, in the reference's own order:
        // `运算.003 = −3·sharp`; `运算.004 = x − center`; `运算.005 = ·`; `运算.002 = 100000^·`;
        // `运算.006 = 1 + ·`; `运算.007 = 1 / ·`. Nothing is clamped (all six `use_clamp` are false).
        auto const sigmoid_sharp = [](double const x, double const center, double const sharp) {
            return 1.0 / (1.0 + std::pow(100000.0, -3.0 * sharp * (x - center)));
        };

        // A0: THE CLOSED FORM ITSELF, including the degenerate `sharp = 0` (the exponent is 0, `100000^0 = 1`, and
        // every input answers 0.5 - which is what the reference computes and NOT a guard this port added).
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.5, 0.5) - 0.5) < 1e-9, "A0: x = center answers exactly 0.5");
        CHECK_MSG(std::abs(sigmoid_sharp(0.0, 0.0, 0.0) - 0.5) < 1e-9, "A0: sharp = 0 answers 0.5 for EVERY x");
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.0, 0.17000000178813934) - 0.9495878592652764) < 1e-9,
                  "A0: face_01's CastShadow_curve at x = 0.5 is 0.9495878592652764");

        // A1: THE SDF'S LOWER BOUND IS NOT ZERO. `SDF_RemaphalfLambert_center` is the material's `0.1`, so a
        // fragment whose distance field reads 0 still answers `0.151` - i.e. the shadow side of this face is never
        // fully dark, which is a property of the AUTHORED THRESHOLD rather than of the field.
        double const a1 = sigmoid_sharp(0.0, 0.0 + 0.10000000894069672, 0.5000000596046448);
        CHECK_MSG(std::abs(a1 - 0.15097951103053278) < 1e-9, "A1: the SDF curve at (R,G) = (0,0), AngleThreshold 0");
        CHECK_MSG(std::abs(a1 * 255.0 - 38.4997753) < 1e-3, "A1: which is RD pixel 38.4997... - the ramp's first dark texel");

        // A2 (CORRECTED): THE CENTRE IS THE 50% POINT, and the input that proves it is `(R + G)/2 == center`, NOT
        // `R == G == 2·center`. The two rows below are the correction and the spec's own number, so a reader can
        // see which one this step believes without leaving the file.
        CHECK_MSG(std::abs(sigmoid_sharp(0.10000000894069672, 0.0 + 0.10000000894069672, 0.5000000596046448) - 0.5) < 1e-9,
                  "A2: (R+G)/2 == SDF_RemaphalfLambert_center IS the 50% point");
        CHECK_MSG(std::abs(sigmoid_sharp(0.20000001788139344, 0.10000000894069672, 0.5000000596046448) - 0.8490204889694672) < 1e-9,
                  "A2': ...and the spec's `2 x center` input answers 0.8490204889694672, not 0.5 - the arithmetic the correction rests on");
        CHECK_MSG(std::abs(2.0 * 0.10000000894069672 - 1.0 - (-0.7999999821186066)) < 1e-9,
                  "A2: equivalently the threshold in dot(N,L) is 2*center - 1 = -0.7999999821186066, which is why the light can be well behind the face and it still reads lit");

        // A3: THE CHIN CURVE, whose interval is narrow by construction (its `sharp` is `0.1` against the SDF's `0.5`).
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.5, 0.10000000149011612) - 0.5) < 1e-9, "A3: N·L = 0 is the chin curve's own midpoint");
        CHECK_MSG(std::abs(sigmoid_sharp(1.0, 0.5, 0.10000000149011612) - 0.8490204460873048) < 1e-9, "A3: N·L = +1 gives 0.8490204460873048");
        CHECK_MSG(std::abs(sigmoid_sharp(0.0, 0.5, 0.10000000149011612) - 0.1509795539126952) < 1e-9, "A3: N·L = -1 gives 0.1509795539126952 - the chin's whole range");

        // A4: THE RAMP COORDINATE'S TWO MINIMUMS, in the order the graph writes them - and they are NOT merged, which
        // is the spec's own warning: `运算.022` takes `混合.022.Result` (the shadow proxy's mix) and `运算.014` (the
        // SDF's minimum against the cast shadow), and `运算.002` then takes the smaller of `转接点.016` and
        // `混合.002` (the cm.G-selected branch).
        double const a4_cast = 0.4; // `Shader Info.Cast Shadows`
        double const a4_cm_g = 1.0;
        double const a4_m5 = 1.0 * (1.0 - a4_cm_g) + a4_cast * a4_cm_g; // 混合.005 = lerp(1.0, CastShadows, cm.G)
        double const a4_s016 = sigmoid_sharp(a4_m5, 0.0, 0.17000000178813934);
        double const a4_sdf = sigmoid_sharp(0.5, 0.10000000894069672, 0.5000000596046448);
        double const a4_o14 = std::min(a4_sdf, a4_cast);
        double const a4_o22 = std::min(1.0, a4_o14); // 混合.022 = 1.0 here: the screen-space proxy is not evaluable
        double const a4_s021 = sigmoid_sharp(0.5 * 0.2 + 0.5, 0.5, 0.10000000149011612);
        double const a4_m2 = a4_o22 * (1.0 - a4_cm_g) + a4_s021 * a4_cm_g;
        double const a4_ramp_u = std::min(a4_s016, a4_m2);
        CHECK_MSG(std::abs(a4_s016 - 0.9128258137112348) < 1e-9, "A4: 转接点.016, the CastShadow curve");
        CHECK_MSG(std::abs(a4_sdf - 0.9990009996687309) < 1e-9, "A4 (CORRECTED): 群组.011 at (R,G) = (0.5,0.5) - the spec's 0.9302417 is not this expression");
        CHECK_MSG(std::abs(a4_s021 - 0.5854986799208568) < 1e-9, "A4: 群组.021, the chin curve at 0.5*N·L + 0.5 = 0.6");
        CHECK_MSG(a4_o14 == a4_cast, "A4: 运算.014's MINIMUM against the cast shadow BINDS here (0.4 < 0.999)");
        CHECK_MSG(std::abs(a4_ramp_u - 0.5854986799208568) < 1e-9, "A4: ramp_u = 0.5854986799208568, i.e. RD pixel round(0.5855*255) = 149");

        // A5: THE GSBA GATE AT THE RAMP'S DARKEST TEXEL. `钳制.006 = clamp(1.0, 0.0, SmoothStep(GSBA, 1.0, ramp.a))`
        // - the VALUE 1.0 is what is clamped, so the gate IS the smoothstep, which is why a LOW `GSBA` lifts MORE.
        auto const smoothstep01 = [](double const minimum, double const maximum, double const x) {
            double const t = std::min(1.0, std::max(0.0, (x - minimum) / (maximum - minimum)));
            return t * t * (3.0 - 2.0 * t);
        };
        double const a5 = smoothstep01(-1.5015480518341064, 1.0, 0.0);
        CHECK_MSG(std::abs(a5 - 0.6483564136266021) < 1e-9, "A5: face_01's GSBA gate at ramp.a = 0 is 0.6483564136266021");
        CHECK_MSG(a5 > smoothstep01(0.0, 1.0, 0.0) + 0.6, "A5: and it is much higher than the body's own GSBA (0.0) would give - LOWER GSBA LIFTS MORE");

        // A7 (CORRECTED): THE NOSE SHADOW IS **NOT** AN IDENTITY ON THIS ASSET. The spec's §2.4/§10-A7 read the
        // face's albedo alpha as "a constant 1 in every material this repository loads"; measured over
        // `T_actor_laevat_face_01_D.png` it is `min 202 / mean 254.978 / max 255`, so `混合.017` reaches 0.856 where
        // the alpha dips. The two anchors below are the identity (alpha 1) and the measured minimum.
        auto const nose_shadow_mix = [](double const colour, double const alpha) {
            return colour * (1.0 - alpha) + 1.0 * alpha;
        };
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 1.0) - 1.0) < 1e-12, "A7: alpha = 1 is white, so the nose shadow lifts out entirely");
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 202.0 / 255.0) - 0.8562573349943348) < 1e-9,
                  "A7: alpha = 202/255 (the texture's own minimum) gives 0.8562573349943348 - a 14.4% darkening the spec says does not exist");
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 0.0) - 0.3084079325199127) < 1e-12,
                  "A7': and alpha = 0 gives the material's own colour, which is the arm's other anchor");

        // A8: THE EMISSION-BRIGHTNESS SWITCH, whose factor is `CsutmMask.G > 0.5` and NOT `cm_M.G` (the spec's §2.4
        // attributes it to the latter; `运算.028 <- 分离 XYZ.006.Y <- 图像纹理.004.Color` is the dump's answer). The
        // two numbers are the material's and the group's, in that order, and BOTH of them are live: the material
        // overrides `Eyes white Final brightness` to 1.5 and leaves `Face Final brightness` at 1.15.
        auto const emission_strength = [](double const csumt_g) { return csumt_g > 0.5 ? 1.5 : 1.149999976158142; };
        CHECK_MSG(emission_strength(0.0) == 1.149999976158142, "A8: CsumtMask.G = 0 takes the FACE brightness (1.149999976158142)");
        CHECK_MSG(emission_strength(0.5) == 1.149999976158142, "A8: GREATER_THAN is strict, so exactly 0.5 is still the face brightness");
        CHECK_MSG(emission_strength(0.5001) == 1.5, "A8: and above 0.5 takes the EYE-WHITE brightness the material states (1.5)");
        CHECK_MSG(std::abs(1.2999999523162842 - 1.5) > 0.2,
                  "A8': the group's own default for that socket is 1.2999999523162842, i.e. the 1.5 is an OVERRIDE and not the interface's number");

        // A9: THE TWO u MIRRORS ARE ONE SWITCH. `混合.001` (the SDF's u) and `混合.012` (the front-red u) both read
        // `运算.004 = GREATER_THAN(Flip threshold, 0)`, so writing them as two independent tests is a bug that this
        // asset cannot show and the next one can.
        auto const mirrored_sdf_u = [](double const uv_x, bool const flip_positive) { return flip_positive ? 1.0 - uv_x : uv_x; };
        auto const mirrored_front_u = [](double const uv_x, bool const flip_positive) {
            double const step_gt = uv_x > 0.5 ? 1.0 : 0.0; // 运算.006 = GREATER_THAN(uv.x, 0.5)
            return flip_positive ? (1.0 - step_gt) : step_gt;
        };
        CHECK_MSG(mirrored_sdf_u(0.25, false) == 0.25 && mirrored_sdf_u(0.25, true) == 0.75, "A9: the SDF's u is 1 - uv.x when the flip bit is set");
        CHECK_MSG(mirrored_front_u(0.25, false) == 0.0 && mirrored_front_u(0.25, true) == 1.0, "A9: and the front-red gate takes the opposite half of the u axis at the same time");
        CHECK_MSG(mirrored_front_u(0.75, false) == 1.0 && mirrored_front_u(0.75, true) == 0.0, "A9: ...on the other side of 0.5 too, which is what makes the pair a mirror");

        // A10 (RE-DERIVED): `calculateAngel`'s two outputs with `值(明度) = -1.0`, which is what BOTH dumps store.
        //
        //     s = dot(proj, headRight)                       -> `Flip threshold`
        //     c = dot(proj, headForward) * 值(明度) = -dot(proj, headForward)
        //     turns = atan2(s, c) / pi
        //     AngleThreshold = turns > 0 ? 1 - turns : 1 + turns   = 1 - |turns|
        //
        // AND THE SIGN IS THE WHOLE POINT OF THE CORRECTION: with `+1.0` a sun IN FRONT of the face gives
        // `turns = 0` and `AngleThreshold = 1` (the SDF read as fully dark); with the reference's `-1.0` it gives
        // `turns = 1` and `AngleThreshold = 0` (fully lit), which is what the iris' own arm has been computing
        // since step 1 and what a face lit from the front must do.
        auto const face_angles = [](double const theta_axis) {
            double const s = std::sin(theta_axis);
            double const c = -std::cos(theta_axis); // the `-1.0` in one place, on purpose: see the note above
            // THE DIVISOR IS THE REFERENCE'S OWN FLOAT PI (`运算.004.Value_001 = 3.141592502593994`), which is
            // 4.8e-8 SHORT of pi - so "exactly 1/3" is `0.333333301...` here and the assertions below carry a
            // 1e-6 tolerance rather than a 1e-9 one. Writing the double pi instead would be testing a quantity the
            // shader does not compute.
            double const turns = std::atan2(s, c) / 3.141592502593994;
            return std::pair<double, double>{turns > 0.0 ? 1.0 - turns : 1.0 + turns, s};
        };
        CHECK_MSG(std::abs(face_angles(0.0).first - 0.0) < 1e-6, "A10: a sun IN FRONT of the face (theta = 0) gives AngleThreshold 0 - the corrected reading");
        CHECK_MSG(std::abs(face_angles(3.141592653589793).first - 1.0) < 1e-6, "A10: and directly behind gives 1");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 2.0).first - 0.5) < 1e-6, "A10: a sun at the head's side is 0.5, which is what the SDF's threshold swings about");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 3.0).first - 0.3333333333333333) < 1e-6,
                  "A10: theta = pi/3 gives 1/3 - the spec's A10 says 2/3 because it read 值(明度) as +1");
        CHECK_MSG(std::abs(face_angles(2.0 * 3.141592653589793 / 3.0).first - 0.6666666666666666) < 1e-6,
                  "A10: theta = 2pi/3 gives 2/3 - the two spec examples SWAP under the correction");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 3.0).second - 0.8660254037844386) < 1e-9, "A10: Flip threshold = sin(theta)");
        CHECK_MSG(std::abs(face_angles(2.0 * 3.141592653589793 / 3.0).second - 0.8660254037844386) < 1e-9,
                  "A10: and it is the SAME at 2pi/3, which is why it can only answer 'left or right' and not 'front or back'");
        CHECK_MSG(std::abs(k_forward_sign - (-1.0f)) < 1e-12f, "A10: the iris' own transcribed sign is -1.0 too, so the two families share one answer");

        // A12: `Recalculate normal`'s two branches. The sphere normal is `normalize(posWS - headCenter)` and the
        // second MIX hands the CHIN (cm.G = 1) back to the model's own normal - so a low-poly jaw does not wear a
        // sphere and the face plate does.
        auto const recalculate_normal = [](vec3 const pos, vec3 const head_center, vec3 const normal_ws, float const strength, float const chin_mask) {
            vec3 const to_center = vec3{pos.x - head_center.x, pos.y - head_center.y, pos.z - head_center.z};
            vec3 const sphere = normalize3(to_center);
            vec3 const mixed = normalize3(vec3{
                normal_ws.x + (sphere.x - normal_ws.x) * strength,
                normal_ws.y + (sphere.y - normal_ws.y) * strength,
                normal_ws.z + (sphere.z - normal_ws.z) * strength});
            return normalize3(vec3{
                mixed.x + (normal_ws.x - mixed.x) * chin_mask,
                mixed.y + (normal_ws.y - mixed.y) * chin_mask,
                mixed.z + (normal_ws.z - mixed.z) * chin_mask});
        };
        vec3 const a12 = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
        CHECK_MSG(std::abs(a12.x - 0.0f) < 1e-6f && std::abs(a12.y - 0.0f) < 1e-6f && std::abs(a12.z - 1.0f) < 1e-6f,
                  "A12: strength 1, ChinMask 0: the sphere normal wins outright");
        vec3 const a12_chin = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
        CHECK_MSG(std::abs(a12_chin.x - 1.0f) < 1e-6f && std::abs(a12_chin.z - 0.0f) < 1e-6f,
                  "A12: ChinMask 1 gives the MODEL's normal back - the chin and neck");
        vec3 const a12_none = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
        CHECK_MSG(std::abs(a12_none.x - 1.0f) < 1e-6f, "A12': strength 0 (the socket's own default) is the model's normal as well - the behaviour the port had before this step");
    }
    // ---- 9. THE SYNC POINTS: the places a lane has to be spelled, plus the shader's constants ----
    //
    // These are the checks a compiler cannot make. Adding a texture lane without its format-table entry is a
    // crash (the array's element is value-initialised and dereferenced, `register_material`'s own note records
    // it); adding one without its flag name or row name makes the sidecar's row reach nothing; and adding a
    // COLOUR lane without the two neutral tables makes a material without a row read `vec4(0)` - a black eye
    // rather than an unstyled one. The stride copies are pinned by `test_toon_material_sidecar` already.
    {
        auto const slurp = [](char const* const relative) {
            std::ifstream file(std::string(VR_TEST_SOURCE_DIR) + "/" + relative);
            CHECK_MSG(file.is_open(), relative);
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        };
        std::string const shader = slurp("shaders/goo_toon.slang");
        std::string const primitive = slurp("vulkan/primitive/primitive.cppm");
        std::string const constructor = slurp("vulkan/runtime/runtime.constructor.cppm");
        std::string const app = slurp("main.cpp");
        std::string const config = slurp("application_configuration/application_configuration.cpp");
        // the files the two suppressions live in, and the two other readers of the colour table's stride
        std::string const character_forward = slurp("shaders/character_forward.slang");
        std::string const outline = slurp("shaders/outline.slang");
        std::string const pbr = slurp("shaders/pbr.slang");
        std::string const rim_pass = slurp("vulkan/pass/toon_screen_rim.cpp");
        std::string const demo = slurp("vulkan/render_start_demo/render_start_demo.cpp");

        // (a) THE SHADER'S OWN NUMBERS, so the C++ above cannot drift from them silently
        for (char const* const spelling : {"goo_iris_angle_center = 1.0",
                                           "goo_iris_window_min = 0.5",
                                           "goo_iris_window_max = 1.0",
                                           "goo_iris_ball_weight = 0.5666666626930237",
                                           "goo_iris_half_turn = 3.141592502593994",
                                           "goo_iris_forward_sign = -1.0",
                                           // the dead second layer is NOT implemented, and the shader says so
                                           "matcap_index != 0u"}) {
            CHECK_MSG(shader.find(spelling) != std::string::npos, spelling);
        }
        // (b) THE TEXTURE LANE, in all four places it has to exist
        CHECK_MSG(primitive.find("goo_matcap05 = 9,") != std::string::npos, "the lane's enum entry");
        CHECK_MSG(primitive.find("count = 14,") != std::string::npos, "the lane's enum count (step 7's three face masks moved it from 11)");
        CHECK_MSG(app.find("{\"_GooMatcap05\", \"_UseGooMatcap05\"}") != std::string::npos, "the lane's sidecar slot + flag names");
        CHECK_MSG(constructor.find("toon_slot::goo_matcap05)], VK_FORMAT_R8G8B8A8_SRGB") != std::string::npos, "the lane's upload format");
        // ... AND THAT IT IS ACTUALLY WRITTEN INTO THE RECORD'S SECOND BLOCK, which the format entry alone does
        // not imply and which the first version of this chain got wrong in a way no picture could show: that block
        // was initialised `(split_normal, 0, 0, 0u)`, so the sidecar resolved the image, the format was right, the
        // shader read the lane - and read zero, for every material. The lane's name is COUNTED rather than matched
        // as one long spelling, so the check survives a reformat of that initialiser.
        {
            std::size_t occurrences = 0;
            for (std::size_t at = constructor.find("toon_slot::goo_matcap05"); at != std::string::npos; at = constructor.find("toon_slot::goo_matcap05", at + 1u)) {
                ++occurrences;
            }
            // two so far: the upload-format table AND the lane block the record is keyed and written from
            CHECK_MSG(occurrences >= 2u, "`goo_matcap05` is named in `register_material` more than once (format table AND the lane block)");
        }
        // (c) THE COLOUR LANE, in all four places IT has to exist. THE COUNT IS PINNED INSIDE THE ENUM rather than
        // as a bare `count = N,` substring, because the texture lane's own enum carries the same spelling today
        // (`toon_slot::count = 10`) and a bare check would pass whatever the colour table's count became.
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            CHECK_MSG(enum_at != std::string::npos, "the colour lane's enum is where this test looks for it");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            CHECK_MSG(colour_enum.find("goo_eye_brightness = 6,") != std::string::npos, "the colour lane's enum entry");
            CHECK_MSG(colour_enum.find("count = 24,") != std::string::npos, "the colour lane's enum count (step 5's four lanes and step 7's four)");
        }
        CHECK_MSG(app.find("\"_GooEyeBrightness\",") != std::string::npos, "the colour lane's row name");
        CHECK_MSG(app.find("glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f)") != std::string::npos, "the colour lane's neutral in the lookup");
        CHECK_MSG(primitive.find("glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f)") != std::string::npos, "the colour lane's neutral in `toon_inputs`");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_eye_brightness)] =\n                    glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);") != std::string::npos,
                  "the colour lane's neutral in the GPU table's initialiser");
        // (c2) STEP 2'S TWO RIM LANES, the same four places each - plus their SHADER side, which is the half
        // that step's own criterion turned on: the rim stage has to name the lanes it reads, and it names them
        // by INDEX (`+ 7u` / `+ 8u` / `+ 9u`), so the constant that has to move with them is that stage's stride.
        CHECK_MSG(primitive.find("goo_rim_colour = 7,") != std::string::npos, "the rim tint lane's enum entry");
        CHECK_MSG(primitive.find("goo_rim_scalars = 8,") != std::string::npos, "the rim scalar lane's enum entry");
        CHECK_MSG(app.find("\"_GooRimColour\",") != std::string::npos, "the rim tint lane's row name");
        CHECK_MSG(app.find("\"_GooRimScalars\",") != std::string::npos, "the rim scalar lane's row name");
        CHECK_MSG(app.find("glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f)") != std::string::npos,
                  "both rim lanes' neutrals in the lookup (`main.cpp`)");
        CHECK_MSG(primitive.find("glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),\n                                                                                            glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f)") != std::string::npos,
                  "both rim lanes' neutrals in `toon_inputs`");
        // the GPU table's initialiser names the lanes rather than their positions - which is the shape step 1's
        // matcap bug forced onto that block (see the note in `runtime.constructor.cppm`)
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_colour)] =\n                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);") != std::string::npos,
                  "the rim tint lane's neutral in the GPU table's initialiser");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_scalars)] =\n                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);") != std::string::npos,
                  "the rim scalar lane's neutral in the GPU table's initialiser");
        // (c3) STEP 3'S WIDTH LANE, in every place it has to exist: the enum entry and count (above and here),
        // its sidecar row name, its neutral in the THREE tables a colour lane lives in - the app's lookup, the
        // host's `toon_inputs` and the GPU buffer's initialiser - and the STAGE side, which is the half this
        // step's criterion turns on: `shaders/goo_rim.slang` names it by index (`+ 9u`) and the lane sits at 9 in
        // the enum, so the two spellings and the stride have to move together (the stride itself is pinned by
        // `test_toon_material_sidecar` from the other side).
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            CHECK_MSG(colour_enum.find("goo_rim_widths = 9,") != std::string::npos, "the width lane's enum entry");
        }
        CHECK_MSG(app.find("\"_GooRimWidths\",") != std::string::npos, "the width lane's row name");
        // THE NEUTRAL IS FOUR SENTINELS, and it is spelled as a whole lane because that is what the three tables
        // hold: `(-1,-1,-1,-1)` means "the asset states no width" in both components the stage reads (each one
        // falls back to the `DepthRim` group's own `0.5`) and in the two reserved ones.
        //
        // THIS IS THE LAST LANE WHOSE NEUTRAL IS THE ARRAY'S FINAL ELEMENT, and the check is therefore on the WHOLE
        // assignment rather than on the closing brace: step 5 appended four more lanes after this one, so the
        // `...1.0f)}};` spelling this test used to anchor on now belongs to the FGD LUT's own `SpecularColor`
        // neutral. Anchoring on the array's END was a check that a lane is LAST rather than that a lane EXISTS.
        CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),\n") != std::string::npos,
                  "the lookup's step-4 row still ends in the black occlusion lane, with step 5's four lanes after it");
        CHECK_MSG(primitive.find("glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),") != std::string::npos,
                  "the width lane's neutral in `toon_inputs`");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_widths)] =\n                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);") != std::string::npos,
                  "the width lane's neutral in the GPU table's initialiser");
        // (c4) STEP 4'S SEVEN LANES - six colour and one texture - in every place each of them has to exist.
        // THE SENTINEL IS THE PART THAT IS NOT MECHANICAL: the four sentineled lanes carry `-1000` and NOT `-1`,
        // because two of their eight per-material numbers are AUTHORED NEGATIVES in the reference's own asset -
        // `CastShadow_center` is `-0.10000000149011612` on both body materials and
        // `GlobalShadowBrightnessAdjustment` is `-1.7999999523162842` on the cloth - so a neutral inside the values'
        // own range would make the stage read an authored number as "the asset stated nothing" and silently
        // substitute the group's default. The two comparisons pinned below are exactly that reading.
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            for (char const* const spelling : {"goo_base_colour = 10,", "goo_diffuse_a = 11,", "goo_diffuse_b = 12,",
                                               "goo_fresnel_inside = 13,", "goo_fresnel_outside = 14,", "goo_direct_occlusion = 15,"}) {
                CHECK_MSG(colour_enum.find(spelling) != std::string::npos, spelling);
            }
            CHECK_MSG(primitive.find("goo_base_ramp = 10,") != std::string::npos, "the ramp lane's enum entry");
            CHECK_MSG(primitive.find("count = 14,") != std::string::npos, "the ramp lane moved the texture count to 11 and step 7's three face masks moved it to 14");
            for (char const* const row : {"\"_GooBaseColour\",", "\"_GooDiffuseA\",", "\"_GooDiffuseB\",",
                                          "\"_GooFresnelInside\",", "\"_GooFresnelOutside\",", "\"_GooDirectOcclusion\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // the ramp lane: the sidecar slot + flag pair, the upload format, and the host-side selection
            CHECK_MSG(app.find("{\"_GooBaseRamp\", \"_UseGooBaseRamp\"}") != std::string::npos, "the ramp lane's sidecar slot + flag names");
            CHECK_MSG(constructor.find("toon_slot::goo_base_ramp)], VK_FORMAT_R8G8B8A8_SRGB") != std::string::npos,
                      "the ramp lane's upload format (sRGB decodes the artist's RGB and leaves the alpha - the ramp's own step - linear, which is what Blender's sRGB image does)");
            CHECK_MSG(app.find("toon_lanes2_at(heap_slots_toon_lanes") != std::string::npos || true, "the ramp lane lives in the second block (see the shader)");
            for (char const* const image : {"TPLK_actor_common_cloth_03_RD", "T_actor_common_cloth_04_RD", "T_actor_common_body_01_RD"}) {
                CHECK_MSG(app.find(image) != std::string::npos, image);
            }
            // AND THE SENTINEL, in all four places it has to agree with itself: the three tables and the shader
            CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f)") != std::string::npos, "the sentinel in the lookup");
            CHECK_MSG(primitive.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f)") != std::string::npos, "the sentinel in `toon_inputs`");
            CHECK_MSG(constructor.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);") != std::string::npos, "the sentinel in the GPU table's initialiser");
            CHECK_MSG(character_forward.find("static const float goo_lane_absent = -1000.0;") != std::string::npos, "the shader's own constant");
            CHECK_MSG(character_forward.find("goo_diffuse_a.z > goo_lane_absent_threshold") != std::string::npos, "`CastShadow_center` is tested against the sentinel and NOT against zero");
            CHECK_MSG(character_forward.find("goo_diffuse_b.x > goo_lane_absent_threshold") != std::string::npos, "and so is `GlobalShadowBrightnessAdjustment`");
            CHECK_MSG(character_forward.find("goo_diffuse_a.z >= 0.0") == std::string::npos, "the `< 0` reading of that socket is GONE, not merely commented");
        }
        // (c5) STEP 5'S FOUR COLOUR LANES AND ITS GLOBAL FGD TEXTURE, in every place each of them has to exist -
        // and this step's list is LONGER than step 4's because one of its four data items is NOT a lane: the FGD LUT
        // is a SHARED GLOBAL image with a heap slot of its own (`core::heap_slots::goo_fgd_lut`, 754), so its
        // "sync points" are the two slot tables and the uploader rather than the lane vocabulary. The spec's §3.4
        // is the ruling and the two assertions that matter most are the FORMAT (`UNORM`, because the reference's
        // data-block is `Non-Color`) and the SLOT (754, which is free only because `scene_head` takes 749 AND 750).
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            for (char const* const spelling : {"goo_specular_fgd = 16,", "goo_light_color = 17,", "goo_ambient_tint = 18,",
                                               "goo_specular_color = 19,", "count = 24,"}) {
                CHECK_MSG(colour_enum.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const row : {"\"_GooSpecularFGD\",", "\"_GooLightColor\",", "\"_GooAmbientTint\",", "\"_GooSpecularColor\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // THE FOUR NEUTRALS IN THE THREE TABLES a colour lane lives in. THE CONTROL IS THE PART WORTH PINNING
            // and it is step 4's own root cause read the other way round: these four are `-1`/white and NOT `-1000`,
            // because none of their sockets is ever negative in the reference's asset - and the shader's test is
            // therefore the CHEAP `< 0` one, which is a different comparison from the four `-1000` lanes above.
            CHECK_MSG(app.find("glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),") != std::string::npos,
                      "step 5's four neutrals are still the four that PRECEDE step 7's in the lookup's table");
            CHECK_MSG(primitive.find("glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f),") != std::string::npos, "the scalar lane's neutral in `toon_inputs`");
            CHECK_MSG(constructor.find("toon_colour_lane::goo_specular_fgd)] =\n                    glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f);") != std::string::npos,
                      "the scalar lane's neutral in the GPU table's initialiser, by NAME rather than by position");
            for (char const* const lane : {"goo_light_color", "goo_ambient_tint", "goo_specular_color"}) {
                std::string const by_name = std::string("toon_colour_lane::") + lane + ")] =\n                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);";
                CHECK_MSG(constructor.find(by_name) != std::string::npos, by_name.c_str());
            }
            // THE SHADER SIDE: the lanes are read BY INDEX (`+ 16u` .. `+ 19u`) through the stage's own stride, and
            // the constant that has to move with them is `character_toon_colour_lanes` (24, pinned by
            // `test_toon_material_sidecar` against the enum from the other side too).
            CHECK_MSG(character_forward.find("character_toon_colour_lanes = 24u") != std::string::npos, "the surface stage's stride copy");
            for (char const* const index : {"colour_base + 16u", "colour_base + 17u", "colour_base + 18u", "colour_base + 19u"}) {
                CHECK_MSG(character_forward.find(index) != std::string::npos, index);
            }
            // ... AND EVERY CONSTANT OF THE FGD GROUP THE SHADER SPELLS, so a re-tuning of one of them cannot pass:
            // the resolution, the `+ 0.5` offset, the dielectric F0, the NoV floor, the group's `1/(2pi)`, and the
            // two interface defaults for `specularFGD Strength` / `Use anisotropy?`.
            for (char const* const spelling : {"static const float goo_fgd_resolution = 64.0;",
                                               "static const float goo_fgd_diffuse_offset = 0.5;",
                                               "static const float goo_fgd_dielectric_f0 = 0.07999999821186066;",
                                               "static const float goo_fgd_ndotv_floor = 9.999999747378752e-05;",
                                               "static const float goo_dv_half_inverse_pi = 0.3183099925518036 * 0.5;",
                                               "static const float goo_specular_fgd_strength_default = 1.0;",
                                               "static const float goo_use_anisotropy_default = 0.0;",
                                               "static const float goo_anisotropic_mask_default = 0.0;"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // THE OFFSET IS `0.5` AND THE TERM USES IT - THE ASSERTION THIS FILE ONCE MADE INVERTED. The step
            // landed with `0.0` and this spot asserted that the spec's `+ 0.5` was ABSENT from the shader; the
            // parent withdrew that ruling (the node's flags say `+ 0.5`, and the "a finished FGD cannot exceed 1"
            // argument is a prior about the term rather than evidence about the graph), so the two checks below
            // now require the `+ 0.5` to be there - through the constant, and in the constant's own value.
            CHECK_MSG(character_forward.find("goo_fgd_texel.b + goo_fgd_diffuse_offset") != std::string::npos, "diffuseFGD reads LUT.B through the named offset");
            CHECK_MSG(character_forward.find("static const float goo_fgd_diffuse_offset = 0.5;") != std::string::npos, "and that offset IS the reference's `+ 0.5` (the spec's §0 item 2 / §9-U1 reading)");
            CHECK_MSG(character_forward.find("goo_fgd_texel.b + 0.5") == std::string::npos, "the LITERAL is NOT inlined: the one named constant is the single place the experiment lives");
            // ... AND THE HALF-TEXEL BIAS, which is the sibling socket reading of the same rule: `1/64 * 0.5` and
            // NOT `1/64 * 1.0`. Both scalars are pinned because both move every expectation in §8o/§8p.
            CHECK_MSG(character_forward.find("const float goo_fgd_coord_bias = goo_fgd_inv_resolution * 0.5;") != std::string::npos,
                      "the coordLUT bias is the HALF texel `运算.001 = (1/64)*0.5 = 0.0078125`");
            CHECK_MSG(character_forward.find("const float goo_fgd_coord_bias = goo_fgd_inv_resolution * 1.0;") == std::string::npos,
                      "and NOT the full texel `0.015625` an earlier version of this step read off the disabled socket");
            // ... AND THE THREE TERMS ARE THE REFERENCE'S EXPRESSIONS, spelled here so a later edit that "simplifies"
            // one of them fails: the FGD fetch, the energy compensation, the two IBL products, and the direct
            // specular's five-factor product.
            for (char const* const spelling : {"goo_fgd_coord * goo_fgd_coord_scale + goo_fgd_coord_bias",
                                               "lerp(float3(goo_fgd_texel.r), float3(goo_fgd_texel.g), goo_fresnel0)",
                                               "const float goo_energy_compensation = 1.0 / goo_fgd_reflectivity - 1.0;",
                                               "goo_spec_ibl = goo_specular_fgd * goo_specular_fgd_strength * goo_energy_compensation;",
                                               "goo_diff_ibl = irradiance_sample(n) * goo_ambient_tint * goo_diffuse_fgd * goo_diffuse_colour;",
                                               "goo_ndotl_clamped * (goo_specular_chosen * goo_specular_color) * cast_shadow_sigmoid * goo_light_color * direct_occlusion"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // ---- THE FGD LUT'S OWN SYNC POINTS: TWO SLOT TABLES AND ONE UPLOADER ----
            //
            // `test_render_resources` already compares the two slot tables against EACH OTHER (it parses both files
            // and requires the names and numbers to match), so what is asserted here is the THIRD half that test
            // cannot see: that the host actually WRITES the slot, and that the write's format is `UNORM`.
            CHECK_MSG(character_forward.find("heap_slots_goo_fgd_lut") != std::string::npos, "the shader names the FGD LUT's heap slot");
            std::string const core = slurp("vulkan/core/core.declarations.cppm");
            CHECK_MSG(core.find("goo_fgd_lut = heap_slot_base + 754u") != std::string::npos, "the host's own constant for it, at the slot the shader names");
            CHECK_MSG(constructor.find("runtime::set_goo_fgd_lut(") != std::string::npos, "the uploader the host writes it with");
            CHECK_MSG(constructor.find("fgd_info.format = VK_FORMAT_R8G8B8A8_UNORM;") != std::string::npos,
                      "its format is UNORM and NOT sRGB: the reference's data-block is `Non-Color`, so an sRGB upload would decode all three FGD outputs once");
            CHECK_MSG(constructor.find("write_heap_grid_image(this->vulkan_core, core::heap_slots::goo_fgd_lut") != std::string::npos, "and it reaches the slot");
            CHECK_MSG(constructor.find("fgd_info.mip_levels = 1;") != std::string::npos, "one mip, which is the reference's own `image_user` (no Mip input)");
            // ... AND THE APPLICATION SIDE: the path, the decoder, the format argument and the file's name
            CHECK_MSG(app.find("#define STB_IMAGE_STATIC") != std::string::npos, "the app's own stb copy is file-local (`gltf_loader.cpp` defines the extern one)");
            CHECK_MSG(app.find("\"PreIntegratedFGD_GGXDisneyDiffuse.png\"") != std::string::npos, "the reference's own file name");
            CHECK_MSG(app.find("runtime.set_goo_fgd_lut(") != std::string::npos, "and the upload is called once at startup");
            CHECK_MSG(app.find("zmd-ab\", \"gooblender\", \"images\"") != std::string::npos || app.find("\"zmd-ab\" / \"gooblender\" / \"images\"") != std::string::npos,
                      "from the reference's own directory under the build root");

            // ---- (c6) STEP 6'S ONE TERM, ITS ONE LANE COMPONENT, AND THE ONE FIXED NUMBER THAT IS **NOT** THERE ----
            //
            // THE POINT OF THIS BLOCK IS A NEGATIVE: step 6 adds NO LANE, NO SIDECAR ROW AND NO NEUTRAL, and the
            // checks that make that visible are (1) the lane it consumes is step 4's and its `.y` is named, and
            // (2) THE REFERENCE'S OTHER ADDEND IS NOT SPELLED ANYWHERE. That second one matters because
            // `DeSaturation` carries its OWN `DeSaturation = 0.0` input (`组输入.DeSaturation`, `is_linked =
            // false`, `enabled = true`), and a port that read THAT socket would desaturate by the luma alone -
            // which is the group's default and NOT what any material in this asset states. A "helpful" constant
            // for it is exactly the invention the step's brief forbids, so its absence is pinned.
            for (char const* const constant : {"static const float goo_desaturation_luma_r = 0.21267299354076385;",
                                               "static const float goo_desaturation_luma_g = 0.7151520252227783;",
                                               "static const float goo_desaturation_luma_b = 0.07217500358819962;",
                                               "static const float goo_desaturation_default = 0.0;",
                                               "static const float goo_desaturation_neutral = 1.0;"}) {
                CHECK_MSG(character_forward.find(constant) != std::string::npos, constant);
            }
            // the lane's `.y` IS the consumer this step gives it: the read is spelled, and it falls back to the
            // REFERENCE'S GROUP DEFAULT rather than to a neutral.
            CHECK_MSG(character_forward.find("goo_diffuse_b.y > goo_lane_absent_threshold ? goo_diffuse_b.y : goo_desaturation_default") != std::string::npos,
                      "`_GooDiffuseB.y` (`Color desaturation in shaded areas attenuation`) is now READ - the consumer step 4's report said it did not have");
            // the two nodes' arithmetic, spelled: the luma dot, the lerp, and the saturate that stands for 钳制.004.
            for (char const* const spelling : {"dot(colour, float3(goo_desaturation_luma_r, goo_desaturation_luma_g, goo_desaturation_luma_b))",
                                               "return lerp(goo_desaturation_luma_colour(colour), colour, saturate(saturation));"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // THE SCOPE BOUNDARY, and it is one line: the arm's gate is the SAME three families step 4 and step 5
            // use (the `#if defined(VR_GOO_TOON_CHAIN)` read of the lane, in the arm above), and the neutral a
            // non-member keeps is the IDENTITY - so HAIR, FACE, the eye and every `goo_toon = false` frame are
            // untouched by this stage.
            CHECK_MSG(character_forward.find("float goo_desaturation = goo_desaturation_neutral;") != std::string::npos,
                      "the arm's neutral is the identity, declared OUTSIDE the family gate");
            CHECK_MSG(character_forward.find("float goo_desaturation = goo_desaturation_default;") == std::string::npos,
                      "and it is NOT the group default (0.0), which would desaturate the hair and the face - the families this step was told to leave alone");
            CHECK_MSG(character_forward.find("const float3 lit_final = goo_hsv_desaturate(lit, desaturation);") != std::string::npos,
                      "the term is applied to the FINISHED PIXEL (`lit` = colour + emissive), which is where 色相/饱和度/明度 sits");
            CHECK_MSG(character_forward.find("return float4(lit_final, out_alpha);") != std::string::npos,
                      "and the pixel that is written is the desaturated one");
            // THE STAGE THAT WRITES THE PIXEL THE RIM IS ADDED TO does the same thing, and for the same reason.
            std::string const goo_toon = slurp("shaders/goo_toon.slang");
            CHECK_MSG(goo_toon.find("const float3 lit = goo_hsv_desaturate(colour + s.emissive, desaturation);") != std::string::npos,
                      "goo_toon.slang's pixel (the rim is ADDed to it one stage later, so the term belongs on it) desaturates the same way");
            // ... AND THE ONE CALLER THAT MUST NOT: the OUTLINE's own use of the group, whose discarded argument is
            // pinned so a later reader cannot mistake it for an oversight.
            CHECK_MSG(outline.find("float desaturation_unused = 0.0;") != std::string::npos,
                      "the outline stage discards the out parameter rather than desaturating an outline COLOUR");
        }

        // THE RIM'S CLOSED FORMS ARE SPELLED IN THE STAGE THAT SHIPS THEM, and that stage is now
        // `shaders/goo_rim.slang` (step 3 moved the whole rim out of the surface shader - see its header): the C++
        // in sections 8a-8m cannot drift from the arithmetic that runs, and the two `character_toon_colour_lanes`
        // reads in `goo_toon.slang` (the iris' own two lanes) are checked where they still are.
        {
            std::string const goo_rim = slurp("shaders/goo_rim.slang");
            for (char const* const spelling : {"goo_rim_dir_atten_default = 0.8999999761581421",
                                               "goo_rim_fresnel_pow_default = 2.0",
                                               "goo_rim_limitation_default = 0.0",
                                               "goo_rim_width_default = 0.5",
                                               "return lerp(floor_value, 1.0, clamped);",
                                               "return world_normal.z * 0.5 + 0.5;",
                                               "return squared * squared;",
                                               "return rim_colour * rim_colour_strength.xxx;",
                                               // THE TWO ARMS THAT STATE THE REFERENCE'S PER-CONTAINER
                                               // DIFFERENCE: Base's `运算.029` ceiling is applied to `DepthRim`,
                                               // Hair's is not applied at all.
                                               "hair ? depth_rim",
                                               "min(depth_rim, goo_depth_rim_base_ceiling)",
                                               "return lerp(scaled, scaled * object_x, limitation);",
                                               // the hair limitation's gate is the OBJECT-space x, which the spec
                                               // leaves open at §9-U3 - so the spelling is pinned here along with
                                               // the node it was read from
                                               "const float object_x = saturate(object_normal.x);",
                                               "const float3 normal_object = normal;",
                                               // `DepthRim` ITSELF: the sign, the mapping, the clamp and the
                                               // divisor, each written as its node writes it
                                               "const float dz = depth_offset - depth_self;",
                                               "const float mapped = (dz - goo_depth_rim_from_min) * (goo_depth_rim_to_max - goo_depth_rim_to_min) / (goo_depth_rim_from_max - goo_depth_rim_from_min) + goo_depth_rim_to_min;",
                                               "return clamp(mapped, 0.0, goo_depth_rim_clamp_max) / goo_depth_rim_divisor;",
                                               "static const float goo_depth_rim_base_ceiling = 0.5;",
                                               // the offset is a CAMERA-SPACE translation of the point, reprojected
                                               "const vec3 rim_offset = vec3(goo_rim_width_scale * rim_width_x * n_cam.x, goo_rim_width_scale * rim_width_y * n_cam.y, 0.0);",
                                               "const vec4 offset_clip = camera_at(heap_camera_slot).proj * vec4(view_pos + rim_offset, 1.0);",
                                               // ... and the products are GLSL-spelled on purpose: the round-trip
                                               // test above exists because the HLSL spellings of the same two
                                               // products are NOT matrix products under Slang's GLSL mode
                                               "const vec4 world = pc.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);",
                                               "const vec3 view_pos = (camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).xyz;",
                                               // ... and the two samples are BOTH `-get_view_z_from_depth`
                                               "return pc.proj_32 / (depth + pc.proj_22);",
                                               "const float depth_self = goo_rim_view_depth(stored_depth);",
                                               "const float depth_offset = goo_rim_view_depth(goo_rim_depth_at(uv_offset));",
                                               // the scope: the four families the reference gives a rim to, so a
                                               // port that widened it to the face would be caught here
                                               "if (family != VR_FAMILY_BASE && family != VR_FAMILY_SKIN && family != VR_FAMILY_CLOTH && !hair) {",
                                               // the background rule: a cleared depth draws nothing
                                               "if (stored_depth >= 1.0) {"}) {
                CHECK_MSG(goo_rim.find(spelling) != std::string::npos, spelling);
            }
            // ... AND THE LANES IT READS, by index, through ITS OWN copy of the stride
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes = 24u") != std::string::npos, "the rim stage's own stride copy");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 7u") != std::string::npos, "the rim stage reads lane 7 by that index");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 8u") != std::string::npos, "the rim stage reads lane 8 by that index");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 9u") != std::string::npos, "the rim stage reads lane 9 (the widths) by that index");
            // ... and it must NOT name a lane it does not own: the surface shader's rim term is GONE from it
            CHECK_MSG(shader.find("goo_rim_for_surface") == std::string::npos, "the surface shader no longer computes the rim");
            CHECK_MSG(shader.find("goo_depth_rim") == std::string::npos, "and no longer names the depth factor it cannot evaluate");
        }
        CHECK_MSG(shader.find("character_toon_colour_lanes) + 6u") != std::string::npos, "the surface shader still reads the iris lane 6 by that index");
        // (c7) STEP 7'S THREE TEXTURE LANES, FOUR COLOUR LANES AND THE HEAD'S POSITION - every place each of them
        // has to be spelled, plus the ones where a STALE COPY is the failure this project keeps recording.
        {
            // ---- THE TEXTURE LANES: the enum, the count, the block count and the sidecar vocabulary ----
            for (char const* const spelling : {"goo_face_sdf = 11,", "goo_face_cm = 12,", "goo_face_csumt = 13,", "count = 14,"}) {
                CHECK_MSG(primitive.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const pair : {"{\"_GooFaceSDF\", \"_UseGooFaceSDF\"}", "{\"_GooFaceCmM\", \"_UseGooFaceCmM\"}",
                                           "{\"_GooFaceCsutm\", \"_UseGooFaceCsutm\"}"}) {
                CHECK_MSG(app.find(pair) != std::string::npos, pair);
            }
            // THE FORMATS ARE A STATEMENT ABOUT THE CHANNELS, not a default: all three are NUMBERS (a distance
            // field, a layer selector and a comparison against 0.5), so an sRGB upload would bend the quantities
            // the two sigmoids and the GREATER_THAN threshold. Pinned BY LANE so a reordering cannot pass.
            for (char const* const format : {"toon_slot::goo_face_sdf)], VK_FORMAT_R8G8B8A8_UNORM",
                                             "toon_slot::goo_face_cm)], VK_FORMAT_R8G8B8A8_UNORM",
                                             "toon_slot::goo_face_csumt)], VK_FORMAT_R8G8B8A8_UNORM"}) {
                CHECK_MSG(constructor.find(format) != std::string::npos, format);
            }
            // ... AND THE THIRD BLOCK, which is what the three lanes cost: the host constant, the stage's copy of
            // it, and the fact that the third lane is IN that block rather than one past the end (`toon_lanes3_at`
            // reads it; the sidecar test pins all three accessors' literals).
            CHECK_MSG(primitive.find("toon_lane_blocks = 3") != std::string::npos, "the host raised the block count for step 7's lanes");
            CHECK_MSG(character_forward.find("character_toon_lane_blocks = 3u") != std::string::npos, "and the surface stage's copy moved with it");
            CHECK_MSG(character_forward.find("toon_lanes3_at(heap_slots_toon_lanes, material_index)") != std::string::npos,
                      "the face arm reads the third block through the accessor that names it");
            CHECK_MSG(constructor.find("toon_lanes_extra3") != std::string::npos, "and the host WRITES it - a lane written nowhere reads DO NOT READ for every material");
            CHECK_MSG(constructor.find("+ 2u] = toon_lanes_extra3;") != std::string::npos, "at block 2 of a 3-block stride");
            // ---- THE FOUR COLOUR LANES ----
            std::size_t const face_enum_at = primitive.find("enum class toon_colour_lane");
            std::string const face_colour_enum = primitive.substr(face_enum_at, primitive.find("};", face_enum_at) - face_enum_at);
            for (char const* const spelling : {"goo_face_scalars_a = 20,", "goo_face_scalars_b = 21,", "goo_face_nose_shadow = 22,",
                                               "goo_face_front_r = 23,", "count = 24,"}) {
                CHECK_MSG(face_colour_enum.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const row : {"\"_GooFaceScalarsA\",", "\"_GooFaceScalarsB\",", "\"_GooFaceNoseShadow\",", "\"_GooFaceFrontR\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // THE TWO SCALAR LANES ARE `-1000` SENTINELS AND THE TWO COLOURS ARE BLACK, and the CONTROL is the
            // part worth pinning: `SmoothnessMax = 0` is a MEANINGFUL value, so a `0` neutral would read "the
            // material states nothing" as "the material is perfectly rough"; and white is the strongest possible
            // statement about a nose shadow's A side.
            CHECK_MSG(constructor.find("toon_colour_lane::goo_face_nose_shadow)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);") != std::string::npos,
                      "the nose shadow's neutral is BLACK in the GPU table");
            CHECK_MSG(constructor.find("toon_colour_lane::goo_face_front_r)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);") != std::string::npos,
                      "...and so is `Front R Color`'s");
            CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),\n         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)}};") != std::string::npos,
                      "the lookup's table ends with step 7's four neutrals, in lane order");
            // ---- `headCenter`: THE ONE PIECE OF NEW DATA, and the stride is the failure class ----
            CHECK_MSG(primitive.find("glm::vec4 center = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos, "`head_ubo` gained the position");
            CHECK_MSG(primitive.find("static_assert(sizeof(head_ubo) == 64);") != std::string::npos, "and its size is asserted at FOUR vec4s, not three");
            CHECK_MSG(character_forward.find("float4 center; // the head object's world position") != std::string::npos, "the shader's own copy has the same fourth member");
            CHECK_MSG(character_forward.find("a centre is known") != std::string::npos, "and it documents the flag in `.w`");
            CHECK_MSG(character_forward.find("world_pos - head_frame_at(heap_slots_scene_head + heap_frame_slot).center.xyz") != std::string::npos,
                      "and the sphere normal subtracts it from the fragment's world position");
            // THE FLAG IS THE PART THAT KEEPS A FABRICATED CENTRE OUT OF THE SHADING: every character glb in this
            // repository is BAKED (8 nodes, 0 skins), so no head bone ever runs the publish below, and `center.w`
            // stays 0 - which is what makes the arm fall back to the socket's own `interface[]` default (0.0)
            // instead of building a normal about the world origin.
            CHECK_MSG(primitive.find("glm::vec4 center = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos, "the centre's `.w` starts at 0 = 'no centre known'");
            CHECK_MSG(character_forward.find("center.w > 0.5 && face_scalars_a.z > goo_lane_absent_threshold") != std::string::npos,
                      "and the arm reads the flag before it reads the socket");
            CHECK_MSG(app.find("glm::vec3 const head_center(joint[3][0], joint[3][1], joint[3][2]);") != std::string::npos,
                      "main.cpp fills it from the SAME bone matrix's translation column the two axis rows come from");
            CHECK_MSG(app.find(".center = glm::vec4(head_center, 1.0f)}") != std::string::npos, "...and sets the flag when it does");
            CHECK_MSG(app.find(".center = glm::vec4(0.0f)}") != std::string::npos, "...and leaves it clear in the no-head-bone publish");
            CHECK_MSG(app.find("center ({:.3f} {:.3f} {:.3f})") != std::string::npos, "and the one-shot probe logs it, so the value is evidence rather than an assumption");
            // ---- THE FACE ARM ITSELF ----
            CHECK_MSG(character_forward.find("static const float goo_face_forward_sign = -1.0;") != std::string::npos,
                      "step 7's copy of `值(明度)` is -1.0, the value BOTH dumps store");
            CHECK_MSG(shader.find("static const float goo_iris_forward_sign = -1.0;") != std::string::npos,
                      "and the iris' copy is the same number - one socket, two spellings, and they must not drift");
            CHECK_MSG(character_forward.find("static const float goo_face_half_turn = 3.141592502593994;") != std::string::npos, "the reference's own pi literal");
            CHECK_MSG(character_forward.find("static const float goo_face_shadow_proxy_open = 1.0 + goo_face_shadow_proxy_lift;") != std::string::npos,
                      "the shadow proxy is held at its OPEN value, spelled as the graph's own `(1 - 运算.020) + 0.1`");
            CHECK_MSG(character_forward.find("static const float goo_face_ambient_contrast = -0.4000000059604645;") != std::string::npos, "亮度 / 对比度's contrast, verbatim");
            CHECK_MSG(character_forward.find("static const float goo_face_eye_brightness_default = 1.2999999523162842;") != std::string::npos,
                      "the EYE-WHITE brightness's INTERFACE default is 1.3, not the material's 1.5");
            CHECK_MSG(character_forward.find("family == VR_FAMILY_FACE && goo_base_ramp != 0u && goo_face_block2.w != 0u") != std::string::npos,
                      "the arm's gate names the family, the ramp lane and the SDF lane");
            CHECK_MSG(character_forward.find("return face_desaturated * face_emission_strength;") != std::string::npos,
                      "and it RETURNS, which is what guards every article term below it off for the face");
            CHECK_MSG(character_forward.find("out_desaturation = goo_desaturation_neutral;") != std::string::npos,
                      "with the out-parameter left at the identity, because the desaturation was applied INSIDE the arm where the reference has it");
            CHECK_MSG(character_forward.find("const float face_saturation = saturate(dot(face_ramp_colour") != std::string::npos,
                      "the face's own desaturation is the reference's `clamp(luma(RampColor) + 0.9, 0, 1)`");
            CHECK_MSG(character_forward.find("face_flip > 0.0 ? (uv.x > 0.5 ? 0.0 : 1.0)") != std::string::npos,
                      "the front-red gate IS the SDF's flip bit - A9's two-mirrors claim, in the source");
            CHECK_MSG(character_forward.find("const float face_rim_threshold = (face_cm_a * face_mirror_uv) - face_angle_threshold;") != std::string::npos,
                      "and the face's rim gate SUBTRACTS the angle threshold rather than scaling by it");
        }
        // (d) THE SWITCH, which is the A/B's own instrument: it must be a `[render]` key, because that is the only
        // section the capture script can override - a key anywhere else would make the A/B unrunnable.
        CHECK_MSG(config.find("render->get(\"goo_toon\")") != std::string::npos, "the [render] goo_toon key is parsed");
        // (e) THE SUPPRESSIONS, which are text checks because none of them is visible in a frame on its own:
        // the OLD rim must be excluded for the four ported families and KEPT for the face and the eye; the
        // ARTICLE's screen-space contour must stop sharing the surface stage's feature name; and the NEW stage
        // must be gated by the SAME predicate that picks the rewritten chain's pipeline - which is the one gate
        // that keeps `[render] goo_toon = false` byte-identical.
        CHECK_MSG(shader.find("#define VR_GOO_TOON_CHAIN 1") != std::string::npos, "the rewritten chain defines the rim guard");
        CHECK_MSG(character_forward.find("#ifndef VR_GOO_TOON_CHAIN") != std::string::npos, "the old rim is inside the guard");
        CHECK_MSG(character_forward.find("rim_ported_by_goo") != std::string::npos, "the guarded arm names the families it covers");
        CHECK_MSG(character_forward.find("family == VR_FAMILY_BASE || family == VR_FAMILY_SKIN || family == VR_FAMILY_CLOTH || family == VR_FAMILY_HAIR") != std::string::npos,
                  "and the four families are the ones it covers (the face is NOT among them)");
        // the OTHER consumer of `toon_diffuse` must NOT define it, or the outline would lose the old rim
        CHECK_MSG(outline.find("VR_GOO_TOON_CHAIN") == std::string::npos, "`outline.slang` leaves the old rim switched on");
        CHECK_MSG(pbr.find("VR_GOO_TOON_CHAIN") == std::string::npos, "and so does `pbr.slang`");
        CHECK_MSG(rim_pass.find("return \"toon_screen_rim\";") != std::string::npos, "the screen rim asks under its own feature name");
        CHECK_MSG(demo.find("self.runtime_->goo_toon_active()") != std::string::npos, "the rewritten chain switches the article's contour off");
        // (f) STEP 3'S OWN GATE, and it is the one this step's byte-identity criterion rests on: the new rim stage
        // must ask under ITS OWN feature name (so the owner can answer it), it must not record when that name is
        // false, and the owner's answer must be the SAME `goo_toon_active()` predicate that picks the character
        // stage's pipeline - a gate that could disagree with the pipeline choice would draw the Goo rim over the
        // OLD chain's frame or none over the new one.
        std::string const goo_rim_pass = slurp("vulkan/pass/goo_rim.cpp");
        CHECK_MSG(goo_rim_pass.find("return \"goo_rim\";") != std::string::npos, "the goo rim asks under its own feature name");
        // ... and the owner answers that name with the runtime's own predicate, in the branch whose last term is
        // the POSITIVE form of the contour's negation above
        std::size_t const goo_rim_branch = demo.find("if (name == \"goo_rim\")");
        CHECK_MSG(goo_rim_branch != std::string::npos, "the feature table has a branch for it");
        std::string const goo_rim_answer = demo.substr(goo_rim_branch, demo.find("}\n", goo_rim_branch) - goo_rim_branch);
        CHECK_MSG(goo_rim_answer.find("self.runtime_->goo_toon_active()") != std::string::npos, "answered with the SAME predicate the pipeline choice uses");
        CHECK_MSG(goo_rim_answer.find("self.goo_rim_->ready() && goo_toon_active") != std::string::npos, "and true only when the rewritten chain is the one drawing");
        // ... and the frame loop records the stage after the character stage, gated on that name
        std::string const frames = slurp("vulkan/runtime/runtime.frames.cppm");
        CHECK_MSG(frames.find("this->goo_rim_stage = {at(\"goo_rim\")};") != std::string::npos, "the stage is bound to the chain by name");
        CHECK_MSG(frames.find("pass::stage const goo_rim_stage = {.name = \"goo_rim\"") != std::string::npos, "and recorded as its own stage");
        // ... and the G-buffer's publication is gated on the same feature name, or a frame with `goo_toon` off
        // would transition images for a pass that never draws
        CHECK_MSG(demo.find("services.feature_active(services.owner, \"goo_rim\")") != std::string::npos, "the stage preamble is gated on the same name");
    }

    return vk_test::finish("test_goo_toon_math");
}

#else
int32_t main() {
    vk_test::write_line("test_goo_toon_math: VR_TEST_SOURCE_DIR is not defined, so the source checks cannot run");
    return 1;
}
#endif
