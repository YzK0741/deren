// Headless unit tests: gltf_loader (pure CPU) =================================
// Loads the repository's DamagedHelmet sample (external .bin buffer + .jpg
// textures resolved relative to the glTF file) and checks error paths. The
// loader is pure CPU - no Vulkan anywhere.
#include "vk_test.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>
#include <yyjson.h>

import deren.gltf_loader;

namespace {
    void test_khr_lights_punctual_minimal() {
        auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/lights_punctual_minimal.gltf");
        CHECK(result.has_value());
        if (!result.has_value()) {
            return;
        }
        deren::gltf::scenes const& scenes = *result;
        CHECK(scenes.lights.size() == 3);
        if (scenes.lights.size() != 3) {
            return;
        }
        // point light: binary-clean values load exactly
        CHECK(scenes.lights[0].type == deren::gltf::light_type::point);
        CHECK(scenes.lights[0].intensity == 2.0f);
        CHECK(scenes.lights[0].range == std::optional<float>(8.0f));
        CHECK(scenes.lights[0].color == glm::vec3(1.0f, 0.5f, 0.25f));
        // spot light: the optional cone angles are exported
        CHECK(scenes.lights[1].type == deren::gltf::light_type::spot);
        CHECK(scenes.lights[1].spot_inner_cone.has_value());
        CHECK(scenes.lights[1].spot_outer_cone.has_value());
        if (scenes.lights[1].spot_outer_cone) {
            CHECK(std::abs(*scenes.lights[1].spot_outer_cone - 0.5f) < 1e-5f);
        }
        // directional light
        CHECK(scenes.lights[2].type == deren::gltf::light_type::directional);
        CHECK(scenes.lights[2].intensity == 4.0f);

        // every KHR node attachment landed on a node (light_index into scenes.lights)
        CHECK(scenes.scene[0].nodes.size() == 4);
        std::size_t attached = 0;
        for (deren::gltf::node const& node : scenes.scene[0].nodes) {
            if (node.light_index) {
                ++attached;
                CHECK(*node.light_index < scenes.lights.size());
            }
        }
        CHECK(attached == 3);
    }

    // A spec-legal file whose vertex attributes are NOT float: TEXCOORD_0 is normalized u8 (core
    // glTF) and NORMAL is signed normalized s16 (KHR_mesh_quantization). The loader's interleaved
    // vertex builder used to reinterpret both as float vectors, which reads 4x (u8) or 2x (s16) past
    // the accessor, so this pins the component-type conversion AND the element-count arithmetic.
    // Reading the interleaved output (rather than the raw accessor) is the point: the raw bytes are
    // correct either way - the corruption happens on the way into the vertex struct.
    void test_quantized_attributes() {
        auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/quantized_attributes.gltf");
        CHECK(result.has_value());
        if (!result.has_value()) {
            return;
        }
        deren::utility::vessel::vector<deren::gltf::resolved_material> const materials = deren::gltf::resolve_materials(*result);
        deren::gltf::drawable_iterator it(*result, materials);
        CHECK(it != deren::gltf::drawable_iterator());
        if (!(it != deren::gltf::drawable_iterator())) {
            return;
        }
        deren::gltf::vertex_view const vertex = it.get_vertex();
        CHECK(vertex.count == 3);
        CHECK(vertex.stride == 80); // position(12) normal(12) uv(8) joints(16) weights(16) tangent(16)
        if (vertex.count != 3 || vertex.stride != 80 || vertex.data.size() < 3u * vertex.stride) {
            return;
        }
        // field offsets inside the interleaved stride
        constexpr std::size_t off_position = 0;
        constexpr std::size_t off_normal = 12;
        constexpr std::size_t off_uv = 24;
        constexpr std::size_t off_weights = 48;
        constexpr std::size_t off_tangent = 64;
        auto const at = [&vertex](std::size_t const index, std::size_t const offset) {
            return vertex.data.data() + index * vertex.stride + offset;
        };
        auto const f32 = [&at](std::size_t const index, std::size_t const offset) {
            return *reinterpret_cast<float const*>(at(index, offset));
        };

        // POSITION: float vec3, taken verbatim (NOT run through the normalized conversion)
        CHECK(f32(0, off_position + 0) == -1.0f);
        CHECK(f32(0, off_position + 4) == -1.0f);
        CHECK(f32(1, off_position + 0) == 1.0f);
        CHECK(f32(2, off_position + 4) == 1.0f);

        // NORMAL: authored (0, 0, 32767) as signed normalized s16 -> +Z. Reading those six bytes as
        // a float vec3 would produce an absurd direction, and from the second vertex on the old code
        // ran past the 18-byte accessor entirely.
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(std::abs(f32(i, off_normal + 0)) < 1e-6f);
            CHECK(std::abs(f32(i, off_normal + 4)) < 1e-6f);
            CHECK(f32(i, off_normal + 8) == 1.0f); // 32767 / 32767
        }

        // TEXCOORD_0: normalized u8 (0,0) (255,0) (0,255) -> 0.0 / 1.0, not the raw byte value and
        // not whatever float bits sat past the 6-byte accessor
        CHECK(f32(0, off_uv + 0) == 0.0f);
        CHECK(f32(0, off_uv + 4) == 0.0f);
        CHECK(f32(1, off_uv + 0) == 1.0f); // 255 / 255
        CHECK(f32(1, off_uv + 4) == 0.0f);
        CHECK(f32(2, off_uv + 0) == 0.0f);
        CHECK(f32(2, off_uv + 4) == 1.0f);

