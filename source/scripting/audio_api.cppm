// module version: 0.1.0
export module deren.scripting.audio_api;
export import deren.vstd;
export import deren.scripting.api_types;
export namespace deren::scripting {
    // Optional audio table. All ids are host-owned; no C++ object or PCM pointer crosses this boundary.
    struct audio_api {
        script_status (*load_buffer)(utf8_view path, std::uint64_t* buffer) = nullptr;
        script_status (*release_buffer)(std::uint64_t buffer) = nullptr;
        script_status (*create)(std::uint64_t buffer, std::uint64_t* audio) = nullptr;
        script_status (*create_from_file)(utf8_view path, std::uint64_t* audio) = nullptr;
        script_status (*clone)(std::uint64_t audio, std::uint8_t clone_state, std::uint64_t* copy) = nullptr;
        script_status (*release)(std::uint64_t audio) = nullptr;
        // command: play, pause, stop, seek, volume, pan, pitch, looping (0..7).
        script_status (*command)(std::uint64_t audio, std::uint32_t command, double value) = nullptr;
        // query: position, duration, playing, at_end, volume, pan, pitch, looping (0..7).
        script_status (*query)(std::uint64_t audio, std::uint32_t query, double* value) = nullptr;
        // buffer_query: channels, sample_rate, frame_count, duration (0..3).
        script_status (*buffer_query)(std::uint64_t buffer, std::uint32_t query, double* value) = nullptr;
    };
    static_assert(sizeof(audio_api) == 9 * sizeof(void*));

} // namespace deren::scripting
