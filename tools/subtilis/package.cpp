#include "document_internal.hpp"
#include "package_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace deren::tools::subtilis {
    namespace {
        path normalized_directory(path const& directory) {
            path result = directory;
            while (!result.empty() && result.filename().empty() && result != result.root_path())
                result = result.parent_path();
            return result;
        }
    } // namespace
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
        if (options.format == output_format::gltf)
            return package_detail::write_gltf_package_impl(d, target, options.overwrite, nullptr);
        if (options.format != output_format::glb)
            return fail("unknown output format", asset::error_code::invalid_input);
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
        return package_detail::write_bytes_staged_impl(target, bytes, verify_glb, overwrite, nullptr);
    }
    namespace package_detail {
        path package_json_path(path const& package_dir) {
            auto leaf = normalized_directory(package_dir).filename();
            if (leaf.empty())
                return {};
            path name = leaf;
            name += ".gltf";
            return normalized_directory(package_dir) / name;
        }
        path package_dir_for_cli_output(path const& output) {
            auto name = output.filename();
            if (name.empty() || name == "." || name == ".." || name.extension() != ".gltf")
                return {};
            auto stem = name.stem();
            if (stem.empty() || stem == "." )
                return {};
            return output.parent_path() / stem;
        }
        bool path_inside(path const& candidate, path const& directory) {
            std::error_code ec;
            auto child = std::filesystem::weakly_canonical(candidate, ec);
            if (ec)
                return true;
            auto parent = std::filesystem::weakly_canonical(directory, ec);
            if (ec)
                return true;
            auto child_it = child.begin();
            for (auto parent_it = parent.begin(); parent_it != parent.end(); ++parent_it, ++child_it) {
                if (child_it == child.end())
                    return false;
                if (child_it->native() != parent_it->native())
                    return false;
            }
            return true;
        }
        namespace {
            constexpr char hex_digits[] = "0123456789ABCDEF";
            std::string percent_encode_uri(std::string_view name) {
                std::string encoded;
                encoded.reserve(name.size());
                for (char raw : name) {
                    auto c = static_cast<unsigned char>(raw);
                    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
                    if (plain) {
                        encoded += static_cast<char>(c);
                    } else {
                        encoded += '%';
                        encoded += hex_digits[c >> 4];
                        encoded += hex_digits[c & 15];
                    }
                }
                return encoded;
            }
            std::string_view image_mime_and_extension(std::span<byte const> bytes) {
                static constexpr std::byte png_prefix[]{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}};
                static constexpr std::byte jpeg_prefix[]{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}};
                if (bytes.size() >= std::size(png_prefix) && std::memcmp(bytes.data(), png_prefix, std::size(png_prefix)) == 0)
                    return "image/png";
                if (bytes.size() >= std::size(jpeg_prefix) && std::memcmp(bytes.data(), jpeg_prefix, std::size(jpeg_prefix)) == 0)
                    return "image/jpeg";
                return {};
            }
            class file_sink final : public byte_sink {
                std::ofstream out_;
            public:
                explicit file_sink(path const& file)
                    : out_(file, std::ios::binary) {
                }
                bool is_open() const {
                    return out_.is_open();
                }
                result<void> write(std::span<std::byte const> bytes) override {
                    out_.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (!out_)
                        return std::unexpected(asset::error{asset::error_code::io, "output write failed", {}, {}});
                    return {};
                }
                result<void> close() override {
                    out_.close();
                    if (!out_)
                        return std::unexpected(asset::error{asset::error_code::io, "output close failed", {}, {}});
                    return {};
                }
            };
            result<std::unique_ptr<byte_sink>> open_sink(path const& file, sink_ops const* ops) {
                if (ops != nullptr && ops->open != nullptr)
                    return ops->open(file, ops->context);
                auto sink = std::make_unique<file_sink>(file);
                if (!sink->is_open())
                    return std::unexpected(asset::error{asset::error_code::io, "cannot create staged file", {}, {}});
                return std::unique_ptr<byte_sink>(std::move(sink));
            }
            result<void> write_staged_file(path const& file, std::span<std::byte const> bytes, sink_ops const* ops) {
                auto opened = open_sink(file, ops);
                if (!opened)
                    return std::unexpected(opened.error());
                auto written = (*opened)->write(bytes);
                if (!written)
                    return std::unexpected(written.error());
                return (*opened)->close();
            }
            result<std::vector<std::byte>> read_file_bytes(path const& file) {
                std::ifstream in(file, std::ios::binary);
                if (!in)
                    return std::unexpected(asset::error{asset::error_code::io, "cannot read staged file", {}, {}});
                std::string text((std::istreambuf_iterator<char>(in)), {});
                if (in.bad())
                    return std::unexpected(asset::error{asset::error_code::io, "cannot read staged file", {}, {}});
                auto const* const data = reinterpret_cast<std::byte const*>(text.data());
                return std::vector<std::byte>(data, data + text.size());
            }
            result<void> verify_staged_hash(path const& file, std::span<std::byte const> expected) {
                auto actual = read_file_bytes(file);
                if (!actual)
                    return std::unexpected(actual.error());
                if (asset::sha256(*actual) != asset::sha256(expected))
                    return std::unexpected(asset::error{asset::error_code::io, "staged output content does not match what was written", {}, {}});
                return {};
            }
            result<void> rename_staged(path const& from, path const& to, sink_ops const* ops) {
                if (ops != nullptr && ops->rename != nullptr)
                    return ops->rename(from, to, ops->context);
                return move_path(from, to);
            }
            result<void> publish_staged(path const& staging, path const& target, bool overwrite, sink_ops const* ops, bool directory) {
                std::error_code ec;
                if (!overwrite)
                    return rename_staged(staging, target, ops);
                bool const exists = std::filesystem::exists(target, ec);
                if (ec)
                    return std::unexpected(asset::error{asset::error_code::io, "cannot inspect output target", {}, {}});
                if (!exists)
                    return rename_staged(staging, target, ops);
                static std::atomic<uint64_t> backup_sequence{0};
                auto const stamp = utility::vessel::format(".old-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), backup_sequence.fetch_add(1));
                path const backup = target.parent_path() / path((target.filename().string() + stamp.c_str()).c_str());
                auto moved = rename_staged(target, backup, ops);
                if (!moved)
                    return moved;
                auto published = rename_staged(staging, target, ops);
                if (!published) {
                    (void)rename_staged(backup, target, ops);
                    return published;
                }
                if (directory)
                    std::filesystem::remove_all(backup, ec);
                else
                    std::filesystem::remove(backup, ec);
                return {};
            }
            result<path> make_staging_directory(path const& parent) {
                static std::atomic<uint64_t> sequence{0};
                std::error_code ec;
                for (unsigned attempt = 0; attempt < 32; ++attempt) {
                    auto const stamp = utility::vessel::format(".subtilis-package-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), sequence.fetch_add(1));
                    path candidate = parent / path(stamp.c_str());
                    if (std::filesystem::create_directory(candidate, ec) && !ec)
                        return candidate;
                    if (ec)
                        return std::unexpected(asset::error{asset::error_code::io, "cannot create staging directory", {}, {}});
                }
                return std::unexpected(asset::error{asset::error_code::io, "cannot reserve staging directory", {}, {}});
            }
            void remove_tree(path const& file) noexcept {
                std::error_code ignored;
                std::filesystem::remove_all(file, ignored);
            }
            struct staged_file {
                path name;
                std::vector<std::byte> bytes;
            };
        } // namespace
        result<void> write_bytes_staged_impl(path const& target, std::span<byte const> bytes, bool verify_glb, bool overwrite, sink_ops const* ops) {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(target, ec);
            if (ec || absolute.filename().empty())
                return fail("invalid output path", asset::error_code::io);
            bool const exists = std::filesystem::exists(absolute, ec);
            if (ec)
                return fail("output cannot be inspected", asset::error_code::io);
            if (exists && !overwrite)
                return fail("output already exists", asset::error_code::io);
            if (exists) {
                auto const status = std::filesystem::status(absolute, ec);
                if (ec)
                    return fail("output cannot be inspected", asset::error_code::io);
                if (std::filesystem::is_directory(status))
                    return fail("output path is a directory", asset::error_code::io);
                constexpr auto write_bits = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
                if ((status.permissions() & write_bits) == std::filesystem::perms::none)
                    return fail("output is read-only", asset::error_code::io);
            }
            auto const parent = absolute.parent_path();
            std::filesystem::create_directories(parent, ec);
            if (ec)
                return fail("cannot create output directory", asset::error_code::io);
            static std::atomic<uint64_t> sequence{0};
            path staging;
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                auto const stamp = utility::vessel::format(".subtilis-artifact-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), sequence.fetch_add(1));
                staging = parent / path(stamp.c_str());
                if (!std::filesystem::exists(staging))
                    break;
            }
            auto written = write_staged_file(staging, bytes, ops);
            if (!written) {
                remove_tree(staging);
                return std::unexpected(written.error());
            }
            if (verify_glb) {
                auto checked = verify_file(staging, true);
                if (!checked) {
                    remove_tree(staging);
                    return std::unexpected(checked.error());
                }
            }
            auto published = publish_staged(staging, absolute, overwrite, ops, false);
            if (!published) {
                remove_tree(staging);
                return published;
            }
            return {};
        }
        result<void> write_gltf_package_impl(document const& d, path const& package_dir, bool overwrite, sink_ops const* ops) {
            if (!d.state)
                return fail("no document to package");
            auto const dir = normalized_directory(package_dir);
            if (dir.empty() || dir.filename().empty())
                return fail("invalid glTF package directory", asset::error_code::io);
            if (!d.state->source_path.empty() && path_inside(d.state->source_path, dir))
                return fail("source file is inside the package that would be replaced", asset::error_code::io);
            auto const leaf = dir.filename();
            path const json_name = [&] { path name = leaf; name += ".gltf"; return name; }();
            auto allocator = json_allocator();
            std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owned(yyjson_mut_doc_mut_copy(d.state->json, &allocator), yyjson_mut_doc_free);
            if (!owned)
                return fail("cannot copy packaging document");
            auto* root_value = yyjson_mut_doc_get_root(owned.get());
            if (root_value == nullptr)
                return fail("package document is empty");
            std::vector<staged_file> files;
            // Buffers: external resources relocate to buffer<id>.bin; data URIs stay inline; the one
            // no-URI buffer (the embedded BIN or the appended payload) becomes <leaf>.bin.
            auto* buffers = member(root_value, "buffers");
            bool embedded_named = false;
            for (size_t i = 0; i < yyjson_mut_arr_size(buffers); ++i) {
                auto* buffer = yyjson_mut_arr_get(buffers, i);
                auto data = buffer_bytes(d, static_cast<uint32_t>(i));
                auto* length = member(buffer, "byteLength");
                if (!yyjson_mut_is_uint(length) || yyjson_mut_get_uint(length) > data.size())
                    return fail("missing buffer data", asset::error_code::io);
                auto* uri_value = member(buffer, "uri");
                if (yyjson_mut_is_str(uri_value)) {
                    std::string_view const uri(yyjson_mut_get_str(uri_value));
                    if (uri.starts_with("data:"))
                        continue;
                    if (uri.empty())
                        return fail("empty external buffer URI", asset::error_code::invalid_input);
                    auto const name = "buffer" + std::to_string(i) + ".bin";
                    yyjson_mut_obj_put(buffer, yyjson_mut_strcpy(owned.get(), "uri"), yyjson_mut_strcpy(owned.get(), percent_encode_uri(name).c_str()));
                    files.push_back(staged_file{path(name.c_str()), std::vector<std::byte>(data.begin(), data.end())});
                } else {
                    if (embedded_named)
                        return fail("multiple buffers without a URI cannot share one package file", asset::error_code::invalid_input);
                    embedded_named = true;
                    path const name = [&] { path item = leaf; item += ".bin"; return item; }();
                    yyjson_mut_obj_put(buffer, yyjson_mut_strcpy(owned.get(), "uri"), yyjson_mut_strcpy(owned.get(), percent_encode_uri(name.generic_string()).c_str()));
                    files.push_back(staged_file{name, std::vector<std::byte>(data.begin(), data.end())});
                }
            }
            // Images: external resources relocate with a MIME-derived extension; data URIs and
            // bufferView-backed images stay entirely unchanged.
            if (auto* images = member(root_value, "images"); yyjson_mut_is_arr(images)) {
                for (size_t i = 0; i < yyjson_mut_arr_size(images); ++i) {
                    auto* image = yyjson_mut_arr_get(images, i);
                    if (member(image, "bufferView") != nullptr)
                        continue;
                    auto* uri_value = member(image, "uri");
                    if (!yyjson_mut_is_str(uri_value))
                        continue;
                    std::string_view const uri(yyjson_mut_get_str(uri_value));
                    if (uri.starts_with("data:"))
                        continue;
                    if (i >= d.state->image_resources.size() || d.state->image_resources[i].empty())
                        return fail("missing external image resource", asset::error_code::io);
                    auto const& image_bytes = d.state->image_resources[i];
                    auto const mime = image_mime_and_extension(std::span<std::byte const>(image_bytes));
                    if (mime.empty())
                        return fail("package images must be PNG or JPEG", asset::error_code::invalid_input);
                    auto const extension = mime == "image/png" ? ".png" : ".jpeg";
                    auto const name = "image" + std::to_string(i) + extension;
                    yyjson_mut_obj_put(image, yyjson_mut_strcpy(owned.get(), "uri"), yyjson_mut_strcpy(owned.get(), percent_encode_uri(name).c_str()));
                    yyjson_mut_obj_put(image, yyjson_mut_strcpy(owned.get(), "mimeType"), yyjson_mut_strcpy(owned.get(), std::string(mime).c_str()));
                    files.push_back(staged_file{path(name.c_str()), std::vector<std::byte>(image_bytes.begin(), image_bytes.end())});
                }
            }
            size_t text_size = 0;
            auto* text = yyjson_mut_write_opts(owned.get(), 0, &allocator, &text_size, nullptr);
            if (!text)
                return fail("cannot serialize package JSON");
            std::vector<staged_file> package_files;
            package_files.push_back(staged_file{json_name, std::vector<std::byte>(reinterpret_cast<std::byte const*>(text), reinterpret_cast<std::byte const*>(text) + text_size)});
            utility::memory::deallocate(text);
            for (auto& entry : files)
                package_files.push_back(std::move(entry));
            std::error_code ec;
            std::filesystem::create_directories(dir.parent_path(), ec);
            if (ec)
                return fail("cannot create package parent directory", asset::error_code::io);
            auto staging = make_staging_directory(dir.parent_path());
            if (!staging)
                return std::unexpected(staging.error());
            auto const cleanup = [&] { remove_tree(*staging); };
            for (auto const& entry : package_files) {
                auto written = write_staged_file(*staging / entry.name, entry.bytes, ops);
                if (!written) {
                    cleanup();
                    return std::unexpected(written.error());
                }
            }
            for (auto const& entry : package_files) {
                auto checked = verify_staged_hash(*staging / entry.name, entry.bytes);
                if (!checked) {
                    cleanup();
                    return std::unexpected(checked.error());
                }
            }
            auto checked = verify_file(*staging / json_name, true);
            if (!checked) {
                cleanup();
                return std::unexpected(checked.error());
            }
            auto published = publish_staged(*staging, dir, overwrite, ops, true);
            if (!published) {
                cleanup();
                return published;
            }
            return {};
        }
    } // namespace package_detail
} // namespace deren::tools::subtilis