        // the identity skin fallback stays intact for an unskinned primitive
        CHECK(f32(0, off_weights + 0) == 1.0f);
        CHECK(f32(0, off_weights + 4) == 0.0f);
        CHECK(f32(0, off_weights + 8) == 0.0f);
        CHECK(f32(0, off_weights + 12) == 0.0f);
        // TANGENT: this fixture has none, so the record is the zero one whose w == 0 is the fragment stage's
        // "no vertex frame, use the derivative fallback" signal
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(f32(i, off_tangent + 0) == 0.0f);
            CHECK(f32(i, off_tangent + 4) == 0.0f);
            CHECK(f32(i, off_tangent + 8) == 0.0f);
            CHECK(f32(i, off_tangent + 12) == 0.0f);
        }
    }

    void test_consuming_resolve_moves_decoded_pixels() {
        auto result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/asset/DamagedHelmet/DamagedHelmet.gltf");
        CHECK(result.has_value());
        if (!result.has_value()) {
            return;
        }
        auto& scenes = *result;
        std::uint64_t before = 0;
        for (auto const& texture : scenes.textures) {
            before += texture.data.size();
        }
        CHECK(before > 0);

        auto const materials = deren::gltf::resolve_materials(scenes, true);
        CHECK(materials.size() == scenes.materials.size());
        if (materials.empty()) {
            return;
        }
        std::uint64_t after = 0;
        for (auto const& texture : scenes.textures) {
            after += texture.data.size();
        }
        // Consuming resolve moves each referenced texture's pixels into its mip chain; it must not
        // keep the complete decoded set next to the full mip set.
        CHECK(after < before);
        auto const slot = materials[0].slots[0];
        CHECK(slot.valid && slot.parent && !slot.data.empty());
        if (slot.parent) {
            CHECK(slot.data.data() == slot.parent->data());
        }
    }

    void test_resolved_mip_payload_has_no_undeclared_tail() {
        struct fixture {
            std::uint32_t width, height, levels;
            std::size_t bytes;
        };
        for (auto const f : {fixture{8, 4, 3, 168}, fixture{1, 8, 1, 32}, fixture{9, 5, 3, 220}, fixture{4, 4, 3, 84}}) {
            for (bool consume : {false, true}) {
                deren::gltf::scenes scene;
                scene.textures.resize(1);
                scene.materials.resize(1);
                auto& texture = scene.textures[0];
                texture.width = f.width;
                texture.height = f.height;
                texture.component = 4;
                texture.data.assign(f.width * f.height * 4, 255);
                scene.materials[0].texture_indices["albedo"] = 0;
                auto resolved = deren::gltf::resolve_materials(scene, consume);
                CHECK(resolved.size() == 1 && resolved[0].slots[0].valid);
                if (resolved.empty() || !resolved[0].slots[0].valid) {
                    continue;
                }
                auto const& image = resolved[0].slots[0];
                CHECK(image.mip_levels == f.levels && image.data.size() == f.bytes);
                CHECK(image.parent && image.parent->size() == f.bytes);
                CHECK(std::all_of(image.data.begin(), image.data.end(), [](auto byte) { return byte == 255; }));
            }
        }
    }

    void test_consuming_resolve_matches_copy_path() {
        auto copied_scene = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/asset/DamagedHelmet/DamagedHelmet.gltf");
        auto moved_scene = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/asset/DamagedHelmet/DamagedHelmet.gltf");
        CHECK(copied_scene.has_value() && moved_scene.has_value());
        if (!copied_scene.has_value() || !moved_scene.has_value()) {
            return;
        }
        auto const copied = deren::gltf::resolve_materials(*copied_scene);
        auto const moved = deren::gltf::resolve_materials(*moved_scene, true);
        CHECK(copied.size() == moved.size());
        for (std::size_t m = 0; m < std::min(copied.size(), moved.size()); ++m) {
            for (std::size_t i = 0; i < copied[m].slots.size(); ++i) {
                auto const& a = copied[m].slots[i];
                auto const& b = moved[m].slots[i];
                CHECK(a.valid == b.valid);
                if (!a.valid || !b.valid) {
                    continue;
                }
                CHECK(a.width == b.width && a.height == b.height && a.mip_levels == b.mip_levels);
                CHECK(a.data.size() == b.data.size());
                if (a.data.size() == b.data.size()) {
                    CHECK(std::equal(a.data.begin(), a.data.end(), b.data.begin()));
                }
            }
        }
    }

    void test_load_damaged_helmet() {
        auto result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/asset/DamagedHelmet/DamagedHelmet.gltf");
        CHECK(result.has_value());
        if (!result.has_value()) {
            return;
        }
        deren::gltf::scenes const& scenes = *result;

        // the sample has one material, several textures and one mesh with one primitive
        CHECK(scenes.materials.size() == 1);
        CHECK(!scenes.textures.empty());
        CHECK(!scenes.scene.empty());
        CHECK(!scenes.scene[0].nodes.empty());
        CHECK(!scenes.node_by_source.empty());

        // world AABB over every drawable primitive
        deren::gltf::scene_bounds const bounds = deren::gltf::compute_scene_bounds(scenes);
        CHECK(bounds.valid);
        CHECK(bounds.primitive_count >= 1);
        CHECK(bounds.min.x <= bounds.max.x);
        CHECK(bounds.min.y <= bounds.max.y);
        CHECK(bounds.min.z <= bounds.max.z);
        // the helmet fits in a modest box around the origin (loose sanity bound)
        CHECK(bounds.max.x - bounds.min.x < 100.0f);
        CHECK(bounds.max.y - bounds.min.y < 100.0f);
        CHECK(bounds.max.z - bounds.min.z < 100.0f);

        // the drawable iterator yields exactly the primitives the bounds counted
        std::size_t drawables = 0;
        for (auto it = scenes.begin(); it != deren::gltf::scenes::end(); ++it) {
            ++drawables;
        }
        CHECK(drawables == bounds.primitive_count);

        // renderer-ready materials resolve one per source material
        deren::utility::vessel::vector<deren::gltf::resolved_material> const resolved = deren::gltf::resolve_materials(scenes);
        CHECK(resolved.size() == scenes.materials.size());
        if (!resolved.empty()) {
            auto const slot = resolved[0].slots[0];
            static_assert(std::is_const_v<typename decltype(slot.parent)::element_type>);
            CHECK(slot.parent && !slot.data.empty());
            if (!slot.parent || slot.data.empty()) {
                return;
            }
            auto before = slot.data[0];
            auto count = scenes.textures.size();
            auto node_count = scenes.node_by_source.size();
            CHECK(deren::gltf::release_texture_pixels(*result) > 0);
            CHECK(scenes.textures.size() == count && scenes.node_by_source.size() == node_count);
            for (auto const& texture : scenes.textures) {
                CHECK(texture.data.empty() && texture.data.capacity() == 0);
            }
            CHECK(slot.parent && slot.data[0] == before);
            CHECK(deren::gltf::release_texture_pixels(*result) == 0);
        }
    }

    void test_async_load_matches_sync() {
        auto future = deren::gltf::load_model_async(VR_TEST_SOURCE_DIR "/asset/DamagedHelmet/DamagedHelmet.gltf");
        auto const result = future.get();
        CHECK(result.has_value());
        if (result.has_value()) {
            deren::gltf::scene_bounds const bounds = deren::gltf::compute_scene_bounds(*result);
            CHECK(bounds.valid);
            CHECK(bounds.primitive_count >= 1);
        }
    }

    void test_missing_file_reports_file_not_found() {
        auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/definitely_missing.gltf");
        CHECK(!result.has_value());
        CHECK(result.error() == deren::gltf::error_code::file_not_found);
    }
} // namespace

// ONE SKIN, TWO NODES. glTF allows it (two meshes, or one mesh instanced twice) and the runtime's
// skin binding used to rig only the first node it found. That binding needs a device, so what this
// test pins down is the DATA it depends on: both nodes reach the scene carrying skin_index 0, and
// the skin is not folded away.
void test_one_skin_used_by_two_nodes() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/shared_skin_two_nodes.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    deren::gltf::scenes const& scenes = *result;
    CHECK(scenes.skins.size() == 1);
    std::size_t skinned = 0;
    for (auto const& [source, loader_node] : scenes.node_by_source) {
        (void)source;
        if (loader_node->skin_index && *loader_node->skin_index == 0) {
            ++skinned;
        }
    }
    CHECK(skinned == 2); // both users of the skin, not just the first
}

