#include "vk_test.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>
import deren.gaussian_loader;
namespace gs = deren::gaussian;
std::string header(std::string format = "ascii", unsigned count = 1) {
    std::string s = "ply\nformat " + format + " 1.0\nelement vertex " + std::to_string(count) + "\n";
    for (auto name : {"x", "y", "z", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"})
        s += "property float " + std::string(name) + "\n";
    return s;
}
auto parse(std::string const& text, gs::load_options options = {}) {
    return gs::parse_ply(std::as_bytes(std::span(text.data(), text.size())), options);
}
int main() {
    auto good = header() + "end_header\n1 2 3 0 0 0 2 0 0 0 0 1 2 3\n";
    auto model = parse(good);
    CHECK(model && model->particles.size() == 1);
    if (model) {
        CHECK(model->sh_degree == 0);
        auto const& p = model->particles[0];
        CHECK(p.center[0] == 1 && p.center[2] == 3);
        CHECK(p.scale[0] == 1 && p.rotation[0] == 1 && p.opacity == 0.5f);
        CHECK(p.sh[0] == 1 && p.sh[1] == 2 && p.sh[2] == 3 && p.sh[3] == 0);
        CHECK(model->bounds_min[0] == -2 && model->bounds_max[2] == 6);
    }
    auto binary = header("binary_little_endian") + "end_header\n";
    for (float f : {1.f, 2.f, 3.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f, 0.f, 0.f, 1.f, 2.f, 3.f}) {
        auto bits = std::bit_cast<std::uint32_t>(f);
        for (unsigned b = 0; b < 4; ++b)
            binary += char((bits >> (b * 8)) & 255);
    }
    auto b = parse(binary);
    CHECK(b && model && b->particles[0].center == model->particles[0].center);
    CHECK(!parse(binary.substr(0, binary.size() - 1)));
    CHECK(!parse(header("binary_big_endian") + "end_header\n"));
    CHECK(!parse(header() + "property list uchar float extra\nend_header\n"));
    CHECK(!parse(header() + "property float x\nend_header\n"));
    CHECK(!parse(header() + "end_header\n1 2"));
    CHECK(!parse(good, {.max_particles = 0}));
    CHECK(!parse(good, {.max_particles = 4, .max_file_bytes = 16}));
    CHECK(!parse(good, {.max_particles = 4, .max_file_bytes = 4096, .max_output_bytes = 1}));
    auto zero = header("ascii", 0) + "end_header\n";
    CHECK(parse(zero));
    for (auto values : {"nan 2 3 0 0 0 1 0 0 0 0 1 2 3", "1 2 3 1000 0 0 1 0 0 0 0 1 2 3", "1 2 3 0 0 0 0 0 0 0 0 1 2 3"})
        CHECK(!parse(header() + "end_header\n" + values));

    for (unsigned degree = 1; degree <= 3; ++degree) {
        unsigned bands = (degree + 1) * (degree + 1) - 1;
        auto h = header();
        for (unsigned i = 0; i < bands * 3; ++i)
            h += "property float f_rest_" + std::to_string(i) + "\n";
        h += "property uchar extra\nend_header\n1 2 3 0 0 0 1 0 0 0 0 1 2 3";
        for (unsigned i = 0; i < bands * 3; ++i)
            h += " " + std::to_string(i + 10);
        auto sh = parse(h + " 255\n");
        CHECK(sh && sh->sh_degree == degree);
        if (sh) {
            CHECK(sh->particles[0].sh[3] == 10);
            CHECK(sh->particles[0].sh[4] == float(10 + bands));
            CHECK(sh->particles[0].sh[5] == float(10 + bands * 2));
        }
        CHECK(!parse(h + " 256\n"));
    }
    auto reordered = header();
    auto x = reordered.find("property float x\n"), y = reordered.find("property float y\n");
    reordered[x + 15] = 'y';
    reordered[y + 15] = 'x';
    auto swap = parse(reordered + "end_header\n2 1 3 0 0 0 1 0 0 0 0 1 2 3\n");
    CHECK(swap && swap->particles[0].center[0] == 1 && swap->particles[0].center[1] == 2);
    CHECK(!parse(header() + "property float f_rest_0\nend_header\n"));
    CHECK(!parse(header() + "end_header\n1 2 3 -1000 0 0 1 0 0 0 0 1 2 3\n"));
    CHECK(!parse(good + "1"));
    auto diagnostics = parse(header() + "end_header\n1 2");
    CHECK(!diagnostics && diagnostics.error().code == gs::error_code::truncated);
    CHECK(!diagnostics && diagnostics.error().byte_offset > 0);
    auto unknown = header("binary_little_endian");
    unknown += "property short extra\nend_header\n";
    auto payload = binary.substr(binary.find("end_header\n") + 11);
    auto negative = parse(unknown + payload + std::string(2, char(255)));
    CHECK(negative);

    auto rotated = parse(header() + "end_header\n0 0 0 0.6931471805599453 0 0 0.7071067811865476 0 0 0.7071067811865476 1000 0 0 0\n");
    CHECK(rotated);
    if (rotated) {
        CHECK(std::abs(rotated->bounds_max[0] - 3.f) < 0.0001f);
        CHECK(std::abs(rotated->bounds_max[1] - 6.f) < 0.0001f);
        CHECK(rotated->particles[0].opacity == 1);
    }
    auto low_opacity = parse(header() + "end_header\n0 0 0 0 0 0 1 0 0 0 -1000 0 0 0\n");
    CHECK(low_opacity && low_opacity->particles[0].opacity == 0);
    auto crlf = good;
    for (std::size_t pos = 0; (pos = crlf.find('\n', pos)) != std::string::npos; pos += 2)
        crlf.insert(pos, 1, '\r');
    CHECK(parse(crlf));
    auto huge_count = header();
    auto count_at = huge_count.find("vertex 1");
    huge_count.replace(count_at, 8, "vertex 18446744073709551615");
    CHECK(!parse(huge_count + "end_header\n"));
    auto large_center = parse(header() + "end_header\n10000000000 0 0 0 0 0 1 0 0 0 0 1 2 3\n");
    CHECK(large_center);
    if (large_center) {
        CHECK(double(large_center->bounds_min[0]) <= 1e10 - 3);
        CHECK(double(large_center->bounds_max[0]) >= 1e10 + 3);
    }
    std::ofstream("gaussian-fixture.ply", std::ios::binary) << binary;
    CHECK(gs::load_ply("gaussian-fixture.ply"));
    CHECK(!gs::load_ply("gaussian-missing.ply"));
    return deren::vk_test::finish("test_gaussian_loader");
}
