// deren.audio:audio_load: routing a file to the decoder that handles its type.
//
// The switch here is the whole job: classify the extension, call that type's loader, and report
// unsupported_type for anything else. The loaders themselves live in their own partitions
// (miniaudio_load for wav / mp3 / flac).
export module deren.audio:audio_load;
export import deren.vstd;

import :declarations;
import :miniaudio_load;
import :vorbis_load;
import :opus_load;
import :qoa_load;

namespace deren::audio {
    namespace {
        // The formats there is a loader for. Anything else is the switch's default: it is not a
        // format we can load, so it has no entry of its own.
        enum class file_type : std::uint8_t {
            mp3,
            wav,
            flac,
            ogg,
            opus,
            qoa,
            other, // any extension that is not one of the formats above
        };

        file_type type_of(std::string extension) {
            for (char& character : extension) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (extension == ".mp3") {
                return file_type::mp3;
            }
            if (extension == ".wav") {
                return file_type::wav;
            }
            if (extension == ".flac") {
                return file_type::flac;
            }
            if (extension == ".ogg" || extension == ".oga") {
                return file_type::ogg;
            }
            if (extension == ".opus") {
                return file_type::opus;
            }
            if (extension == ".qoa") {
                return file_type::qoa;
            }
            return file_type::other;
        }
    } // namespace
} // namespace deren::audio

namespace deren::audio {
    // Codec implementations return an owning value; the public boundary publishes one shared owner.
    std::expected<audio_buffer, audio_file_error> decode_audio_file(std::filesystem::path const& file) {
        switch (type_of(file.extension().string())) {
        case file_type::mp3:
        case file_type::wav:
        case file_type::flac:
            return miniaudio_load(file);
        case file_type::ogg: {
            auto result = opus_load(file);
            if (result || result.error() != audio_file_error::decoder_init_failed) {
                return result;
            }
            return vorbis_load(file);
        }
        case file_type::opus:
            return opus_load(file);
        case file_type::qoa:
            return qoa_load(file);
        default:
            return std::unexpected(audio_file_error::unsupported_type);
        }
    }
} // namespace deren::audio

export namespace deren::audio {
    std::expected<std::shared_ptr<audio_buffer>, audio_file_error> load_audio_buffer_from_file(std::filesystem::path const& file) {
        auto decoded = decode_audio_file(file);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return std::make_shared<audio_buffer>(std::move(*decoded));
    }
} // namespace deren::audio