// THE FACE SDF'S HEAD FRAME: the name matcher, the matrix-to-frame extraction, and the fallback. All three are
// pure, which is the point of testing them here - the model this lane was written for has NO SKELETON at all, so
// the only way the bone half can have any evidence behind it is as arithmetic rather than as pixels.
void test_the_head_frame_matcher_rejects_lookalikes() {
    // WHAT COUNTS AS A HEAD BONE, and the negative cases are the test: every rig names its head bone something
    // like these four, and every character with a hat or a hairstyle also carries `headgear`, `overhead` or
    // `Forehead` in the same skeleton. A substring test would take those.
    CHECK(deren::gltf::looks_like_head_joint("head"));
    CHECK(deren::gltf::looks_like_head_joint("Head"));
    CHECK(deren::gltf::looks_like_head_joint("Bip01 Head"));
    CHECK(deren::gltf::looks_like_head_joint("J_Head"));
    CHECK(deren::gltf::looks_like_head_joint("Head_Nub"));
    CHECK(deren::gltf::looks_like_head_joint("頭_01"));
    CHECK(deren::gltf::looks_like_head_joint("头"));
    CHECK(!deren::gltf::looks_like_head_joint("headgear"));
    CHECK(!deren::gltf::looks_like_head_joint("overhead"));
    CHECK(!deren::gltf::looks_like_head_joint("Forehead"));
    CHECK(!deren::gltf::looks_like_head_joint("neck"));
    CHECK(!deren::gltf::looks_like_head_joint(""));
}

