#include "document_internal.hpp"

#include <fastgltf/base64.hpp>

namespace deren::tools::subtilis {
    namespace {
        // Extensions whose binary/external-reference semantics the tool models well enough to
        // re-serialize a GLB at a new location. Anything else could carry a URI that becomes
        // dangling after the relocation, which the design explicitly refuses to guess.
        bool known_glb_extension(std::string_view name) {
            return name == "DEREN_subtilis" || name == "KHR_mesh_quantization" || name == "KHR_texture_transform" || name == "KHR_materials_unlit" || name == "KHR_lights_punctual";
        }
        result<void> check_relocatable_extensions(yyjson_mut_val* value) {
            if (!value)
                return {};
            if (yyjson_mut_is_obj(value)) {
                yyjson_mut_obj_iter iterator;
                yyjson_mut_obj_iter_init(value, &iterator);
                while (auto* key = yyjson_mut_obj_iter_next(&iterator)) {
                    auto* child = yyjson_mut_obj_iter_get_val(key);
                    std::string_view name(yyjson_mut_get_str(key));
                    if (name == "extras")
                        continue; // arbitrary application data, not glTF extension semantics
                    if (name == "extensionsUsed" || name == "extensionsRequired") {
                        if (!yyjson_mut_is_arr(child))
                            return fail("invalid extension list", asset::error_code::io);
                        for (size_t i = 0; i < yyjson_mut_arr_size(child); ++i) {
                            auto* entry = yyjson_mut_arr_get(child, i);
                            if (!yyjson_mut_is_str(entry))
                                return fail("invalid extension name", asset::error_code::io);
                            if (!known_glb_extension(std::string_view(yyjson_mut_get_str(entry))))
                                return fail("unknown extension prevents safe GLB relocation; use a glTF package", asset::error_code::io);
                        }
                        continue;
                    }
                    if (name == "extensions") {
                        if (!yyjson_mut_is_obj(child)) {
                            if (child && !yyjson_mut_is_null(child))
                                return fail("invalid extensions object", asset::error_code::io);
                            continue;
                        }
                        yyjson_mut_obj_iter extensions;
                        yyjson_mut_obj_iter_init(child, &extensions);
                        while (auto* extension = yyjson_mut_obj_iter_next(&extensions)) {
                            if (!known_glb_extension(std::string_view(yyjson_mut_get_str(extension))))
                                return fail("unknown extension prevents safe GLB relocation; use a glTF package", asset::error_code::io);
                            auto nested = check_relocatable_extensions(yyjson_mut_obj_iter_get_val(extension));
                            if (!nested)
                                return nested;
                        }
                        continue;
                    }
                    auto nested = check_relocatable_extensions(child);
                    if (!nested)
                        return nested;
                }
                return {};
            }
            if (yyjson_mut_is_arr(value)) {
                for (size_t i = 0; i < yyjson_mut_arr_size(value); ++i) {
                    auto nested = check_relocatable_extensions(yyjson_mut_arr_get(value, i));
                    if (!nested)
                        return nested;
                }
            }
            return {};
        }
    } // namespace
    // This is deliberately DOM-based (not parsed-array-based): a non-conformant input can carry an
    // extension object without listing it in extensionsUsed, and that must not bypass the rule.
    result<void> glb_relocation_safe(document const& d) {
        if (!d.state)
            return fail("empty document", asset::error_code::io);
        return check_relocatable_extensions(root(d));
    }
    result<vector<byte>> read_local_resource(path const& parent, std::string_view uri, uint64_t max_bytes) {
        if (uri.starts_with("data:")) {
            auto comma = uri.find(',');
            if (comma == std::string_view::npos || !uri.substr(0, comma).ends_with(";base64"))
                return fail("only base64 data URI resources are supported");
            auto encoded = uri.substr(comma + 1);
            if (encoded.size() < 4 || encoded.size() % 4)
                return fail("invalid base64 resource length");
            auto padding = fastgltf::base64::getPadding(encoded);
            for (size_t i = 0; i < encoded.size(); ++i) {
                char c = encoded[i];
                bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
                if (!alpha && !(c == '=' && i >= encoded.size() - padding))
                    return fail("invalid base64 resource character");
            }
            auto size = fastgltf::base64::getOutputSize(encoded.size(), padding);
            if (size > max_bytes)
                return fail("data URI byte limit", asset::error_code::capacity);
            vector<byte> bytes(size);
            fastgltf::base64::decode_inplace(encoded, reinterpret_cast<uint8_t*>(bytes.data()), padding);
            return bytes;
        }
        string decoded;
        auto hex = [](char c) -> int { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; };
        for (size_t i = 0; i < uri.size(); ++i) {
            char c = uri[i];
            if (c == '%') {
                if (uri.size() - i < 3 || hex(uri[i + 1]) < 0 || hex(uri[i + 2]) < 0)
                    return fail("invalid URI escape");
                c = static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
                i += 2;
            }
            if (!c || c == ':' || c == '?' || c == '#' || c == '\\')
                return fail("resource URI must be a local relative path without query/fragment");
            decoded += c;
        }
        auto relative = path(std::u8string(reinterpret_cast<char8_t const*>(decoded.data()), decoded.size()));
        if (relative.empty() || relative.is_absolute() || relative.has_root_path())
            return fail("resource URI must be relative");
        std::error_code ec;
        auto file = parent / relative;
        auto size = std::filesystem::file_size(file, ec);
        if (ec)
            return fail("cannot open local resource", asset::error_code::io);
        if (size > max_bytes || size > SIZE_MAX || size > uint64_t(std::numeric_limits<std::streamsize>::max()))
            return fail("resource byte limit", asset::error_code::capacity);
        vector<byte> bytes(static_cast<size_t>(size));
        std::ifstream in(file, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return fail("cannot read local resource", asset::error_code::io);
        return bytes;
    }
    std::span<byte const> buffer_bytes(document const& d, uint32_t id) {
        if (!d.state)
            return {};
        if (id == d.state->append_buffer)
            return d.state->binary;
        if (id >= d.state->parsed.buffers.size())
            return {};
        return std::visit([](auto const& data) -> std::span<byte const> {
            using T = std::remove_cvref_t<decltype(data)>;
            if constexpr (std::is_same_v<T, fastgltf::sources::Array> || std::is_same_v<T, fastgltf::sources::Vector> || std::is_same_v<T, fastgltf::sources::ByteView>)
                return {data.bytes.data(), data.bytes.size()};
            else
                return {};
        },
                          d.state->parsed.buffers[id].data);
    }
    result<void> ensure_append_buffer(document& d) {
        if (d.state->append_buffer != asset::none)
            return {};
        auto* buffers = member(root(d), "buffers");
        if (!yyjson_mut_is_arr(buffers) || yyjson_mut_arr_size(buffers) >= d.state->limits.max_records || yyjson_mut_arr_size(buffers) >= UINT32_MAX)
            return fail("append buffer record limit", asset::error_code::capacity);
        d.state->append_buffer = static_cast<uint32_t>(yyjson_mut_arr_size(buffers));
        auto* buffer = yyjson_mut_obj(d.state->json);
        yyjson_mut_obj_add_uint(d.state->json, buffer, "byteLength", 0);
        yyjson_mut_arr_append(buffers, buffer);
        return {};
    }
    result<vector<byte>> pack_glb(document const& d) {
        // Merging GLB writer: the same relocation-safety precondition as the self-contained fast
        // path, so the two writers cannot disagree about an input.
        auto safe = glb_relocation_safe(d);
        if (!safe)
            return std::unexpected(safe.error());
        auto allocator = json_allocator();
        auto* copied = yyjson_mut_doc_mut_copy(d.state->json, &allocator);
        std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owner(copied, yyjson_mut_doc_free);
        if (!copied)
            return fail("cannot copy packaging document");
        auto* r = yyjson_mut_doc_get_root(copied);
        auto* buffers = member(r, "buffers");
        vector<uint64_t> offsets;
        vector<byte> merged;
        for (size_t i = 0; i < yyjson_mut_arr_size(buffers); ++i) {
            auto* original = yyjson_mut_arr_get(buffers, i);
            if (i && yyjson_mut_obj_size(original) > (member(original, "uri") ? 2u : 1u))
                return fail("buffer metadata prevents lossless GLB merge; use a glTF package");
            auto data = buffer_bytes(d, static_cast<uint32_t>(i));
            auto* length = member(original, "byteLength");
            if (!yyjson_mut_is_uint(length) || yyjson_mut_get_uint(length) > data.size())
                return fail("missing buffer data");
            while (merged.size() % 8)
                merged.push_back(byte{0});
            offsets.push_back(merged.size());
            if (merged.size() > d.state->limits.max_input_bytes || data.size() > d.state->limits.max_input_bytes - merged.size() || uint64_t(data.size()) + merged.size() > UINT32_MAX)
                return fail("merged GLB byte limit", asset::error_code::capacity);
            merged.insert(merged.end(), data.begin(), data.end());
        }
        auto* views = member(r, "bufferViews");
        for (size_t i = 0; i < yyjson_mut_arr_size(views); ++i) {
            auto* view = yyjson_mut_arr_get(views, i);
            auto* buffer = member(view, "buffer");
            auto* offset = member(view, "byteOffset");
            if (!yyjson_mut_is_uint(buffer) || yyjson_mut_get_uint(buffer) >= offsets.size() || (offset && !yyjson_mut_is_uint(offset)))
                return fail("invalid buffer relocation");
            uint64_t start = offset ? yyjson_mut_get_uint(offset) : 0;
            auto base = offsets[yyjson_mut_get_uint(buffer)];
            if (start > UINT32_MAX - base)
                return fail("GLB offset overflow", asset::error_code::capacity);
            yyjson_mut_set_uint(buffer, 0);
            yyjson_mut_obj_remove_key(view, "byteOffset");
            yyjson_mut_obj_add_uint(copied, view, "byteOffset", base + start);
        }
        auto* images = member(r, "images");
        for (size_t i = 0; i < yyjson_mut_arr_size(images); ++i) {
            auto* image = yyjson_mut_arr_get(images, i);
            auto* uri = member(image, "uri");
            if (!uri)
                continue;
            if (i >= d.state->image_resources.size())
                return fail("missing image snapshot");
            auto const& data = d.state->image_resources[i];
            auto starts = [&](std::initializer_list<unsigned> signature) { if(data.size()<signature.size())return false;size_t n=0;for(auto c:signature)if(std::to_integer<unsigned>(data[n++])!=c)return false;return true; };
            char const* mime = starts({137, 80, 78, 71, 13, 10, 26, 10}) ? "image/png" : starts({255, 216, 255}) ? "image/jpeg"
                                                                                                                 : nullptr;
            if (!mime)
                return fail("cannot embed image: supported formats are PNG and JPEG");
            auto* declared = member(image, "mimeType");
            if (declared && (!yyjson_mut_is_str(declared) || std::string_view(yyjson_mut_get_str(declared)) != mime))
                return fail("image MIME/signature mismatch");
            auto value = std::string_view(yyjson_mut_get_str(uri));
            if (value.starts_with("data:") && value.substr(5, value.find(';') - 5) != mime)
                return fail("image data URI MIME/signature mismatch");
            while (merged.size() % 4)
                merged.push_back(byte{0});
            if (merged.size() > d.state->limits.max_input_bytes || data.size() > d.state->limits.max_input_bytes - merged.size() || uint64_t(merged.size()) + data.size() > UINT32_MAX)
                return fail("image embedding byte limit", asset::error_code::capacity);
            auto* view = yyjson_mut_obj(copied);
            yyjson_mut_obj_add_uint(copied, view, "buffer", 0);
            yyjson_mut_obj_add_uint(copied, view, "byteOffset", merged.size());
            yyjson_mut_obj_add_uint(copied, view, "byteLength", data.size());
            if (!yyjson_mut_is_arr(views))
                return fail("missing bufferViews");
            yyjson_mut_obj_add_uint(copied, image, "bufferView", yyjson_mut_arr_size(views));
            yyjson_mut_arr_append(views, view);
            yyjson_mut_obj_remove_key(image, "uri");
            yyjson_mut_obj_remove_key(image, "mimeType");
            yyjson_mut_obj_add_str(copied, image, "mimeType", mime);
            merged.insert(merged.end(), data.begin(), data.end());
        }
        auto* single = yyjson_mut_val_mut_copy(copied, yyjson_mut_arr_get(buffers, 0));
        yyjson_mut_obj_remove_key(single, "uri");
        yyjson_mut_set_uint(member(single, "byteLength"), merged.size());
        yyjson_mut_arr_clear(buffers);
        yyjson_mut_arr_append(buffers, single);
        size_t size = 0;
        auto* text = yyjson_mut_write_opts(copied, 0, &allocator, &size, nullptr);
        if (!text)
            return fail("cannot serialize package JSON");
        auto packed = make_glb(std::string_view(text, size), merged, d.state->limits);
        utility::memory::deallocate(text);
        return packed;
    }
} // namespace deren::tools::subtilis
