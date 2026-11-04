// Private implementation of the standalone motion binding contract.
namespace deren::gltf {
    namespace motion_detail {
        struct entry {
            deren::utility::vessel::string path;
            node const* value = nullptr;
            bool joint = false;
        };
        using entries = deren::utility::vessel::vector<entry>;

        std::expected<entries, std::string_view> skeleton(scenes const& model) {
            using namespace deren::utility;
            vessel::unordered_map<std::size_t, std::size_t> parents;
            for (auto const& scene : model.scene) {
                for (auto const& parent : scene.nodes) {
                    for (auto child : parent.children) {
                        if (child >= scene.nodes.size()) {
                            return std::unexpected("invalid skeleton child");
                        }
                        auto [it, inserted] = parents.try_emplace(scene.nodes[child].source_index, parent.source_index);
                        if (!inserted && it->second != parent.source_index) {
                            return std::unexpected("skeleton node has multiple parents");
                        }
                    }
                }
            }
            vessel::set<std::size_t> joints;
            vessel::set<std::size_t> selected;
            for (auto const& skin : model.skins) {
                joints.insert(skin.joints.begin(), skin.joints.end());
            }
            if (joints.empty()) {
                return std::unexpected("skeleton has no joints");
            }
            for (auto joint : joints) {
                vessel::set<std::size_t> visited;
                for (;;) {
                    if (!visited.insert(joint).second || !model.node_by_source.contains(joint)) {
                        return std::unexpected("missing or cyclic skeleton node");
                    }
                    selected.insert(joint);
                    auto const parent = parents.find(joint);
                    if (parent == parents.end()) {
                        break;
                    }
                    joint = parent->second;
                }
            }
            entries result;
            for (auto source : selected) {
                vessel::vector<std::string_view> names;
                auto ancestor = source;
                for (;;) {
                    auto const* n = model.node_by_source.at(ancestor);
                    if (!n || n->name.empty() || n->name.find('/') != vessel::string::npos) {
                        return std::unexpected("binding nodes need nonempty names without slashes");
                    }
                    names.push_back(n->name);
                    auto const parent = parents.find(ancestor);
                    if (parent == parents.end()) {
                        break;
                    }
                    ancestor = parent->second;
                }
                vessel::string path;
                for (auto it = names.rbegin(); it != names.rend(); ++it) {
                    path += '/';
                    path.append(*it);
                }
                result.push_back({std::move(path), model.node_by_source.at(source), joints.contains(source)});
            }
            std::sort(result.begin(), result.end(), [](entry const& a, entry const& b) { return a.path < b.path; });
            for (std::size_t i = 1; i < result.size(); ++i) {
                if (result[i - 1].path == result[i].path) {
                    return std::unexpected("ambiguous skeleton path");
                }
            }
            return result;
        }
    } // namespace motion_detail