void test_the_head_frame_from_a_bone_and_when_there_is_none() {
    // THE FALLBACK IS glTF'S BASIS, and its exact values are the assertion because they are what a face's
    // SDF is measured against: the front of a glTF asset faces `+Z` and its right is `-X`. It is deliberately NOT
    // the reference's constant `(0,0,-1)`, which points 180 degrees away from the face of every model measured
    // here - see the note on `head_basis` for what that sign costs (94.1% of a face read as shadow).
    deren::gltf::head_basis const fallback = deren::gltf::head_basis_fallback();
    CHECK(fallback.front == glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(fallback.right == glm::vec3(-1.0f, 0.0f, 0.0f));
    CHECK(fallback.up == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(!fallback.from_skeleton);
    // THE THREE AXES ARE A FRAME, which is the property that ties the fallback to `head_basis_from_axes`: a bone
    // path whose handedness disagreed with the fallback's would shade a static and a skinned model differently.
    CHECK(glm::length(glm::cross(fallback.right, fallback.front) - fallback.up) < 1e-6f);

    // AN IDENTITY BONE. `row3` is `+Z` and `row1` is `+X`, so the frame is the fallback's - and it must come
    // back flagged as a REAL frame rather than as the fallback, because those two are different answers that look
    // identical.
    deren::gltf::head_basis const identity = deren::gltf::head_basis_from_axes(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(identity.from_skeleton);
    CHECK(identity.front == glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(identity.right == glm::vec3(-1.0f, 0.0f, 0.0f));
    CHECK(identity.up == glm::vec3(0.0f, 1.0f, 0.0f));

    // A DEGENERATE BONE IS THE FALLBACK. An unposed or missing bone arrives as a zero matrix, and normalising
    // that would put NaNs in the shader - i.e. a black or flickering face rather than an error.
    CHECK(!deren::gltf::head_basis_from_axes(glm::vec3(0.0f), glm::vec3(0.0f)).from_skeleton);
    // ... and so are two PARALLEL axes, where there is no third one to build.
    CHECK(!deren::gltf::head_basis_from_axes(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-2.0f, 0.0f, 0.0f)).from_skeleton);

    // A NON-ORTHOGONAL BONE IS RE-ORTHOGONALISED rather than taken as it is: the returned frame's three axes
    // have to BE a frame, because the SDF's angle is taken between the light and them.
    deren::gltf::head_basis const skewed = deren::gltf::head_basis_from_axes(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.5f));
    CHECK(skewed.from_skeleton);
    CHECK(std::abs(glm::dot(skewed.front, skewed.right)) < 1e-4f);
    CHECK(std::abs(glm::dot(skewed.front, skewed.up)) < 1e-4f);
    CHECK(std::abs(glm::dot(skewed.right, skewed.up)) < 1e-4f);
}

void test_no_head_bone_is_found_where_there_is_none() {
    // THE TWO SKINNED FIXTURES HAVE JOINTS CALLED `pole`/`arm` AND `joint` - no head - so the finder must answer
    // "no head bone" rather than take the first joint it sees, which is the failure that would matter: a face
    // shaded from an arm's frame looks like a face shaded from a head's until the arm moves.
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/animated_skin_plane.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    CHECK(result->skins.size() == 1);
    CHECK(!deren::gltf::head_joint_of(*result, 0, 0).has_value());
    // OUT-OF-RANGE ASKS NOTHING RATHER THAN CRASHING: a caller that indexes a scene or a skin that this file
    // does not have gets "no head bone", which is the same answer it would get from a model with no skeleton.
    CHECK(!deren::gltf::head_joint_of(*result, 99, 0).has_value());
    CHECK(!deren::gltf::head_joint_of(*result, 0, 99).has_value());
}

void test_a_head_bone_is_found_at_its_joint_index() {
    // THE SUCCESS PATH, which the other fixtures cannot exercise: both of them have joints called `pole`/`arm`
    // and `joint`, so until this file existed `head_joint_of` was only ever tested answering "no".
    //
    // THE FIXTURE IS BUILT SO THE ANSWER IS AN INDEX THAT IS NOT ZERO - its skin's joints are `neck` then
    // `Head`, so the head is joint 1. A finder that returned the first joint, or an asset node index instead of
    // a joint index, would pass a test written against joint 0 and fail here, and both of those are real
    // mistakes: the joint index is what picks a matrix out of the per-frame skin matrix array, so an off-by-one
    // shades a face from a neck.
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/skinned_head.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    CHECK(result->skins.size() == 1);
    if (result->skins.size() != 1) {
        return;
    }
    CHECK(result->skins[0].joints.size() == 2);
    std::optional<std::size_t> const head = deren::gltf::head_joint_of(*result, 0, 0);
    CHECK(head.has_value());
    CHECK(head == std::optional<std::size_t>{1});
    // AND IT IS AN INDEX INTO `joints`, NOT AN ASSET NODE INDEX: the two differ here on purpose (the head is
    // asset node 3 and joint 1), so a finder that returned the wrong one of them cannot pass both assertions.
    CHECK(result->skins[0].joints[1] != 1u);
}

void test_the_toon_family_matcher_reads_mmd_material_names() {
    // WHAT THIS GUARDS, and it is a failure with no symptom anywhere in the loader: a material whose name
    // matches no pattern is `toon_family::none`, and `none` is the family-independent toon path. A table that
    // cannot read a model's script therefore reports nothing at all - it just shades every material of that
    // model without its family, and the frame still looks like a toon character. That is exactly what
    // happened to the MMD character models this branch imports: their materials are named `颜`, `发`, `目`,
    // `眉`, `睫`, `口`, `齿`, `舌`, `鼻`, `鞋`, `裤`, while the table listed the Japanese and two-character
    // spellings, so 25 of one model's 35 materials fell through to `none` - every `face` and every `hair`
    // material among them, which is what silently cost the hair its highlight and the eye its soft ramp.
    //
    // EVERY NAME BELOW IS TAKEN FROM A MODEL IN THIS REPOSITORY rather than invented. The first group is
    // `zhuangfy_toon.glb`'s material list, in its spellings.
    CHECK(deren::gltf::toon_family_of("颜") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("肌上") == deren::gltf::toon_family::skin);
    CHECK(deren::gltf::toon_family_of("肌-手") == deren::gltf::toon_family::skin);
    CHECK(deren::gltf::toon_family_of("肌-耳") == deren::gltf::toon_family::skin);
    CHECK(deren::gltf::toon_family_of("肌下-隐藏") == deren::gltf::toon_family::skin);
    CHECK(deren::gltf::toon_family_of("目") == deren::gltf::toon_family::eye);
    CHECK(deren::gltf::toon_family_of("目白") == deren::gltf::toon_family::eye);
    CHECK(deren::gltf::toon_family_of("目HL") == deren::gltf::toon_family::eye);
    CHECK(deren::gltf::toon_family_of("眉") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("睫") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("舌") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("口线") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("鼻线") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("齿") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("二重") == deren::gltf::toon_family::face);
    // `表情` is the expression overlay - blush and tears drawn over the same face geometry - and it is the
    // last name in these models to fall through, so it is asserted with them.
    CHECK(deren::gltf::toon_family_of("表情") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("发") == deren::gltf::toon_family::hair);
    CHECK(deren::gltf::toon_family_of("前发饰") == deren::gltf::toon_family::hair);
    CHECK(deren::gltf::toon_family_of("发簪") == deren::gltf::toon_family::hair);
    CHECK(deren::gltf::toon_family_of("鞋") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("裤") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("裤-alpha") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("手") == deren::gltf::toon_family::skin);

    // THE SAME WORD IN ANOTHER SCRIPT IS A DIFFERENT WORD TO A BYTE COMPARISON, so each spelling is asserted
    // alone: a table that gained only one of a pair reads half the models it exists to serve.
    CHECK(deren::gltf::toon_family_of("髪") == deren::gltf::toon_family::hair);  // Japanese
    CHECK(deren::gltf::toon_family_of("髮") == deren::gltf::toon_family::hair);  // traditional
    CHECK(deren::gltf::toon_family_of("顔") == deren::gltf::toon_family::face);  // Japanese
    CHECK(deren::gltf::toon_family_of("臉") == deren::gltf::toon_family::face);  // traditional
    CHECK(deren::gltf::toon_family_of("靴") == deren::gltf::toon_family::cloth); // Japanese
    CHECK(deren::gltf::toon_family_of("褲") == deren::gltf::toon_family::cloth); // traditional
    CHECK(deren::gltf::toon_family_of("襪") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("腳") == deren::gltf::toon_family::skin);
    // ... AND THE FACE IS THE ONE PLACE THE LASH AND BROW LAYERS OF `zhuangfy_toon.glb` DISAGREE WITH THE
    // BARE CHARACTERS: `睫眉` is ONE material covering both, so it must still reach `face` through either.
    CHECK(deren::gltf::toon_family_of("睫眉") == deren::gltf::toon_family::face);

    // THE COLLISIONS THE GROUP ORDER RESOLVES, and both directions matter: `手套` (glove) must be cloth even
    // though it contains `手` (skin), and `袖口` (cuff) / `领口` (collar) must be cloth even though they
    // contain `口` (face). A table that gained the bare characters WITHOUT cloth ahead of skin and face
    // passes every assertion above and fails these three.
    CHECK(deren::gltf::toon_family_of("手套") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("袖口") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("领口") == deren::gltf::toon_family::cloth);

    // THE REFERENCE'S NAMES ARE UNCHANGED, which is the widest risk in adding patterns: one that steals
    // a material the game data already classified moves a frame that was not supposed to move. These are
    // `actor_zhuangfy.glb`'s names, and `eyeshadow` and `tail` must stay `none` - they are the two names in
    // that file no family matches, and `eyeshadow` is the near miss that would catch a careless `eye` pattern.
    CHECK(deren::gltf::toon_family_of("M_actor_zhuangfy_face_01") == deren::gltf::toon_family::face);
    CHECK(deren::gltf::toon_family_of("M_actor_zhuangfy_hair_01") == deren::gltf::toon_family::hair);
    CHECK(deren::gltf::toon_family_of("M_actor_zhuangfy_iris_01") == deren::gltf::toon_family::eye);
    CHECK(deren::gltf::toon_family_of("M_actor_zhuangfy_body_01") == deren::gltf::toon_family::skin);
    CHECK(deren::gltf::toon_family_of("M_actor_zhuangfy_cloth_01") == deren::gltf::toon_family::cloth);
    CHECK(deren::gltf::toon_family_of("M_S_actor_zhuangfy_eyebrow_01_lod0") == deren::gltf::toon_family::face);
    // THE TWO OVERLAY MATERIALS ARE `none`, NOT `hair`: the article draws them in their framebuffer-multiply
    // pass (`MyZmdEyeDarkShader` / `MyZmdHairShadowShader`), so no toon family may match them - see the note at the
    // top of `toon_family_of`. The hair-shadow line here asserted `hair` until that pass was being ported, which is
    // the reference's classification of the NAME and not of the SHADING.
    CHECK(deren::gltf::toon_family_of("M_S_actor_zhuangfy_hairshadow_01_lod0") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_S_actor_zhuangfy_eyeshadow_01_lod0") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_eyeshadow_common_01") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_hairshadow_common_01") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_S_actor_zhuangfy_tail_02_lod0") == deren::gltf::toon_family::none);

    // AN EMPTY NAME ASKS NOTHING and an unrecognised one is `none` rather than the first pattern's family -
    // an unnamed material is common in these files and must keep the family-independent path.
    CHECK(deren::gltf::toon_family_of("") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("biaoq") == deren::gltf::toon_family::none);
}

