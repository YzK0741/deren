// Shared bounds and Unicode-aware file reading for the codec partitions.
export module deren.audio:decode_helpers;
export import deren.vstd;
export import deren.utility.shared;
import deren.utility;
import :declarations;
export namespace deren::audio {
    // Bound whole-file decoding before allocating from untrusted stream metadata.
    inline constexpr std::size_t max_pcm_samples = 128u * 1024u * 1024u;
    std::expected<utility::vessel::vector<std::uint8_t>, audio_file_error> read_audio_bytes(std::filesystem::path const& file) {
        auto bytes = utility::read_binary_to_vector(file, 256u * 1024u * 1024u);
        if (!bytes || bytes->empty()) {
            return std::unexpected(audio_file_error::decoder_init_failed);
        }
        return std::move(*bytes);
    }
} // namespace deren::audio
