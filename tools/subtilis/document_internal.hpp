#pragma once
#include "document.hpp"

#include <cstring>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fstream>
#include <yyjson.h>

namespace deren::tools::subtilis {
    inline yyjson_alc json_allocator() {
        return {
            [](void*, size_t size) -> void* { return utility::memory::allocate(size); },
            [](void*, void* old, size_t old_size, size_t size) -> void* {auto* next=utility::memory::allocate(size);if(old) std::memcpy(next,old,std::min(old_size,size));utility::memory::deallocate(old);return next; },
            [](void*, void* p) { utility::memory::deallocate(p); }, nullptr};
    }
    inline auto fail(char const* message, asset::error_code code = asset::error_code::invalid_input) {
        return std::unexpected(asset::error{code, message, {}, {}});
    }
    struct document_state {
        yyjson_mut_doc* json = nullptr;
        fastgltf::Asset parsed;
        vector<byte> binary;
        vector<vector<byte>> external_buffers;
        vector<vector<byte>> image_resources;
        uint32_t append_buffer = asset::none;
        vector<std::pair<primitive_key, vector<std::uint32_t>>> index_overrides;
        path source_path;
        asset::limits limits;
        bool self_contained = false;
        ~document_state() {
            yyjson_mut_doc_free(json);
        }
    };
    inline yyjson_mut_val* root(document const& d) {
        return d.state ? yyjson_mut_doc_get_root(d.state->json) : nullptr;
    }
    inline yyjson_mut_val* member(yyjson_mut_val* v, char const* key) {
        return yyjson_mut_obj_get(v, key);
    }
    inline yyjson_mut_val* primitive_value(document const& d, primitive_key key) {
        return yyjson_mut_arr_get(member(yyjson_mut_arr_get(member(root(d), "meshes"), key.mesh), "primitives"), key.primitive);
    }
    inline std::uint32_t get_u32(std::span<byte const> b, size_t offset) {
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i)
            n |= std::uint32_t(std::to_integer<unsigned>(b[offset + i])) << (i * 8);
        return n;
    }
    inline void put_u32(vector<byte>& b, std::uint32_t n) {
        for (unsigned i = 0; i < 4; ++i)
            b.push_back(byte((n >> (i * 8)) & 255));
    }
    result<void> validate_accessors(document const&);
    result<void> preflight_references(document const&);
    bool eligible(document const&, primitive_key, bool skinned_mesh);
    result<vector<byte>> make_glb(std::string_view, std::span<byte const>, asset::limits const&);
    result<void> publish_no_replace(path const& staging, path const& target);
    result<void> publish_replace(path const& staging, path const& target);
    bool same_path(path const&, path const&);
    vector<primitive_key> asset_keys(document const&);
    result<asset::validated_asset> full_asset(document const&, primitive_key);
    result<uint64_t> verify_file(path const&, bool);
    result<void> write_bytes_staged(path const&, std::span<byte const>, bool verify_glb, bool overwrite = false);
    result<vector<byte>> read_local_resource(path const& parent, std::string_view uri, uint64_t max_bytes);
    std::span<byte const> buffer_bytes(document const&, uint32_t);
    result<void> ensure_append_buffer(document&);
    result<vector<byte>> pack_glb(document const&);
} // namespace deren::tools::subtilis