// ---------------------------------------------------------------------------------------------------
// THE OVERLAY MATCHER, the other half of the same "no family shades them" decision above.
//
// WHY IT NEEDS its TEST AND NOT JUST A SECOND `CHECK` LINE: the two functions are ONE decision read from
// two sides (see the note at the top of `toon_family_of`). A name matched by neither is a material that is
// never drawn at all - not a wrong picture, a disappearing surface - so the invariant that matters is the PAIR:
// every name `overlay_kind_of` matches must be `toon_family::none`, and the two overlay assets' names must be
// matched by exactly one of the two channels.
//
// THE NUMBERING IS ASSERTED HERE TOO, because `register_material` compares the value against the literals 1 and
// 2 to set the material record's flag bits 6 and 7 (deren.engine.runtime does not import the loader's types, so the
// value crosses the boundary as a number). Renumbering this enum would therefore change which shader branch an
// overlay takes with no compile error anywhere - so the numbers are pinned where the enum is.
void test_the_overlay_matcher_claims_the_two_masks() {
    static_assert(static_cast<uint32_t>(deren::gltf::overlay_kind::none) == 0u);
    static_assert(static_cast<uint32_t>(deren::gltf::overlay_kind::eye_dark) == 1u);
    static_assert(static_cast<uint32_t>(deren::gltf::overlay_kind::hair_shadow) == 2u);

    // the two materials `chars\chen_full2.glb` carries, which is the asset this port is measured on
    CHECK(deren::gltf::overlay_kind_of("M_eyeshadow_common_01") == deren::gltf::overlay_kind::eye_dark);
    CHECK(deren::gltf::overlay_kind_of("M_eyewhiteshadow_common_01") == deren::gltf::overlay_kind::eye_dark);
    CHECK(deren::gltf::toon_family_of("M_eyewhiteshadow_common_01") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::overlay_kind_of("M_hairshadow_common_01") == deren::gltf::overlay_kind::hair_shadow);
    // ... and the game's names for the same two, which are the ones the loader meets first in practice
    CHECK(deren::gltf::overlay_kind_of("M_S_actor_zhuangfy_eyeshadow_01_lod0") == deren::gltf::overlay_kind::eye_dark);
    CHECK(deren::gltf::overlay_kind_of("M_S_actor_zhuangfy_hairshadow_01_lod0") == deren::gltf::overlay_kind::hair_shadow);
    // the matcher is case-insensitive ASCII, like `toon_family_of`
    CHECK(deren::gltf::overlay_kind_of("M_Actor_Chen_EyeShadow_01") == deren::gltf::overlay_kind::eye_dark);

    // THE INVARIANT THE PAIR HAS TO KEEP: an overlay is not a family, so every listed name is `none` there -
    // and `hairshadow` contains `hair`, which is exactly the collision the top of `toon_family_of` exists for.
    CHECK(deren::gltf::toon_family_of("M_eyeshadow_common_01") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_hairshadow_common_01") == deren::gltf::toon_family::none);
    CHECK(deren::gltf::toon_family_of("M_S_actor_zhuangfy_hairshadow_01_lod0") == deren::gltf::toon_family::none);

    // NOTHING ELSE IS AN OVERLAY, and the near miss is the point: `shadow` alone is a word that appears in
    // plenty of ordinary decals and shadow-catcher materials, so a matcher that accepted it would silently turn
    // one of them into a framebuffer multiply.
    CHECK(deren::gltf::overlay_kind_of("M_shadow_decal_01") == deren::gltf::overlay_kind::none);
    CHECK(deren::gltf::overlay_kind_of("M_actor_chen_face_01") == deren::gltf::overlay_kind::none);
    CHECK(deren::gltf::overlay_kind_of("M_actor_chen_hair_01") == deren::gltf::overlay_kind::none);
    CHECK(deren::gltf::overlay_kind_of("") == deren::gltf::overlay_kind::none);
}

// ---------------------------------------------------------------------------------------------------
// THE LISTED `extras` ROWS, which are the loader's third data source and the only place an ALLOW-LIST
// decides what enters a model.
//
// WHY THIS IS TESTED RATHER THAN TRUSTED: the block it reads (`efFloats`) holds 190 names on chen's cloth
// material and the port reads two, so "import the block and let consumers pick" and "import the listed names"
// produce the same picture and differ by 188 unread values that would each be a second source of truth for a
// property with no reader. The fixture also carries an UNLISTED name next to the listed ones precisely so
// this test can tell the two implementations apart - without it, both pass.
void test_the_extras_block_is_read_through_the_claimed_table() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/claimed_extras.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    deren::gltf::scenes const& scenes = *result;
    CHECK(scenes.materials.size() == 3);
    if (scenes.materials.size() != 3) {
        return;
    }
    // THE LISTED ROWS ARRIVE, as the asset wrote them - and a stated ZERO is a value rather than an absence,
    // which is exactly why these lanes' "nothing stated" had to be A separate sentinel.
    CHECK(scenes.materials[0].extras_floats.count("_Specular") == 1);
    CHECK(scenes.materials[0].extras_floats.at("_Specular") == 0.0f);
    // ... AND SO DOES THE SECOND LISTED ROW, which is the pair the whitelist is now made of: a table that
    // carried one name and a reader that asked for two would silently import nothing for the second property.
    CHECK(scenes.materials[0].extras_floats.count("_ParallaxScale") == 1);
    CHECK(std::abs(scenes.materials[0].extras_floats.at("_ParallaxScale") - 0.5f) < 1e-6f);
    // ... AND THE TWO ROWS ARE INDEPENDENT PER MATERIAL, which is what makes them per-material facts rather
    // than one block's: material 1 states a different value of the same row and no `_OutlineOffsetZ` at all.
    CHECK(std::abs(scenes.materials[1].extras_floats.at("_ParallaxScale") - 0.03f) < 1e-6f);
    // ... AND A ROW THE PORT HAS NOT LISTED DOES NOT, even though it sits in the same block. This is the
    // assertion the whole allow-list exists for.
    CHECK(scenes.materials[0].extras_floats.count("_UnclaimedRow") == 0);
    CHECK(scenes.materials[0].extras_floats.count("_OutlineOffsetZ") == 0);
    // a colour row inside the same `extras` is not a listed scalar: only `efFloats` is read at all
    CHECK(scenes.materials[0].extras_floats.count("_SdfColor") == 0);
    // a FRACTIONAL value survives the JSON round trip (the callback reads a double and the map holds a float)
    CHECK(scenes.materials[1].extras_floats.count("_Specular") == 1);
    CHECK(std::abs(scenes.materials[1].extras_floats.at("_Specular") - 0.454f) < 1e-6f);
    // A MATERIAL WITH NO `extras` AT ALL IS THE SAME STATE as one that states none of the listed rows: empty,
    // so every consumer's fallback answers rather than a zero being invented here.
    CHECK(scenes.materials[2].extras_floats.empty());

    // THE NAME JOIN the extras are read through: a consumer has a material NAME (that is how the toon lookup
    // is asked) and needs the per-material facts glTF did not carry, so the two lookups have to agree - and an
    // unnamed or unknown name must be a MISS rather than a match on the first material.
    CHECK(scenes.material_by_name("M_fractional_extras") == &scenes.materials[1]);
    CHECK(scenes.material_by_name("M_no_extras") == &scenes.materials[2]);
    CHECK(scenes.material_by_name("M_not_in_this_file") == nullptr);
    CHECK(scenes.material_by_name("") == nullptr);
}

