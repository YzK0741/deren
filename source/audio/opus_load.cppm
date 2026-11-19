module;
#include <opusfile.h>
export module deren.audio:opus_load;
export import deren.vstd;
import :declarations;
import :decode_helpers;
export namespace deren::audio {
    std::expected<audio_buffer, audio_file_error> opus_load(std::filesystem::path const& file) {
        auto bytes = read_audio_bytes(file);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        int error = 0;
        std::unique_ptr<OggOpusFile, decltype(&op_free)> decoder(op_open_memory(bytes->data(), bytes->size(), &error), &op_free);
        if (!decoder) {
            return std::unexpected(audio_file_error::decoder_init_failed);
        }
        int const channels = op_channel_count(decoder.get(), 0);
        if (channels <= 0 || channels > 255) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        // A single ma_audio_buffer needs one channel layout across every chained link.
        for (int i = 0; i < op_link_count(decoder.get()); ++i) {
            if (op_channel_count(decoder.get(), i) != channels) {
                return std::unexpected(audio_file_error::unsupported_type);
            }
        }
        auto const expected = op_pcm_total(decoder.get(), -1);
        if (expected <= 0 || static_cast<std::uint64_t>(expected) > max_pcm_samples / channels) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        utility::vessel::vector<float> samples;
        samples.reserve(static_cast<std::size_t>(expected) * channels);
        for (;;) {
            auto const base = samples.size();
            auto const count = 4096u * channels;
            if (base > max_pcm_samples - count) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            samples.resize(base + count);
            int const read = op_read_float(decoder.get(), samples.data() + base, count, nullptr);
            if (read < 0) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            samples.resize(base + static_cast<std::size_t>(read) * channels);
            if (read == 0) {
                break;
            }
        }
        if (samples.size() / channels != static_cast<std::size_t>(expected)) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        return audio_buffer::from_pcm(samples, channels, 48000);
    }
} // namespace deren::audio
