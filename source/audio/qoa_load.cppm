module;
#include <qoa.h>
export module deren.audio:qoa_load;
export import deren.vstd;
import :declarations;
import :decode_helpers;
export namespace deren::audio {
    std::expected<audio_buffer, audio_file_error> qoa_load(std::filesystem::path const& file) {
        auto bytes = read_audio_bytes(file);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        qoa_desc desc{};
        auto offset = qoa_decode_header(bytes->data(), static_cast<int>(bytes->size()), &desc);
        if (offset == 0) {
            return std::unexpected(audio_file_error::decoder_init_failed);
        }
        if (desc.samples > max_pcm_samples / desc.channels) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        // A QOA slice encodes at most 20 samples in 8 bytes. Reject impossible lengths before allocating.
        if (static_cast<std::uint64_t>(desc.samples) * desc.channels > bytes->size() / 8 * 20) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        utility::vessel::vector<float> samples;
        samples.reserve(static_cast<std::size_t>(desc.samples) * desc.channels);
        utility::vessel::vector<short> frame(QOA_FRAME_LEN * desc.channels);
        std::uint32_t decoded = 0;
        while (decoded < desc.samples) {
            unsigned int count = 0;
            auto const consumed = qoa_decode_frame(bytes->data() + offset, static_cast<unsigned int>(bytes->size() - offset), &desc, frame.data(), &count);
            if (consumed == 0 || count == 0 || count > desc.samples - decoded ||
                (count != QOA_FRAME_LEN && decoded + count != desc.samples)) {
                return std::unexpected(audio_file_error::decode_failed);
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(count) * desc.channels; ++i) {
                samples.push_back(frame[i] / 32768.0f);
            }
            offset += consumed;
            decoded += count;
        }
        if (offset != bytes->size()) {
            return std::unexpected(audio_file_error::decode_failed);
        }
        return audio_buffer::from_pcm(samples, desc.channels, desc.samplerate);
    }
} // namespace deren::audio