void test_subtilis_extraction() {
    std::string path = VR_TEST_SOURCE_DIR "/source/tests/fixtures/subtilis/cross_dag.glb";
    auto off = deren::gltf::load_model(path);
    CHECK(off);
    if (off) {
        auto materials = deren::gltf::resolve_materials(*off);
        deren::gltf::drawable_iterator it(*off, materials);
        CHECK(!it.get_subtilis().present);
    }
    auto on = deren::gltf::load_model(path, {true});
    CHECK(on);
    if (on) {
        auto materials = deren::gltf::resolve_materials(*on);
        deren::gltf::drawable_iterator it(*on, materials);
        auto payload = it.get_subtilis();
        CHECK(payload.present && payload.version == 1 && payload.profile == "shared-vertices" && payload.profile_attributes && !payload.bytes.empty());
        std::ifstream in(VR_TEST_SOURCE_DIR "/source/tests/fixtures/subtilis/cross_dag.payload", std::ios::binary);
        std::string expected((std::istreambuf_iterator<char>(in)), {});
        CHECK(payload.bytes.size() == expected.size());
        if (payload.bytes.size() == expected.size()) {
            CHECK(std::equal(payload.bytes.begin(), payload.bytes.end(), reinterpret_cast<uint8_t const*>(expected.data())));
        }
        ++it;
        auto second = it.get_subtilis();
        CHECK(second.present && second.bytes.data() == payload.bytes.data());
        auto moved = std::move(*on);
        auto moved_materials = deren::gltf::resolve_materials(moved);
        deren::gltf::drawable_iterator after(moved, moved_materials);
        CHECK(after.get_subtilis().present);
    }
    auto future = deren::gltf::load_model_async(path, {true});
    auto async = future.get();
    CHECK(async);
    if (async) {
        auto m = deren::gltf::resolve_materials(*async);
        deren::gltf::drawable_iterator it(*async, m);
        CHECK(it.get_subtilis().present);
    }
    CHECK(!deren::gltf::drawable_iterator{}.get_subtilis().present);
    std::ifstream file(path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(file)), {});
    auto u32 = [&](size_t at) {uint32_t v=0;for(unsigned i=0;i<4;++i){v|=uint32_t(static_cast<uint8_t>(raw[at+i]))<<(i*8);
}return v; };
    CHECK(raw.size() >= 28);
    if (raw.size() < 28) {
        return;
    }
    auto json_length = u32(12);
    CHECK(json_length <= raw.size() - 28);
    if (json_length > raw.size() - 28) {
        return;
    }
    auto dir = std::filesystem::current_path() / std::format("subtilis-loader-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    CHECK(!ec);
    {
        std::ofstream out(dir / "shared data.bin", std::ios::binary);
        out.write(raw.data() + 28 + json_length, static_cast<std::streamsize>(raw.size() - 28 - json_length));
    }
    auto variant = [&](auto edit) {
        auto* original = yyjson_read(raw.data() + 20, json_length, 0);
        CHECK(original);
        auto* doc = yyjson_doc_mut_copy(original, nullptr);
        yyjson_doc_free(original);
        auto* root = yyjson_mut_doc_get_root(doc);
        auto* buffer = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        yyjson_mut_obj_add_str(doc, buffer, "uri", "shared%20data.bin");
        auto* primitive = yyjson_mut_arr_get_first(yyjson_mut_obj_get(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "meshes")), "primitives"));
        edit(doc, root, primitive);
        size_t size = 0;
        auto* text = yyjson_mut_write(doc, 0, &size);
        {
            std::ofstream out(dir / "variant.gltf", std::ios::binary);
            out.write(text, static_cast<std::streamsize>(size));
        }
        std::free(text);
        yyjson_mut_doc_free(doc);
        return deren::gltf::load_model((dir / "variant.gltf").generic_string(), {true});
    };
    auto check_view = [&](auto& scene, auto check) {CHECK(scene);if(scene){auto m=deren::gltf::resolve_materials(*scene);deren::gltf::drawable_iterator it(*scene,m);check(it.get_subtilis());} };
    auto external = variant([](auto*, auto*, auto*) {});
    check_view(external, [&](auto p) { CHECK(p.present && p.profile_attributes && !p.bytes.empty()); });
    auto out_of_range = variant([](auto*, auto*, auto* primitive) {auto* ext=yyjson_mut_obj_get(yyjson_mut_obj_get(primitive,"extensions"),"DEREN_subtilis");yyjson_mut_set_uint(yyjson_mut_obj_get(ext,"bufferView"),UINT32_MAX); });
    check_view(out_of_range, [&](auto p) { CHECK(p.present && p.bytes.empty()); });
    auto malformed = variant([](auto* d, auto*, auto* primitive) {auto* ext=yyjson_mut_obj_get(primitive,"extensions");yyjson_mut_obj_remove_key(ext,"DEREN_subtilis");yyjson_mut_obj_add_str(d,ext,"DEREN_subtilis","malformed metadata"); });
    check_view(malformed, [&](auto p) { CHECK(p.present && p.version == 0 && p.bytes.empty()); });
    auto attributes = variant([](auto* d, auto*, auto* primitive) { yyjson_mut_obj_add_uint(d, yyjson_mut_obj_get(primitive, "attributes"), "_CUSTOM", 1); });
    check_view(attributes, [&](auto p) { CHECK(p.present && !p.profile_attributes); });
    auto absent = variant([](auto*, auto*, auto* primitive) { yyjson_mut_obj_remove_key(yyjson_mut_obj_get(primitive, "extensions"), "DEREN_subtilis"); });
    check_view(absent, [&](auto p) { CHECK(!p.present); });
    auto unknown = variant([](auto* d, auto* root, auto*) { yyjson_mut_arr_add_str(d, yyjson_mut_obj_get(root, "extensionsUsed"), "VENDOR_optional"); });
    check_view(unknown, [&](auto p) { CHECK(p.present && !p.bytes.empty()); });
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}
void test_standalone_motion_binding() {
    auto target = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/animated_skin_plane.gltf");
    auto motion = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/animated_skin_plane.gltf");
    CHECK(target.has_value() && motion.has_value());
    if (!target || !motion) {
        return;
    }
    auto hash = deren::gltf::skeleton_fingerprint(*target);
    CHECK(hash.has_value());
    if (!hash) {
        return;
    }
    // Reordering file node IDs must not change the binding contract.
    motion->node_by_source.clear();
    for (auto& scene : motion->scene) {
        for (auto& node : scene.nodes) {
            node.source_index += 100;
            motion->node_by_source.emplace(node.source_index, &node);
        }
    }
    for (auto& skin : motion->skins) {
        for (auto& joint : skin.joints) {
            joint += 100;
        }
    }
    for (auto& clip : motion->animations) {
        for (auto& channel : clip.channels) {
            channel.target_node += 100;
        }
    }
    CHECK(deren::gltf::skeleton_fingerprint(*motion) == hash);
    auto const count = target->animations.size();
    CHECK(deren::gltf::bind_motion(*target, *motion, *hash).has_value());
    CHECK(target->animations.size() == count + motion->animations.size());
    CHECK(target->animations.back().channels[0].target_node < 100);
    auto const unchanged = target->animations.size();
    // Matching matrix hashes alone do not make matrix-only base poses playable:
    // rotation sampling must preserve the target node's actual translation/scale.
    auto const animated = target->animations.back().channels[0].target_node;
    for (auto& scene : target->scene) {
        for (auto& node : scene.nodes) {
            if (node.source_index == animated) {
                node.local_transform[3][0] += 0.25f;
            }
        }
    }
    for (auto& scene : motion->scene) {
        for (auto& node : scene.nodes) {
            if (node.source_index == animated + 100) {
                node.local_transform[3][0] += 0.25f;
            }
        }
    }
    auto matrix_hash = deren::gltf::skeleton_fingerprint(*target);
    CHECK(matrix_hash);
    if (matrix_hash) {
        CHECK(!deren::gltf::bind_motion(*target, *motion, *matrix_hash));
        CHECK(target->animations.size() == unchanged);
    }
    for (auto& scene : target->scene) {
        for (auto& node : scene.nodes) {
            if (node.source_index == animated) {
                node.local_transform[3][0] -= 0.25f;
            }
        }
    }
    for (auto& scene : motion->scene) {
        for (auto& node : scene.nodes) {
            if (node.source_index == animated + 100) {
                node.local_transform[3][0] -= 0.25f;
            }
        }
    }
    motion->scene[0].nodes[0].local_transform[3][0] += 0.25f;
    CHECK(!deren::gltf::bind_motion(*target, *motion, *hash));
    CHECK(target->animations.size() == unchanged);
    motion->scene[0].nodes[0].local_transform[3][0] -= 0.25f;
    motion->animations[0].samplers[0].times[0] = -1.0f;
    CHECK(!deren::gltf::bind_motion(*target, *motion, *hash));
    CHECK(target->animations.size() == unchanged);
}

