#include "../../tools/subtilis/commands.hpp"
#include "../../tools/subtilis/package_internal.hpp"
#include "../../tools/subtilis/document_internal.hpp"
#include "../../tools/subtilis/package_limits.hpp"
#include "subtilis_dag_fixtures.hpp"
#include "vk_test.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <yyjson.h>
namespace tool = deren::tools::subtilis;
namespace sub = deren::asset::subtilis;
#include "subtilis_document_fixtures.hpp"
using namespace subtilis_document_fixture;

namespace {
    // Forward declarations keep the Task 6 directory-package block self-contained.
    void fault_injection_tests(tool::path const& root);
    void cross_volume_tests(tool::path const& root, tool::path const& rich_source);
    std::string relative_generic(tool::path const& file, tool::path const& root);

    // ------------------------------------------------------------------ Task 6 helpers
    tool::path path_from_utf8(std::string_view text) {
        return tool::path(std::u8string(reinterpret_cast<char8_t const*>(text.data()), text.size()));
    }
    std::string read_text(tool::path const& file) {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        return text;
    }
    void write_text(tool::path const& file, std::string_view text) {
        std::ofstream out(file, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        CHECK(out.good());
    }
    void write_text(tool::path const& file, char const* text) {
        write_text(file, std::string_view(text));
    }
    void write_text(tool::path const& file, sub::string const& text) {
        write_text(file, std::string_view(text.data(), text.size()));
    }
    sub::string as_sub_string(std::string const& text) {
        return sub::string(text.data(), text.size());
    }
    sub::vector<std::byte> read_bytes(tool::path const& file) {
        auto text = read_text(file);
        auto raw = std::as_bytes(std::span(text));
        return sub::vector<std::byte>(raw.begin(), raw.end());
    }
    void write_bytes(tool::path const& file, std::span<std::byte const> bytes) {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    }
    sub::digest hash_bytes(std::span<std::byte const> bytes) {
        return sub::sha256(bytes);
    }
    bool same_bytes(std::span<std::byte const> a, std::span<std::byte const> b) {
        return a.size() == b.size() && hash_bytes(a) == hash_bytes(b);
    }
    std::string hex_digest(sub::digest const& value) {
        constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (auto byte_value : value) {
            auto c = std::to_integer<unsigned>(byte_value);
            text += digits[c >> 4];
            text += digits[c & 15];
        }
        return text;
    }
    std::string percent_encode(std::string_view decoded) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        for (char raw : decoded) {
            auto c = static_cast<unsigned char>(raw);
            bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
            if (unreserved) {
                encoded += static_cast<char>(c);
            } else {
                encoded += '%';
                encoded += digits[c >> 4];
                encoded += digits[c & 15];
            }
        }
        return encoded;
    }
    std::optional<std::string> percent_decode(std::string_view uri) {
        auto hex = [](char c) -> int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; };
        std::string decoded;
        for (size_t i = 0; i < uri.size(); ++i) {
            char c = uri[i];
            if (c == '%') {
                if (uri.size() - i < 3)
                    return std::nullopt;
                auto high = hex(uri[i + 1]), low = hex(uri[i + 2]);
                if (high < 0 || low < 0)
                    return std::nullopt;
                c = static_cast<char>(high * 16 + low);
                i += 2;
            }
            if (!c || c == ':' || c == '?' || c == '#' || c == '\\')
                return std::nullopt;
            decoded += c;
        }
        auto relative = path_from_utf8(decoded);
        if (relative.empty() || relative.is_absolute() || relative.has_root_path())
            return std::nullopt;
        for (auto const& part : relative)
            if (part == ".." || part == ".")
                return std::nullopt;
        return decoded;
    }
    // Verifies one generated resource URI: relative, canonical percent encoding, exists
    // inside the package, and its bytes hash to the expected source resource bytes.
    void check_uri_resource(tool::path const& package, std::string const& uri, std::span<std::byte const> expected) {
        auto decoded = percent_decode(uri);
        CHECK_MSG(decoded.has_value(), uri.c_str());
        if (!decoded)
            return;
        CHECK_MSG(percent_encode(*decoded) == uri, uri.c_str());
        auto file = package / path_from_utf8(*decoded);
        std::error_code ec;
        CHECK_MSG(std::filesystem::is_regular_file(file, ec) && !ec, uri.c_str());
        auto bytes = read_bytes(file);
        CHECK_MSG(same_bytes(bytes, expected), uri.c_str());
    }
    void check_no_staging(tool::path const& root) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
            return;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec)
                break;
            auto name = entry.path().filename().string();
            CHECK_MSG(!name.starts_with(".subtilis-"), entry.path().generic_string().c_str());
        }
    }
    std::map<std::string, std::string> tree_hashes(tool::path const& root) {
        std::map<std::string, std::string> result;
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
            return result;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec)
                break;
            std::error_code file_error;
            if (!entry.is_regular_file(file_error) || file_error)
                continue;
            auto relative = std::filesystem::relative(entry.path(), root, file_error);
            if (file_error)
                continue;
            result[relative.generic_string()] = hex_digest(hash_bytes(read_bytes(entry.path())));
        }
        return result;
    }
    std::vector<std::string> directory_files(tool::path const& root) {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec)
                break;
            if (entry.is_regular_file(ec) && !ec)
                names.push_back(entry.path().filename().generic_string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
    yyjson_val* first_primitive(yyjson_val* root_value) {
        return yyjson_arr_get(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(root_value, "meshes"), 0), "primitives"), 0);
    }
    yyjson_val* json_member(yyjson_val* object, char const* key) {
        return yyjson_obj_get(object, key);
    }
    int run_cli(std::vector<std::string> const& parts) {
        std::vector<std::string_view> arguments;
        arguments.reserve(parts.size());
        for (auto const& part : parts)
            arguments.push_back(part);
        auto parsed = tool::parse_arguments(arguments);
        CHECK(parsed);
        if (!parsed)
            return -1;
        return tool::run(*parsed);
    }
    bool parse_fails(std::vector<std::string> const& parts) {
        std::vector<std::string_view> arguments;
        arguments.reserve(parts.size());
        for (auto const& part : parts)
            arguments.push_back(part);
        auto parsed = tool::parse_arguments(arguments);
        if (parsed)
            CHECK_MSG(false, parts.empty() ? "" : parts.back().c_str());
        return !parsed;
    }
    bool json_array_contains(yyjson_val* array, std::string_view name) {
        for (size_t i = 0; i < yyjson_arr_size(array); ++i) {
            auto* value = yyjson_arr_get(array, i);
            if (yyjson_is_str(value) && std::string_view(yyjson_get_str(value)) == name)
                return true;
        }
        return false;
    }
    bool json_values_equal(yyjson_val* before, yyjson_val* after) {
        if (!before || !after)
            return before == after;
        return yyjson_equals(before, after);
    }
    void check_object_members_equal(yyjson_val* before, yyjson_val* after, std::initializer_list<char const*> names) {
        for (auto name : names)
            CHECK_MSG(json_values_equal(yyjson_obj_get(before, name), yyjson_obj_get(after, name)), name);
    }
    void check_same_json_value(yyjson_val* before, yyjson_val* after, char const* label) {
        CHECK_MSG(json_values_equal(before, after), label);
    }

    // ------------------------------------------------------------------ rich source fixture
    struct rich_fixture {
        sub::string json;
        sub::vector<std::byte> geometry, animation, png;
    };
    rich_fixture write_rich_fixture(tool::path const& directory, fixture const& base, sub::vector<std::byte> const& png) {
        rich_fixture rich;
        rich.geometry = base.bin;
        rich.png = png;
        for (float value : {0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f})
            put(rich.animation, std::bit_cast<std::uint32_t>(value));
        rich.json = edit_json(base.json, [&](auto* d, auto* root) {
            auto* buffers = yyjson_mut_obj_get(root, "buffers");
            auto* first = yyjson_mut_arr_get_first(buffers);
            yyjson_mut_obj_remove_key(first, "uri");
            yyjson_mut_obj_add_str(d, first, "uri", "geometry%20data.bin");
            yyjson_mut_obj_add_str(d, first, "name", "geometry-source");
            auto* second = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, second, "uri", "animation data.bin");
            yyjson_mut_obj_add_uint(d, second, "byteLength", rich.animation.size());
            yyjson_mut_obj_add_str(d, second, "name", "animation-source");
            yyjson_mut_arr_append(buffers, second);
            auto* views = yyjson_mut_obj_get(root, "bufferViews");
            auto* time_view = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, time_view, "buffer", 1);
            yyjson_mut_obj_add_uint(d, time_view, "byteOffset", 0);
            yyjson_mut_obj_add_uint(d, time_view, "byteLength", 8);
            yyjson_mut_arr_append(views, time_view);
            auto* move_view = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, move_view, "buffer", 1);
            yyjson_mut_obj_add_uint(d, move_view, "byteOffset", 8);
            yyjson_mut_obj_add_uint(d, move_view, "byteLength", 24);
            yyjson_mut_arr_append(views, move_view);
            auto* accessors = yyjson_mut_obj_get(root, "accessors");
            auto* time_accessor = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, time_accessor, "bufferView", 4);
            yyjson_mut_obj_add_uint(d, time_accessor, "componentType", 5126);
            yyjson_mut_obj_add_uint(d, time_accessor, "count", 2);
            yyjson_mut_obj_add_str(d, time_accessor, "type", "SCALAR");
            auto* time_min = yyjson_mut_arr(d);
            auto* time_max = yyjson_mut_arr(d);
            yyjson_mut_arr_add_real(d, time_min, 0);
            yyjson_mut_arr_add_real(d, time_max, 1);
            yyjson_mut_obj_add_val(d, time_accessor, "min", time_min);
            yyjson_mut_obj_add_val(d, time_accessor, "max", time_max);
            yyjson_mut_arr_append(accessors, time_accessor);
            auto* move_accessor = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, move_accessor, "bufferView", 5);
            yyjson_mut_obj_add_uint(d, move_accessor, "componentType", 5126);
            yyjson_mut_obj_add_uint(d, move_accessor, "count", 2);
            yyjson_mut_obj_add_str(d, move_accessor, "type", "VEC3");
            yyjson_mut_arr_append(accessors, move_accessor);
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
            auto* channel_target = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, channel_target, "node", 1);
            yyjson_mut_obj_add_str(d, channel_target, "path", "translation");
            yyjson_mut_obj_add_val(d, channel, "target", channel_target);
            yyjson_mut_arr_append(channels, channel);
            yyjson_mut_obj_add_val(d, animation, "channels", channels);
            yyjson_mut_arr_append(animations, animation);
            yyjson_mut_obj_add_val(d, root, "animations", animations);
            auto* images = yyjson_mut_arr(d);
            auto* image = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, image, "uri", "pixel.png");
            yyjson_mut_obj_add_str(d, image, "name", "pixel-image");
            yyjson_mut_arr_append(images, image);
            yyjson_mut_obj_add_val(d, root, "images", images);
            auto* textures = yyjson_mut_arr(d);
            auto* texture = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, texture, "source", 0);
            yyjson_mut_arr_append(textures, texture);
            yyjson_mut_obj_add_val(d, root, "textures", textures);
            auto* material = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "materials"));
            auto* pbr = yyjson_mut_obj(d);
            auto* texture_info = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, texture_info, "index", 0);
            yyjson_mut_obj_add_val(d, pbr, "baseColorTexture", texture_info);
            yyjson_mut_obj_add_val(d, material, "pbrMetallicRoughness", pbr);
            // Unknown optional extension: the glTF package must preserve it (GLB relocation refuses
            // it, which the self-contained-GLB regression in section F2 covers).
            auto* used = yyjson_mut_obj_get(root, "extensionsUsed");
            if (!used) {
                used = yyjson_mut_arr(d);
                yyjson_mut_obj_add_val(d, root, "extensionsUsed", used);
            }
            yyjson_mut_arr_add_str(d, used, "VENDOR_metadata");
            auto* extensions = yyjson_mut_obj(d);
            auto* metadata = yyjson_mut_obj(d);
            auto* nested = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, nested, "u", UINT64_MAX);
            yyjson_mut_obj_add_str(d, nested, "text", "keep me");
            yyjson_mut_obj_add_val(d, metadata, "nested", nested);
            yyjson_mut_obj_add_val(d, extensions, "VENDOR_metadata", metadata);
            yyjson_mut_obj_add_val(d, root, "extensions", extensions);
        });
        write_bytes(directory / "geometry data.bin", rich.geometry);
        write_bytes(directory / "animation data.bin", rich.animation);
        write_bytes(directory / "pixel.png", rich.png);
        write_text(directory / "rich.gltf", rich.json);
        return rich;
    }
    // The rich source carries an unknown (optional) extension on purpose: GLB packaging
    // must refuse it, while the directory package must preserve it and its binary
    // semantics (buffer IDs and bufferView ranges stay identical).
    void check_package_dom(std::string_view input_text, std::string_view output_text, tool::path const& package, rich_fixture const& rich, std::span<std::byte const> expected_payload) {
        auto* before = yyjson_read(input_text.data(), input_text.size(), 0);
        auto* after = yyjson_read(output_text.data(), output_text.size(), 0);
        CHECK(before && after);
        if (!before || !after) {
            yyjson_doc_free(before);
            yyjson_doc_free(after);
            return;
        }
        auto* a = yyjson_doc_get_root(before);
        auto* b = yyjson_doc_get_root(after);
        // Every top-level value with stable IDs is preserved exactly (extension list
        // legitimately grows by DEREN_subtilis; buffers/images relocate their URIs).
        for (auto key : {"asset", "scene", "scenes", "nodes", "materials", "accessors", "animations", "textures", "samplers", "skins", "cameras", "extras", "extensions"})
            check_same_json_value(yyjson_obj_get(a, key), yyjson_obj_get(b, key), key);
        auto* used_before = yyjson_obj_get(a, "extensionsUsed");
        auto* used_after = yyjson_obj_get(b, "extensionsUsed");
        CHECK(yyjson_is_arr(used_after));
        for (size_t i = 0; i < yyjson_arr_size(used_before); ++i) {
            auto* name = yyjson_arr_get(used_before, i);
            CHECK(yyjson_is_str(name) && json_array_contains(used_after, std::string_view(yyjson_get_str(name))));
        }
        CHECK(json_array_contains(used_after, "DEREN_subtilis"));
        // Meshes: identity of every non-relocated field; only primitive extensions change.
        auto* meshes_before = yyjson_obj_get(a, "meshes");
        auto* meshes_after = yyjson_obj_get(b, "meshes");
        CHECK(yyjson_arr_size(meshes_before) == yyjson_arr_size(meshes_after));
        for (size_t m = 0; m < yyjson_arr_size(meshes_before); ++m) {
            auto* mesh_before = yyjson_arr_get(meshes_before, m);
            auto* mesh_after = yyjson_arr_get(meshes_after, m);
            check_object_members_equal(mesh_before, mesh_after, {"name", "extras", "extensions", "weights"});
            auto* prims_before = yyjson_obj_get(mesh_before, "primitives");
            auto* prims_after = yyjson_obj_get(mesh_after, "primitives");
            CHECK(yyjson_arr_size(prims_before) == yyjson_arr_size(prims_after));
            for (size_t p = 0; p < yyjson_arr_size(prims_before); ++p) {
                auto* prim_before = yyjson_arr_get(prims_before, p);
                auto* prim_after = yyjson_arr_get(prims_after, p);
                for (auto key : {"attributes", "indices", "material", "mode", "targets"})
                    check_same_json_value(yyjson_obj_get(prim_before, key), yyjson_obj_get(prim_after, key), key);
                auto* ext_before = yyjson_obj_get(prim_before, "extensions");
                auto* ext_after = yyjson_obj_get(prim_after, "extensions");
                yyjson_obj_iter iterator = yyjson_obj_iter_with(ext_before);
                while (auto* key = yyjson_obj_iter_next(&iterator))
                    check_same_json_value(yyjson_obj_iter_get_val(key), yyjson_obj_get(ext_after, yyjson_get_str(key)), "primitive extension member");
                CHECK(yyjson_obj_get(ext_after, "DEREN_subtilis") != nullptr);
            }
        }
        // Buffer views keep their array IDs; only payload views are appended.
        auto* views_before = yyjson_obj_get(a, "bufferViews");
        auto* views_after = yyjson_obj_get(b, "bufferViews");
        CHECK(yyjson_arr_size(views_after) > yyjson_arr_size(views_before));
        for (size_t i = 0; i < yyjson_arr_size(views_before); ++i)
            check_same_json_value(yyjson_arr_get(views_before, i), yyjson_arr_get(views_after, i), "bufferView identity");
        // Buffers: original IDs and metadata survive; local URIs relocate to package files
        // whose bytes hash back to the snapshotted source resource.
        auto* buffers_before = yyjson_obj_get(a, "buffers");
        auto* buffers_after = yyjson_obj_get(b, "buffers");
        CHECK(yyjson_arr_size(buffers_after) == yyjson_arr_size(buffers_before) + 1);
        for (size_t i = 0; i < yyjson_arr_size(buffers_before); ++i) {
            auto* buffer_before = yyjson_arr_get(buffers_before, i);
            auto* buffer_after = yyjson_arr_get(buffers_after, i);
            check_object_members_equal(buffer_before, buffer_after, {"byteLength", "name", "extras", "extensions"});
            auto* uri_before = yyjson_obj_get(buffer_before, "uri");
            auto* uri_after = yyjson_obj_get(buffer_after, "uri");
            CHECK(yyjson_is_str(uri_after));
            auto text = std::string(yyjson_get_str(uri_after), yyjson_get_len(uri_after));
            if (yyjson_is_str(uri_before) && std::string_view(yyjson_get_str(uri_before)).starts_with("data:")) {
                check_same_json_value(uri_before, uri_after, "data URI buffer identity");
            } else if (i == 0) {
                check_uri_resource(package, text, rich.geometry);
            } else {
                check_uri_resource(package, text, rich.animation);
            }
        }
        auto* payload_buffer = yyjson_arr_get(buffers_after, yyjson_arr_size(buffers_before));
        auto* payload_uri = json_member(payload_buffer, "uri");
        CHECK(yyjson_is_str(payload_uri));
        auto payload_uri_text = std::string(yyjson_get_str(payload_uri), yyjson_get_len(payload_uri));
        auto payload_decoded = percent_decode(payload_uri_text);
        CHECK(payload_decoded.has_value());
        CHECK(yyjson_get_uint(json_member(payload_buffer, "byteLength")) == expected_payload.size());
        if (!payload_decoded)
            return;
        auto payload_file = read_bytes(package / path_from_utf8(*payload_decoded));
        // Images: URIs relocate with the MIME-derived extension; data URIs stay inline.
        auto* images_before = yyjson_obj_get(a, "images");
        auto* images_after = yyjson_obj_get(b, "images");
        CHECK(yyjson_arr_size(images_before) == yyjson_arr_size(images_after));
        for (size_t i = 0; i < yyjson_arr_size(images_before); ++i) {
            auto* image_before = yyjson_arr_get(images_before, i);
            auto* image_after = yyjson_arr_get(images_after, i);
            check_object_members_equal(image_before, image_after, {"name", "extras", "extensions", "bufferView"});
            auto* uri_before = yyjson_obj_get(image_before, "uri");
            auto* uri_after = yyjson_obj_get(image_after, "uri");
            if (yyjson_is_str(uri_before) && std::string_view(yyjson_get_str(uri_before)).starts_with("data:")) {
                check_same_json_value(uri_before, uri_after, "data URI image identity");
                continue;
            }
            CHECK(yyjson_is_str(uri_after));
            // MIME sniffing must give a PNG extension and the matching declared type.
            auto mime = json_member(image_after, "mimeType");
            CHECK(yyjson_is_str(mime) && std::string_view(yyjson_get_str(mime)) == "image/png");
            auto uri_text = std::string(yyjson_get_str(uri_after), yyjson_get_len(uri_after));
            CHECK(uri_text.ends_with(".png"));
            check_uri_resource(package, uri_text, rich.png);
        }
        // The payload bufferView points at the appended buffer with the expected range and
        // the same DEREN wire bytes as the payload extracted from an equivalent GLB build.
        auto* extension = json_member(json_member(first_primitive(b), "extensions"), "DEREN_subtilis");
        CHECK(extension);
        if (!extension) {
            yyjson_doc_free(before);
            yyjson_doc_free(after);
            return;
        }
        CHECK(yyjson_get_uint(json_member(extension, "version")) == 1);
        CHECK(std::string_view(yyjson_get_str(json_member(extension, "profile"))) == "shared-vertices");
        auto view_id = yyjson_get_uint(json_member(extension, "bufferView"));
        auto* payload_view = yyjson_arr_get(views_after, view_id);
        CHECK(yyjson_get_uint(json_member(payload_view, "buffer")) == yyjson_arr_size(buffers_before));
        auto offset = yyjson_get_uint(json_member(payload_view, "byteOffset"));
        auto length = yyjson_get_uint(json_member(payload_view, "byteLength"));
        CHECK(length == expected_payload.size());
        CHECK(offset <= payload_file.size() && length <= payload_file.size() - offset);
        if (offset <= payload_file.size() && length <= payload_file.size() - offset && length == expected_payload.size()) {
            auto slice = std::span(payload_file).subspan(static_cast<size_t>(offset), static_cast<size_t>(length));
            CHECK(same_bytes(slice, expected_payload));
            auto decoded = sub::decode(slice, {});
            CHECK(decoded);
            if (decoded)
                CHECK(sub::validate_structure(*decoded, {}));
        }
        yyjson_doc_free(before);
        yyjson_doc_free(after);
    }

    // ------------------------------------------------------------------ fault injection seam
    struct fault_plan {
        enum class write_mode { normal,
                                error_short,
                                silent_short,
                                close_failure } mode = write_mode::normal;
        size_t limit = 0;
        std::string only_file;
        std::vector<tool::path> opened;
        std::vector<tool::path> renamed;
        int fail_rename_call = 0;
        int rename_calls = 0;
    };
    class fault_sink final : public tool::package_detail::byte_sink {
        std::ofstream out_;
        fault_plan* plan_ = nullptr;
        bool injected_ = false;

    public:
        fault_sink(tool::path const& file, fault_plan* plan, bool injected)
            : out_(file, std::ios::binary)
            , plan_(plan)
            , injected_(injected) {
        }
        bool is_open() const {
            return out_.is_open();
        }
        tool::result<void> write(std::span<std::byte const> bytes) override {
            auto count = bytes.size();
            if (injected_ && (plan_->mode == fault_plan::write_mode::error_short || plan_->mode == fault_plan::write_mode::silent_short))
                count = std::min(count, plan_->limit);
            out_.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(count));
            if (injected_ && plan_->mode == fault_plan::write_mode::error_short)
                return std::unexpected(sub::error{sub::error_code::io, "simulated short write", {}, {}});
            if (!out_)
                return std::unexpected(sub::error{sub::error_code::io, "output write failed", {}, {}});
            return {};
        }
        tool::result<void> close() override {
            out_.close();
            if (injected_ && plan_->mode == fault_plan::write_mode::close_failure)
                return std::unexpected(sub::error{sub::error_code::io, "simulated close failure", {}, {}});
            if (!out_)
                return std::unexpected(sub::error{sub::error_code::io, "output close failed", {}, {}});
            return {};
        }
    };
    tool::result<std::unique_ptr<tool::package_detail::byte_sink>> fault_open(tool::path const& file, void* context) {
        auto* plan = static_cast<fault_plan*>(context);
        plan->opened.push_back(file);
        bool injected = plan->only_file.empty() || file.filename().string().find(plan->only_file) != std::string::npos;
        auto sink = std::make_unique<fault_sink>(file, plan, injected);
        if (!sink->is_open())
            return std::unexpected(sub::error{sub::error_code::io, "cannot create staging file", {}, {}});
        return std::unique_ptr<tool::package_detail::byte_sink>(std::move(sink));
    }
    tool::result<void> fault_rename(tool::path const& from, tool::path const& to, void* context) {
        auto* plan = static_cast<fault_plan*>(context);
        ++plan->rename_calls;
        plan->renamed.push_back(from);
        if (plan->fail_rename_call == plan->rename_calls)
            return std::unexpected(sub::error{sub::error_code::io, "simulated final publication failure", {}, {}});
        return tool::package_detail::move_path(from, to);
    }
    struct injected_ops {
        tool::package_detail::sink_ops ops;
        fault_plan plan;
        explicit injected_ops(fault_plan::write_mode mode = fault_plan::write_mode::normal, size_t limit = 0, std::string only = {}, int fail_rename = 0) {
            plan.mode = mode;
            plan.limit = limit;
            plan.only_file = std::move(only);
            plan.fail_rename_call = fail_rename;
            ops.open = &fault_open;
            ops.rename = &fault_rename;
            ops.context = &plan;
        }
    };

    // ------------------------------------------------------------------ directory package tests
    void directory_package_tests(tool::path const& dir, fixture const& base, sub::vector<std::byte> const& png) {
        std::error_code ec;
        auto root = dir / "task6-packages";
        std::filesystem::create_directory(root, ec);
        CHECK(!ec);
        auto input_glb = (dir / "input.glb").generic_string();
        auto cooked_glb = (dir / "cooked.glb").generic_string();

        // A. CLI --format selection, package layout and independent read-back validation.
        auto package_out = (root / "cooked.gltf").generic_string();
        auto package_dir = root / "cooked";
        auto package_json = tool::package_detail::package_json_path(package_dir);
        CHECK(package_json == package_dir / "cooked.gltf");
        CHECK(!std::filesystem::exists(package_dir, ec));
        CHECK(run_cli({"build", input_glb, "-o", package_out, "--format", "gltf"}) == 0);
        CHECK(std::filesystem::is_directory(package_dir, ec) && !ec);
        CHECK(std::filesystem::is_regular_file(package_json, ec) && !ec);
        CHECK(std::filesystem::is_regular_file(package_dir / "cooked.bin", ec) && !ec);
        CHECK(directory_files(package_dir) == (std::vector<std::string>{"cooked.bin", "cooked.gltf"}));
        CHECK(!std::filesystem::exists(root / "cooked.gltf", ec));
        check_no_staging(root);
        auto package_json_text = package_json.generic_string();
        CHECK(run_cli({"validate", package_json_text, "--full"}) == 0);
        CHECK(run_cli({"inspect", package_json_text}) == 0);
        {
            // The package can be read back with only the tool's public document API.
            auto doc = tool::read_document(package_json, {});
            CHECK(doc);
            if (doc) {
                auto wire = tool::payload_bytes(*doc, {0, 0});
                auto geometry = tool::extract_geometry(*doc, {0, 0});
                CHECK(wire && geometry);
                auto payload = wire ? sub::decode(*wire, {}) : sub::result<sub::payload>(std::unexpected(sub::error{sub::error_code::invalid_input, "missing", {}, {}}));
                CHECK(payload);
                if (geometry && payload) {
                    auto asset = sub::make_asset(std::move(*geometry), std::move(*payload), {});
                    CHECK(asset);
                }
            }
        }
        {
            auto source = tool::read_document(dir / "input.glb", {});
            CHECK(source);
            if (source)
                CHECK(!tool::payload_bytes(*source, {0, 0}));
        }
        // Format selection is explicit; the old default must not silently write a package.
        CHECK(parse_fails({"build", input_glb, "-o", (root / "default.gltf").generic_string()}));
        CHECK(parse_fails({"build", input_glb, "-o", (root / "wrong.glb").generic_string(), "--format", "gltf"}));
        CHECK(parse_fails({"build", input_glb, "-o", (root / "noext").generic_string(), "--format", "gltf"}));
        CHECK(parse_fails({"build", input_glb, "-o", (root / "bad-format.glb").generic_string(), "--format", "weird"}));
        CHECK(parse_fails({"build", input_glb, "-o", (root / "swapped.gltf").generic_string(), "--format", "glb"}));
        CHECK(run_cli({"build", input_glb, "-o", (root / "explicit.glb").generic_string(), "--format", "glb"}) == 0);
        CHECK(std::filesystem::is_regular_file(root / "explicit.glb", ec) && !ec);
        // Existing package is refused without --overwrite and its bytes are untouched.
        auto prior = tree_hashes(package_dir);
        CHECK(!prior.empty());
        CHECK(run_cli({"build", input_glb, "-o", package_out, "--format", "gltf"}) == 5);
        CHECK(tree_hashes(package_dir) == prior);
        // The source may not be consumed by the publication it would replace.
        CHECK(run_cli({"build", package_json_text, "-o", package_out, "--format", "gltf", "--overwrite"}) == 5);
        CHECK(tree_hashes(package_dir) == prior);
        // Nor may the optional report live inside the package being replaced.
        CHECK(run_cli({"build", input_glb, "-o", package_out, "--format", "gltf", "--report", (package_dir / "build-report.json").generic_string(), "--overwrite"}) == 5);
        CHECK(tree_hashes(package_dir) == prior);
        // --overwrite replaces the whole package directory, including stale files.
        write_text(package_dir / "stale-marker.txt", "stale");
        CHECK(run_cli({"build", input_glb, "-o", package_out, "--format", "gltf", "--overwrite"}) == 0);
        CHECK(!std::filesystem::exists(package_dir / "stale-marker.txt", ec));
        CHECK(std::filesystem::is_regular_file(package_json, ec) && !ec);
        check_no_staging(root);
        // An existing regular file at the package path can be replaced with --overwrite.
        write_text(root / "file-target", "old file");
        CHECK(run_cli({"build", input_glb, "-o", (root / "file-target.gltf").generic_string(), "--format", "gltf", "--overwrite"}) == 0);
        CHECK(std::filesystem::is_directory(root / "file-target", ec) && !ec);
        CHECK(std::filesystem::is_regular_file(root / "file-target" / "file-target.gltf", ec) && !ec);
        // export-cut accepts the same directory package format.
        CHECK(run_cli({"export-cut", cooked_glb, "--error", "0.01", "-o", (root / "cut.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto cut_json = tool::package_detail::package_json_path(root / "cut");
            auto cut_doc = tool::read_document(cut_json, {});
            CHECK(cut_doc);
            if (cut_doc) {
                auto text = tool::serialize_json(*cut_doc);
                CHECK(text && text->find("DEREN_subtilis") == sub::string::npos);
                auto geometry = tool::extract_geometry(*cut_doc, {0, 0});
                CHECK(geometry && !geometry->indices.empty());
            }
        }

        // B. Data URI resources stay inline in the package.
        CHECK(run_cli({"build", (dir / "data.gltf").generic_string(), "-o", (root / "data-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto data_dir = root / "data-out";
            CHECK(directory_files(data_dir) == (std::vector<std::string>{"data-out.bin", "data-out.gltf"}));
            auto input_text = read_text(dir / "data.gltf");
            auto output_text = read_text(tool::package_detail::package_json_path(data_dir));
            auto* input_root = yyjson_read(input_text.data(), input_text.size(), 0);
            auto* output_root = yyjson_read(output_text.data(), output_text.size(), 0);
            CHECK(input_root && output_root);
            if (input_root && output_root) {
                auto* input_buffers = yyjson_obj_get(yyjson_doc_get_root(input_root), "buffers");
                auto* output_buffers = yyjson_obj_get(yyjson_doc_get_root(output_root), "buffers");
                check_same_json_value(yyjson_obj_get(yyjson_arr_get(input_buffers, 0), "uri"), yyjson_obj_get(yyjson_arr_get(output_buffers, 0), "uri"), "data URI buffer identity");
                auto* input_images = yyjson_obj_get(yyjson_doc_get_root(input_root), "images");
                auto* output_images = yyjson_obj_get(yyjson_doc_get_root(output_root), "images");
                check_same_json_value(yyjson_obj_get(yyjson_arr_get(input_images, 0), "uri"), yyjson_obj_get(yyjson_arr_get(output_images, 0), "uri"), "data URI image identity");
            }
            yyjson_doc_free(input_root);
            yyjson_doc_free(output_root);
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(data_dir).generic_string(), "--full"}) == 0);
        }

        // C. External resources: buffers relocate with every ID and byte preserved.
        auto geometry_bin = dir / "geometry data.bin";
        auto geometry_before = hash_bytes(read_bytes(geometry_bin));
        CHECK(run_cli({"build", (dir / "external.gltf").generic_string(), "-o", (root / "external-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto external_dir = root / "external-out";
            CHECK(directory_files(external_dir) == (std::vector<std::string>{"buffer0.bin", "external-out.bin", "external-out.gltf"}));
            CHECK(hash_bytes(read_bytes(external_dir / "buffer0.bin")) == geometry_before);
            CHECK(hash_bytes(read_bytes(geometry_bin)) == geometry_before);
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(external_dir).generic_string(), "--full"}) == 0);
            auto packaged = tool::read_document(tool::package_detail::package_json_path(external_dir), {});
            CHECK(packaged);
            if (packaged) {
                auto geometry = tool::extract_geometry(*packaged, {0, 0});
                auto original = subtilis_fixture::cross_dag().source;
                CHECK(geometry && geometry->positions == original.positions && geometry->indices == original.indices);
            }
        }
        // D. Several external buffers keep their buffer IDs in a package.
        CHECK(run_cli({"build", (dir / "multiple.gltf").generic_string(), "-o", (root / "multiple-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto multiple_dir = root / "multiple-out";
            CHECK(hash_bytes(read_bytes(multiple_dir / "buffer0.bin")) == hash_bytes(read_bytes(dir / "attributes.bin")));
            CHECK(hash_bytes(read_bytes(multiple_dir / "buffer1.bin")) == hash_bytes(read_bytes(dir / "indices.bin")));
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(multiple_dir).generic_string(), "--full"}) == 0);
            auto multiple = tool::read_document(tool::package_detail::package_json_path(multiple_dir), {});
            CHECK(multiple);
            if (multiple) {
                auto geometry = tool::extract_geometry(*multiple, {0, 0});
                auto original = subtilis_fixture::cross_dag().source;
                CHECK(geometry && geometry->positions == original.positions && geometry->indices == original.indices);
            }
        }
        // E. Images relocate with a MIME-derived extension and preserved bytes.
        CHECK(run_cli({"build", (dir / "images.gltf").generic_string(), "-o", (root / "images-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto images_dir = root / "images-out";
            CHECK(std::filesystem::is_regular_file(images_dir / "image0.png", ec) && !ec);
            CHECK(hash_bytes(read_bytes(images_dir / "image0.png")) == hash_bytes(png));
            auto output_text = read_text(tool::package_detail::package_json_path(images_dir));
            auto* output_root = yyjson_read(output_text.data(), output_text.size(), 0);
            CHECK(output_root);
            if (output_root) {
                auto* image = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(output_root), "images"));
                auto* uri = yyjson_obj_get(image, "uri");
                auto* mime = yyjson_obj_get(image, "mimeType");
                CHECK(yyjson_is_str(uri) && std::string_view(yyjson_get_str(uri)) == "image0.png");
                CHECK(yyjson_is_str(mime) && std::string_view(yyjson_get_str(mime)) == "image/png");
                CHECK(yyjson_obj_get(image, "bufferView") == nullptr);
            }
            yyjson_doc_free(output_root);
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(images_dir).generic_string(), "--full"}) == 0);
        }
        // F. Unknown binary semantics: GLB packaging refuses, the package preserves the DOM.
        CHECK(run_cli({"build", (dir / "unknown.gltf").generic_string(), "-o", (root / "unknown.glb").generic_string()}) == 5);
        CHECK(run_cli({"build", (dir / "unknown.gltf").generic_string(), "-o", (root / "unknown-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto output_text = read_text(tool::package_detail::package_json_path(root / "unknown-out"));
            auto* output_root = yyjson_read(output_text.data(), output_text.size(), 0);
            CHECK(output_root);
            if (output_root) {
                auto* used = yyjson_obj_get(yyjson_doc_get_root(output_root), "extensionsUsed");
                CHECK(yyjson_is_arr(used) && json_array_contains(used, "VENDOR_binary"));
            }
            yyjson_doc_free(output_root);
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(root / "unknown-out").generic_string(), "--full"}) == 0);
        }
        // F2. Review I-1 regression: the GLB relocation-safety rule is one shared precondition for
        // BOTH GLB writers, so a self-contained GLB input cannot bypass it. Before the fix these
        // probes exited 0 and the relocated output kept the dangling unknown-extension URI.
        auto bad_extension_json = edit_json(base.json, [](auto* d, auto* root) {
            auto* used = yyjson_mut_arr(d);
            yyjson_mut_arr_add_str(d, used, "VENDOR_binary");
            yyjson_mut_obj_add_val(d, root, "extensionsUsed", used);
            auto* extensions = yyjson_mut_obj(d);
            auto* binary = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, binary, "uri", "vendor-blob.bin");
            yyjson_mut_obj_add_val(d, extensions, "VENDOR_binary", binary);
            yyjson_mut_obj_add_val(d, root, "extensions", extensions);
        });
        auto bad_self_contained = dir / "unknown-self-contained.glb";
        write(bad_self_contained, glb(bad_extension_json, base.bin));
        // (a) self-contained GLB + unknown binary extension, no image: the fast GLB writer refuses.
        CHECK(run_cli({"build", bad_self_contained.generic_string(), "-o", (root / "bad-self.glb").generic_string()}) == 5);
        CHECK(!std::filesystem::exists(root / "bad-self.glb", ec));
        auto sentinel = root / "bad-self-target.glb";
        write_text(sentinel, "sentinel");
        CHECK(run_cli({"build", bad_self_contained.generic_string(), "-o", sentinel.generic_string(), "--overwrite"}) == 5);
        CHECK(read_text(sentinel) == "sentinel");
        check_no_staging(root);
        auto bad_doc = tool::read_document(bad_self_contained, {});
        CHECK(bad_doc);
        if (bad_doc) {
            auto direct = tool::write_package(*bad_doc, root / "bad-self-direct.glb", {});
            CHECK(!direct && direct.error().code == sub::error_code::io);
            auto packed = tool::pack_glb(*bad_doc); // second writer, no CLI wrapper
            CHECK(!packed && packed.error().code == sub::error_code::io);
        }
        // The same self-contained GLB with a data-URI image selects the merging writer; that writer
        // applies the same precondition (it used to skip its allowlist for self-contained inputs).
        auto bad_image_json = edit_json(bad_extension_json, [](auto* d, auto* root) {
            auto* images = yyjson_mut_arr(d);
            auto* image = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, image, "uri", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ9sAAAAASUVORK5CYII=");
            yyjson_mut_arr_append(images, image);
            yyjson_mut_obj_add_val(d, root, "images", images);
        });
        auto bad_image_glb = dir / "unknown-self-contained-image.glb";
        write(bad_image_glb, glb(bad_image_json, base.bin));
        CHECK(run_cli({"build", bad_image_glb.generic_string(), "-o", (root / "bad-self-image.glb").generic_string()}) == 5);
        CHECK(!std::filesystem::exists(root / "bad-self-image.glb", ec));
        auto bad_image_doc = tool::read_document(bad_image_glb, {});
        CHECK(bad_image_doc);
        if (bad_image_doc) {
            auto packed = tool::pack_glb(*bad_image_doc);
            CHECK(!packed && packed.error().code == sub::error_code::io);
        }
        // (b) the same bad extension in a non-self-contained glTF still fails (arm F above), and its
        // merging writer reports the same typed error.
        auto unknown_doc = tool::read_document(dir / "unknown.gltf", {});
        CHECK(unknown_doc);
        if (unknown_doc) {
            auto packed = tool::pack_glb(*unknown_doc);
            CHECK(!packed && packed.error().code == sub::error_code::io);
        }
        // (c) M-1 strict decision: an unknown extension object that is NOT listed in extensionsUsed
        // must not bypass the same helper (non-conformant input, refused rather than guessed).
        auto unlisted_json = edit_json(base.json, [](auto* d, auto* root) {
            auto* extensions = yyjson_mut_obj(d);
            auto* binary = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, binary, "uri", "vendor-blob.bin");
            yyjson_mut_obj_add_val(d, extensions, "VENDOR_binary", binary);
            yyjson_mut_obj_add_val(d, root, "extensions", extensions);
        });
        auto unlisted_glb = dir / "unknown-unlisted.glb";
        write(unlisted_glb, glb(unlisted_json, base.bin));
        CHECK(run_cli({"build", unlisted_glb.generic_string(), "-o", (root / "unlisted.glb").generic_string()}) == 5);
        CHECK(!std::filesystem::exists(root / "unlisted.glb", ec));
        auto unlisted_doc = tool::read_document(unlisted_glb, {});
        CHECK(unlisted_doc);
        if (unlisted_doc) {
            auto packed = tool::pack_glb(*unlisted_doc);
            CHECK(!packed && packed.error().code == sub::error_code::io);
        }
        // Known relocation-safe extension names remain accepted for GLB output.
        auto known_json = edit_json(base.json, [](auto* d, auto* root) {
            auto* used = yyjson_mut_arr(d);
            yyjson_mut_arr_add_str(d, used, "KHR_mesh_quantization");
            yyjson_mut_obj_add_val(d, root, "extensionsUsed", used);
        });
        auto known_glb = dir / "known-extension.glb";
        write(known_glb, glb(known_json, base.bin));
        CHECK(run_cli({"build", known_glb.generic_string(), "-o", (root / "known-extension.glb").generic_string()}) == 0);
        // (d) positive control: a known-extension cooked GLB reuses its payload and re-serializes to
        // byte-identical bytes (the reviewer's 451dcd9a... archived-asset check is reproduced in the
        // task report; this is the same arm on the committed fixture).
        auto known_cooked_bytes = read_bytes(dir / "cooked.glb");
        CHECK(!known_cooked_bytes.empty());
        auto reused_known = root / "known-reused.glb";
        auto reuse_report = tool::convert_file(dir / "cooked.glb", reused_known, {});
        CHECK(reuse_report && reuse_report->reused == 1 && reuse_report->converted == 0);
        CHECK(read_bytes(reused_known) == known_cooked_bytes);

        // G. Buffer metadata that prevents a lossless GLB merge is preserved in a package.
        auto multiple_text = read_text(dir / "multiple.gltf");
        auto metadata_text = edit_json(as_sub_string(multiple_text), [](auto* d, auto* root) {
            auto* second = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "buffers"), 1);
            yyjson_mut_obj_add_str(d, second, "name", "second-buffer");
        });
        auto metadata_input = dir / "metadata.gltf";
        write_text(metadata_input, metadata_text);
        CHECK(run_cli({"build", metadata_input.generic_string(), "-o", (root / "metadata.glb").generic_string()}) == 5);
        CHECK(run_cli({"build", metadata_input.generic_string(), "-o", (root / "metadata-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto output_text = read_text(tool::package_detail::package_json_path(root / "metadata-out"));
            auto* output_root = yyjson_read(output_text.data(), output_text.size(), 0);
            CHECK(output_root);
            if (output_root) {
                auto* second = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(output_root), "buffers"), 1);
                auto* name = yyjson_obj_get(second, "name");
                CHECK(yyjson_is_str(name) && std::string_view(yyjson_get_str(name)) == "second-buffer");
            }
            yyjson_doc_free(output_root);
            CHECK(run_cli({"validate", tool::package_detail::package_json_path(root / "metadata-out").generic_string(), "--full"}) == 0);
        }
        // H. Package directory names with spaces exercise percent-encoded URIs.
        CHECK(run_cli({"build", input_glb, "-o", (root / "out package.gltf").generic_string(), "--format", "gltf"}) == 0);
        {
            auto spaced = root / "out package";
            CHECK(directory_files(spaced) == (std::vector<std::string>{"out package.bin", "out package.gltf"}));
            auto output_text = read_text(spaced / "out package.gltf");
            auto* output_root = yyjson_read(output_text.data(), output_text.size(), 0);
            CHECK(output_root);
            if (output_root) {
                auto* uri = yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(output_root), "buffers")), "uri");
                CHECK(yyjson_is_str(uri) && std::string_view(yyjson_get_str(uri)) == "out%20package.bin");
                auto decoded = percent_decode(yyjson_get_str(uri));
                CHECK(decoded && *decoded == "out package.bin");
            }
            yyjson_doc_free(output_root);
            CHECK(run_cli({"validate", (spaced / "out package.gltf").generic_string(), "--full"}) == 0);
        }

        // I. Rich document: external buffers plus an animation, image and unknown extension.
        auto rich_source = root / "rich-src";
        std::filesystem::create_directory(rich_source, ec);
        CHECK(!ec);
        auto rich = write_rich_fixture(rich_source, base, png);
        auto rich_inputs_before = tree_hashes(rich_source);
        auto clean_text = edit_json(rich.json, [](auto*, auto* root) {
            yyjson_mut_obj_remove_key(root, "extensions");
            yyjson_mut_obj_remove_key(root, "extensionsUsed");
            // GLB packaging refuses per-buffer metadata; strip the fixture's names for the clean arm.
            auto* buffers = yyjson_mut_obj_get(root, "buffers");
            for (size_t i = 0; i < yyjson_mut_arr_size(buffers); ++i)
                yyjson_mut_obj_remove_key(yyjson_mut_arr_get(buffers, i), "name");
        });
        write_text(rich_source / "rich-clean.gltf", clean_text);
        CHECK(run_cli({"build", (rich_source / "rich-clean.gltf").generic_string(), "-o", (root / "rich-clean.glb").generic_string()}) == 0);
        auto clean_glb = tool::read_document(root / "rich-clean.glb", {});
        CHECK(clean_glb);
        auto expected_payload = clean_glb ? tool::payload_bytes(*clean_glb, {0, 0}) : sub::result<sub::vector<std::byte>>(std::unexpected(sub::error{sub::error_code::invalid_input, "missing", {}, {}}));
        CHECK(expected_payload);
        CHECK(run_cli({"build", (rich_source / "rich.gltf").generic_string(), "-o", (root / "rich-out.gltf").generic_string(), "--format", "gltf"}) == 0);
        auto rich_out = root / "rich-out";
        CHECK(directory_files(rich_out) == (std::vector<std::string>{"buffer0.bin", "buffer1.bin", "image0.png", "rich-out.bin", "rich-out.gltf"}));
        // Source resources and the source document itself are never mutated.
        rich_inputs_before.erase(relative_generic(rich_source / "rich-clean.gltf", rich_source));
        auto rich_inputs_after = tree_hashes(rich_source);
        rich_inputs_after.erase(relative_generic(rich_source / "rich-clean.gltf", rich_source));
        CHECK(rich_inputs_before == rich_inputs_after);
        CHECK(hash_bytes(read_bytes(rich_out / "buffer0.bin")) == hash_bytes(rich.geometry));
        CHECK(hash_bytes(read_bytes(rich_out / "buffer1.bin")) == hash_bytes(rich.animation));
        CHECK(hash_bytes(read_bytes(rich_out / "image0.png")) == hash_bytes(rich.png));
        if (expected_payload)
            check_package_dom(std::string_view(rich.json.data(), rich.json.size()), read_text(rich_out / "rich-out.gltf"), rich_out, rich, *expected_payload);
        CHECK(run_cli({"validate", (rich_out / "rich-out.gltf").generic_string(), "--full"}) == 0);
        // The package is relocatable: copying the directory keeps it readable, because all
        // URIs are relative to the entry JSON.
        {
            auto relocated = root / "relocated";
            std::filesystem::copy(rich_out, relocated, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
            CHECK(run_cli({"validate", (relocated / "rich-out.gltf").generic_string(), "--full"}) == 0);
        }
        // The same input and output name in different directories is byte-for-byte identical.
        {
            std::filesystem::create_directory(root / "det-a", ec);
            std::filesystem::create_directory(root / "det-b", ec);
            CHECK(run_cli({"build", (rich_source / "rich.gltf").generic_string(), "-o", (root / "det-a" / "same.gltf").generic_string(), "--format", "gltf"}) == 0);
            CHECK(run_cli({"build", (rich_source / "rich.gltf").generic_string(), "-o", (root / "det-b" / "same.gltf").generic_string(), "--format", "gltf"}) == 0);
            auto a = tree_hashes(root / "det-a" / "same");
            auto b = tree_hashes(root / "det-b" / "same");
            CHECK(a == b);
        }

        // J. Test-only fault injection: partial writes, close failures and final
        //    publication failures must never publish a partial package.
        fault_injection_tests(root);
        // K. Cross-volume destination: staging must happen on the destination volume.
        cross_volume_tests(root, rich_source);
    }

    std::string relative_generic(tool::path const& file, tool::path const& root) {
        std::error_code ec;
        auto value = std::filesystem::relative(file, root, ec);
        return ec ? file.generic_string() : value.generic_string();
    }

    void fault_injection_tests(tool::path const& root) {
        std::error_code ec;
        auto source_file = root / "rich-src" / "rich.gltf";
        auto source = tool::read_document(source_file, {});
        CHECK(source);
        if (!source)
            return;
        auto target = root / "faults" / "out";
        std::filesystem::create_directories(target.parent_path(), ec);
        CHECK(!ec);
        CHECK(tool::package_detail::write_gltf_package_impl(*source, target, false, nullptr));
        auto prior = tree_hashes(target);
        CHECK(!prior.empty());
        // 1) A reported short write on the entry JSON aborts before publication.
        {
            injected_ops injection(fault_plan::write_mode::error_short, 8, "out.gltf");
            auto failed = tool::package_detail::write_gltf_package_impl(*source, target, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(tree_hashes(target) == prior);
            check_no_staging(root / "faults");
        }
        // 2) A silently short resource write is caught by the staged hash validation.
        {
            injected_ops injection(fault_plan::write_mode::silent_short, 4, "buffer0.bin");
            auto failed = tool::package_detail::write_gltf_package_impl(*source, target, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(tree_hashes(target) == prior);
            check_no_staging(root / "faults");
        }
        // 3) A close failure is reported and the prior package survives.
        {
            injected_ops injection(fault_plan::write_mode::close_failure, 0, "out.gltf");
            auto failed = tool::package_detail::write_gltf_package_impl(*source, target, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().context.find("close") != sub::string::npos);
            CHECK(tree_hashes(target) == prior);
            check_no_staging(root / "faults");
        }
        // 4) A final directory rename failure leaves an absent target absent.
        {
            auto absent = root / "faults" / "absent";
            injected_ops injection(fault_plan::write_mode::normal, 0, {}, 1);
            auto failed = tool::package_detail::write_gltf_package_impl(*source, absent, false, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(!std::filesystem::exists(absent, ec));
            check_no_staging(root / "faults");
        }
        // 5) In --overwrite mode a failure after the old target was moved aside restores it.
        {
            injected_ops injection(fault_plan::write_mode::normal, 0, {}, 2);
            auto failed = tool::package_detail::write_gltf_package_impl(*source, target, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(injection.plan.rename_calls == 3); // old aside, failed publish, rollback
            CHECK(tree_hashes(target) == prior);
            check_no_staging(root / "faults");
        }
        // 6) GLB path: reported short write, close failure and publish failure all keep the
        //    previous target bytes and clean the staging file.
        auto glb_bytes = read_bytes(root.parent_path() / "cooked.glb");
        CHECK(!glb_bytes.empty());
        auto glb_target = root / "faults" / "prior.glb";
        auto old_glb = std::string("previous staged bytes");
        write_text(glb_target, old_glb);
        auto old_glb_bytes = read_bytes(glb_target);
        {
            injected_ops injection(fault_plan::write_mode::error_short, 8, "artifact");
            auto failed = tool::package_detail::write_bytes_staged_impl(glb_target, glb_bytes, true, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(same_bytes(read_bytes(glb_target), old_glb_bytes));
            check_no_staging(root / "faults");
        }
        {
            injected_ops injection(fault_plan::write_mode::silent_short, 8, "artifact");
            auto failed = tool::package_detail::write_bytes_staged_impl(glb_target, glb_bytes, true, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::invalid_input); // staged GLB failed full validation
            CHECK(same_bytes(read_bytes(glb_target), old_glb_bytes));
            check_no_staging(root / "faults");
        }
        {
            injected_ops injection(fault_plan::write_mode::close_failure, 0, "artifact");
            auto failed = tool::package_detail::write_bytes_staged_impl(glb_target, glb_bytes, true, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().context.find("close") != sub::string::npos);
            CHECK(same_bytes(read_bytes(glb_target), old_glb_bytes));
            check_no_staging(root / "faults");
        }
        {
            injected_ops injection(fault_plan::write_mode::normal, 0, {}, 1);
            auto failed = tool::package_detail::write_bytes_staged_impl(glb_target, glb_bytes, true, true, &injection.ops);
            CHECK(!failed);
            if (!failed)
                CHECK(failed.error().code == sub::error_code::io);
            CHECK(same_bytes(read_bytes(glb_target), old_glb_bytes));
            check_no_staging(root / "faults");
        }
    }

    std::optional<tool::path> other_writable_volume(tool::path const& directory, std::string& last_error) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(directory, ec);
        if (ec) {
            last_error = ec.message();
            return std::nullopt;
        }
        auto current_root = absolute.root_name();
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (char letter = 'C'; letter <= 'Z'; ++letter) {
            tool::path root = tool::path(std::string(1, letter) + ":\\");
            if (root.root_name() == current_root)
                continue;
            if (!std::filesystem::exists(root, ec) || ec) {
                ec.clear();
                continue;
            }
            std::array<tool::path, 2> candidates{root, root / "deren-assets"};
            for (auto const& candidate : candidates) {
                if (!std::filesystem::is_directory(candidate, ec) || ec) {
                    ec.clear();
                    continue;
                }
                auto probe = candidate / tool::path(deren::utility::vessel::format("deren-subtilis-volume-probe-{}", stamp).c_str());
                if (std::filesystem::create_directory(probe, ec) && !ec) {
                    std::error_code ignored;
                    std::filesystem::remove(probe, ignored);
                    return candidate;
                }
                last_error = ec.message();
                ec.clear();
            }
        }
        return std::nullopt;
    }

    void cross_volume_tests(tool::path const& root, tool::path const& rich_source) {
        std::string probe_error;
        auto other = other_writable_volume(root, probe_error);
        if (!other) {
            deren::vk_test::write_line("[subtilis_converter] NOTE: cross-volume glTF package check SKIPPED (no second writable volume available to this process; last probe error: '{}') - same-volume staging/publish semantics are covered by the fault-injection tests", probe_error);
            CHECK(true);
            return;
        }
        auto source = tool::read_document(rich_source / "rich.gltf", {});
        CHECK(source);
        if (!source) {
            deren::vk_test::write_line("[subtilis_converter] NOTE: cross-volume glTF package check SKIPPED (source unreadable)");
            return;
        }
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto base = *other / tool::path(deren::utility::vessel::format("deren-subtilis-cross-{}", stamp).c_str());
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        CHECK(!ec);
        if (ec)
            return;
        auto target = base / "remote";
        fault_plan plan; // pass-through sink that records every staged path
        tool::package_detail::sink_ops ops;
        ops.open = &fault_open;
        ops.context = &plan;
        auto written = tool::package_detail::write_gltf_package_impl(*source, target, false, &ops);
        CHECK(written);
        CHECK(!plan.opened.empty());
        auto destination_root = std::filesystem::absolute(target, ec).root_name();
        size_t staged_on_destination = 0;
        for (auto const& file : plan.opened) {
            auto absolute = std::filesystem::absolute(file, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            auto volume = absolute.root_name();
            bool inside_staging = absolute.parent_path().filename().string().starts_with(".subtilis-");
            bool within_destination = tool::package_detail::path_inside(absolute, base);
            CHECK(volume == destination_root);
            CHECK(inside_staging);
            CHECK(within_destination);
            if (volume == destination_root && inside_staging && within_destination)
                ++staged_on_destination;
        }
        CHECK(staged_on_destination == plan.opened.size());
        auto json = tool::package_detail::package_json_path(target).generic_string();
        CHECK(run_cli({"validate", json, "--full"}) == 0);
        // Destination bytes hash back to the source resources even across volumes.
        CHECK(hash_bytes(read_bytes(target / "buffer0.bin")) == hash_bytes(read_bytes(rich_source / "geometry data.bin")));
        if (written)
            deren::vk_test::write_line("[subtilis_converter] NOTE: cross-volume glTF package check PERFORMED (source on {}, destination/staging on {})", std::filesystem::absolute(rich_source, ec).root_name().string(), destination_root.string());
        std::filesystem::remove_all(base, ec);
        CHECK(!ec);
    }
} // namespace
int main() {
    CHECK(!tool::package_detail::glb_length(UINT64_MAX, 0, {}));
    CHECK(!tool::package_detail::glb_length(20, (uint64_t(1) << 32) + 1, {}));
    CHECK(!tool::package_detail::glb_length(UINT32_MAX - 22u, 0, {}));
    auto boundary = tool::package_detail::glb_length(UINT32_MAX - 23u, 0, {});
    CHECK(boundary && *boundary == UINT32_MAX - 3u);
    sub::limits tiny_limit;
    tiny_limit.max_input_bytes = 32;
    CHECK(!tool::package_detail::glb_length(20, 0, tiny_limit));
    auto dir = std::filesystem::current_path() / tool::path(deren::utility::vessel::format("subtilis-converter-{}", std::chrono::steady_clock::now().time_since_epoch().count()).c_str());
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    CHECK(!ec);
    auto f = make_fixture(subtilis_fixture::cross_dag().source);
    write(dir / "input.glb", glb(f.json, f.bin));
    auto input = (dir / "input.glb").generic_string(), cooked = (dir / "cooked.glb").generic_string(), report = (dir / "report.json").generic_string();
    std::array<std::string_view, 8> build_args{"build", input, "-o", cooked, "--threads", "1", "--report", report};
    auto parsed = tool::parse_arguments(build_args);
    CHECK(parsed);
    if (!parsed) {
        std::filesystem::remove_all(dir, ec);
        return deren::vk_test::finish("subtilis_converter");
    }
    CHECK(tool::run(*parsed) == 0);
    CHECK(std::filesystem::exists(dir / "report.json"));
    {
        std::ifstream in(dir / "report.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            auto* root = yyjson_doc_get_root(r);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "converted")) == 1);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "skipped")) == 0);
            auto* row = yyjson_arr_get_first(yyjson_obj_get(root, "primitives"));
            CHECK(yyjson_get_uint(yyjson_obj_get(row, "sourceTriangles")) == 24);
            yyjson_doc_free(r);
        }
    }
    std::array<std::string_view, 2> inspect_args{"inspect", cooked};
    auto inspect = tool::parse_arguments(inspect_args);
    CHECK(inspect && tool::run(*inspect) == 0);
    std::array<std::string_view, 3> validate_args{"validate", cooked, "--full"};
    auto validate = tool::parse_arguments(validate_args);
    CHECK(validate && tool::run(*validate) == 0);
    auto doc = tool::read_document(dir / "cooked.glb", {});
    CHECK(doc);
    if (doc) {
        auto raw = tool::payload_bytes(*doc, {0, 0});
        CHECK(raw);
        if (raw) {
            auto p = sub::decode(*raw, {});
            CHECK(p && !p->groups.empty());
        }
    }
    unsigned n = 0;
    for (auto threshold : {"0", "0.01", "100"}) {
        auto out = (dir / tool::path(deren::utility::vessel::format("cut-{}.glb", n++).c_str())).generic_string();
        std::array<std::string_view, 6> cut_args{"export-cut", cooked, "--error", threshold, "-o", out};
        auto cut = tool::parse_arguments(cut_args);
        CHECK(cut && tool::run(*cut) == 0);
        auto preview = tool::read_document(tool::path(out), {});
        CHECK(preview);
        if (preview) {
            auto json = tool::serialize_json(*preview);
            CHECK(json && json->find("DEREN_subtilis") == sub::string::npos);
            auto geometry = tool::extract_geometry(*preview, {0, 0});
            CHECK(geometry);
            if (geometry && doc) {
                auto raw = tool::payload_bytes(*doc, {0, 0});
                auto source = tool::extract_geometry(*doc, {0, 0});
                CHECK(raw && source);
                if (raw && source) {
                    auto p = sub::decode(*raw, {});
                    CHECK(p);
                    if (p) {
                        auto a = sub::make_asset(std::move(*source), std::move(*p), {});
                        CHECK(a);
                        if (a) {
                            sub::cut_parameters parameters;
                            parameters.threshold = std::strtof(threshold, nullptr);
                            auto selected = sub::select_cut(*a, parameters);
                            CHECK(selected);
                            sub::vector<uint32_t> expected;
                            if (selected)
                                for (auto id : *selected) {
                                    auto const& c = a->data().clusters[id];
                                    auto window = std::span(a->data().indices).subspan(c.first_index, c.index_count);
                                    expected.insert(expected.end(), window.begin(), window.end());
                                }
                            CHECK(geometry->indices == expected);
                            CHECK(geometry->positions == a->geometry().positions);
                            CHECK(geometry->normals == a->geometry().normals);
                            CHECK(geometry->uv0 == a->geometry().uv0);
                        }
                    }
                }
            }
        }
    }
    auto skipped = f;
    replace(skipped.json, "\"NORMAL\":1,", "");
    write(dir / "ineligible.glb", glb(skipped.json, skipped.bin));
    auto ineligible = (dir / "ineligible.glb").generic_string();
    auto missing_out = (dir / "ineligible-cooked.glb").generic_string();
    std::array<std::string_view, 4> no_targets{"build", ineligible, "-o", missing_out};
    auto no_targets_command = tool::parse_arguments(no_targets);
    CHECK(no_targets_command && tool::run(*no_targets_command) == 4);
    CHECK(!std::filesystem::exists(dir / "ineligible-cooked.glb"));
    auto mixed_json = edit_json(f.json, [](auto* d, auto* root) {
        auto* primitives = yyjson_mut_obj_get(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "meshes")), "primitives");
        auto* skipped = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get_first(primitives));
        yyjson_mut_obj_remove_key(yyjson_mut_obj_get(skipped, "attributes"), "NORMAL");
        yyjson_mut_arr_append(primitives, skipped);
    });
    write(dir / "mixed.glb", glb(mixed_json, f.bin));
    auto mixed_input = (dir / "mixed.glb").generic_string();
    auto strict_output = (dir / "strict.glb").generic_string();
    std::array<std::string_view, 5> strict_args{"build", mixed_input, "-o", strict_output, "--strict"};
    auto strict_command = tool::parse_arguments(strict_args);
    CHECK(strict_command && tool::run(*strict_command) == 4);
    CHECK(!std::filesystem::exists(dir / "strict.glb"));
    auto partial_report = (dir / "partial.json").generic_string();
    std::array<std::string_view, 6> partial_args{"build", mixed_input, "-o", strict_output, "--report", partial_report};
    auto partial_command = tool::parse_arguments(partial_args);
    CHECK(partial_command && tool::run(*partial_command) == 0);
    {
        std::ifstream in(dir / "partial.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "skipped")) == 1);
            yyjson_doc_free(r);
        }
    }
    auto tiny = subtilis_fixture::cross_dag().source;
    tiny.indices.resize(3);
    auto tiny_fixture = make_fixture(tiny);
    write(dir / "tiny.glb", glb(tiny_fixture.json, tiny_fixture.bin));
    auto tiny_input = (dir / "tiny.glb").generic_string();
    auto tiny_output = (dir / "tiny-cooked.glb").generic_string();
    auto tiny_report = (dir / "tiny-report.json").generic_string();
    std::array<std::string_view, 6> tiny_args{"build", tiny_input, "-o", tiny_output, "--report", tiny_report};
    auto tiny_command = tool::parse_arguments(tiny_args);
    CHECK(tiny_command && tool::run(*tiny_command) == 0);
    {
        std::ifstream in(dir / "tiny-report.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "lod0Only")) == 1);
            yyjson_doc_free(r);
        }
    }
    std::array<std::string_view, 6> thread_args{"build", input, "-o", cooked, "--threads", "0"};
    CHECK(!tool::parse_arguments(thread_args));
    thread_args[5] = "-2";
    CHECK(!tool::parse_arguments(thread_args));
    thread_args[5] = "x";
    CHECK(!tool::parse_arguments(thread_args));
    thread_args[5] = "2";
    auto two_threads = tool::parse_arguments(thread_args);
    CHECK(two_threads && std::get<tool::build_command>(*two_threads).execution.threads == 2);
    thread_args[5] = "12";
    auto twelve_threads = tool::parse_arguments(thread_args);
    CHECK(twelve_threads && std::get<tool::build_command>(*twelve_threads).execution.threads == 12);
    auto unchanged = tool::read_document(dir / "input.glb", {});
    CHECK(unchanged);
    if (unchanged)
        CHECK(!tool::payload_bytes(*unchanged, {0, 0}));
    CHECK(tool::run(*parsed) == 5); // default refuses an existing target
    auto reused_output = (dir / "reused.glb").generic_string();
    auto reused_report = (dir / "reused.json").generic_string();
    std::array<std::string_view, 6> reuse_args{"build", cooked, "-o", reused_output, "--report", reused_report};
    auto reuse = tool::parse_arguments(reuse_args);
    CHECK(reuse && tool::run(*reuse) == 0);
    if (std::filesystem::exists(dir / "reused.glb")) {
        CHECK(std::filesystem::file_size(dir / "reused.glb") == std::filesystem::file_size(dir / "cooked.glb"));
        std::ifstream in(dir / "reused.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "reused")) == 1);
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "converted")) == 0);
            yyjson_doc_free(r);
        }
    }
    if (reuse) {
        auto different = *reuse;
        std::get<tool::build_command>(different).options.relative_error *= .5f;
        auto mismatch_output = dir / "mismatch.glb";
        std::get<tool::build_command>(different).output = mismatch_output;
        std::get<tool::build_command>(different).report.reset();
        CHECK(tool::run(different) == 4);
        CHECK(!std::filesystem::exists(mismatch_output));
    }
    auto read_bytes = [](auto const& file) {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        sub::vector<std::byte> out;
        auto bytes = std::as_bytes(std::span(text));
        out.assign(bytes.begin(), bytes.end());
        return out;
    };
    auto original_cooked = read_bytes(dir / "cooked.glb");
    if (doc) {
        auto raw = tool::payload_bytes(*doc, {0, 0});
        CHECK(raw);
        if (raw) {
            auto p = sub::decode(*raw, {});
            CHECK(p);
            if (p) {
                p->source_hash[0] ^= std::byte{1};
                auto bad_wire = sub::encode(*p, {});
                CHECK(bad_wire && bad_wire->size() == raw->size());
                if (bad_wire && bad_wire->size() == raw->size()) {
                    auto corrupt = original_cooked;
                    auto at = std::search(corrupt.begin(), corrupt.end(), raw->begin(), raw->end());
                    CHECK(at != corrupt.end());
                    if (at != corrupt.end())
                        std::copy(bad_wire->begin(), bad_wire->end(), at);
                    write(dir / "corrupt.glb", corrupt);
                    auto corrupt_input = (dir / "corrupt.glb").generic_string();
                    auto corrupt_output = (dir / "corrupt-output.glb").generic_string();
                    std::array<std::string_view, 4> args{"build", corrupt_input, "-o", corrupt_output};
                    auto command = tool::parse_arguments(args);
                    CHECK(command && tool::run(*command) == 4);
                    CHECK(!std::filesystem::exists(dir / "corrupt-output.glb"));
                }
            }
        }
    }
    CHECK(read_bytes(dir / "cooked.glb") == original_cooked);
    auto corrupt_source = read_bytes(dir / "corrupt.glb");
    if (!corrupt_source.empty()) {
        auto json_length = get(corrupt_source, 12);
        sub::string json(reinterpret_cast<char const*>(corrupt_source.data() + 20), json_length);
        auto hidden_json = edit_json(json, [](auto* d, auto* root) {
            auto* primitives = yyjson_mut_obj_get(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "meshes")), "primitives");
            auto* plain = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get_first(primitives));
            yyjson_mut_obj_remove_key(plain, "extensions");
            yyjson_mut_obj_remove_key(plain, "material");
            yyjson_mut_arr_append(primitives, plain);
            yyjson_mut_obj_add_str(d, yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "materials")), "alphaMode", "BLEND");
        });
        sub::vector<std::byte> bin(corrupt_source.begin() + 28 + json_length, corrupt_source.end());
        write(dir / "hidden-corrupt.glb", glb(hidden_json, bin));
        auto hidden_input = (dir / "hidden-corrupt.glb").generic_string(), hidden_output = (dir / "hidden-output.glb").generic_string();
        std::array<std::string_view, 4> hidden_args{"build", hidden_input, "-o", hidden_output};
        auto hidden_command = tool::parse_arguments(hidden_args);
        CHECK(hidden_command && tool::run(*hidden_command) == 4);
        CHECK(!std::filesystem::exists(dir / "hidden-output.glb"));
    }
    auto external_json = edit_json(f.json, [](auto* d, auto* root) {
        yyjson_mut_obj_remove_key(root, "extensions");
        yyjson_mut_obj_remove_key(root, "extensionsUsed");
        auto* buffer = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        yyjson_mut_obj_add_str(d, buffer, "uri", "geometry%20data.bin");
    });
    write(dir / "geometry data.bin", f.bin);
    {
        std::ofstream out(dir / "external.gltf", std::ios::binary);
        out << external_json.c_str();
    }
    auto external_input = (dir / "external.gltf").generic_string();
    auto external_output = (dir / "new-dir" / "external.glb").generic_string();
    std::array<std::string_view, 4> external_args{"build", external_input, "-o", external_output};
    auto external_command = tool::parse_arguments(external_args);
    CHECK(external_command && tool::run(*external_command) == 0);
    CHECK(read_bytes(dir / "geometry data.bin") == f.bin);
    auto packaged = tool::read_document(tool::path(external_output), {});
    CHECK(packaged);
    if (packaged) {
        auto geometry = tool::extract_geometry(*packaged, {0, 0});
        auto original = subtilis_fixture::cross_dag().source;
        CHECK(geometry && geometry->indices == original.indices && geometry->positions == original.positions);
    }
    sub::vector<std::byte> png;
    std::string_view png_hex = "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000b49444154789c636000020000050001a5f645400000000049454e44ae426082";
    auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < png_hex.size(); i += 2)
        png.push_back(std::byte(hex(png_hex[i]) * 16 + hex(png_hex[i + 1])));
    write(dir / "pixel.png", png);
    auto textured_json = edit_json(external_json, [](auto* d, auto* root) {
        auto* images = yyjson_mut_arr(d);
        auto* image = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, image, "uri", "pixel.png");
        yyjson_mut_arr_append(images, image);
        yyjson_mut_obj_add_val(d, root, "images", images);
    });
    {
        std::ofstream out(dir / "images.gltf", std::ios::binary);
        out << textured_json.c_str();
    }
    auto image_input = (dir / "images.gltf").generic_string();
    auto image_output = (dir / "images.glb").generic_string();
    std::array<std::string_view, 4> image_args{"build", image_input, "-o", image_output};
    auto image_command = tool::parse_arguments(image_args);
    CHECK(image_command && tool::run(*image_command) == 0);
    if (std::filesystem::exists(dir / "images.glb")) {
        auto bytes = read_bytes(dir / "images.glb");
        auto jl = get(bytes, 12);
        auto* j = yyjson_read(reinterpret_cast<char const*>(bytes.data() + 20), jl, 0);
        CHECK(j);
        if (j) {
            auto* root = yyjson_doc_get_root(j);
            auto* image = yyjson_arr_get_first(yyjson_obj_get(root, "images"));
            CHECK(!yyjson_obj_get(image, "uri"));
            CHECK(std::string_view(yyjson_get_str(yyjson_obj_get(image, "mimeType"))) == "image/png");
            auto* view = yyjson_arr_get(yyjson_obj_get(root, "bufferViews"), yyjson_get_uint(yyjson_obj_get(image, "bufferView")));
            auto at = 28 + jl + yyjson_get_uint(yyjson_obj_get(view, "byteOffset"));
            CHECK(yyjson_get_uint(yyjson_obj_get(view, "byteLength")) == png.size());
            CHECK(at + png.size() <= bytes.size());
            if (at + png.size() <= bytes.size())
                CHECK(std::equal(png.begin(), png.end(), bytes.begin() + at));
            yyjson_doc_free(j);
        }
    }
    auto base64 = [](std::span<std::byte const> bytes) {
        constexpr std::string_view chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        sub::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            uint32_t value = uint32_t(std::to_integer<unsigned>(bytes[i])) << 16;
            if (i + 1 < bytes.size())
                value |= uint32_t(std::to_integer<unsigned>(bytes[i + 1])) << 8;
            if (i + 2 < bytes.size())
                value |= std::to_integer<unsigned>(bytes[i + 2]);
            out += chars[(value >> 18) & 63];
            out += chars[(value >> 12) & 63];
            out += i + 1 < bytes.size() ? chars[(value >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? chars[value & 63] : '=';
        }
        return out;
    };
    auto image_uri = sub::string("data:image/png;base64,") + base64(png);
    auto embedded_json = edit_json(textured_json, [&](auto* d, auto* root) {
        auto* image = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "images"));
        yyjson_mut_obj_remove_key(image, "uri");
        yyjson_mut_obj_add_strcpy(d, image, "uri", image_uri.c_str());
        auto* buffer = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        auto uri = sub::string("data:application/octet-stream;base64,") + base64(f.bin);
        yyjson_mut_obj_remove_key(buffer, "uri");
        yyjson_mut_obj_add_strcpy(d, buffer, "uri", uri.c_str());
    });
    {
        std::ofstream out(dir / "data.gltf", std::ios::binary);
        out << embedded_json.c_str();
    }
    auto data_input = (dir / "data.gltf").generic_string();
    auto data_output = (dir / "data.glb").generic_string();
    std::array<std::string_view, 4> data_args{"build", data_input, "-o", data_output};
    auto data_command = tool::parse_arguments(data_args);
    CHECK(data_command && tool::run(*data_command) == 0);
    // Two independent original buffers must merge without changing accessor IDs.
    auto original_source = subtilis_fixture::cross_dag().source;
    size_t split = original_source.positions.size() * 32;
    write(dir / "attributes.bin", sub::vector<std::byte>(f.bin.begin(), f.bin.begin() + split));
    write(dir / "indices.bin", sub::vector<std::byte>(f.bin.begin() + split, f.bin.end()));
    auto multiple_json = edit_json(external_json, [&](auto* d, auto* root) {
        auto* buffers = yyjson_mut_obj_get(root, "buffers");
        auto* first = yyjson_mut_arr_get_first(buffers);
        yyjson_mut_obj_remove_key(first, "uri");
        yyjson_mut_obj_add_str(d, first, "uri", "attributes.bin");
        yyjson_mut_set_uint(yyjson_mut_obj_get(first, "byteLength"), split);
        auto* second = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, second, "uri", "indices.bin");
        yyjson_mut_obj_add_uint(d, second, "byteLength", f.bin.size() - split);
        yyjson_mut_arr_append(buffers, second);
        auto* view = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "bufferViews"), 3);
        yyjson_mut_set_uint(yyjson_mut_obj_get(view, "buffer"), 1);
        yyjson_mut_set_uint(yyjson_mut_obj_get(view, "byteOffset"), 0);
    });
    {
        std::ofstream out(dir / "multiple.gltf", std::ios::binary);
        out << multiple_json.c_str();
    }
    auto multiple_input = (dir / "multiple.gltf").generic_string(), multiple_output = (dir / "multiple.glb").generic_string();
    std::array<std::string_view, 4> multiple_args{"build", multiple_input, "-o", multiple_output};
    auto multiple_command = tool::parse_arguments(multiple_args);
    CHECK(multiple_command && tool::run(*multiple_command) == 0);
    auto multiple = tool::read_document(dir / "multiple.glb", {});
    CHECK(multiple);
    if (multiple) {
        auto geometry = tool::extract_geometry(*multiple, {0, 0});
        CHECK(geometry && geometry->positions == original_source.positions && geometry->indices == original_source.indices);
    }
    auto unknown_json = edit_json(external_json, [](auto* d, auto* root) {
        auto* used = yyjson_mut_arr(d);
        yyjson_mut_arr_add_str(d, used, "VENDOR_binary");
        yyjson_mut_obj_add_val(d, root, "extensionsUsed", used);
    });
    {
        std::ofstream out(dir / "unknown.gltf", std::ios::binary);
        out << unknown_json.c_str();
    }
    auto unknown_input = (dir / "unknown.gltf").generic_string(), unknown_output = (dir / "unknown.glb").generic_string();
    std::array<std::string_view, 4> unknown_args{"build", unknown_input, "-o", unknown_output};
    auto unknown_command = tool::parse_arguments(unknown_args);
    CHECK(unknown_command && tool::run(*unknown_command) == 5);
    CHECK(!std::filesystem::exists(dir / "unknown.glb"));
    for (auto uri : {"geometry%GGdata.bin", "https://example.com/data.bin", "geometry%00data.bin", "data:application/octet-stream;base64,===="}) {
        auto bad_json = edit_json(external_json, [&](auto* d, auto* root) {auto* b=yyjson_mut_arr_get_first(yyjson_mut_obj_get(root,"buffers"));yyjson_mut_obj_remove_key(b,"uri");yyjson_mut_obj_add_strcpy(d,b,"uri",uri); });
        {
            std::ofstream out(dir / "bad-uri.gltf", std::ios::binary);
            out << bad_json.c_str();
        }
        CHECK(!tool::read_document(dir / "bad-uri.gltf", {}));
    }
    auto overwrite_output = (dir / "overwrite.glb").generic_string();
    {
        std::ofstream old(dir / "overwrite.glb");
        old << "old target";
    }
    std::array<std::string_view, 5> overwrite_args{"build", input, "-o", overwrite_output, "--overwrite"};
    auto overwrite = tool::parse_arguments(overwrite_args);
    CHECK(overwrite && tool::run(*overwrite) == 0);
    CHECK(tool::read_document(dir / "overwrite.glb", {}));
    std::array<std::string_view, 5> inplace_args{"build", cooked, "-o", cooked, "--overwrite"};
    auto inplace = tool::parse_arguments(inplace_args);
    CHECK(inplace && tool::run(*inplace) == 5);
    CHECK(read_bytes(dir / "cooked.glb") == original_cooked);
    auto directory_output = (dir / "target-directory.glb").generic_string();
    std::filesystem::create_directory(dir / "target-directory.glb", ec);
    {
        std::ofstream marker(dir / "target-directory.glb" / "keep");
        marker << "preserve";
    }
    std::array<std::string_view, 5> directory_args{"build", input, "-o", directory_output, "--overwrite"};
    auto directory_command = tool::parse_arguments(directory_args);
    CHECK(directory_command && tool::run(*directory_command) == 5);
    CHECK(std::filesystem::exists(dir / "target-directory.glb" / "keep"));
    for (auto const& entry : std::filesystem::directory_iterator(dir))
        CHECK(!entry.path().filename().string().starts_with(".subtilis-"));
    auto api = tool::convert_file(dir / "input.glb", dir / "api.glb", {});
    CHECK(api && api->converted == 1 && api->reused == 0 && api->skipped == 0);
    auto api_reuse = tool::convert_file(dir / "api.glb", dir / "api-reused.glb", {});
    CHECK(api_reuse && api_reuse->converted == 0 && api_reuse->reused == 1);
    auto alias_report = input;
    auto alias_output = (dir / "alias-output.glb").generic_string();
    std::array<std::string_view, 7> alias_args{"build", input, "-o", alias_output, "--report", alias_report, "--overwrite"};
    auto alias = tool::parse_arguments(alias_args);
    CHECK(alias && tool::run(*alias) == 5);
    CHECK(!std::filesystem::exists(dir / "alias-output.glb"));
    auto excessive_json = edit_json(external_json, [](auto*, auto* root) {
        auto* b = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        yyjson_mut_set_uint(yyjson_mut_obj_get(b, "byteLength"), UINT64_MAX);
    });
    {
        std::ofstream out(dir / "excessive.gltf");
        out << excessive_json.c_str();
    }
    auto excessive = tool::read_document(dir / "excessive.gltf", {});
    CHECK(!excessive && excessive.error().code == sub::error_code::capacity);
#ifdef _WIN32
    auto readonly_output = (dir / "readonly.glb").generic_string();
    {
        std::ofstream out(dir / "readonly.glb");
        out << "keep original";
    }
    auto readonly_bytes = read_bytes(dir / "readonly.glb");
    std::filesystem::permissions(dir / "readonly.glb", std::filesystem::perms::owner_read, ec);
    CHECK(!ec);
    std::array<std::string_view, 5> readonly_args{"build", input, "-o", readonly_output, "--overwrite"};
    auto readonly_command = tool::parse_arguments(readonly_args);
    CHECK(readonly_command && tool::run(*readonly_command) == 5);
    CHECK(read_bytes(dir / "readonly.glb") == readonly_bytes);
    std::filesystem::permissions(dir / "readonly.glb", std::filesystem::perms::owner_all, ec);
    CHECK(!ec);
#endif
    directory_package_tests(dir, f, png);
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
    return deren::vk_test::finish("subtilis_converter");
}
