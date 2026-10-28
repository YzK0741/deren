#include "document_internal.hpp"
#include "package_limits.hpp"

namespace deren::tools::subtilis {
    document::document() = default;
    document::~document() = default;
    document::document(document&&) noexcept = default;
    document& document::operator=(document&&) noexcept = default;
    void document_deleter::operator()(document_state* p) const noexcept {
        if (p) {
            std::destroy_at(p);
            utility::memory::deallocate(p);
        }
    }
    result<vector<byte>> make_glb(std::string_view json, std::span<byte const> bin, asset::limits const& lim) {
        auto checked = package_detail::glb_length(json.size(), bin.size(), lim);
        if (!checked)
            return std::unexpected(checked.error());
        uint64_t j = (uint64_t(json.size()) + 3) & ~uint64_t(3), b = (uint64_t(bin.size()) + 3) & ~uint64_t(3);
        uint64_t total = *checked;
        vector<byte> bytes;
        bytes.reserve(static_cast<size_t>(total));
        put_u32(bytes, 0x46546c67);
        put_u32(bytes, 2);
        put_u32(bytes, static_cast<uint32_t>(total));
        put_u32(bytes, static_cast<uint32_t>(j));
        put_u32(bytes, 0x4e4f534a);
        auto chars = std::as_bytes(std::span(json));
        bytes.insert(bytes.end(), chars.begin(), chars.end());
        while (bytes.size() % 4)
            bytes.push_back(byte{' '});
        if (b) {
            put_u32(bytes, static_cast<uint32_t>(b));
            put_u32(bytes, 0x004e4942);
            bytes.insert(bytes.end(), bin.begin(), bin.end());
            while (bytes.size() % 4)
                bytes.push_back(byte{0});
        }
        return bytes;
    }
    result<string> serialize_json(document const& d) {
        if (!d.state)
            return fail("empty document");
        auto allocator = json_allocator();
        size_t length = 0;
        auto* text = yyjson_mut_write_opts(d.state->json, 0, &allocator, &length, nullptr);
        if (!text)
            return fail("cannot serialize JSON");
        string out(text, length);
        utility::memory::deallocate(text);
        return out;
    }
    result<vector<byte>> serialize_embedded_glb(document const& d) {
        if (!d.state)
            return fail("empty document");
        // Shared GLB-relocation precondition (both the fast self-contained path and the merging
        // writer below were checked separately before; a self-contained GLB short-circuited past it).
        auto safe = glb_relocation_safe(d);
        if (!safe)
            return std::unexpected(safe.error());
        if (!d.state->self_contained)
            return pack_glb(d);
        auto* images = member(root(d), "images");
        for (size_t i = 0; i < yyjson_mut_arr_size(images); ++i)
            if (member(yyjson_mut_arr_get(images, i), "uri"))
                return pack_glb(d);
        auto json = serialize_json(d);
        if (!json)
            return std::unexpected(json.error());
        return make_glb(*json, d.state->binary, d.state->limits);
    }
    result<document> read_document(path const& file, asset::limits const& lim) {
        std::error_code ec;
        auto length = std::filesystem::file_size(file, ec);
        if (ec)
            return fail("cannot open input file", asset::error_code::io);
        if (length > lim.max_input_bytes || length > SIZE_MAX || length > uint64_t(std::numeric_limits<std::streamsize>::max()))
            return fail("input byte limit", asset::error_code::capacity);
        vector<byte> bytes(static_cast<size_t>(length));
        std::ifstream in(file, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return fail("cannot read input file", asset::error_code::io);
        std::span<byte const> json = bytes, bin;
        bool is_glb = bytes.size() >= 4 && get_u32(bytes, 0) == 0x46546c67;
        if (is_glb) {
            if (bytes.size() < 20 || get_u32(bytes, 4) != 2 || get_u32(bytes, 8) != bytes.size() || get_u32(bytes, 16) != 0x4e4f534a)
                return fail("invalid GLB header");
            auto json_length = get_u32(bytes, 12);
            if (json_length % 4 || json_length > bytes.size() - 20)
                return fail("invalid GLB JSON range");
            json = std::span(bytes).subspan(20, json_length);
            size_t at = 20 + json_length;
            if (at < bytes.size()) {
                if (bytes.size() - at < 8 || get_u32(bytes, at + 4) != 0x004e4942)
                    return fail("unsupported GLB chunk");
                auto bin_length = get_u32(bytes, at);
                if (bin_length % 4 || bin_length != bytes.size() - at - 8)
                    return fail("invalid GLB BIN range");
                bin = std::span(bytes).subspan(at + 8, bin_length);
            }
        }
        auto allocator = json_allocator();
        auto* original = yyjson_read_opts(reinterpret_cast<char*>(const_cast<byte*>(json.data())), json.size(), 0, &allocator, nullptr);
        if (!original)
            return fail("invalid UTF-8 JSON");
        auto* mutable_doc = yyjson_doc_mut_copy(original, &allocator);
        yyjson_doc_free(original);
        if (!mutable_doc)
            return fail("cannot copy document");
        document d;
        d.state.reset(std::construct_at(static_cast<document_state*>(utility::memory::allocate(sizeof(document_state), alignof(document_state)))));
        d.state->json = mutable_doc;
        d.state->source_path = file;
        d.state->limits = lim;
        d.state->binary.assign(bin.begin(), bin.end());
        if (!yyjson_mut_is_obj(root(d)))
            return fail("glTF root is not an object");
        auto* buffers = member(root(d), "buffers");
        uint64_t total = 0;
        if (yyjson_mut_arr_size(buffers) > lim.max_records)
            return fail("buffer record limit", asset::error_code::capacity);
        d.state->external_buffers.resize(yyjson_mut_arr_size(buffers));
        for (size_t i = 0; i < yyjson_mut_arr_size(buffers); ++i) {
            auto* buffer = yyjson_mut_arr_get(buffers, i);
            auto* n = member(buffer, "byteLength");
            if (!yyjson_mut_is_uint(n))
                return fail("invalid buffer length");
            auto size = yyjson_mut_get_uint(n);
            if (size > lim.max_input_bytes - total)
                return fail("decoded buffer byte limit", asset::error_code::capacity);
            total += size;
            if (auto* uri = member(buffer, "uri")) {
                if (!yyjson_mut_is_str(uri))
                    return fail("invalid buffer URI");
                auto value = std::string_view(yyjson_mut_get_str(uri), yyjson_mut_get_len(uri));
                {
                    auto resource = read_local_resource(file.parent_path(), value, lim.max_input_bytes - total + size);
                    if (!resource)
                        return std::unexpected(resource.error());
                    if (resource->size() < size)
                        return fail("buffer shorter than byteLength");
                    total += resource->size() - size;
                    d.state->external_buffers[i] = std::move(*resource);
                }
            }
        }
        auto* images = member(root(d), "images");
        if (yyjson_mut_arr_size(images) > lim.max_records)
            return fail("image record limit", asset::error_code::capacity);
        d.state->image_resources.resize(yyjson_mut_arr_size(images));
        for (size_t i = 0; i < yyjson_mut_arr_size(images); ++i)
            if (auto* uri = member(yyjson_mut_arr_get(images, i), "uri")) {
                if (!yyjson_mut_is_str(uri))
                    return fail("invalid image URI");
                auto bytes = read_local_resource(file.parent_path(), std::string_view(yyjson_mut_get_str(uri), yyjson_mut_get_len(uri)), lim.max_input_bytes - total);
                if (!bytes)
                    return std::unexpected(bytes.error());
                total += bytes->size();
                d.state->image_resources[i] = std::move(*bytes);
            }
        d.state->self_contained = is_glb && yyjson_mut_arr_size(buffers) <= 1 && !member(yyjson_mut_arr_get(buffers, 0), "uri");
        if (is_glb && yyjson_mut_arr_size(buffers) && !member(yyjson_mut_arr_get(buffers, 0), "uri"))
            d.state->append_buffer = 0;
        // Filter only our supported required extension in a parser-owned temporary copy.
        auto* original_required = member(root(d), "extensionsRequired");
        auto* used = member(root(d), "extensionsUsed");
        for (size_t i = 0; i < yyjson_mut_arr_size(original_required); ++i) {
            auto* name = yyjson_mut_arr_get(original_required, i);
            if (!yyjson_mut_is_str(name))
                return fail("required extension is not a string");
            bool found = false;
            for (size_t j = 0; j < yyjson_mut_arr_size(used); ++j) {
                auto* candidate = yyjson_mut_arr_get(used, j);
                if (yyjson_mut_is_str(candidate) && std::string_view(yyjson_mut_get_str(candidate)) == yyjson_mut_get_str(name))
                    found = true;
            }
            if (!found)
                return fail("required extension absent from extensionsUsed");
        }
        auto* parser_doc = yyjson_mut_val_mut_copy(mutable_doc, root(d));
        auto* required = member(parser_doc, "extensionsRequired");
        bool requires_subtilis = false;
        for (size_t i = yyjson_mut_arr_size(required); i > 0; --i) {
            auto* name = yyjson_mut_arr_get(required, i - 1);
            if (yyjson_mut_is_str(name) && std::string_view(yyjson_mut_get_str(name)) == "DEREN_subtilis") {
                requires_subtilis = true;
                yyjson_mut_arr_remove(required, i - 1);
            }
        }
        size_t parser_length = 0;
        auto* parser_json = yyjson_mut_val_write_opts(parser_doc, 0, &allocator, &parser_length, nullptr);
        if (!parser_json)
            return fail("cannot prepare parser document");
        vector<byte> parser_bytes;
        if (is_glb) {
            auto packed = make_glb(std::string_view(parser_json, parser_length), bin, lim);
            utility::memory::deallocate(parser_json);
            if (!packed)
                return std::unexpected(packed.error());
            parser_bytes = std::move(*packed);
        } else {
            auto raw = std::as_bytes(std::span(parser_json, parser_length));
            parser_bytes.assign(raw.begin(), raw.end());
            utility::memory::deallocate(parser_json);
        }
        auto input = fastgltf::GltfDataBuffer::FromBytes(parser_bytes.data(), parser_bytes.size());
        if (!input)
            return fail("cannot prepare glTF input");
        auto enabled = fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_texture_transform | fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_lights_punctual;
        fastgltf::Parser parser(enabled);
        auto parsed = parser.loadGltf(input.get(), file.parent_path());
        if (!parsed)
            return fail("glTF parser rejected input/required extension");
        d.state->parsed = std::move(parsed.get());
        for (size_t i = 0; i < d.state->external_buffers.size(); ++i)
            if (!d.state->external_buffers[i].empty()) {
                if (i >= d.state->parsed.buffers.size())
                    return fail("parsed buffer count differs");
                auto const& bytes = d.state->external_buffers[i];
                d.state->parsed.buffers[i].data = fastgltf::sources::ByteView{fastgltf::span<byte const>(bytes.data(), bytes.size()), fastgltf::MimeType::None};
            }
        auto references = preflight_references(d);
        if (!references)
            return std::unexpected(references.error());
        if (fastgltf::validate(d.state->parsed) != fastgltf::Error::None)
            return fail("invalid core glTF structure");
        auto checked = validate_accessors(d);
        if (!checked)
            return std::unexpected(checked.error());
        if (requires_subtilis) {
            bool found = false;
            for (uint32_t m = 0; m < d.state->parsed.meshes.size(); ++m)
                for (uint32_t p = 0; p < d.state->parsed.meshes[m].primitives.size(); ++p) {
                    primitive_key key{m, p};
                    if (!member(member(primitive_value(d, key), "extensions"), "DEREN_subtilis"))
                        continue;
                    found = true;
                    auto checked_payload = payload_bytes(d, key);
                    if (!checked_payload)
                        return std::unexpected(checked_payload.error());
                }
            if (!found)
                return fail("required DEREN_subtilis has no payload");
        }
        return d;
    }
    result<vector<primitive_key>> eligible_primitives(document const& d) {
        if (!d.state)
            return fail("empty document");
        auto const& a = d.state->parsed;
        vector<bool> skinned(a.meshes.size(), false);
        for (auto const& n : a.nodes)
            if (n.meshIndex && n.skinIndex)
                skinned[*n.meshIndex] = true;
        vector<primitive_key> keys;
        for (uint32_t m = 0; m < a.meshes.size(); ++m)
            for (uint32_t p = 0; p < a.meshes[m].primitives.size(); ++p)
                if (eligible(d, {m, p}, skinned[m]))
                    keys.push_back({m, p});
        return keys;
    }
    result<vector<byte>> payload_bytes(document const& d, primitive_key key) {
        auto* ext = member(member(primitive_value(d, key), "extensions"), "DEREN_subtilis");
        auto* version = member(ext, "version");
        auto* profile = member(ext, "profile");
        auto* view = member(ext, "bufferView");
        if (!yyjson_mut_is_uint(version) || yyjson_mut_get_uint(version) != 1 || !yyjson_mut_is_str(profile) || std::string_view(yyjson_mut_get_str(profile)) != "shared-vertices" || !yyjson_mut_is_uint(view))
            return fail("missing or unsupported DEREN_subtilis extension");
        auto* bv = yyjson_mut_arr_get(member(root(d), "bufferViews"), yyjson_mut_get_uint(view));
        auto* buffer = member(bv, "buffer");
        auto* offset = member(bv, "byteOffset");
        auto* length = member(bv, "byteLength");
        if (!yyjson_mut_is_uint(buffer) || yyjson_mut_get_uint(buffer) > UINT32_MAX || !yyjson_mut_is_uint(length) || member(bv, "target") || (offset && !yyjson_mut_is_uint(offset)))
            return fail("invalid payload bufferView");
        uint64_t at = offset ? yyjson_mut_get_uint(offset) : 0, n = yyjson_mut_get_uint(length);
        auto backing = buffer_bytes(d, static_cast<uint32_t>(yyjson_mut_get_uint(buffer)));
        if (!d.state || at > backing.size() || n > backing.size() - at || n > d.state->limits.max_payload_bytes)
            return fail("payload range exceeds buffer");
        auto bytes = backing.subspan(static_cast<size_t>(at), static_cast<size_t>(n));
        auto payload = asset::decode(bytes, d.state->limits);
        if (!payload)
            return std::unexpected(payload.error());
        return vector<byte>(bytes.begin(), bytes.end());
    }
    result<void> attach_payload(document& d, primitive_key key, std::span<byte const> bytes) {
        if (!d.state || !primitive_value(d, key))
            return fail("payload append requires a valid primitive");
        bool skinned = false;
        for (auto const& node : d.state->parsed.nodes)
            if (node.meshIndex && *node.meshIndex == key.mesh && node.skinIndex)
                skinned = true;
        if (!eligible(d, key, skinned))
            return fail("primitive is outside shared-vertices profile");
        auto decoded = asset::decode(bytes, d.state->limits);
        if (!decoded)
            return std::unexpected(decoded.error());
        auto source = extract_geometry(d, key);
        if (!source)
            return std::unexpected(source.error());
        auto validated = asset::make_asset(std::move(*source), std::move(*decoded), d.state->limits);
        if (!validated)
            return std::unexpected(validated.error());
        auto* primitive = primitive_value(d, key);
        auto* extensions = member(primitive, "extensions");
        if (member(extensions, "DEREN_subtilis"))
            return fail("primitive already has a subtilis payload");
        auto prepared = ensure_append_buffer(d);
        if (!prepared)
            return prepared;
        auto at = (uint64_t(d.state->binary.size()) + 7) & ~uint64_t(7);
        if (at > d.state->limits.max_input_bytes || bytes.size() > d.state->limits.max_input_bytes - at)
            return fail("append byte limit", asset::error_code::capacity);
        auto* json = d.state->json;
        auto* views = member(root(d), "bufferViews");
        if (!yyjson_mut_is_arr(views)) {
            views = yyjson_mut_arr(json);
            yyjson_mut_obj_add_val(json, root(d), "bufferViews", views);
        }
        auto* bv = yyjson_mut_obj(json);
        yyjson_mut_obj_add_uint(json, bv, "buffer", d.state->append_buffer);
        yyjson_mut_obj_add_uint(json, bv, "byteOffset", at);
        yyjson_mut_obj_add_uint(json, bv, "byteLength", bytes.size());
        auto index = yyjson_mut_arr_size(views);
        yyjson_mut_arr_append(views, bv);
        if (!extensions) {
            extensions = yyjson_mut_obj(json);
            yyjson_mut_obj_add_val(json, primitive, "extensions", extensions);
        }
        auto* ext = yyjson_mut_obj(json);
        yyjson_mut_obj_add_uint(json, ext, "version", 1);
        yyjson_mut_obj_add_str(json, ext, "profile", "shared-vertices");
        yyjson_mut_obj_add_uint(json, ext, "bufferView", index);
        yyjson_mut_obj_add_val(json, extensions, "DEREN_subtilis", ext);
        auto* used = member(root(d), "extensionsUsed");
        if (!used) {
            used = yyjson_mut_arr(json);
            yyjson_mut_obj_add_val(json, root(d), "extensionsUsed", used);
        }
        bool found = false;
        for (size_t i = 0; i < yyjson_mut_arr_size(used); ++i) {
            auto* v = yyjson_mut_arr_get(used, i);
            if (yyjson_mut_is_str(v) && std::string_view(yyjson_mut_get_str(v)) == "DEREN_subtilis")
                found = true;
        }
        if (!found)
            yyjson_mut_arr_add_str(json, used, "DEREN_subtilis");
        d.state->binary.resize(static_cast<size_t>(at), byte{0});
        d.state->binary.insert(d.state->binary.end(), bytes.begin(), bytes.end());
        auto* buffer = yyjson_mut_arr_get(member(root(d), "buffers"), d.state->append_buffer);
        yyjson_mut_set_uint(member(buffer, "byteLength"), d.state->binary.size());
        return {};
    }
} // namespace deren::tools::subtilis