void test_standalone_motion_file() {
    auto target = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/animated_skin_plane.gltf");
    CHECK(target);
    if (!target) {
        return;
    }
    auto hash = deren::gltf::skeleton_fingerprint(*target);
    CHECK(hash);
    if (!hash) {
        return;
    }
    auto dir = std::filesystem::temp_directory_path() / "deren-motion-binding-test";
    std::filesystem::create_directories(dir);
    auto path = dir / "motion.gltf";
    float const keys[] = {0, 1, 0, 0, 0, 1, 0, 0, 0.6f, 0.8f};
    {
        std::ofstream bin(dir / "motion.bin", std::ios::binary);
        bin.write(reinterpret_cast<char const*>(keys), sizeof(keys));
    }
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"root","children":[1]},{"name":"pole","translation":[-1,0,0],"children":[2]},{"name":"arm"}],
        "buffers":[{"uri":"motion.bin","byteLength":40}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":32}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                     {"bufferView":1,"componentType":5126,"count":2,"type":"VEC4"}],
        "animations":[{"samplers":[{"input":0,"output":1}],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]}],
        "extras":{"deren_motion":{"binding":"bip001-rest-v1","joints":[1,2],"skeleton_hash":")";
    json.append(hash->data(), hash->size());
    json += "\"}}}";
    auto write = [&](std::string const& text) {
        std::ofstream output(path);
        output << text;
    };
    write(json);
    auto const before = target->animations.size();
    CHECK(deren::gltf::load_motion(*target, path.string()));
    CHECK(target->animations.size() == before + 1);
    auto invalid = json;
    invalid.replace(invalid.find("\"node\":2"), 8, "\"node\":999");
    write(invalid);
    CHECK(!deren::gltf::load_motion(*target, path.string()));
    CHECK(target->animations.size() == before + 1);
    invalid = json;
    invalid.replace(invalid.find("skeleton_hash") + 16, 1, "x");
    write(invalid);
    CHECK(!deren::gltf::load_motion(*target, path.string()));
    CHECK(target->animations.size() == before + 1);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}

// THE TWO BYTE RANGES fastgltf::validate() LEAVES UNCHECKED, pinned with files whose numbers point past the end.
//
// WHY THESE ARE TESTED AT ALL: neither is a decode bug. The JSON is legal, every index resolves, and fastgltf's
// the validation accepts both files - what is wrong is only their EXTENT. The first file's second bufferView runs
// 8 bytes past the end of a 12-byte buffer; the second file's morph target names accessor 7 of a table holding
// one. Before the loader compared those ranges itself, the first fed span::subspan an out-of-range pair
// (undefined behaviour, already inside get_source_bytes) and the second indexed asset.accessors past the end, so
// these files either crashed or loaded whatever happened to sit in memory - and no assertion could tell "rejected"
// from "read garbage". The two tests below pin the rejection and the drop respectively.
void test_a_buffer_view_past_the_buffer_end_is_rejected() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/invalid_buffer_view_range.gltf");
    CHECK(!result.has_value());
    if (!result.has_value()) {
        // THE SAME CLASS AS EVERY OTHER STRUCTURAL ERROR, so a caller's "this file is malformed" branch catches a
        // range that is past the end just as it catches an index that is: neither is a transient read failure.
        CHECK(result.error() == deren::gltf::error_code::file_type_error);
    }
}

// THE ACCESSOR INDEX IN A MORPH TARGET IS UNVALIDATED BY fastgltf (validate walks primitive.attributes, not
// primitive.targets), so this file used to index asset.accessors out of bounds. The loader now drops the one
// attribute whose index is past the end and keeps the target it belonged to: node.weights and mesh.weights are
// indexed by target, so dropping the whole target would rotate every later weight onto the wrong delta.
void test_a_morph_target_past_the_accessor_table_drops_its_attribute() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/invalid_morph_target_accessor.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    deren::gltf::scenes const& scenes = *result;
    CHECK(scenes.scene.size() == 1);
    if (scenes.scene.size() != 1 || scenes.scene[0].nodes.size() != 1) {
        return;
    }
    deren::gltf::node const& node = scenes.scene[0].nodes[0];
    CHECK(node.meshes.size() == 1);
    if (node.meshes.size() != 1 || node.meshes[0].primitives.size() != 1) {
        return;
    }
    deren::gltf::primitive const& primitive = node.meshes[0].primitives[0];
    CHECK(primitive.targets.size() == 1);
    if (primitive.targets.size() != 1) {
        return;
    }
    // the bad delta is gone, the target it belonged to is not
    CHECK(primitive.targets[0].attributes.empty());
    // ... AND THE BASE VERTEX DATA IS UNTOUCHED, which is what distinguishes "one delta dropped" from "the whole
    // primitive failed to decode".
    CHECK(primitive.vertex.count("POSITION") == 1);
}

