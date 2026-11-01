module;

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <system_error>
#include <toml++/toml.hpp>

module deren.asset_library;
import deren.utility.shared;

namespace deren::asset_library {
    namespace {
        namespace fs = std::filesystem;

        void warn(std::vector<std::string>& warnings, fs::path const& manifest, std::string_view const text) {
            warnings.push_back(manifest.string() + ": " + std::string(text));
        }

        std::optional<std::string_view> string_value(toml::table const& table, std::string_view const key) {
            toml::node const* node = table.get(key);
            if (node == nullptr) {
                return std::nullopt;
            }
            return node->value<std::string_view>();
        }

        std::optional<bool> bool_value(toml::table const& table, std::string_view const key) {
            toml::node const* node = table.get(key);
            if (node == nullptr) {
                return std::nullopt;
            }
            return node->value<bool>();
        }

        std::optional<int64_t> integer_value(toml::table const& table, std::string_view const key) {
            toml::node const* node = table.get(key);
            if (node == nullptr) {
                return std::nullopt;
            }
            return node->value<int64_t>();
        }

        std::optional<double> number_value(toml::node const& node) {
            if (std::optional<double> const value = node.value<double>()) {
                return value;
            }
            if (std::optional<int64_t> const value = node.value<int64_t>()) {
                return static_cast<double>(*value);
            }
            return std::nullopt;
        }

        /// a path from a manifest is only allowed INSIDE the asset directory: an absolute path, a drive
        /// prefix or any `..` component is rejected instead of being normalized away, because a library
        /// that can point outside its root turns a data file into a way to read the whole disk.
        std::optional<fs::path> resolve_inside(fs::path const& directory, std::string_view const raw, std::string_view const field,
                                               fs::path const& manifest, std::vector<std::string>& warnings) {
            if (raw.empty()) {
                warn(warnings, manifest, std::string(field) + " is empty");
                return std::nullopt;
            }
            fs::path const relative(raw);
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
                warn(warnings, manifest, std::string(field) + " must be relative to the asset directory");
                return std::nullopt;
            }
            for (fs::path const& component : relative) {
                if (component == "..") {
                    warn(warnings, manifest, std::string(field) + " must not contain '..'");
                    return std::nullopt;
                }
            }
            return (directory / relative).lexically_normal();
        }

        /// ids are used as command-line values and log keys; keeping them to one filename-safe token
        /// makes `--asset <id>` unambiguous without an escaping rule.
        bool valid_id(std::string_view const id) {
            return !id.empty() && std::ranges::all_of(id, [](char const character) {
                auto const byte = static_cast<unsigned char>(character);
                return std::isalnum(byte) != 0 || character == '-' || character == '_' || character == '.';
            });
        }

        bool starts_with(std::string_view const text, std::string_view const prefix) {
            return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
        }

        bool is_ignored(std::string_view const relative, std::vector<std::string> const& patterns) {
            return std::ranges::any_of(patterns, [relative](std::string const& pattern) {
                return !pattern.empty() &&
                       (relative == pattern ||
                        (starts_with(relative, pattern) && relative.size() > pattern.size() &&
                         relative[pattern.size()] == '/'));
            });
        }

        std::optional<std::array<float, 6>> camera_pose_value(toml::table const& table, fs::path const& manifest,
                                                             std::vector<std::string>& warnings) {
            toml::node const* node = table.get("pose");
            if (node == nullptr) {
                return std::nullopt;
            }
            toml::array const* array = node->as_array();
            if (array == nullptr || array->size() != 6) {
                warn(warnings, manifest, "camera.pose must be an array of six numbers");
                return std::nullopt;
            }
            std::array<float, 6> pose = {};
            for (std::size_t i = 0; i < pose.size(); ++i) {
                toml::node const* element = array->get(i);
                std::optional<double> const value = element != nullptr ? number_value(*element) : std::nullopt;
                if (!value) {
                    warn(warnings, manifest, "camera.pose contains a non-numeric value");
                    return std::nullopt;
                }
                pose[i] = static_cast<float>(*value);
            }
            return pose;
        }

