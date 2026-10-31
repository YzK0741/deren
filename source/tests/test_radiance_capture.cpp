#include "../app/radiance_cube_capture.hpp"
#include "../engine/lighting/reflection_probe.hpp"
#include "vk_test.h"

#include <array>
#include <chrono>
#include <fstream>
#include <vector>
import deren.engine.runtime;
import deren.engine.render_layout;
#include "probe_backend.hpp"

int main() {
    namespace lp = deren::engine::probe_detail;
    lp::box box{{0, 0, 0}, {-2, -2, -2}, {2, 2, 2}, .5f, 1};
    CHECK(lp::valid(box));
    CHECK(glm::length(lp::lookup(box, {1, 0, 0}, {0, 0, 1}).direction - glm::vec3(1, 0, 2)) < 1e-5f);
    CHECK(lp::lookup(box, {3, 0, 0}, {1, 0, 0}).weight == 0);
    CHECK(lp::lookup(box, {1.75f, 0, 0}, {1, 0, 0}).weight == .5f);
    CHECK(lp::lookup(box, {0, 0, 0}, {0, 0, 0}).weight == 0);
    CHECK(!lp::decode(std::span<std::byte const>{}));
    for (auto direction : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)}) {
        auto hit = lp::lookup(box, {0, 0, 0}, direction);
        CHECK(glm::length(hit.direction - direction * 2.f) < 1e-6f);
        CHECK(hit.weight == 1);
    }
    CHECK(lp::lookup(box, {2, 0, 0}, {1, 0, 0}).weight == 0);
    box.fade = 0;
    CHECK(lp::lookup(box, {2, 0, 0}, {1, 0, 0}).weight == 1);
    box.position.x = 3;
    CHECK(!lp::valid(box));
    box.position.x = 0;
    std::vector<std::byte> packaged(32 + 16368);
    auto put = [&](size_t offset, uint64_t v, unsigned n) {for(unsigned i=0;i<n;++i)packaged[offset+i]=std::byte((v>>(i*8))&255); };
    put(0, 0x31425244, 4);
    put(4, 1, 4);
    put(8, 16, 4);
    put(12, 5, 4);
    put(16, 16368, 8);
    put(24, 6249256310024487717ull, 8);
    for (size_t i = 39; i < packaged.size(); i += 8)
        packaged[i] = std::byte(0x3c);
    auto decoded = lp::decode(packaged);
    CHECK(decoded.has_value());
    packaged.back() = std::byte(0);
    CHECK(!lp::decode(packaged));
    packaged.back() = std::byte(0x3c);
    CHECK(!lp::decode(std::span<std::byte const>(packaged).first(packaged.size() - 1)));
    namespace rc = deren::app::radiance_capture;
    rc::options parsed;
    CHECK(rc::parse_option(parsed, "--capture-radiance-cube", "test.ibl"));
    CHECK(rc::parse_option(parsed, "--probe-size", "16"));
    CHECK(rc::valid(parsed));
    CHECK(rc::parse_option(parsed, "--probe-size", "16.5"));
    CHECK(!rc::valid(parsed));
    rc::options o;
    CHECK(rc::valid(o));
    o.enabled = true;
    o.output = "cube.ibl";
    CHECK(rc::valid(o));
    o.size = 17;
    CHECK(!rc::valid(o));
    o.size = 16;
    o.near_plane = 0;
    CHECK(!rc::valid(o));
    o.near_plane = .05f;
    o.far_plane = .01f;
    CHECK(!rc::valid(o));
    o.far_plane = 100;
    o.position.x = NAN;
    CHECK(!rc::valid(o));
    o.position.x = 0;
    CHECK(rc::parse_position("1,2,3").has_value());
    CHECK(!rc::parse_position("1,2,3,4"));
    CHECK(!rc::parse_position("1,NaN,3"));
    CHECK(!rc::parse_position("1,2"));
    auto path = std::filesystem::temp_directory_path() / std::filesystem::path("deren-radiance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ibl");
    std::vector<float> pixels(6 * 16 * 16 * 4, 1);
    pixels[0] = 32;
    CHECK(rc::write_cube(path, 16, pixels));
    auto bytes = std::filesystem::file_size(path);
    CHECK(bytes == 12 + pixels.size() * 4);
    std::ifstream in(path, std::ios::binary);
    char magic[4]{};
    in.read(magic, 4);
    CHECK(std::string(magic, 4) == "IBL1");
    in.seekg(12);
    float hdr = 0;
    in.read(reinterpret_cast<char*>(&hdr), 4);
    CHECK(hdr == 32);
    in.close();
    CHECK(!rc::write_cube(path, 16, pixels));
    CHECK(std::filesystem::file_size(path) == bytes);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    pixels[0] = INFINITY;
    CHECK(!rc::write_cube(path, 16, pixels));
    CHECK(!std::filesystem::exists(path));
    pixels[0] = -1;
    CHECK(!rc::write_cube(path, 16, pixels));

    namespace rhi = deren::promise::rhi;
    rhi::create_info opts{};
    opts.window_width = 32;
    opts.window_height = 32;
    auto core = deren::vk_test::probe_make_core(opts, rhi::abi_version, true);
    deren::engine::runtime runtime{core, opts};
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(runtime.set_local_probe(decoded->half, 16, 5, {0, 0, 0}, {-2, -2, -2}, {2, 2, 2}, .5f, 1));
        auto const probe_slot = deren::engine::render_layout::heap_slot_offset(deren::engine::render_layout::heap_slots::local_probe);
        auto* previous_probe = deren::vk_test::probe_heap_image(*core, probe_slot);
        CHECK(previous_probe != nullptr);
        deren::vk_test::probe_resource_failure(*core, 0);
        CHECK(!runtime.set_local_probe(decoded->half, 16, 5, {0, 0, 0}, {-2, -2, -2}, {2, 2, 2}, .5f, 1));
        deren::vk_test::probe_resource_failure(*core, -1);
        deren::vk_test::probe_heap_image_failure(*core, 0);
        CHECK(!runtime.set_local_probe(decoded->half, 16, 5, {0, 0, 0}, {-2, -2, -2}, {2, 2, 2}, .5f, 1));
        CHECK(deren::vk_test::probe_heap_image(*core, probe_slot) == previous_probe);
        deren::vk_test::probe_heap_image_failure(*core, -1);
    }
    CHECK(!runtime.acquire_radiance_image());
    CHECK(runtime.request_radiance_capture());
    CHECK(!runtime.request_radiance_capture());
    CHECK(!runtime.acquire_radiance_image());
    return deren::vk_test::finish("test_radiance_capture");
}
