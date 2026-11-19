module;
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
export module deren.audio:vorbis_load;
export import deren.vstd;
import :declarations;
import :decode_helpers;
export namespace deren::audio {
    std::expected<audio_buffer, audio_file_error> vorbis_load(std::filesystem::path const& file) {
        auto bytes = read_audio_bytes(file);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        int error = 0;
        std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> decoder(
            stb_vorbis_open_memory(bytes->data(), static_cast<int>(bytes->size()), &error, nullptr), &stb_vorbis_close);
        if (!decoder) {
            return std::unexpected(audio_file_error::decoder_init_failed);
        }
        auto const info = stb_vorbis_get_info(decoder.get());
        if (info.channels <= 0 || info.channels > 256 || info.sample_rate == 0) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        utility::vessel::vector<float> samples;
        for (;;) {
            auto const base = samples.size();
            auto const count = 4096u * info.channels;
            if (base > max_pcm_samples - count) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            samples.resize(base + count);
            int const read = stb_vorbis_get_samples_float_interleaved(decoder.get(), info.channels, samples.data() + base, count);
            samples.resize(base + static_cast<std::size_t>(read) * info.channels);
            if (stb_vorbis_get_error(decoder.get()) != VORBIS__no_error) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            if (read == 0) {
                break;
            }
        }
        auto const expected = stb_vorbis_stream_length_in_samples(decoder.get());
        if (expected != 0 && samples.size() / info.channels != expected) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        return audio_buffer::from_pcm(samples, info.channels, info.sample_rate);
    }
} // namespace deren::audio