        std::optional<asset_entry> parse_manifest(fs::path const& manifest, fs::path const& directory,
                                                  std::vector<std::string>& warnings) {
            toml::parse_result const parsed = toml::parse_file(manifest.string());
            if (!parsed) {
                warn(warnings, manifest, parsed.error().description());
                return std::nullopt;
            }
            toml::table const& table = parsed.table();

            asset_entry entry;
            entry.directory = directory;

            if (toml::node const* id_node = table.get("id")) {
                std::optional<std::string_view> const id = id_node->value<std::string_view>();
                if (!id) {
                    warn(warnings, manifest, "id must be a string");
                    return std::nullopt;
                }
                entry.id = *id;
            } else {
                entry.id = directory.filename().string();
            }
            if (!valid_id(entry.id)) {
                warn(warnings, manifest, "id '" + entry.id + "' is not a filename-safe identifier");
                return std::nullopt;
            }

            if (std::optional<std::string_view> const name = string_value(table, "name")) {
                entry.name = *name;
            } else {
                entry.name = entry.id;
            }

            if (toml::node const* type_node = table.get("type")) {
                std::optional<std::string_view> const type = type_node->value<std::string_view>();
                if (!type) {
                    warn(warnings, manifest, "type must be a string");
                    return std::nullopt;
                }
                std::optional<asset_type> const parsed_type = parse_asset_type(*type);
                if (!parsed_type) {
                    warn(warnings, manifest, "unknown type '" + std::string(*type) + "'");
                    return std::nullopt;
                }
                entry.type = *parsed_type;
            }

            std::optional<std::string_view> const model = string_value(table, "model");
            if (!model) {
                warn(warnings, manifest, "model is required and must be a string");
                return std::nullopt;
            }
            std::optional<fs::path> const model_path = resolve_inside(directory, *model, "model", manifest, warnings);
            if (!model_path) {
                return std::nullopt;
            }
            if (!fs::is_regular_file(*model_path)) {
                warn(warnings, manifest, "model '" + model_path->string() + "' does not exist");
                return std::nullopt;
            }
            entry.model = *model_path;

            if (std::optional<std::string_view> const sidecar = string_value(table, "sidecar")) {
                std::optional<fs::path> const sidecar_path =
                    resolve_inside(directory, *sidecar, "sidecar", manifest, warnings);
                if (!sidecar_path) {
                    return std::nullopt;
                }
                if (!fs::is_regular_file(*sidecar_path)) {
                    warn(warnings, manifest, "sidecar '" + sidecar_path->string() + "' does not exist");
                    return std::nullopt;
                }
                entry.sidecar = *sidecar_path;
            }

            if (std::optional<std::string_view> const license = string_value(table, "license")) {
                if (std::optional<fs::path> const path = resolve_inside(directory, *license, "license", manifest, warnings)) {
                    if (fs::is_regular_file(*path)) {
                        entry.license = *path;
                    } else {
                        warn(warnings, manifest, "license '" + path->string() + "' does not exist; ignored");
                    }
                }
            }
            if (std::optional<std::string_view> const thumbnail = string_value(table, "thumbnail")) {
                if (std::optional<fs::path> const path = resolve_inside(directory, *thumbnail, "thumbnail", manifest, warnings)) {
                    if (fs::is_regular_file(*path)) {
                        entry.thumbnail = *path;
                    } else {
                        warn(warnings, manifest, "thumbnail '" + path->string() + "' does not exist; ignored");
                    }
                }
            }

            if (toml::node const* tags_node = table.get("tags")) {
                toml::array const* tags = tags_node->as_array();
                if (tags == nullptr) {
                    warn(warnings, manifest, "tags must be an array of strings");
                } else {
                    for (toml::node const& tag : *tags) {
                        std::optional<std::string_view> const text = tag.value<std::string_view>();
                        if (!text) {
                            warn(warnings, manifest, "tags contains a non-string value; ignored");
                        } else {
                            entry.tags.emplace_back(*text);
                        }
                    }
                }
            }

            if (toml::node const* toon_node = table.get("toon")) {
                toml::table const* toon = toon_node->as_table();
                if (toon == nullptr) {
                    warn(warnings, manifest, "[toon] must be a table");
                } else {
                    if (std::optional<bool> const game_toon = bool_value(*toon, "game_toon")) {
                        entry.game_toon = *game_toon;
                    }
                    if (std::optional<std::string_view> const profile = string_value(*toon, "profile")) {
                        entry.toon_profile = *profile;
                    }
                    if (std::optional<int64_t> const tone_mapping = integer_value(*toon, "tone_mapping")) {
                        if (*tone_mapping >= 0 && *tone_mapping <= 2) {
                            entry.tone_mapping = static_cast<int32_t>(*tone_mapping);
                        } else {
                            warn(warnings, manifest, "toon.tone_mapping must be 0, 1 or 2; ignored");
                        }
                    }
                }
            }

            if (toml::node const* environment_node = table.get("environment")) {
                toml::table const* environment = environment_node->as_table();
                if (environment == nullptr) {
                    warn(warnings, manifest, "[environment] must be a table");
                } else if (std::optional<std::string_view> const hdr = string_value(*environment, "hdr")) {
                    if (std::optional<fs::path> const path =
                            resolve_inside(directory, *hdr, "environment.hdr", manifest, warnings)) {
                        if (fs::is_regular_file(*path)) {
                            entry.environment_hdr = *path;
                        } else {
                            warn(warnings, manifest, "environment.hdr '" + path->string() + "' does not exist; ignored");
                        }
                    }
                }
            }

            if (toml::node const* camera_node = table.get("camera")) {
                toml::table const* camera = camera_node->as_table();
                if (camera == nullptr) {
                    warn(warnings, manifest, "[camera] must be a table");
                } else {
                    entry.camera_pose = camera_pose_value(*camera, manifest, warnings);
                }
            }

            return entry;
        }
    } // namespace

    std::string_view asset_type_name(asset_type const type) noexcept {
        switch (type) {
            case asset_type::pbr: return "pbr";
            case asset_type::toon: return "toon";
            case asset_type::unlit: return "unlit";
            case asset_type::gaussian: return "gaussian";
        }
        return "pbr";
    }

    std::optional<asset_type> parse_asset_type(std::string_view const text) noexcept {
        std::string lowered;
        lowered.reserve(text.size());
        for (char const character : text) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
        if (lowered == "pbr") {
            return asset_type::pbr;
        }
        if (lowered == "toon") {
            return asset_type::toon;
        }
        if (lowered == "unlit") {
            return asset_type::unlit;
        }
        if (lowered == "gaussian") {
            return asset_type::gaussian;
        }
        return std::nullopt;
    }

    asset_library asset_library::scan(fs::path root) {
        asset_library library;
        library.root_ = std::move(root);
        std::error_code error;
        if (library.root_.empty() || !fs::is_directory(library.root_, error)) {
            library.error_ = "asset library root '" + library.root_.string() + "' is not a directory";
            return library;
        }
        if (fs::path absolute = fs::absolute(library.root_, error); !error) {
            library.root_ = absolute.lexically_normal();
        }

        int32_t scan_depth = 1;
        std::vector<std::string> ignore;
        fs::path const config_path = library.root_ / "library.toml";
        if (fs::is_regular_file(config_path, error)) {
            toml::parse_result const parsed = toml::parse_file(config_path.string());
            if (!parsed) {
                warn(library.warnings_, config_path, parsed.error().description());
            } else {
                toml::table const& table = parsed.table();
                if (std::optional<int64_t> const version = integer_value(table, "version")) {
                    if (*version != 1) {
                        warn(library.warnings_, config_path, "unsupported version " + std::to_string(*version) + "; expected 1");
                    }
                }
                if (std::optional<std::string_view> const requested_default = string_value(table, "default")) {
                    library.default_id_ = *requested_default;
                }
                if (std::optional<int64_t> const depth = integer_value(table, "scan_depth")) {
                    scan_depth = static_cast<int32_t>(std::clamp<int64_t>(*depth, 0, 4));
                }
                if (toml::node const* ignore_node = table.get("ignore")) {
                    toml::array const* patterns = ignore_node->as_array();
                    if (patterns == nullptr) {
                        warn(library.warnings_, config_path, "ignore must be an array of strings");
                    } else {
                        for (toml::node const& pattern : *patterns) {
                            std::optional<std::string_view> const text = pattern.value<std::string_view>();
                            if (!text) {
                                warn(library.warnings_, config_path, "ignore contains a non-string value; ignored");
                            } else {
                                ignore.emplace_back(*text);
                            }
                        }
                    }
                }
            }
        }

        std::set<std::string> ids;
        auto scan_directory = [&](auto&& self, fs::path const& directory, int32_t const depth_left) -> void {
            if (depth_left <= 0) {
                return;
            }
            std::vector<fs::path> children;
            std::error_code iteration_error;
            fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, iteration_error);
            fs::directory_iterator const end;
            if (iteration_error) {
                warn(library.warnings_, directory, "cannot scan directory: " + iteration_error.message());
                return;
            }
            for (; iterator != end; iterator.increment(iteration_error)) {
                if (iteration_error) {
                    warn(library.warnings_, directory, "cannot scan directory: " + iteration_error.message());
                    return;
                }
                std::string const name = iterator->path().filename().string();
                if (name.empty() || name.front() == '.') {
                    continue; // hidden / tooling directories are never assets
                }
                std::error_code type_error;
                if (iterator->is_directory(type_error)) {
                    children.push_back(iterator->path());
                }
                if (type_error) {
                    warn(library.warnings_, iterator->path(), "cannot inspect: " + type_error.message());
                }
            }
            std::sort(children.begin(), children.end());

            for (fs::path const& child : children) {
                fs::path const relative_path = child.lexically_relative(library.root_);
                if (is_ignored(relative_path.generic_string(), ignore)) {
                    continue;
                }
                fs::path const manifest = child / "asset.toml";
                std::error_code manifest_error;
                if (fs::is_regular_file(manifest, manifest_error)) {
                    if (std::optional<asset_entry> entry = parse_manifest(manifest, child, library.warnings_)) {
                        if (!ids.insert(entry->id).second) {
                            warn(library.warnings_, manifest, "duplicate asset id '" + entry->id + "'; ignored");
                        } else {
                            library.entries_.push_back(std::move(*entry));
                        }
                    }
                    continue; // an asset folder is a leaf: nested manifests are not silently merged
                }
                self(self, child, depth_left - 1);
            }
        };
        scan_directory(scan_directory, library.root_, scan_depth);

        std::sort(library.entries_.begin(), library.entries_.end(),
                  [](asset_entry const& left, asset_entry const& right) { return left.id < right.id; });
        if (library.entries_.empty()) {
            library.error_ = "no assets found under '" + library.root_.string() + "'";
            return library;
        }

        std::string const requested_default = library.default_id_;
        library.default_id_ = library.entries_.front().id;
        if (!requested_default.empty()) {
            if (library.find(requested_default) != nullptr) {
                library.default_id_ = requested_default;
            } else {
                warn(library.warnings_, config_path, "default asset '" + requested_default + "' not found; using '" +
                                                          library.default_id_ + "'");
            }
        }
        return library;
    }

    asset_entry const* asset_library::default_asset() const noexcept {
        return find(default_id_);
    }

    asset_entry const* asset_library::find(std::string_view const id) const noexcept {
        for (asset_entry const& entry : entries_) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace deren::asset_library
