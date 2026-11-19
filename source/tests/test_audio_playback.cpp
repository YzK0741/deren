#include "vk_test.h"

#include <cmath>
import deren.vstd;
import deren.audio;
extern "C" bool deren_audio_test_no_device();
extern "C" bool deren_audio_test_read_frames(float* output, std::uint64_t samples);
int run_tests() {
    namespace audio = deren::audio;
    CHECK(deren_audio_test_no_device());
    // File creation retains decoder error details and starts in the same stopped state as create().
    std::filesystem::path const fixtures = VR_AUDIO_FIXTURES;
    auto loaded = audio::create_from_file(fixtures / "tone.opus");
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(!(*loaded)->is_playing());
        CHECK((*loaded)->position() == 0.0);
        CHECK((*loaded)->get_buffer()->sample_rate() == 48000);
    }
    auto missing = audio::create_from_file(fixtures / "missing.opus");
    CHECK(!missing);
    if (!missing) {
        CHECK(std::holds_alternative<audio::audio_file_error>(missing.error()));
    }
    auto unsupported = audio::create_from_file(fixtures / "unsupported.xyz");
    CHECK(!unsupported);
    if (!unsupported) {
        CHECK(unsupported.error() == audio::audio_error{audio::audio_file_error::unsupported_type});
    }

    std::vector<float> pcm(48000 * 2, 0.125f);
    auto decoded = audio::audio_buffer::from_pcm(pcm, 2, 48000);
    CHECK(decoded.has_value());
    if (!decoded) {
        return deren::vk_test::finish("test_audio_playback");
    }
    auto buffer = std::make_shared<audio::audio_buffer>(std::move(*decoded));
    auto original = audio::create(buffer);
    CHECK(original.has_value());
    if (!original) {
        return deren::vk_test::finish("test_audio_playback");
    }
    CHECK(!audio::create(nullptr));
    CHECK((*original)->set_volume(0.5f));
    CHECK((*original)->set_pan(-0.2f));
    CHECK((*original)->set_pitch(1.25f));
    (*original)->set_looping(true);
    CHECK((*original)->seek(0.25));
    auto fresh = (*original)->clone();
    CHECK(fresh.has_value());
    if (fresh) {
        CHECK((*fresh)->get_buffer().get() == buffer.get());
        CHECK((*fresh)->position() == 0.0);
        CHECK(!(*fresh)->is_playing());
        CHECK((*fresh)->volume() == 1.0f);
        CHECK(!(*fresh)->looping());
    }
    auto paused = (*original)->clone(true);
    CHECK(paused.has_value());
    if (paused) {
        CHECK(std::abs((*paused)->position() - 0.25) < 1.0 / 48000);
        CHECK((*paused)->volume() == 0.5f);
        CHECK((*paused)->pan() == -0.2f);
        CHECK((*paused)->pitch() == 1.25f);
        CHECK((*paused)->looping());
        CHECK(!(*paused)->is_playing());
    }
    CHECK((*original)->play());
    auto playing = (*original)->clone(true);
    CHECK(playing.has_value());
    if (playing) {
        CHECK((*playing)->is_playing());
    }
    std::vector<float> output(256 * 2);
    CHECK(deren_audio_test_read_frames(output.data(), output.size()));
    CHECK((*original)->position() > 0.25);
    if (fresh) {
        CHECK((*fresh)->position() == 0.0);
    }
    if (paused) {
        CHECK(std::abs((*paused)->position() - 0.25) < 1.0 / 48000);
    }
    CHECK((*original)->pause());
    auto const stopped_position = (*original)->position();
    CHECK(deren_audio_test_read_frames(output.data(), output.size()));
    CHECK((*original)->position() == stopped_position);
    CHECK((*original)->stop());
    CHECK((*original)->position() == 0.0);
    CHECK(!(*original)->seek(-1));
    CHECK(!(*original)->seek(2));
    CHECK(!(*original)->set_volume(-1));
    CHECK(!(*original)->set_pitch(0));
    CHECK(!(*original)->set_pan(2));
    // Audio objects retain decoded samples after the caller releases its owner.
    std::weak_ptr<audio::audio_buffer> lifetime = buffer;
    buffer.reset();
    CHECK(!lifetime.expired());
    CHECK((*original)->play());
    original->reset();
    fresh = std::unexpected(audio::playback_error::invalid_buffer);
    paused = std::unexpected(audio::playback_error::invalid_buffer);
    playing = std::unexpected(audio::playback_error::invalid_buffer);
    CHECK(lifetime.expired());
    return deren::vk_test::finish("test_audio_playback");
}

namespace {
    int preview(std::filesystem::path const& file) {
        auto result = deren::audio::create_from_file(file);
        if (!result) {
            std::visit([](auto error) { std::fprintf(stderr, "Audio creation failed: stage=%s code=%u\n",
                                                     std::is_same_v<decltype(error), deren::audio::audio_file_error> ? "decode" : "playback", static_cast<unsigned>(error)); }, result.error());
            return 1;
        }
        auto& music = **result;
        music.set_volume(0.35f);
        auto const& buffer = *music.get_buffer();
        std::printf("Loaded: %u channels, %u Hz, %.2f seconds\n", buffer.channels(), buffer.sample_rate(), music.duration());
        if (!music.play()) {
            std::fprintf(stderr, "Could not start playback\n");
            return 2;
        }
        std::printf("Playing through the default output device at volume 0.35\n");
        std::fflush(stdout);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<long long>(music.duration()) + 15);
        double reported = -5;
        while (!music.at_end() && std::chrono::steady_clock::now() < deadline) {
            auto const position = music.position();
            if (position >= reported + 5) {
                std::printf("Position: %.2f / %.2f seconds\n", position, music.duration());
                std::fflush(stdout);
                reported = position;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!music.at_end()) {
            std::fprintf(stderr, "Playback did not advance to the end\n");
            return 3;
        }
        return 0;
    }
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc == 2) {
        return preview(std::filesystem::path(argv[1]));
    }
    return run_tests();
}