// A QUANTIZED POSITION IS 6 BYTES PER VERTEX, NOT 12. The bounds walk used to divide the payload by
// sizeof(glm::vec3) and reinterpret the bytes as float, so a legal s16-normalized POSITION (KHR_mesh_quantization)
// decoded into NaN/denormal vertices and the whole model's AABB came out non-finite even though the mesh loads
// and renders. The three vertices below are (-1,-1,0), (1,-1,0), (0,1,0) after the normalized conversion, so a
// finite, exact AABB is the only passing result.
void test_a_quantized_position_yields_a_finite_scene_bounds() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/quantized_position.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    deren::gltf::scene_bounds const bounds = deren::gltf::compute_scene_bounds(*result);
    CHECK(bounds.valid);
    CHECK(bounds.primitive_count == 1);
    if (!bounds.valid) {
        return;
    }
    CHECK(std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) && std::isfinite(bounds.min.z));
    CHECK(std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y) && std::isfinite(bounds.max.z));
    CHECK(std::abs(bounds.min.x - -1.0f) < 1e-6f);
    CHECK(std::abs(bounds.min.y - -1.0f) < 1e-6f);
    CHECK(std::abs(bounds.min.z) < 1e-6f);
    CHECK(std::abs(bounds.max.x - 1.0f) < 1e-6f);
    CHECK(std::abs(bounds.max.y - 1.0f) < 1e-6f);
    CHECK(std::abs(bounds.max.z) < 1e-6f);
}

// A 2x2 GRAY + ALPHA PNG (color type 4). stb reports two channels, and the converter used to fall through to
// `default`, clearing the buffer: load_model() still succeeded, so nothing upstream could tell the image had been
// dropped - only the resolved material's albedo slot was invalid. The four (gray, alpha) pairs are distinct per
// channel, so a conversion that fed alpha into RGB or gray into alpha cannot pass.
void test_a_two_channel_image_converts_to_rgba() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/gray_alpha_texture.gltf");
    CHECK(result.has_value());
    if (!result.has_value()) {
        return;
    }
    deren::gltf::scenes const& scenes = *result;
    CHECK(scenes.textures.size() == 1);
    if (scenes.textures.size() != 1) {
        return;
    }
    CHECK(scenes.textures[0].component == 2);
    CHECK(scenes.textures[0].width == 2 && scenes.textures[0].height == 2);
    auto const materials = deren::gltf::resolve_materials(scenes);
    CHECK(materials.size() == 1);
    if (materials.size() != 1) {
        return;
    }
    deren::gltf::image_view const& albedo = materials[0].slots[0];
    CHECK(albedo.valid);
    CHECK(albedo.width == 2 && albedo.height == 2);
    CHECK(albedo.mip_levels == 2); // 2x2 then 1x1
    std::uint8_t const expected[] = {
        16,
        16,
        16,
        0,
        128,
        128,
        128,
        64,
        255,
        255,
        255,
        128,
        192,
        192,
        192,
        255,
    };
    CHECK(albedo.data.size() >= sizeof(expected));
    if (albedo.data.size() >= sizeof(expected)) {
        bool matches = true;
        for (std::size_t i = 0; i < sizeof(expected); ++i) {
            matches = matches && albedo.data[i] == expected[i];
        }
        CHECK(matches);
    }
}

// count * sizeof(float[3]) is 12 GiB for this accessor: the loader used to hand that straight to a vector
// allocation, so the failure surfaced as std::bad_alloc thrown out of load_model() (or, where size_t is 32-bit,
// as a silently wrapped small allocation that copyFromAccessor then wrote past). The documented 256 MiB ceiling
// rejects it at validation time and reports the same structural error as every other un-loadable file.
void test_an_accessor_past_the_allocation_ceiling_is_rejected() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/oversized_accessor.gltf");
    CHECK(!result.has_value());
    if (!result.has_value()) {
        CHECK(result.error() == deren::gltf::error_code::file_type_error);
    }
}

// sparse.count MUST be <= accessor.count (the substitution table cannot replace more elements than the accessor
// has). fastgltf::validate() never checked it and copyFromAccessor() silently stopped at accessor.count, so a
// sparse block larger than the accessor it patches used to load without a word.
void test_a_sparse_count_past_the_accessor_count_is_rejected() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/invalid_sparse_count.gltf");
    CHECK(!result.has_value());
    if (!result.has_value()) {
        CHECK(result.error() == deren::gltf::error_code::file_type_error);
    }
}

// fastgltf::validate() dereferences asset.bufferViews[sparse.indicesBufferView] itself, with no bounds check, so a
// file pointing that index past the table used to crash the validator before any loader code could object. The
// range pass now runs FIRST and rejects the file as a structural error instead.
void test_a_sparse_buffer_view_past_the_table_is_rejected() {
    auto const result = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/invalid_sparse_buffer_view.gltf");
    CHECK(!result.has_value());
    if (!result.has_value()) {
        CHECK(result.error() == deren::gltf::error_code::file_type_error);
    }
}

int32_t main() {
    test_standalone_motion_binding();
    test_standalone_motion_file();
    test_subtilis_extraction();
    test_consuming_resolve_moves_decoded_pixels();
    test_resolved_mip_payload_has_no_undeclared_tail();
    test_consuming_resolve_matches_copy_path();
    test_load_damaged_helmet();
    test_async_load_matches_sync();
    test_missing_file_reports_file_not_found();
    test_khr_lights_punctual_minimal();
    test_a_buffer_view_past_the_buffer_end_is_rejected();
    test_a_morph_target_past_the_accessor_table_drops_its_attribute();
    test_a_quantized_position_yields_a_finite_scene_bounds();
    test_a_two_channel_image_converts_to_rgba();
    test_an_accessor_past_the_allocation_ceiling_is_rejected();
    test_a_sparse_count_past_the_accessor_count_is_rejected();
    test_a_sparse_buffer_view_past_the_table_is_rejected();
    test_one_skin_used_by_two_nodes();
    test_quantized_attributes();
    test_the_head_frame_matcher_rejects_lookalikes();
    test_the_head_frame_from_a_bone_and_when_there_is_none();
    test_no_head_bone_is_found_where_there_is_none();
    test_a_head_bone_is_found_at_its_joint_index();
    test_the_toon_family_matcher_reads_mmd_material_names();
    test_the_overlay_matcher_claims_the_two_masks();
    test_the_extras_block_is_read_through_the_claimed_table();
    return deren::vk_test::finish("test_gltf_loader");
}
