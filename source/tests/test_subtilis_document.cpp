#include "../../tools/subtilis/document.hpp"
#include "subtilis_dag_fixtures.hpp"
#include "vk_test.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <yyjson.h>
namespace tool = deren::tools::subtilis;
namespace sub = deren::asset::subtilis;
#include "subtilis_document_fixtures.hpp"
using namespace subtilis_document_fixture;

int main() {
    auto fixture = subtilis_fixture::cross_dag();
    auto f = make_fixture(fixture.source);
    auto run_name = deren::utility::vessel::format("subtilis-document-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::current_path() / tool::path(run_name.c_str());
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    CHECK(!ec);
    auto file = dir / "input.glb";
    write(file, glb(f.json, f.bin));
    auto doc = tool::read_document(file, {});
    CHECK(doc);
    if (!doc) {
        std::filesystem::remove_all(dir, ec);
        return deren::vk_test::finish("subtilis_document");
    }
    auto eligible = tool::eligible_primitives(*doc);
    CHECK(eligible && *eligible == sub::vector<tool::primitive_key>({{0, 0}}));
    auto geometry = tool::extract_geometry(*doc, {0, 0});
    CHECK(geometry);
    if (geometry) {
        CHECK(geometry->positions == fixture.source.positions && geometry->indices == fixture.source.indices);
        CHECK(sub::source_digest(*geometry) == sub::source_digest(fixture.source));
    }
    auto wire = sub::encode(fixture.data, {});
    CHECK(wire && tool::attach_payload(*doc, {0, 0}, *wire));
    auto cooked = tool::serialize_embedded_glb(*doc);
    CHECK(cooked);
    if (cooked) {
        auto bin = 20 + get(*cooked, 12) + 8;
        CHECK(std::equal(f.bin.begin(), f.bin.end(), cooked->begin() + bin));
        write(dir / "cooked.glb", *cooked);
        auto reloaded = tool::read_document(dir / "cooked.glb", {});
        CHECK(reloaded);
        if (reloaded) {
            auto raw = tool::payload_bytes(*reloaded, {0, 0});
            CHECK(raw && *raw == *wire);
            auto json = tool::serialize_json(*reloaded);
            CHECK(json && json->find("18446744073709551615") != sub::string::npos && json->find("keep me") != sub::string::npos);
            if (json) {
                auto* before = yyjson_read(f.json.data(), f.json.size(), 0);
                auto* after = yyjson_read(json->data(), json->size(), 0);
                CHECK(before && after);
                if (before && after) {
                    auto* a = yyjson_doc_get_root(before);
                    auto* b = yyjson_doc_get_root(after);
                    for (auto key : {"nodes", "scenes", "materials", "accessors", "extras", "extensions"})
                        CHECK(yyjson_equals(yyjson_obj_get(a, key), yyjson_obj_get(b, key)));
                    auto primitive = [](yyjson_val* root) { return yyjson_arr_get(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(root, "meshes"), 0), "primitives"), 0); };
                    for (auto key : {"attributes", "indices", "material"})
                        CHECK(yyjson_equals(yyjson_obj_get(primitive(a), key), yyjson_obj_get(primitive(b), key)));
                    auto* metadata = yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(b, "extensions"), "VENDOR_metadata"), "nested"), "u");
                    CHECK(yyjson_is_uint(metadata) && yyjson_get_uint(metadata) == UINT64_MAX);
                    for (size_t i = 0; i < 4; ++i)
                        CHECK(yyjson_equals(yyjson_arr_get(yyjson_obj_get(a, "bufferViews"), i), yyjson_arr_get(yyjson_obj_get(b, "bufferViews"), i)));
                }
                yyjson_doc_free(before);
                yyjson_doc_free(after);
                auto required_json = *json;
                replace(required_json, "\"asset\":", "\"extensionsRequired\":[\"DEREN_subtilis\"],\"asset\":");
                sub::vector<std::byte> cooked_bin(cooked->begin() + bin, cooked->end());
                write(dir / "required.glb", glb(required_json, cooked_bin));
                auto required_doc = tool::read_document(dir / "required.glb", {});
                CHECK(required_doc);
                if (required_doc) {
                    auto saved = tool::serialize_json(*required_doc);
                    CHECK(saved && saved->find("\"extensionsRequired\":[\"DEREN_subtilis\"]") != sub::string::npos);
                }
                replace(required_json, "\"VENDOR_metadata\",\"DEREN_subtilis\"", "\"VENDOR_metadata\"");
                write(dir / "not-used.glb", glb(required_json, cooked_bin));
                CHECK(!tool::read_document(dir / "not-used.glb", {}));
            }
            auto source = tool::extract_geometry(*reloaded, {0, 0});
            auto payload = raw ? sub::decode(*raw, {}) : sub::result<sub::payload>(std::unexpected(sub::error{sub::error_code::invalid_input, "missing", {}, {}}));
            CHECK(source && payload);
            if (source && payload) {
                auto a = sub::make_asset(std::move(*source), std::move(*payload), {});
                CHECK(a);
                if (a) {
                    sub::cut_parameters p;
                    p.threshold = .2f;
                    auto cut = sub::select_cut(*a, p);
                    CHECK(cut && *cut == sub::vector<std::uint32_t>({8, 9}));
                }
            }
        }
    }
    auto check_eligible = [&](sub::string json, size_t expected) {write(dir/"variant.glb",glb(std::move(json),f.bin)); auto d=tool::read_document(dir/"variant.glb",{});CHECK(d);if(d){auto e=tool::eligible_primitives(*d);CHECK(e && e->size()==expected);} };
    auto no_normal = f.json;
    replace(no_normal, "\"NORMAL\":1,", "");
    check_eligible(no_normal, 0);
    auto tangent = f.json;
    replace(tangent, "\"NORMAL\":1,", "\"NORMAL\":1,\"_CUSTOM\":1,");
    check_eligible(tangent, 0);
    write(dir / "unsupported-attribute.glb", glb(tangent, f.bin));
    auto unsupported_attribute = tool::read_document(dir / "unsupported-attribute.glb", {});
    CHECK(unsupported_attribute);
    if (unsupported_attribute && wire)
        CHECK(!tool::attach_payload(*unsupported_attribute, {0, 0}, *wire));
    auto no_uv = f.json;
    replace(no_uv, ",\"TEXCOORD_0\":2", "");
    check_eligible(no_uv, 1);
    for (auto mode : {"MASK", "BLEND"})
        check_eligible(edit_json(f.json, [&](auto* d, auto* root) {auto* material=yyjson_mut_arr_get(yyjson_mut_obj_get(root,"materials"),0);yyjson_mut_obj_add_strcpy(d,material,"alphaMode",mode); }), 0);
    auto tangent_json = edit_json(f.json, [&](auto* d, auto* root) {
        auto* a = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, a, "componentType", 5126);
        yyjson_mut_obj_add_uint(d, a, "count", fixture.source.positions.size());
        yyjson_mut_obj_add_str(d, a, "type", "VEC4");
        yyjson_mut_arr_append(yyjson_mut_obj_get(root, "accessors"), a);
        auto* p = yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "meshes"), 0), "primitives"), 0);
        yyjson_mut_obj_add_uint(d, yyjson_mut_obj_get(p, "attributes"), "TANGENT", 4);
    });
    check_eligible(tangent_json, 0);
    auto morph_json = edit_json(f.json, [&](auto* d, auto* root) {auto* p=yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root,"meshes"),0),"primitives"),0);auto* targets=yyjson_mut_arr(d);auto* target=yyjson_mut_obj(d);yyjson_mut_obj_add_uint(d,target,"POSITION",0);yyjson_mut_arr_append(targets,target);yyjson_mut_obj_add_val(d,p,"targets",targets); });
    check_eligible(morph_json, 0);

    auto skin_bin = f.bin;
    for (size_t i = 0; i < fixture.source.positions.size(); ++i) {
        put(skin_bin, std::bit_cast<std::uint32_t>(1.f));
        put(skin_bin, 0);
        put(skin_bin, 0);
        put(skin_bin, 0);
    }
    auto skin_json = edit_json(f.json, [&](auto* d, auto* root) {
        auto* accessors = yyjson_mut_obj_get(root, "accessors");
        for (unsigned i = 0; i < 2; ++i) {
            auto* a = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, a, "componentType", i ? 5126 : 5123);
            yyjson_mut_obj_add_uint(d, a, "count", fixture.source.positions.size());
            yyjson_mut_obj_add_str(d, a, "type", "VEC4");
            if (i)
                yyjson_mut_obj_add_uint(d, a, "bufferView", 4);
            yyjson_mut_arr_append(accessors, a);
        }
        auto* view = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, view, "buffer", 0);
        yyjson_mut_obj_add_uint(d, view, "byteOffset", f.bin.size());
        yyjson_mut_obj_add_uint(d, view, "byteLength", skin_bin.size() - f.bin.size());
        yyjson_mut_arr_append(yyjson_mut_obj_get(root, "bufferViews"), view);
        yyjson_mut_set_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "buffers"), 0), "byteLength"), skin_bin.size());
        auto* p = yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "meshes"), 0), "primitives"), 0);
        auto* attributes = yyjson_mut_obj_get(p, "attributes");
        yyjson_mut_obj_add_uint(d, attributes, "JOINTS_0", 4);
        yyjson_mut_obj_add_uint(d, attributes, "WEIGHTS_0", 5);
        auto* nodes = yyjson_mut_obj_get(root, "nodes");
        yyjson_mut_obj_add_uint(d, yyjson_mut_arr_get(nodes, 1), "skin", 0);
        yyjson_mut_arr_append(nodes, yyjson_mut_obj(d));
        auto* skins = yyjson_mut_arr(d);
        auto* skin = yyjson_mut_obj(d);
        auto* joints = yyjson_mut_arr(d);
        yyjson_mut_arr_add_uint(d, joints, 2);
        yyjson_mut_obj_add_val(d, skin, "joints", joints);
        yyjson_mut_arr_append(skins, skin);
        yyjson_mut_obj_add_val(d, root, "skins", skins);
    });
    write(dir / "mixed-skin.glb", glb(skin_json, skin_bin));
    auto skin_doc = tool::read_document(dir / "mixed-skin.glb", {});
    CHECK(skin_doc);
    if (skin_doc) {
        auto e = tool::eligible_primitives(*skin_doc);
        CHECK(e && e->empty());
        auto saved = tool::serialize_json(*skin_doc);
        CHECK(saved && saved->find("\"skin\":0") != sub::string::npos);
    }
    auto animated_bin = f.bin;
    for (float value : {0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f})
        put(animated_bin, std::bit_cast<std::uint32_t>(value));
    auto animated_json = edit_json(f.json, [&](auto* d, auto* root) {
        auto* views = yyjson_mut_obj_get(root, "bufferViews");
        auto* accessors = yyjson_mut_obj_get(root, "accessors");
        for (unsigned i = 0; i < 2; ++i) {
            auto* view = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, view, "buffer", 0);
            yyjson_mut_obj_add_uint(d, view, "byteOffset", f.bin.size() + (i ? 8 : 0));
            yyjson_mut_obj_add_uint(d, view, "byteLength", i ? 24 : 8);
            yyjson_mut_arr_append(views, view);
            auto* a = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, a, "bufferView", 4 + i);
            yyjson_mut_obj_add_uint(d, a, "componentType", 5126);
            yyjson_mut_obj_add_uint(d, a, "count", 2);
            yyjson_mut_obj_add_str(d, a, "type", i ? "VEC3" : "SCALAR");
            if (!i) {
                auto* min = yyjson_mut_arr(d);
                auto* max = yyjson_mut_arr(d);
                yyjson_mut_arr_add_real(d, min, 0);
                yyjson_mut_arr_add_real(d, max, 1);
                yyjson_mut_obj_add_val(d, a, "min", min);
                yyjson_mut_obj_add_val(d, a, "max", max);
            }
            yyjson_mut_arr_append(accessors, a);
        }
        yyjson_mut_set_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "buffers"), 0), "byteLength"), animated_bin.size());
        auto* animations = yyjson_mut_arr(d);
        auto* animation = yyjson_mut_obj(d);
        auto* samplers = yyjson_mut_arr(d);
        auto* sampler = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, sampler, "input", 4);
        yyjson_mut_obj_add_uint(d, sampler, "output", 5);
        yyjson_mut_arr_append(samplers, sampler);
        yyjson_mut_obj_add_val(d, animation, "samplers", samplers);
        auto* channels = yyjson_mut_arr(d);
        auto* channel = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, channel, "sampler", 0);
        auto* target = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, target, "node", 1);
        yyjson_mut_obj_add_str(d, target, "path", "translation");
        yyjson_mut_obj_add_val(d, channel, "target", target);
        yyjson_mut_arr_append(channels, channel);
        yyjson_mut_obj_add_val(d, animation, "channels", channels);
        yyjson_mut_arr_append(animations, animation);
        yyjson_mut_obj_add_val(d, root, "animations", animations);
    });
    write(dir / "animated.glb", glb(animated_json, animated_bin));
    auto animated_doc = tool::read_document(dir / "animated.glb", {});
    CHECK(animated_doc);
    if (animated_doc && wire) {
        CHECK(tool::attach_payload(*animated_doc, {0, 0}, *wire));
        auto saved = tool::serialize_json(*animated_doc);
        CHECK(saved);
        if (saved) {
            auto* before = yyjson_read(animated_json.data(), animated_json.size(), 0);
            auto* after = yyjson_read(saved->data(), saved->size(), 0);
            CHECK(before && after);
            if (before && after)
                CHECK(yyjson_equals(yyjson_obj_get(yyjson_doc_get_root(before), "animations"), yyjson_obj_get(yyjson_doc_get_root(after), "animations")));
            yyjson_doc_free(before);
            yyjson_doc_free(after);
        }
    }
    auto textured_no_uv = edit_json(no_uv, [&](auto* d, auto* root) {
        auto* texture_info = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, texture_info, "index", 0);
        auto* pbr = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, pbr, "baseColorTexture", texture_info);
        yyjson_mut_obj_add_val(d, yyjson_mut_arr_get(yyjson_mut_obj_get(root, "materials"), 0), "pbrMetallicRoughness", pbr);
        auto* images = yyjson_mut_arr(d);
        auto* image = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, image, "uri", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ9sAAAAASUVORK5CYII=");
        yyjson_mut_arr_append(images, image);
        yyjson_mut_obj_add_val(d, root, "images", images);
        auto* textures = yyjson_mut_arr(d);
        auto* texture = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, texture, "source", 0);
        yyjson_mut_arr_append(textures, texture);
        yyjson_mut_obj_add_val(d, root, "textures", textures);
    });
    check_eligible(textured_no_uv, 0);
    auto required = f.json;
    replace(required, "\"extensionsUsed\":", "\"extensionsRequired\":[\"UNKNOWN_required\"],\"extensionsUsed\":");
    write(dir / "unknown.glb", glb(required, f.bin));
    CHECK(!tool::read_document(dir / "unknown.glb", {}));
    // A known name does not make an unsupported major/profile usable when required.
    auto bad_required = f.json;
    replace(bad_required, "\"attributes\":", "\"extensions\":{\"DEREN_subtilis\":{\"version\":2,\"profile\":\"shared-vertices\",\"bufferView\":0}},\"attributes\":");
    replace(bad_required, "\"extensionsUsed\":[\"VENDOR_metadata\"]", "\"extensionsUsed\":[\"VENDOR_metadata\",\"DEREN_subtilis\"],\"extensionsRequired\":[\"DEREN_subtilis\"]");
    write(dir / "unsupported.glb", glb(bad_required, f.bin));
    CHECK(!tool::read_document(dir / "unsupported.glb", {}));
    // Unsupported optional payload leaves ordinary LOD0 readable.
    replace(bad_required, ",\"extensionsRequired\":[\"DEREN_subtilis\"]", "");
    write(dir / "optional.glb", glb(bad_required, f.bin));
    CHECK(tool::read_document(dir / "optional.glb", {}));
    auto bad_window = f.json;
    replace(bad_window, "\"bufferView\":0,\"componentType\":5126", "\"bufferView\":0,\"byteOffset\":18446744073709551612,\"componentType\":5126");
    write(dir / "overflow.glb", glb(bad_window, f.bin));
    CHECK(!tool::read_document(dir / "overflow.glb", {}));
    auto stride = f.json;
    replace(stride, "\"bufferViews\":[{\"buffer\":0,", "\"bufferViews\":[{\"buffer\":0,\"byteStride\":4,");
    write(dir / "stride.glb", glb(stride, f.bin));
    CHECK(!tool::read_document(dir / "stride.glb", {}));

    sub::source_geometry tri;
    tri.positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    tri.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    tri.uv0 = {{0, 0}, {1, 0}, {0, 1}};
    tri.indices = {0, 1, 2};
    auto plain = make_fixture(tri);
    auto unindexed = plain.json;
    replace(unindexed, "\"indices\":3,", "");
    write(dir / "unindexed.glb", glb(unindexed, plain.bin));
    auto unindexed_doc = tool::read_document(dir / "unindexed.glb", {});
    CHECK(unindexed_doc);
    if (unindexed_doc) {
        auto g = tool::extract_geometry(*unindexed_doc, {0, 0});
        CHECK(g && g->indices == sub::vector<std::uint32_t>({0, 1, 2}));
    }
    auto missing_uv = plain.json;
    replace(missing_uv, ",\"TEXCOORD_0\":2", "");
    write(dir / "missing-uv.glb", glb(missing_uv, plain.bin));
    auto uv_doc = tool::read_document(dir / "missing-uv.glb", {});
    CHECK(uv_doc);
    if (uv_doc) {
        auto g = tool::extract_geometry(*uv_doc, {0, 0});
        CHECK((g && g->uv0 == sub::vector<sub::array<float, 2>>(3, {0, 0})));
    }

    // Quantized POSITION/NORMAL/UV with valid interleaved strides and signed normalization.
    sub::vector<std::byte> quant;
    for (auto row : sub::array<sub::array<std::int16_t, 3>, 3>{{{-32768, -32768, 0}, {32767, -32768, 0}, {0, 32767, 0}}}) {
        for (auto v : row) {
            quant.push_back(std::byte(std::uint16_t(v) & 255));
            quant.push_back(std::byte(std::uint16_t(v) >> 8));
        }
        quant.insert(quant.end(), 2, std::byte{0});
    }
    for (unsigned i = 0; i < 3; ++i)
        quant.insert(quant.end(), {std::byte{0}, std::byte{0}, std::byte{127}, std::byte{0}});
    for (auto uv : sub::array<sub::array<std::uint16_t, 2>, 3>{{{0, 0}, {65535, 0}, {0, 65535}}})
        for (auto v : uv) {
            quant.push_back(std::byte(v & 255));
            quant.push_back(std::byte(v >> 8));
        }
    quant.insert(quant.end(), {std::byte{0}, std::byte{1}, std::byte{2}});
    sub::string quant_json = R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_mesh_quantization"],"extensionsRequired":["KHR_mesh_quantization"],"buffers":[{"byteLength":51}],"bufferViews":[{"buffer":0,"byteLength":24,"byteStride":8},{"buffer":0,"byteOffset":24,"byteLength":12,"byteStride":4},{"buffer":0,"byteOffset":36,"byteLength":12},{"buffer":0,"byteOffset":48,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5122,"normalized":true,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5120,"normalized":true,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5121,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}],"nodes":[{"mesh":0,"translation":[100,0,0]}],"scenes":[{"nodes":[0]}],"scene":0})";
    write(dir / "quantized.glb", glb(quant_json, quant));
    auto uri_json = quant_json;
    replace(uri_json, "\"buffers\":[{\"byteLength\":51}]", "\"buffers\":[{\"byteLength\":51,\"uri\":\"data:application/octet-stream;base64,AIAAgAAAAAD/fwCAAAAAAAAA/38AAAAAAAB/AAAAfwAAAH8AAAAAAP//AAAAAP//AAEC\"}]");
    auto uri_bytes = std::as_bytes(std::span(uri_json));
    write(dir / "data-uri.gltf", sub::vector<std::byte>(uri_bytes.begin(), uri_bytes.end()));
    auto uri_doc = tool::read_document(dir / "data-uri.gltf", {});
    CHECK(uri_doc);
    if (uri_doc) {
        auto g = tool::extract_geometry(*uri_doc, {0, 0});
        CHECK(g && g->positions == tri.positions && sub::source_digest(*g) == sub::source_digest(tri));
    }
    auto quant_doc = tool::read_document(dir / "quantized.glb", {});
    CHECK(quant_doc);
    if (quant_doc) {
        auto g = tool::extract_geometry(*quant_doc, {0, 0});
        CHECK(g && g->positions == tri.positions && g->normals == tri.normals && g->uv0 == tri.uv0 && sub::source_digest(*g) == sub::source_digest(tri));
    }

    // Sparse source has no base bufferView. Its sorted index/value windows override zeros.
    auto sparse = plain;
    sparse.bin.insert(sparse.bin.end(), {std::byte{0}, std::byte{1}, std::byte{2}, std::byte{0}});
    sparse.bin.insert(sparse.bin.end(), plain.bin.begin(), plain.bin.begin() + 36);
    replace(sparse.json, "\"byteLength\":108", "\"byteLength\":148");
    replace(sparse.json, "],\"accessors\":", ",{\"buffer\":0,\"byteOffset\":108,\"byteLength\":3},{\"buffer\":0,\"byteOffset\":112,\"byteLength\":36}],\"accessors\":");
    replace(sparse.json, "\"bufferView\":0,\"componentType\":5126", "\"sparse\":{\"count\":3,\"indices\":{\"bufferView\":4,\"componentType\":5121},\"values\":{\"bufferView\":5}},\"componentType\":5126");
    write(dir / "sparse.glb", glb(sparse.json, sparse.bin));
    auto sparse_doc = tool::read_document(dir / "sparse.glb", {});
    CHECK(sparse_doc);
    if (sparse_doc) {
        auto g = tool::extract_geometry(*sparse_doc, {0, 0});
        CHECK(g && g->positions == tri.positions);
    }
    auto sparse_view = sparse.json;
    replace(sparse_view, "\"bufferView\":4,\"componentType\":5121", "\"bufferView\":999,\"componentType\":5121");
    write(dir / "bad-sparse-view.glb", glb(sparse_view, sparse.bin));
    CHECK(!tool::read_document(dir / "bad-sparse-view.glb", {}));
    sparse.bin[108] = std::byte{1};
    sparse.bin[109] = std::byte{0};
    write(dir / "bad-sparse.glb", glb(sparse.json, sparse.bin));
    CHECK(!tool::read_document(dir / "bad-sparse.glb", {}));
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
    return deren::vk_test::finish("subtilis_document");
}
