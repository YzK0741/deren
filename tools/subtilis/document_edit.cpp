#include "document_internal.hpp"
namespace deren::tools::subtilis {
    result<void> replace_indices(document& d, primitive_key key, std::span<std::uint32_t const> indices) {
        if (!d.state || !primitive_value(d, key) || indices.empty() || indices.size() % 3 || indices.size() > UINT32_MAX)
            return fail("invalid cut index replacement");
        auto source = extract_geometry(d, key);
        if (!source)
            return std::unexpected(source.error());
        for (auto id : indices)
            if (id >= source->positions.size())
                return fail("cut vertex outside source");
        auto prepared = ensure_append_buffer(d);
        if (!prepared)
            return prepared;
        auto at = (uint64_t(d.state->binary.size()) + 3) & ~uint64_t(3), length = uint64_t(indices.size()) * 4;
        if (at > d.state->limits.max_input_bytes || length > d.state->limits.max_input_bytes - at)
            return fail("cut exceeds output limit", asset::error_code::capacity);
        auto* json = d.state->json;
        auto* views = member(root(d), "bufferViews");
        auto* accessors = member(root(d), "accessors");
        auto view_id = yyjson_mut_arr_size(views), accessor_id = yyjson_mut_arr_size(accessors);
        if (view_id >= UINT32_MAX || accessor_id >= UINT32_MAX)
            return fail("cut accessor/view limit", asset::error_code::capacity);
        auto* view = yyjson_mut_obj(json);
        yyjson_mut_obj_add_uint(json, view, "buffer", d.state->append_buffer);
        yyjson_mut_obj_add_uint(json, view, "byteOffset", at);
        yyjson_mut_obj_add_uint(json, view, "byteLength", length);
        yyjson_mut_obj_add_uint(json, view, "target", 34963);
        yyjson_mut_arr_append(views, view);
        auto* accessor = yyjson_mut_obj(json);
        yyjson_mut_obj_add_uint(json, accessor, "bufferView", view_id);
        yyjson_mut_obj_add_uint(json, accessor, "componentType", 5125);
        yyjson_mut_obj_add_uint(json, accessor, "count", indices.size());
        yyjson_mut_obj_add_str(json, accessor, "type", "SCALAR");
        yyjson_mut_arr_append(accessors, accessor);
        auto* primitive = primitive_value(d, key);
        yyjson_mut_obj_remove_key(primitive, "indices");
        yyjson_mut_obj_add_uint(json, primitive, "indices", accessor_id);
        auto* extensions = member(primitive, "extensions");
        yyjson_mut_obj_remove_key(extensions, "DEREN_subtilis");
        if (!yyjson_mut_obj_size(extensions))
            yyjson_mut_obj_remove_key(primitive, "extensions");
        d.state->binary.resize(static_cast<size_t>(at), byte{0});
        for (auto id : indices)
            put_u32(d.state->binary, id);
        yyjson_mut_set_uint(member(yyjson_mut_arr_get(member(root(d), "buffers"), d.state->append_buffer), "byteLength"), d.state->binary.size());
        d.state->index_overrides.push_back({key, vector<std::uint32_t>(indices.begin(), indices.end())});
        bool remaining = false;
        auto const& a = d.state->parsed;
        for (uint32_t m = 0; m < a.meshes.size(); ++m)
            for (uint32_t p = 0; p < a.meshes[m].primitives.size(); ++p)
                if (member(member(primitive_value(d, {m, p}), "extensions"), "DEREN_subtilis"))
                    remaining = true;
        if (!remaining)
            for (auto name : {"extensionsUsed", "extensionsRequired"}) {
                auto* list = member(root(d), name);
                for (size_t i = yyjson_mut_arr_size(list); i > 0; --i) {
                    auto* value = yyjson_mut_arr_get(list, i - 1);
                    if (yyjson_mut_is_str(value) && std::string_view(yyjson_mut_get_str(value)) == "DEREN_subtilis")
                        yyjson_mut_arr_remove(list, i - 1);
                }
                if (!yyjson_mut_arr_size(list))
                    yyjson_mut_obj_remove_key(root(d), name);
            }
        return {};
    }
} // namespace deren::tools::subtilis
