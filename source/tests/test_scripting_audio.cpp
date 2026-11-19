#include "vk_test.h"
import deren.vstd;
import deren.scripting.audio_host;
extern "C" bool deren_audio_test_no_device();
int main() {
    using namespace deren::scripting;
    CHECK(deren_audio_test_no_device());
    bool const active = audio_host::begin_session();
    CHECK(active);
    if (!active) {
        return deren::vk_test::finish("test_scripting_audio");
    }
    CHECK(!audio_host::begin_session());
    auto const& api = audio_host::table();
    CHECK(api.load_buffer && api.create && api.clone && api.command && api.query && api.release && api.buffer_query);
    if (!api.load_buffer || !api.create || !api.clone || !api.query || !api.command || !api.release || !api.buffer_query) {
        audio_host::end_session();
        return deren::vk_test::finish("test_scripting_audio");
    }
    auto path = (std::filesystem::path(VR_AUDIO_FIXTURES) / "tone.opus").u8string();
    utf8_view file{reinterpret_cast<char const*>(path.data()), static_cast<std::uint32_t>(path.size())};
    std::uint64_t buffer = 0, original = 0, copy = 0;
    CHECK(api.load_buffer(file, &buffer) == script_status::success && buffer != 0);
    CHECK(api.create(buffer, &original) == script_status::success && original != 0);
    CHECK(api.command(buffer, 0, 0) == script_status::asset_invalid);
    CHECK(api.release_buffer(buffer) == script_status::success);
    CHECK(api.release_buffer(buffer) == script_status::asset_invalid);
    CHECK(api.command(original, 3, 0.025) == script_status::success);
    CHECK(api.clone(original, 1, &copy) == script_status::success);
    double cursor = 0;
    CHECK(api.query(copy, 0, &cursor) == script_status::success && cursor > 0.02);
    CHECK(api.command(original, 3, 0) == script_status::success);
    CHECK(api.query(copy, 0, &cursor) == script_status::success && cursor > 0.02);
    CHECK(api.command(copy, 3, -1) == script_status::invalid_argument);
    CHECK(api.query(copy, 99, &cursor) == script_status::invalid_argument);
    CHECK(api.clone(copy, 2, &buffer) == script_status::invalid_argument);
    CHECK(api.release(original) == script_status::success);
    CHECK(api.query(original, 0, &cursor) == script_status::asset_invalid);
    audio_host::end_session();
    CHECK(api.query(copy, 0, &cursor) == script_status::module_unavailable);
    CHECK(audio_host::begin_session());
    CHECK(api.query(copy, 0, &cursor) == script_status::asset_invalid);
    audio_host::end_session();
    return deren::vk_test::finish("test_scripting_audio");
}
