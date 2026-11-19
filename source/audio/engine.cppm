module;
#include "miniaudio.h"

// module version: 0.1.0
// One lazily initialized device/mixer for this process. This partition is internal.
export module deren.audio:engine;
import deren.vstd;

namespace deren::audio::detail {
#ifdef DEREN_AUDIO_TESTING
    struct engine_test_settings {
        bool no_device = false;
        bool opened = false;
    };
    engine_test_settings& test_settings() {
        static engine_test_settings settings;
        return settings;
    }
#endif
    class engine {
    public:
        static engine& instance() {
            static engine value;
            return value;
        }
        engine(engine const&) = delete;
        engine& operator=(engine const&) = delete;
        engine(engine&&) = delete;
        engine& operator=(engine&&) = delete;
        ma_engine* native() noexcept {
            return this->initialized ? &this->mixer : nullptr;
        }

    private:
        engine() {
            auto config = ma_engine_config_init();
#ifdef DEREN_AUDIO_TESTING
            auto& settings = test_settings();
            settings.opened = true;
            if (settings.no_device) {
                config.noDevice = true;
                config.channels = 2;
                config.sampleRate = 48000;
            }
#endif
            this->initialized = ma_engine_init(&config, &this->mixer) == MA_SUCCESS;
        }
        ~engine() {
            if (this->initialized) {
                ma_engine_uninit(&this->mixer);
            }
        }
        ma_engine mixer{};
        bool initialized = false;
    };
} // namespace deren::audio::detail

#ifdef DEREN_AUDIO_TESTING
// No exported scripting/C++ API: headless tests select manual mixing before first create().
extern "C" bool deren_audio_test_no_device() {
    auto& settings = deren::audio::detail::test_settings();
    if (settings.opened) {
        return false;
    }
    settings.no_device = true;
    return true;
}
extern "C" bool deren_audio_test_read_frames(float* output, std::uint64_t samples) {
    auto const& settings = deren::audio::detail::test_settings();
    if (!settings.opened || !settings.no_device || !output) {
        return false;
    }
    auto* mixer = deren::audio::detail::engine::instance().native();
    if (!mixer) {
        return false;
    }
    auto const channels = ma_engine_get_channels(mixer);
    if (samples == 0 || channels == 0 || samples % channels != 0) {
        return false;
    }
    ma_uint64 read = 0;
    auto const count = samples / channels;
    return ma_engine_read_pcm_frames(mixer, output, count, &read) == MA_SUCCESS && read == count;
}
#endif