    std::expected<deren::utility::vessel::string, std::string_view> skeleton_fingerprint(scenes const& model) {
        auto const entries = motion_detail::skeleton(model);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        // Matrix form handles TRS/matrix equivalence and quaternion signs. Quantization to
        // 1e-6 also canonicalizes negative zero; text integers avoid native byte order.
        deren::utility::vessel::string bytes = "deren-motion-rest-matrix-v1\n";
        for (auto const& e : *entries) {
            bytes += std::to_string(e.path.size());
            bytes += ':';
            bytes += e.path;
            bytes += e.joint ? ":1" : ":0";
            for (int32_t c = 0; c < 4; ++c) {
                for (int32_t r = 0; r < 4; ++r) {
                    double const v = e.value->local_transform[c][r];
                    if (!std::isfinite(v) || std::abs(v) > 1e9) {
                        return std::unexpected("invalid skeleton rest matrix");
                    }
                    bytes += ':';
                    bytes += std::to_string(std::llround(v * 1e6));
                }
            }
            bytes += '\n';
        }
        auto const digest = deren::utility::xxh3_128bits(
            std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
        return digest.to_hex_string();
    }

    std::expected<std::size_t, std::string_view> bind_motion(scenes& model, scenes const& motion,
                                                             std::string_view declared_hash) {
        auto const target_hash = skeleton_fingerprint(model);
        auto const motion_hash = skeleton_fingerprint(motion);
        if (!target_hash || !motion_hash || *target_hash != declared_hash || *motion_hash != declared_hash) {
            return std::unexpected("motion skeleton fingerprint mismatch");
        }
        auto const target_nodes = motion_detail::skeleton(model);
        auto const motion_nodes = motion_detail::skeleton(motion);
        if (target_nodes->size() != motion_nodes->size()) {
            return std::unexpected("motion skeleton node counts differ");
        }
        deren::utility::vessel::unordered_map<std::size_t, std::size_t> remap;
        for (std::size_t i = 0; i < target_nodes->size(); ++i) {
            if ((*target_nodes)[i].path != (*motion_nodes)[i].path) {
                return std::unexpected("motion skeleton paths differ");
            }
            remap.emplace((*motion_nodes)[i].value->source_index, (*target_nodes)[i].value->source_index);
        }
        if (motion.animations.empty()) {
            return std::unexpected("motion contains no animation");
        }
        // Validate the complete input before mutating the target.
        for (auto const& clip : motion.animations) {
            if (clip.channels.empty()) {
                return std::unexpected("motion contains an empty clip");
            }
            deren::utility::vessel::set<std::pair<std::size_t, int32_t>> targets;
            for (auto const& c : clip.channels) {
                if (!remap.contains(c.target_node) || c.sampler >= clip.samplers.size() ||
                    c.path == animation_path::weights ||
                    !targets.emplace(c.target_node, static_cast<int32_t>(c.path)).second) {
                    return std::unexpected("invalid or duplicate motion channel");
                }
                auto const& s = clip.samplers[c.sampler];
                auto const* target = model.node_by_source.at(remap.at(c.target_node));
                auto const trs = glm::translate(glm::mat4(1.0f), target->translation) *
                                 glm::mat4_cast(target->rotation) * glm::scale(glm::mat4(1.0f), target->scale);
                for (int32_t col = 0; col < 4; ++col) {
                    for (int32_t row = 0; row < 4; ++row) {
                        if (std::abs(trs[col][row] - target->local_transform[col][row]) > 1e-5f) {
                            return std::unexpected("animated target requires a decoded TRS rest pose");
                        }
                    }
                }
                auto const width = c.path == animation_path::rotation ? 4u : 3u;
                auto const factor = s.interpolation == animation_interpolation::cubic_spline ? 3u : 1u;
                if (s.times.empty() || s.per_key != width || s.values.size() != s.times.size() * width * factor) {
                    return std::unexpected("invalid motion sampler shape");
                }
                float previous = -1.0f;
                for (float t : s.times) {
                    if (!std::isfinite(t) || t < 0.0f || t <= previous) {
                        return std::unexpected("motion times must be finite and strictly increasing");
                    }
                    previous = t;
                }
                for (float v : s.values) {
                    if (!std::isfinite(v)) {
                        return std::unexpected("non-finite motion value");
                    }
                }
                if (width == 4) {
                    for (std::size_t k = 0; k < s.times.size(); ++k) {
                        auto const offset = (k * factor + (factor == 3 ? 1u : 0u)) * 4;
                        float norm = 0;
                        for (std::size_t j = 0; j < 4; ++j) {
                            norm += s.values[offset + j] * s.values[offset + j];
                        }
                        if (std::abs(norm - 1.0f) > 0.01f) {
                            return std::unexpected("motion rotation is not a unit quaternion");
                        }
                    }
                }
            }
        }
        for (auto clip : motion.animations) {
            for (auto& c : clip.channels) {
                c.target_node = remap.at(c.target_node);
            }
            model.animations.push_back(std::move(clip));
        }
        return motion.animations.size();
    }

    std::expected<std::size_t, std::string_view> load_motion(scenes& model, std::string_view file_name) {
        auto const path = std::filesystem::path(file_name);
        // This entry deliberately accepts JSON glTF only, so extras are verified before
        // the general loader decodes the animation buffers.
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return std::unexpected("cannot open motion glTF");
        }
        deren::utility::vessel::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        yyjson_doc* raw = yyjson_read(text.data(), text.size(), 0);
        if (!raw) {
            return std::unexpected("invalid motion glTF JSON");
        }
        std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(raw, yyjson_doc_free);
        auto* root = yyjson_doc_get_root(raw);
        auto* meta = yyjson_obj_get(yyjson_obj_get(root, "extras"), "deren_motion");
        auto* binding = yyjson_obj_get(meta, "binding");
        auto* hash = yyjson_obj_get(meta, "skeleton_hash");
        auto* joints = yyjson_obj_get(meta, "joints");
        if (!yyjson_is_str(binding) || std::string_view(yyjson_get_str(binding)) != "bip001-rest-v1" ||
            !yyjson_is_str(hash) || !yyjson_is_arr(joints) || yyjson_arr_size(joints) == 0 ||
            yyjson_arr_size(yyjson_obj_get(root, "meshes")) || yyjson_arr_size(yyjson_obj_get(root, "skins"))) {
            return std::unexpected("invalid standalone motion metadata");
        }
        auto motion = load_model(file_name);
        if (!motion) {
            return std::unexpected("cannot decode motion glTF");
        }
        skin joint_set;
        deren::utility::vessel::set<std::size_t> unique;
        for (std::size_t i = 0; i < yyjson_arr_size(joints); ++i) {
            auto* value = yyjson_arr_get(joints, i);
            auto const joint = yyjson_get_uint(value);
            if (!yyjson_is_uint(value) || !motion->node_by_source.contains(joint) || !unique.insert(joint).second) {
                return std::unexpected("invalid motion joint table");
            }
            joint_set.joints.push_back(joint);
        }
        motion->skins.push_back(std::move(joint_set));
        auto* clips = yyjson_obj_get(root, "animations");
        if (yyjson_arr_size(clips) != motion->animations.size()) {
            return std::unexpected("motion animations were not fully decoded");
        }
        for (std::size_t i = 0; i < motion->animations.size(); ++i) {
            auto* channels = yyjson_obj_get(yyjson_arr_get(clips, i), "channels");
            if (yyjson_arr_size(channels) != motion->animations[i].channels.size()) {
                return std::unexpected("motion channels were not fully decoded");
            }
        }
        return bind_motion(model, *motion, yyjson_get_str(hash));
    }
} // namespace deren::gltf
