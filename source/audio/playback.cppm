module;
#include "miniaudio.h"

// module version: 0.1.0
// Each audio owns its sound and cursor; clones share immutable PCM storage.
export module deren.audio:playback;
export import deren.vstd;
import :engine;
export import :declarations;
import :audio_load;

export namespace deren::audio {
    class audio;
    std::expected<std::shared_ptr<audio>, playback_error> create(std::shared_ptr<audio_buffer const> buffer);
    std::expected<std::shared_ptr<audio>, audio_error> create_from_file(std::filesystem::path const& file);
    // Control calls on each audio object are serialized by the caller. The engine's mixer runs on the audio thread.
    // Initialized miniaudio objects keep stable addresses; clone creates a new instance.
    class audio {
    public:
        ~audio();
        audio(audio const&) = delete;
        audio& operator=(audio const&) = delete;
        audio(audio&&) = delete;
        audio& operator=(audio&&) = delete;

        // false: defaults, cursor zero, stopped. true: settings, cursor and running state.
        std::expected<std::shared_ptr<audio>, playback_error> clone(bool clone_state = false) const;
        bool play() noexcept;
        bool pause() noexcept;
        bool stop() noexcept;
        bool seek(double seconds) noexcept;
        double position() const noexcept;
        double duration() const noexcept;
        bool is_playing() const noexcept;
        bool at_end() const noexcept;
        bool set_volume(float value) noexcept;
        float volume() const noexcept;
        bool set_pan(float value) noexcept;
        float pan() const noexcept;
        bool set_pitch(float value) noexcept;
        float pitch() const noexcept;
        void set_looping(bool value) noexcept;
        bool looping() const noexcept;
        std::shared_ptr<audio_buffer const> const& get_buffer() const noexcept {
            return this->buffer;
        }

    private:
        friend std::expected<std::shared_ptr<audio>, playback_error> create(std::shared_ptr<audio_buffer const> buffer);
        explicit audio(std::shared_ptr<audio_buffer const> buffer)
            : buffer(std::move(buffer)) {
        }
        std::shared_ptr<audio_buffer const> buffer;
        struct playback_source {
            ma_data_source_base base{};
            ma_audio_buffer_ref data{};
            // Only the mixer mutates data.cursor. Queries from the control thread read this snapshot.
            std::atomic<std::uint64_t> cursor{0};
            static ma_result read(ma_data_source* opaque, void* output, ma_uint64 count, ma_uint64* read) {
                auto& self = *static_cast<playback_source*>(opaque);
                auto const actual = ma_audio_buffer_ref_read_pcm_frames(&self.data, output, count, MA_FALSE);
                self.cursor.store(self.data.cursor, std::memory_order_relaxed);
                if (read) {
                    *read = actual;
                }
                return actual != 0 ? MA_SUCCESS : MA_AT_END;
            }
            static ma_result seek(ma_data_source* opaque, ma_uint64 frame) {
                auto& self = *static_cast<playback_source*>(opaque);
                auto const result = ma_audio_buffer_ref_seek_to_pcm_frame(&self.data, frame);
                self.cursor.store(self.data.cursor, std::memory_order_relaxed);
                return result;
            }
            static ma_result format(ma_data_source* opaque, ma_format* format, ma_uint32* channels,
                                    ma_uint32* rate, ma_channel* map, std::size_t capacity) {
                auto& self = *static_cast<playback_source*>(opaque);
                return ma_data_source_get_data_format(&self.data, format, channels, rate, map, capacity);
            }
            static ma_result position(ma_data_source* opaque, ma_uint64* cursor) {
                auto& self = *static_cast<playback_source*>(opaque);
                *cursor = self.cursor.load(std::memory_order_relaxed);
                return MA_SUCCESS;
            }
            static ma_result length(ma_data_source* opaque, ma_uint64* frames) {
                auto& self = *static_cast<playback_source*>(opaque);
                *frames = self.data.sizeInFrames;
                return MA_SUCCESS;
            }
            inline static ma_data_source_vtable const callbacks{read, seek, format, position, length, nullptr, 0};
        } source;

        ma_sound sound{};
        bool source_initialized = false;
        bool sound_initialized = false;
    };

