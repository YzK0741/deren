#include "vk_test.h"

#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
import deren.engine.gaussian_splatting;
import deren.engine.gaussian_splatting.raster;
namespace gs = deren::engine::gaussian_splatting;
namespace {
    bool near(float a, float b, float tolerance = 0.002f) {
        return std::abs(a - b) <= tolerance;
    }
    gs::raster_camera camera() {
        gs::raster_camera c;
        c.view_projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
        c.view_projection[1][1] *= -1;
        c.extent = {200, 200};
        return c;
    }
    gs::geometry_record geometry() {
        return {{0, 0, -2, 0.5f}, {0.01f, 0, 0, 0}, {0.04f, 0, 0.01f, 0}};
    }
    void test_layout() {
        CHECK(sizeof(gs::raster_push) == 112);
        CHECK(offsetof(gs::raster_push, camera_position) == 64);
        CHECK(offsetof(gs::raster_push, references_address) == 80);
        CHECK(offsetof(gs::raster_push, instances_address) == 88);
        CHECK(offsetof(gs::raster_push, viewport_width) == 96);
        CHECK(offsetof(gs::raster_push, visible_count) == 104);
        CHECK(offsetof(gs::raster_push, flags) == 108);
    }
    void test_projection() {
        auto c = camera();
        auto g = geometry();
        auto f = gs::validate_raster_camera(c);
        CHECK(f);
        if (f)
            CHECK(near(f->forward.z, -1));
        auto p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p) {
            CHECK(p->visibility == gs::projection_visibility::visible);
            CHECK(near(p->center_pixel.x, 100) && near(p->center_pixel.y, 100));
            CHECK(near(p->covariance.x, 25.3f) && near(p->covariance.y, 0) && near(p->covariance.z, 100.3f));
            CHECK(near(p->basis_x.x * p->basis_x.x + p->basis_y.x * p->basis_y.x, p->covariance.x));
            CHECK(near(p->basis_x.x * p->basis_x.y + p->basis_y.x * p->basis_y.y, p->covariance.y));
        }
        g.center_opacity[0] = 1;
        g.center_opacity[1] = 0.5f;
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p) {
            CHECK(near(p->center_pixel.x, 150) && near(p->center_pixel.y, 75));
            CHECK(near(p->covariance.x, 31.55f) && near(p->covariance.y, -3.125f) && near(p->covariance.z, 101.8625f));
        }
        g = geometry();
        p = gs::project_splat(g, glm::scale(glm::mat4{1}, glm::vec3{2}), c);
        CHECK(p);
        if (p)
            CHECK(near(p->covariance.x, 25.3f) && near(p->covariance.z, 100.3f));
        p = gs::project_splat(g, glm::rotate(glm::mat4{1}, glm::radians(45.0f), glm::vec3{0, 0, 1}), c);
        CHECK(p);
        if (p)
            CHECK(near(p->covariance.x, 62.8f) && near(p->covariance.y, 37.5f) && near(p->covariance.z, 62.8f));
        g = geometry();
        g.center_opacity[2] = -0.2f;
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p)
            CHECK(p->visibility == gs::projection_visibility::near_omitted);
        g = geometry();
        g.center_opacity[2] = 2;
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p)
            CHECK(p->visibility == gs::projection_visibility::outside);
        g = geometry();
        g.center_opacity[0] = 5;
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p)
            CHECK(p->visibility == gs::projection_visibility::outside);
        g = geometry();
        g.center_opacity[0] = 2.1f;
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p)
            CHECK(p->visibility == gs::projection_visibility::visible);
        g = geometry();
        g.covariance_x[0] = 0;
        g.covariance_yz = {0, 0, 0, 0};
        p = gs::project_splat(g, glm::mat4{1}, c);
        CHECK(p);
        if (p)
            CHECK(near(p->covariance.x, 0.3f) && near(p->covariance.z, 0.3f));
        // A tolerated input eigenvalue error can become a negative screen-space variance.
        g = {{0, -20, -20, 0.5f}, {1e8f, 0, 0, 0}, {10, 10.5f, 10, 0}};
        CHECK(!gs::project_splat(g, glm::mat4{1}, camera()));
        c.extent.width = 0;
        CHECK(!gs::validate_raster_camera(c));
        c = camera();
        c.view_projection = glm::mat4{1};
        CHECK(!gs::validate_raster_camera(c));
        c = camera();
        c.position.z = 1;
        CHECK(!gs::validate_raster_camera(c));
        c = camera();
        c.view_projection[0][0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!gs::validate_raster_camera(c));
        g = geometry();
        g.covariance_x[1] = 1;
        CHECK(!gs::project_splat(g, glm::mat4{1}, camera()));
    }
    void test_sh_kernel() {
        gs::sh_record sh;
        constexpr float c0 = 0.28209479177387814f;
        sh.coefficients[0] = 1.5f / c0;
        sh.coefficients[1] = -0.6f / c0;
        sh.coefficients[2] = 0.2f / c0;
        auto rgb = gs::evaluate_sh(sh, 0, {0, 0, -1});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 2) && near(rgb->y, 0) && near(rgb->z, 0.7f));
        sh = {};
        sh.coefficients[6] = 1;
        rgb = gs::evaluate_sh(sh, 1, {0, 0, 1});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 0.9886025f));
        rgb = gs::evaluate_sh(sh, 1, {0, 0, -1});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 0.0113975f));
        sh = {};
        sh.coefficients[3] = 1;
        rgb = gs::evaluate_sh(sh, 1, {0, 1, 0});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 0.0113975f));
        sh = {};
        sh.coefficients[9] = 1;
        rgb = gs::evaluate_sh(sh, 1, {1, 0, 0});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 0.0113975f));
        sh = {};
        sh.coefficients[18] = 1;
        rgb = gs::evaluate_sh(sh, 2, {0, 0, 1});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 1.1307831f));
        sh = {};
        sh.coefficients[36] = 1;
        rgb = gs::evaluate_sh(sh, 3, {0, 0, 1});
        CHECK(rgb);
        if (rgb)
            CHECK(near(rgb->x, 1.2463527f));
        CHECK(!gs::evaluate_sh(sh, 4, {0, 0, 1}));
        CHECK(!gs::evaluate_sh(sh, 3, {0, 0, 0}));
        sh.coefficients[47] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!gs::evaluate_sh(sh, 0, {0, 0, 1}));
        CHECK(near(gs::fragment_alpha(0.5f, {0, 0}), 0.5f, 1e-6f));
        CHECK(near(gs::fragment_alpha(0.5f, {3, 0}), 0.5f * std::exp(-4.5f), 1e-6f));
        CHECK(gs::fragment_alpha(0.5f, {3.001f, 0}) == 0);
        CHECK(near(gs::fragment_alpha(1, {0, 0}), 0.99f, 1e-6f));
        CHECK(gs::fragment_alpha(0.001f, {0, 0}) == 0);
        CHECK(gs::fragment_alpha(-1, {0, 0}) == 0);
    }
} // namespace
int main() {
    test_layout();
    test_projection();
    test_sh_kernel();
    return deren::vk_test::finish("gaussian_raster");
}
