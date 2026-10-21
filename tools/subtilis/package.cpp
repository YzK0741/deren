#include "document_internal.hpp"

#include <atomic>
#include <chrono>

namespace deren::tools::subtilis {
    bool same_path(path const& a, path const& b) {
        std::error_code ec;
        auto x = std::filesystem::weakly_canonical(a, ec);
        if (ec)
            return true;
        auto y = std::filesystem::weakly_canonical(b, ec);
        if (ec)
            return true;
        if (x == y)
            return true;
        return std::filesystem::equivalent(x, y, ec) && !ec;
    }
    result<void> write_package(document const& d, path const& target, write_options const& options) {
        if (!d.state || same_path(d.state->source_path, target))
            return fail("source file cannot be overwritten", asset::error_code::io);
        if (options.format != output_format::glb)
            return fail("glTF package output is not available yet", asset::error_code::io);
        auto bytes = serialize_embedded_glb(d);
        if (!bytes)
            return std::unexpected(bytes.error());
        return write_bytes_staged(target, *bytes, true, options.overwrite);
    }
    result<void> identity(asset::payload const& p) {
        auto allocator = json_allocator();
        auto* raw = yyjson_read_opts(const_cast<char*>(p.build_meta.data()), p.build_meta.size(), 0, &allocator, nullptr);
        std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(raw, yyjson_doc_free);
        if (!doc)
            return fail("invalid build metadata");
        auto* root = yyjson_doc_get_root(doc.get());
        auto string_is = [](yyjson_val* v, std::string_view expected) { return yyjson_is_str(v) && std::string_view(yyjson_get_str(v)) == expected; };
        if (!string_is(yyjson_obj_get(root, "algorithm"), "subtilis-dag-v1") || !string_is(yyjson_obj_get(root, "profile"), "shared-vertices"))
            return fail("unsupported build algorithm/profile");
        auto* parameters = yyjson_obj_get(root, "parameters");
        asset::build_options options;
        auto u32 = [&](char const* name, uint32_t& out) {auto* v=yyjson_obj_get(parameters,name);if(!yyjson_is_uint(v) || yyjson_get_uint(v)>UINT32_MAX)return false;out=static_cast<uint32_t>(yyjson_get_uint(v));return true; };
        auto f32 = [&](char const* name, float& out) {auto* v=yyjson_obj_get(parameters,name);if(!yyjson_is_num(v))return false;out=static_cast<float>(yyjson_get_num(v));return std::isfinite(out); };
        if (!u32("triangleLimit", options.triangle_limit) || !u32("vertexLimit", options.vertex_limit) || !u32("targetGroupSize", options.target_group_size) || !u32("maxRounds", options.max_rounds) || !f32("targetRatio", options.target_ratio) || !f32("relativeError", options.relative_error) || !f32("normalWeight", options.normal_weight) || !f32("uvWeight", options.uv_weight))
            return fail("incomplete build parameters");
        auto digest = asset::build_digest(options);
        if (!digest || *digest != p.build_hash)
            return fail("build fingerprint mismatch");
        auto* dependencies = yyjson_obj_get(root, "dependencies");
        if (!string_is(yyjson_obj_get(dependencies, "meshoptimizer"), "9e1f07b159d3cb777f1c67ed31fc11fd117986f4") || !string_is(yyjson_obj_get(dependencies, "fastgltf"), "0.9.0") || !string_is(yyjson_obj_get(dependencies, "simdjson"), "4.6.9") || !string_is(yyjson_obj_get(dependencies, "yyjson"), "0.12.0"))
            return fail("build dependency identity mismatch");
        return {};
    }
    vector<primitive_key> asset_keys(document const& d) {
        vector<primitive_key> keys;
        auto const& a = d.state->parsed;
        for (uint32_t m = 0; m < a.meshes.size(); ++m)
            for (uint32_t p = 0; p < a.meshes[m].primitives.size(); ++p)
                if (member(member(primitive_value(d, {m, p}), "extensions"), "DEREN_subtilis"))
                    keys.push_back({m, p});
        return keys;
    }
    result<asset::validated_asset> full_asset(document const& d, primitive_key key) {
        auto bytes = payload_bytes(d, key);
        if (!bytes)
            return std::unexpected(bytes.error());
        auto p = asset::decode(*bytes, d.state->limits);
        if (!p)
            return std::unexpected(p.error());
        auto digest = identity(*p);
        if (!digest)
            return std::unexpected(digest.error());
        auto source = extract_geometry(d, key);
        if (!source)
            return std::unexpected(source.error());
        return asset::make_asset(std::move(*source), std::move(*p), d.state->limits);
    }
    result<uint64_t> verify_file(path const& file, bool full) {
        auto d = read_document(file, {});
        if (!d)
            return std::unexpected(d.error());
        auto keys = asset_keys(*d);
        for (auto key : keys) {
            if (full) {
                auto a = full_asset(*d, key);
                if (!a)
                    return std::unexpected(a.error());
            } else {
                auto bytes = payload_bytes(*d, key);
                if (!bytes)
                    return std::unexpected(bytes.error());
                auto p = asset::decode(*bytes, {});
                if (!p)
                    return std::unexpected(p.error());
                auto valid = asset::validate_structure(*p, {});
                if (!valid)
                    return std::unexpected(valid.error());
            }
        }
        return keys.size();
    }
    result<void> write_bytes_staged(path const& target, std::span<byte const> bytes, bool verify_glb, bool overwrite) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(target, ec);
        if (ec || absolute.filename().empty())
            return fail("invalid output path", asset::error_code::io);
        if ((std::filesystem::exists(absolute, ec) && !overwrite) || ec)
            return fail("output already exists or cannot be inspected", asset::error_code::io);
        auto parent = absolute.parent_path();
        std::filesystem::create_directories(parent, ec);
        if (ec)
            return fail("cannot create output directory", asset::error_code::io);
        static std::atomic<uint64_t> sequence{0};
        path staging;
        bool created = false;
        for (unsigned attempt = 0; attempt < 32 && !created; ++attempt) {
            auto name = utility::vessel::format(".subtilis-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), sequence.fetch_add(1));
            staging = parent / path(name.c_str());
            created = std::filesystem::create_directory(staging, ec);
            if (ec)
                return fail("cannot create staging directory", asset::error_code::io);
        }
        if (!created)
            return fail("cannot reserve staging directory", asset::error_code::io);
        auto temporary = staging / "artifact";
        auto cleanup = [&] {std::error_code ignored;std::filesystem::remove(temporary,ignored);std::filesystem::remove(staging,ignored); };
        {
            std::ofstream out(temporary, std::ios::binary);
            if (!out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
                out.close();
                cleanup();
                return fail("output write failed", asset::error_code::io);
            }
            out.close();
            if (!out) {
                cleanup();
                return fail("output close failed", asset::error_code::io);
            }
        }
        if (verify_glb) {
            auto checked = verify_file(temporary, true);
            if (!checked) {
                cleanup();
                return std::unexpected(checked.error());
            }
        }
        auto published = overwrite ? publish_replace(temporary, absolute) : publish_no_replace(temporary, absolute);
        cleanup();
        return published;
    }
} // namespace deren::tools::subtilis