    std::expected<std::shared_ptr<audio>, playback_error> create(std::shared_ptr<audio_buffer const> buffer) {
        if (!buffer || !buffer->frames() || buffer->frame_count() == 0 || buffer->channels() == 0 || buffer->sample_rate() == 0) {
            return std::unexpected(playback_error::invalid_buffer);
        }
        auto* engine = detail::engine::instance().native();
        if (!engine) {
            return std::unexpected(playback_error::engine_init_failed);
        }
        std::shared_ptr<audio> result(new audio(std::move(buffer)));
        auto const& data = *result->buffer;
        if (ma_audio_buffer_ref_init(ma_format_f32, data.channels(), data.frames(), data.frame_count(), &result->source.data) != MA_SUCCESS) {
            return std::unexpected(playback_error::source_init_failed);
        }
        auto source_config = ma_data_source_config_init();
        source_config.vtable = &audio::playback_source::callbacks;
        if (ma_data_source_init(&source_config, &result->source) != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&result->source.data);
            return std::unexpected(playback_error::source_init_failed);
        }
        result->source_initialized = true;
        // The ref API initializes sampleRate to zero; provide the decoded stream's actual rate.
        result->source.data.sampleRate = data.sample_rate();
        if (ma_sound_init_from_data_source(engine, &result->source, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                           nullptr, &result->sound) != MA_SUCCESS) {
            return std::unexpected(playback_error::audio_init_failed);
        }
        result->sound_initialized = true;
        return result;
    }
    std::expected<std::shared_ptr<audio>, audio_error> create_from_file(std::filesystem::path const& file) {
        auto decoded = load_audio_buffer_from_file(file);
        if (!decoded) {
            return std::unexpected(audio_error{decoded.error()});
        }
        auto result = create(std::move(*decoded));
        if (!result) {
            return std::unexpected(audio_error{result.error()});
        }
        return std::move(*result);
    }
    audio::~audio() {
        if (this->sound_initialized) {
            ma_sound_uninit(&this->sound);
        }
        if (this->source_initialized) {
            ma_data_source_uninit(&this->source);
            ma_audio_buffer_ref_uninit(&this->source.data);
        }
        // Member owners release after sound detachment, keeping PCM alive through teardown; the singleton outlives normal audio owners.
    }
    std::expected<std::shared_ptr<audio>, playback_error> audio::clone(bool clone_state) const {
        auto result = deren::audio::create(this->buffer);
        if (!result || !clone_state) {
            return result;
        }
        ma_uint64 cursor = 0;
        if (ma_sound_get_cursor_in_pcm_frames(&this->sound, &cursor) != MA_SUCCESS) {
            return std::unexpected(playback_error::state_copy_failed);
        }
        auto& copy = **result;
        copy.set_volume(this->volume());
        copy.set_pan(this->pan());
        copy.set_pitch(this->pitch());
        copy.set_looping(this->looping());
        // Capture a moment in time; separate audio objects are not sample-synchronized by cloning.
        if (ma_sound_seek_to_pcm_frame(&copy.sound, cursor) != MA_SUCCESS ||
            (this->is_playing() && !copy.play())) {
            return std::unexpected(playback_error::state_copy_failed);
        }
        return result;
    }
    bool audio::play() noexcept {
        if (this->at_end() && ma_sound_seek_to_pcm_frame(&this->sound, 0) != MA_SUCCESS) {
            return false;
        }
        return ma_sound_start(&this->sound) == MA_SUCCESS;
    }
    bool audio::pause() noexcept {
        return ma_sound_stop(&this->sound) == MA_SUCCESS;
    }
    bool audio::stop() noexcept {
        return ma_sound_stop(&this->sound) == MA_SUCCESS && ma_sound_seek_to_pcm_frame(&this->sound, 0) == MA_SUCCESS;
    }
    bool audio::seek(double seconds) noexcept {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > this->duration()) {
            return false;
        }
        auto const frames = std::min(this->buffer->frame_count(), static_cast<std::uint64_t>(seconds * this->buffer->sample_rate()));
        return ma_sound_seek_to_pcm_frame(&this->sound, frames) == MA_SUCCESS;
    }
    double audio::position() const noexcept {
        ma_uint64 cursor = 0;
        if (ma_sound_get_cursor_in_pcm_frames(&this->sound, &cursor) != MA_SUCCESS) {
            return 0;
        }
        return static_cast<double>(cursor) / this->buffer->sample_rate();
    }
    double audio::duration() const noexcept {
        return static_cast<double>(this->buffer->frame_count()) / this->buffer->sample_rate();
    }
    bool audio::is_playing() const noexcept {
        return ma_sound_is_playing(&this->sound) && !this->at_end();
    }
    bool audio::at_end() const noexcept {
        ma_uint64 cursor = 0;
        return ma_sound_get_cursor_in_pcm_frames(&this->sound, &cursor) == MA_SUCCESS && cursor >= this->buffer->frame_count();
    }
    bool audio::set_volume(float value) noexcept {
        if (!std::isfinite(value) || value < 0) {
            return false;
        }
        ma_sound_set_volume(&this->sound, value);
        return true;
    }
    float audio::volume() const noexcept {
        return ma_sound_get_volume(&this->sound);
    }
    bool audio::set_pan(float value) noexcept {
        if (!std::isfinite(value) || value < -1 || value > 1) {
            return false;
        }
        ma_sound_set_pan(&this->sound, value);
        return true;
    }
    float audio::pan() const noexcept {
        return ma_sound_get_pan(&this->sound);
    }
    bool audio::set_pitch(float value) noexcept {
        if (!std::isfinite(value) || value <= 0) {
            return false;
        }
        ma_sound_set_pitch(&this->sound, value);
        return true;
    }
    float audio::pitch() const noexcept {
        return ma_sound_get_pitch(&this->sound);
    }
    void audio::set_looping(bool value) noexcept {
        ma_sound_set_looping(&this->sound, value);
    }
    bool audio::looping() const noexcept {
        return ma_sound_is_looping(&this->sound);
    }
} // namespace deren::audio
