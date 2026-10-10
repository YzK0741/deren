#include "vk_test.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

void test_models() {
    glm::mat4 identity{1};
    CHECK(gs::validate_model(identity));
    auto model = glm::translate(identity, glm::vec3{2, 3, 4});
    model = glm::rotate(model, 0.4f, glm::vec3{0, 0, 1});
    model = glm::scale(model, glm::vec3{2});
    auto inv = gs::validate_model(model);
    CHECK(inv);
    if (inv) {
        auto product = model * *inv;
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned j = 0; j < 4; ++j)
                CHECK(close(product[i][j], i == j ? 1.0f : 0.0f));
    }
    auto bad = identity;
    bad[0][0] = 2;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[0][0] = -1;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[1][0] = 0.25f;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[0][3] = 0.01f;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[3][3] = 2;
    CHECK(!gs::validate_model(bad));
    bad = glm::mat4{0};
    CHECK(!gs::validate_model(bad));
    bad = glm::scale(identity, glm::vec3{std::numeric_limits<float>::denorm_min()});
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[3][0] = std::numeric_limits<float>::infinity();
    CHECK(!gs::validate_model(bad));
}
void test_sorting() {
    auto a = sample();
    a.particles[0].center = {0, 0, -1};
    a.particles.push_back(a.particles[0]);
    a.particles[1].center[2] = -3;
    std::array<gs::sort_instance, 2> instances = {gs::sort_instance{&a, glm::mat4{1}, 20}, gs::sort_instance{&a, glm::translate(glm::mat4{1}, glm::vec3{0, 0, -1}), 10}};
    auto sorted = gs::sort_draw_references(instances, {});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 1 && (*sorted)[0].local_gaussian_index == 1);
        CHECK((*sorted)[1].instance_index == 0 && (*sorted)[1].local_gaussian_index == 1);
        CHECK((*sorted)[2].instance_index == 1 && (*sorted)[2].local_gaussian_index == 0);
        CHECK((*sorted)[3].instance_index == 0 && (*sorted)[3].local_gaussian_index == 0);
    }
    sorted = gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, 1}});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && !sorted->empty())
        CHECK((*sorted)[0].instance_index == 0 && (*sorted)[0].local_gaussian_index == 0);
    instances[1].model = glm::mat4{1};
    a.particles[1].center[2] = -1;
    sorted = gs::sort_draw_references(instances, {});
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 1 && (*sorted)[0].local_gaussian_index == 0);
        CHECK((*sorted)[1].instance_index == 1 && (*sorted)[1].local_gaussian_index == 1);
        CHECK((*sorted)[2].instance_index == 0 && (*sorted)[2].local_gaussian_index == 0);
    }
    std::swap(instances[0], instances[1]);
    sorted = gs::sort_draw_references(instances, {});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 0);
        CHECK((*sorted)[0].local_gaussian_index == 0);
        CHECK((*sorted)[1].local_gaussian_index == 1);
    }
    std::swap(instances[0], instances[1]);
    auto overflow = instances;
    overflow[0].model = glm::scale(glm::mat4{1}, glm::vec3{std::numeric_limits<float>::max()});
    a.particles[0].center[2] = -2;
    CHECK(!gs::sort_draw_references(overflow, {}));
    a.particles[0].center[2] = -1;
    CHECK(gs::sort_draw_references(instances, {}, {4, 32}));
    CHECK(!gs::sort_draw_references(instances, {}, {3, 32}));
    CHECK(!gs::sort_draw_references(instances, {}, {4, 31}));
    instances[1].stable_id = 20;
    CHECK(!gs::sort_draw_references(instances, {}));
    instances[1].stable_id = 10;
    instances[1].asset = nullptr;
    CHECK(!gs::sort_draw_references(instances, {}));
    instances[1].asset = &a;
    CHECK(!gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, 0}}));
    CHECK(!gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, -2}}));
    CHECK(!gs::sort_draw_references(instances, {{std::numeric_limits<float>::infinity(), 0, 0}, {0, 0, -1}}));
    a.particles[0].center[2] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!gs::sort_draw_references(instances, {}));
    a.particles.clear();
    CHECK(gs::sort_draw_references(instances, {})->empty());
    instances[1].stable_id = 20;
    CHECK(!gs::sort_draw_references(instances, {}));
    CHECK(gs::sort_draw_references({}, {})->empty());
}

int main() {
    test_packing();
    test_invalid_packing();
    test_models();
    test_sorting();
    return deren::vk_test::finish("gaussian_splatting");
}