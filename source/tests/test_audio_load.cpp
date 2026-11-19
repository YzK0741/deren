#include "vk_test.h"

#include <cmath>
import deren.vstd;
import deren.audio;
int main() {
    namespace audio = deren::audio;
    std::filesystem::path const fixtures = VR_AUDIO_FIXTURES;
    for (auto const* name : {"tone.wav", "tone.mp3", "tone.flac", "tone.ogg", "tone.opus", "tone.qoa"}) {
        auto result = audio::load_audio_buffer_from_file(fixtures / name);
        CHECK_MSG(result.has_value(), name);
        if (!result) {
            continue;
        }
        CHECK(*result != nullptr);
        if (!*result) {
            continue;
        }
        auto shared = *result;
        CHECK(shared.get() == result->get());
        CHECK(shared.use_count() == 2);
        auto& buffer = **result;
        CHECK(buffer.channels() == 2);
        CHECK(buffer.sample_rate() == 48000);
        CHECK(buffer.frame_count() >= 4000 && buffer.frame_count() <= 8000);
        double energy = 0;
        for (std::uint64_t i = 0; i < buffer.frame_count() * buffer.channels(); ++i) {
            CHECK(std::isfinite(buffer.frames()[i]));
            energy += buffer.frames()[i] * buffer.frames()[i];
        }
        CHECK(energy > 1.0);
        auto const count = buffer.frame_count();
        auto moved = std::move(buffer);
        CHECK(moved.frame_count() == count);
        CHECK(buffer.frames() == nullptr);
        audio::audio_buffer assigned;
        assigned = std::move(moved);
        CHECK(assigned.frame_count() == count);
        CHECK(moved.frames() == nullptr);
        assigned.release();
        assigned.release();
        CHECK(assigned.frames() == nullptr);
    }
    CHECK(audio::load_audio_buffer_from_file(fixtures / "block.wav").has_value());
    CHECK(!audio::load_audio_buffer_from_file(fixtures / "missing.opus"));
    CHECK(!audio::load_audio_buffer_from_file(fixtures / "missing.ogg"));
    CHECK(!audio::load_audio_buffer_from_file(fixtures / "missing.qoa"));
    CHECK(!audio::load_audio_buffer_from_file(fixtures / "tone.unknown"));
    return deren::vk_test::finish("test_audio_load");
}
