module;

#include "miniaudio.h"

// deren.audio:declarations: the audio module's public types.
//
// Nothing here loads a file: audio_load routes a path to the loader that handles it, and
// miniaudio_load is that loader's side of the hand-off. This unit owns only what the two halves
// share - the failure enum and the decoded buffer.
export module deren.audio:declarations;
export import deren.vstd;

export namespace deren::audio {
    // Why a load failed. The enum is deliberately small: a caller either retries with another
    // file or reports the failure, so the raw decoder result is not carried up.
    enum class audio_file_error : std::uint8_t {
        unsupported_type,    // there is no decoder for this type
        decoder_init_failed, // the file could not be opened or its header was rejected
        decode_failed,       // the stream broke while frames were being read
        buffer_init_failed,  // the decoded frames could not be copied into the buffer
    };

    enum class playback_error : std::uint8_t {
        invalid_buffer,
        engine_init_failed,
        source_init_failed,
        audio_init_failed,
        state_copy_failed
    };

    // Preserve whether a file factory failed during decoding or playback initialization.
    using audio_error = std::variant<audio_file_error, playback_error>;

    // An in-memory, interleaved f32 buffer of decoded audio: raii wrapper of ma_audio_buffer.
    // Move-only: the samples live in one allocation with exactly one owner, and a moved-from
    // buffer must not be read.
    class audio_buffer {
        ma_audio_buffer buffer{};

    public:
        // THE EMPTY BUFFER, and the factory's result assigned to it.
        audio_buffer() noexcept = default;
        explicit audio_buffer(ma_audio_buffer&& buffer) noexcept;
        static std::expected<audio_buffer, audio_file_error> from_pcm(std::span<float const> samples, std::uint32_t channels, std::uint32_t sample_rate);

        [[nodiscard]] std::uint32_t channels() const noexcept;
        [[nodiscard]] std::uint32_t sample_rate() const noexcept;
        [[nodiscard]] std::uint64_t frame_count() const noexcept;
        // Interleaved frames: frame_count() * channels() floats.
        [[nodiscard]] float const* frames() const noexcept;

        void release() noexcept;
        ~audio_buffer() noexcept;

        audio_buffer(audio_buffer&) = delete ("audio_buffer owns a miniaudio buffer; construct a distinct buffer or move it with std::move");
        audio_buffer(audio_buffer&& other) noexcept;
        audio_buffer& operator=(audio_buffer&) = delete ("audio_buffer owns a miniaudio buffer; move-assign it with std::move");
        audio_buffer& operator=(audio_buffer&& other) noexcept;
    };
} // namespace deren::audio

namespace deren::audio {
    audio_buffer::audio_buffer(ma_audio_buffer&& buffer) noexcept
        : buffer(buffer) {
        this->buffer.ref.ds.pCurrent = &this->buffer;
        buffer = {};
    }

    audio_buffer::audio_buffer(audio_buffer&& other) noexcept
        : buffer(other.buffer) {
        this->buffer.ref.ds.pCurrent = &this->buffer;
        other.buffer = {};
    }

    audio_buffer& audio_buffer::operator=(audio_buffer&& other) noexcept {
        if (this != &other) {
            release();
            buffer = other.buffer;
            this->buffer.ref.ds.pCurrent = &this->buffer;
            other.buffer = {};
        }
        return *this;
    }

    audio_buffer::~audio_buffer() noexcept {
        release();
    }

    void audio_buffer::release() noexcept {
        ma_audio_buffer_uninit(&buffer);
        buffer = {};
    }

    std::uint32_t audio_buffer::channels() const noexcept {
        return buffer.ref.channels;
    }

    std::uint32_t audio_buffer::sample_rate() const noexcept {
        return buffer.ref.sampleRate;
    }

    std::uint64_t audio_buffer::frame_count() const noexcept {
        return buffer.ref.sizeInFrames;
    }

    float const* audio_buffer::frames() const noexcept {
        return static_cast<float const*>(buffer.ref.pData);
    }
} // namespace deren::audio

namespace deren::audio {
    std::expected<audio_buffer, audio_file_error> audio_buffer::from_pcm(std::span<float const> samples, std::uint32_t channels, std::uint32_t sample_rate) {
        if (channels == 0 || sample_rate == 0 || samples.empty() || samples.size() % channels != 0) {
            return std::unexpected(audio_file_error::buffer_init_failed);
        }
        auto config = ma_audio_buffer_config_init(ma_format_f32, channels, samples.size() / channels, samples.data(), nullptr);
        config.sampleRate = sample_rate;
        ma_audio_buffer native{};
        if (ma_audio_buffer_init_copy(&config, &native) != MA_SUCCESS) {
            return std::unexpected(audio_file_error::buffer_init_failed);
        }
        return audio_buffer(std::move(native));
    }
} // namespace deren::audio
