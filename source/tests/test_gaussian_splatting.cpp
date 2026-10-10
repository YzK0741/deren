#include "vk_test.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
import deren.gaussian_loader;
import deren.engine.gaussian_splatting;
namespace gs = deren::engine::gaussian_splatting;
namespace {
    deren::gaussian::asset sample() {
        deren::gaussian::asset a{};
        deren::gaussian::particle p{};
        p.center = {1, 2, 3};
        p.scale = {1, 2, 3};
        p.rotation = {1, 0, 0, 0};
        p.opacity = 0.5f;
        p.sh[0] = 1;
        p.sh[1] = 2;
        p.sh[2] = 3;
        a.particles.push_back(p);
        return a;
    }
    bool close(float a, float b) {
        return std::abs(a - b) < 1e-5f;
    }
    void test_packing() {
        CHECK(sizeof(gs::geometry_record) == 48);
        CHECK(sizeof(gs::sh_record) == 192);
        CHECK(sizeof(gs::instance_record) == 160);
        CHECK(sizeof(gs::draw_reference) == 8);
        auto a = sample();
        auto result = gs::pack_asset(a);
        CHECK(result.has_value());
        if (!result || result->geometry.empty()) {
            CHECK(false);
            return;
        }
        auto const& g = result->geometry[0];
        CHECK((g.center_opacity == std::array<float, 4>({1, 2, 3, 0.5f})));
        CHECK((g.covariance_x == std::array<float, 4>({1, 0, 0, 0})));
        CHECK((g.covariance_yz == std::array<float, 4>({4, 0, 9, 0})));
        CHECK(result->sh[0].coefficients == a.particles[0].sh);
        CHECK((result->bounds_min == std::array<float, 3>({-2, -4, -6})));
        CHECK((result->bounds_max == std::array<float, 3>({4, 8, 12})));
        a.bounds_min = {100, 100, 100};
        CHECK(gs::pack_asset(a)->bounds_min[0] == -2);
        float const q = std::sqrt(0.5f);
        a.particles[0].rotation = {q, 0, 0, q};
        result = gs::pack_asset(a);
        CHECK(close(result->geometry[0].covariance_x[0], 4));
        CHECK(close(result->geometry[0].covariance_yz[0], 1));
        a.particles[0].rotation = {std::cos(0.3926990817f), 0, 0, std::sin(0.3926990817f)};
        result = gs::pack_asset(a);
        CHECK(close(result->geometry[0].covariance_x[0], 2.5f));
        CHECK(close(result->geometry[0].covariance_x[1], -1.5f));
        CHECK(close(result->geometry[0].covariance_yz[0], 2.5f));
        CHECK(close(result->geometry[0].covariance_yz[2], 9));
        CHECK(gs::pack_asset(deren::gaussian::asset{})->geometry.empty());
        a = sample();
        a.particles[0].center = {1, 1, 1};
        a.particles[0].scale = {1e-8f, 1e-8f, 1e-8f};
        result = gs::pack_asset(a);
        CHECK(result);
        CHECK(result->bounds_min[0] < 1.0f);
        CHECK(result->bounds_max[0] > 1.0f);
        a = sample();
        a.particles[0].rotation = {-1, 0, 0, 0};
        a.particles[0].opacity = 0;
        CHECK(gs::pack_asset(a)->geometry[0].covariance_x[0] == 1);
        a.particles[0].opacity = 1;
        CHECK(gs::pack_asset(a));
        for (std::uint32_t degree = 0; degree <= 3; ++degree) {
            a = sample();
            a.sh_degree = degree;
            unsigned const used = 3 * (degree + 1) * (degree + 1);
            for (unsigned i = 0; i < used; ++i)
                a.particles[0].sh[i] = static_cast<float>(i) + 0.25f;
            for (unsigned i = used; i < 48; ++i)
                a.particles[0].sh[i] = 123.0f;
            result = gs::pack_asset(a);
            CHECK(result && result->sh_degree == degree);
            for (unsigned i = 0; i < 48; ++i)
                CHECK(result->sh[0].coefficients[i] == (i < used ? a.particles[0].sh[i] : 0.0f));
        }
    }
    void test_invalid_packing() {
        auto check = [](auto mutate) {auto a=sample();mutate(a);CHECK(!gs::pack_asset(a)); };
        float const nan = std::numeric_limits<float>::quiet_NaN();
        check([nan](auto& a) { a.particles[0].center[0] = nan; });
        check([](auto& a) { a.particles[0].scale[0] = 0; });
        check([](auto& a) { a.particles[0].scale[0] = -1; });
        check([](auto& a) { a.particles[0].scale[0] = std::numeric_limits<float>::max(); });
        check([](auto& a) { a.particles[0].rotation = {0, 0, 0, 0}; });
        check([](auto& a) { a.particles[0].rotation = {2, 0, 0, 0}; });
        check([](auto& a) { a.particles[0].opacity = -0.1f; });
        check([](auto& a) { a.particles[0].opacity = 1.1f; });
        check([nan](auto& a) { a.particles[0].sh[0] = nan; });
        check([](auto& a) { a.sh_degree = 4; });
        check([nan](auto& a) { a.particles[0].rotation[0] = nan; });
        check([nan](auto& a) { a.particles[0].sh[47] = nan; });
        CHECK(gs::packed_byte_size(1).value() == 240);
        CHECK(gs::packed_byte_size(1, {1, 240}));
        CHECK(!gs::packed_byte_size(1, {1, 239}));
        CHECK(!gs::packed_byte_size(2, {1, 1000}));
        CHECK(!gs::packed_byte_size(std::numeric_limits<std::uint64_t>::max(), {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()}));
        CHECK(!gs::pack_asset(sample(), {1, 239}));
        CHECK(gs::pack_asset(sample(), {1, 240}));
    }
} // namespace
int main() {
    test_packing();
    test_invalid_packing();
    return deren::vk_test::finish("gaussian_splatting");
}
