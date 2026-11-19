// module version: 0.1.0
// Session-owned ids adapt the native shared audio objects to the managed wire table.
export module deren.scripting.audio_host;
export import deren.vstd;
export import deren.scripting.audio_api;
import deren.audio;
import deren.utility.shared;

export namespace deren::scripting {
    class audio_host {
    public:
        static bool begin_session() {
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (state.active) {
                return false;
            }
            state.active = true;
            return true;
        }
        static void end_session() {
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            state.active = false;
            state.audio.clear();
            state.buffers.clear();
        }
        static audio_api const& table() {
            static audio_api const value{load_buffer, release_buffer, create, create_from_file, clone,
                                         release, command, query, buffer_query};
            return value;
        }

    private:
        struct state {
            std::mutex mutex;
            bool active = false;
            std::uint64_t next_id = 1;
            utility::vessel::map<std::uint64_t, std::shared_ptr<deren::audio::audio_buffer>> buffers;
            utility::vessel::map<std::uint64_t, std::shared_ptr<deren::audio::audio>> audio;
        };
        static state& registry() {
            // Like the asset registry, storage outlives static mixer teardown. end_session owns cleanup.
            static auto* value = new state;
            return *value;
        }
        static std::expected<std::filesystem::path, script_status> path(utf8_view value) {
            if (!value.data || value.length == 0 || value.length > 32768 ||
                std::string_view(value.data, value.length).find('\0') != std::string_view::npos) {
                return std::unexpected(script_status::invalid_argument);
            }
            std::filesystem::path result(std::u8string(reinterpret_cast<char8_t const*>(value.data), value.length));
            std::error_code error;
            if (!std::filesystem::is_regular_file(result, error)) {
                return std::unexpected(script_status::asset_not_found);
            }
            return result;
        }
        template <class T>
        static script_status store(state& state, T& values, typename T::mapped_type value, std::uint64_t* output) {
            if (state.next_id == 0) {
                return script_status::script_failure;
            }
            auto const id = state.next_id++;
            values.emplace(id, std::move(value));
            *output = id;
            return script_status::success;
        }
        static script_status load_buffer(utf8_view file, std::uint64_t* output) {
            if (!output) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto resolved = path(file);
            if (!resolved) {
                return resolved.error();
            }
            auto result = deren::audio::load_audio_buffer_from_file(*resolved);
            if (!result) {
                return script_status::asset_invalid;
            }
            return store(state, state.buffers, std::move(*result), output);
        }
        static script_status release_buffer(std::uint64_t value) {
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            return state.buffers.erase(value) != 0 ? script_status::success : script_status::asset_invalid;
        }
        static script_status create(std::uint64_t buffer, std::uint64_t* output) {
            if (!output) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto it = state.buffers.find(buffer);
            if (it == state.buffers.end()) {
                return script_status::asset_invalid;
            }
            auto result = deren::audio::create(it->second);
            if (!result) {
                return script_status::module_unavailable;
            }
            return store(state, state.audio, std::move(*result), output);
        }
        static script_status create_from_file(utf8_view file, std::uint64_t* output) {
            if (!output) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto resolved = path(file);
            if (!resolved) {
                return resolved.error();
            }
            auto result = deren::audio::create_from_file(*resolved);
            if (!result) {
                return std::holds_alternative<deren::audio::audio_file_error>(result.error())
                           ? script_status::asset_invalid
                           : script_status::module_unavailable;
            }
            return store(state, state.audio, std::move(*result), output);
        }
        static script_status clone(std::uint64_t value, std::uint8_t clone_state, std::uint64_t* output) {
            if (!output || clone_state > 1) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto it = state.audio.find(value);
            if (it == state.audio.end()) {
                return script_status::asset_invalid;
            }
            auto result = it->second->clone(clone_state != 0);
            if (!result) {
                return script_status::script_failure;
            }
            return store(state, state.audio, std::move(*result), output);
        }
        static script_status release(std::uint64_t value) {
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            return state.audio.erase(value) != 0 ? script_status::success : script_status::asset_invalid;
        }
        static script_status command(std::uint64_t value, std::uint32_t command, double argument) {
            if (!std::isfinite(argument)) {
                return script_status::invalid_argument;
            }
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto it = state.audio.find(value);
            if (it == state.audio.end()) {
                return script_status::asset_invalid;
            }
            auto& audio = *it->second;
            bool accepted = false;
            switch (command) {
            case 0:
                accepted = audio.play();
                break;
            case 1:
                accepted = audio.pause();
                break;
            case 2:
                accepted = audio.stop();
                break;
            case 3:
                accepted = audio.seek(argument);
                break;
            case 4:
                accepted = argument >= 0 && argument <= std::numeric_limits<float>::max() && audio.set_volume(static_cast<float>(argument));
                break;
            case 5:
                accepted = argument >= -1 && argument <= 1 && audio.set_pan(static_cast<float>(argument));
                break;
            case 6:
                accepted = argument > 0 && argument <= std::numeric_limits<float>::max() && audio.set_pitch(static_cast<float>(argument));
                break;
            case 7:
                if (argument != 0 && argument != 1) {
                    return script_status::invalid_argument;
                }
                audio.set_looping(argument != 0);
                accepted = true;
                break;
            default:
                return script_status::invalid_argument;
            }
            return accepted ? script_status::success : script_status::invalid_argument;
        }
        static script_status query(std::uint64_t value, std::uint32_t query, double* output) {
            if (!output) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto it = state.audio.find(value);
            if (it == state.audio.end()) {
                return script_status::asset_invalid;
            }
            auto const& audio = *it->second;
            switch (query) {
            case 0:
                *output = audio.position();
                break;
            case 1:
                *output = audio.duration();
                break;
            case 2:
                *output = audio.is_playing() ? 1 : 0;
                break;
            case 3:
                *output = audio.at_end() ? 1 : 0;
                break;
            case 4:
                *output = audio.volume();
                break;
            case 5:
                *output = audio.pan();
                break;
            case 6:
                *output = audio.pitch();
                break;
            case 7:
                *output = audio.looping() ? 1 : 0;
                break;
            default:
                return script_status::invalid_argument;
            }
            return script_status::success;
        }
        static script_status buffer_query(std::uint64_t value, std::uint32_t query, double* output) {
            if (!output) {
                return script_status::invalid_argument;
            }
            *output = 0;
            auto& state = registry();
            std::lock_guard lock(state.mutex);
            if (!state.active) {
                return script_status::module_unavailable;
            }
            auto it = state.buffers.find(value);
            if (it == state.buffers.end()) {
                return script_status::asset_invalid;
            }
            auto const& buffer = *it->second;
            switch (query) {
            case 0:
                *output = buffer.channels();
                break;
            case 1:
                *output = buffer.sample_rate();
                break;
            case 2:
                *output = static_cast<double>(buffer.frame_count());
                break;
            case 3:
                *output = static_cast<double>(buffer.frame_count()) / buffer.sample_rate();
                break;
            default:
                return script_status::invalid_argument;
            }
            return script_status::success;
        }
    };
} // namespace deren::scripting
