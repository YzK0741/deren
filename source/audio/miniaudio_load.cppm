module;

#include "miniaudio.h"

// deren.audio:miniaudio_load: the wav / mp3 / flac loader.
//
// One function: read every frame of a file through miniaudio's decoder and hand them back as an
// audio_buffer. The routing (which extension goes here) is audio_load's job, not this unit's.
export module deren.audio:miniaudio_load;
export import deren.vstd;

import :declarations;
import :decode_helpers;

export namespace deren::audio {
    // Loads `file` completely. The frames are always f32 at the stream's own channel count and
    // sample rate.
    std::expected<audio_buffer, audio_file_error> miniaudio_load(std::filesystem::path const& file) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);

        ma_decoder decoder{};
        auto bytes = read_audio_bytes(file);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        ma_result const opened = ma_decoder_init_memory(bytes->data(), bytes->size(), &config, &decoder);
        if (opened != MA_SUCCESS) {
            return std::unexpected(audio_file_error::decoder_init_failed);
        }
        auto const close_decoder = [](ma_decoder* open_decoder) { ma_decoder_uninit(open_decoder); };
        std::unique_ptr<ma_decoder, decltype(close_decoder)> const decoder_guard(&decoder, close_decoder);

        // The decoder's reported length is not trusted: a short read is the end of the stream,
        // which is also what mp3 needs.
        utility::vessel::vector<float> frames;
        std::uint32_t const channels = decoder.outputChannels;
        for (;;) {
            std::size_t const base = frames.size();
            if (channels == 0 || channels > 256 || base > max_pcm_samples - 4096u * channels) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            frames.resize(base + 4096u * channels);
            ma_uint64 read = 0;
            ma_result const result = ma_decoder_read_pcm_frames(&decoder, frames.data() + base, 4096, &read);
            frames.resize(base + static_cast<std::size_t>(read) * channels);
            if (result != MA_SUCCESS && result != MA_AT_END) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            if (read < 4096) {
                break;
            }
        }
        if (frames.empty()) {
            return std::unexpected(audio_file_error::decode_failed);
        }

        return audio_buffer::from_pcm(frames, channels, decoder.outputSampleRate);
    }
} // namespace deren::audio
