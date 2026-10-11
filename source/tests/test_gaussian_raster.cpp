#include "vk_test.h"

#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
import deren.engine.gaussian_splatting;
import deren.engine.gaussian_splatting.raster;
import deren.engine.pass;
import deren.engine.pass.gaussian_splatting;
#include "raster_probe.hpp"
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
    void test_camera_jacobian() {
        auto c = camera();
        c.position = {2, 1, 3};
        c.extent = {321, 193};
        auto projection = glm::perspectiveRH_ZO(glm::radians(67.0f), 1.7f, 0.1f, 50.0f);
        projection[1][1] *= -1;
        projection[2][0] = 0.13f;
        projection[2][1] = -0.07f;
        c.view_projection = projection * glm::lookAtRH(c.position, glm::vec3{0}, glm::vec3{0, 1, 0});
        auto model = glm::translate(glm::mat4{1}, glm::vec3{0.2f, -0.1f, 0.3f});
        model = glm::rotate(model, glm::radians(29.0f), glm::normalize(glm::vec3{1, 2, 3}));
        model = glm::scale(model, glm::vec3{1.3f});
        auto g = geometry();
        g.center_opacity = {0.3f, -0.2f, 0.1f, 0.5f};
        auto projected = gs::project_splat(g, model, c);
        CHECK(projected);
        if (!projected) {
            return;
        }
        CHECK(projected->visibility == gs::projection_visibility::visible);
        // Finite differences of the actual camera mapping independently check the Jacobian.
        auto pixel = [&](glm::dvec3 point) {
            glm::dvec4 clip = glm::dmat4{c.view_projection} * glm::dmat4{model} * glm::dvec4{point, 1};
            return (glm::dvec2{clip} / clip.w + glm::dvec2{1}) * glm::dvec2{c.extent.width, c.extent.height} * 0.5;
        };
        glm::dvec3 center{g.center_opacity[0], g.center_opacity[1], g.center_opacity[2]};
        glm::dvec3 jx, jy;
        for (int axis = 0; axis < 3; ++axis) {
            glm::dvec3 step{0};
            step[axis] = 1e-4;
            glm::dvec2 derivative = (pixel(center + step) - pixel(center - step)) / 2e-4;
            jx[axis] = derivative.x;
            jy[axis] = derivative.y;
        }
        glm::dmat3 covariance{0};
        covariance[0][0] = 0.01;
        covariance[1][1] = 0.04;
        covariance[2][2] = 0.01;
        auto expected_center = pixel(center);
        CHECK(near(projected->center_pixel.x, static_cast<float>(expected_center.x)) && near(projected->center_pixel.y, static_cast<float>(expected_center.y)));
        CHECK(near(projected->covariance.x, static_cast<float>(glm::dot(jx, covariance * jx) + 0.3)));
        CHECK(near(projected->covariance.y, static_cast<float>(glm::dot(jx, covariance * jy))));
        CHECK(near(projected->covariance.z, static_cast<float>(glm::dot(jy, covariance * jy) + 0.3)));
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

namespace {
    deren::gaussian::asset raster_source() {
        deren::gaussian::asset a;
        for (auto const& center : {std::array<float, 3>{0, 0, -1}, std::array<float, 3>{0, 0, -3}, std::array<float, 3>{0, 0, -0.15f}, std::array<float, 3>{10, 0, -2}}) {
            deren::gaussian::particle p;
            p.center = center;
            p.scale = {0.05f, 0.05f, 0.05f};
            p.rotation = {1, 0, 0, 0};
            p.opacity = 0.5f;
            a.particles.push_back(p);
        }
        return a;
    }
    void test_frame_upload() {
        raster_probe_core probe;
        auto asset = gs::make_raster_asset(probe, raster_source());
        CHECK(asset);
        if (!asset) {
            return;
        }
        CHECK(probe.calls == 2);
        std::array<gs::raster_instance, 2> items = {gs::raster_instance{*asset, glm::mat4{1}, 20}, gs::raster_instance{*asset, glm::translate(glm::mat4{1}, glm::vec3{0, 0, -1}), 10}};
        auto frame = gs::prepare_raster_frame(probe, items, camera());
        CHECK(frame);
        if (!frame) {
            return;
        }
        CHECK((*frame)->params().visible_count == 5);
        CHECK((*frame)->total_count() == 8 && (*frame)->near_omitted_count() == 1 && (*frame)->outside_count() == 2);
        CHECK((*frame)->references().front().instance_index == 1 && (*frame)->references().front().local_gaussian_index == 1);
        CHECK((*frame)->references().back().instance_index == 0 && (*frame)->references().back().local_gaussian_index == 0);
        CHECK((*frame)->instances().size() == 2 && (*frame)->byte_size() == 360);
        CHECK((*frame)->params().instances_address != 0 && (*frame)->params().references_address != 0);
        CHECK(probe.descriptors[2].usage == rhi::buffer_usage::storage_coherent);
        CHECK(rhi::has_flag(probe.descriptors[2].flags, rhi::buffer_flag::device_address));
        unsigned const calls = probe.calls;
        auto limited = gs::prepare_raster_frame(probe, items, camera(), {8, 359});
        CHECK(!limited && limited.error().code == gs::error_code::limit_exceeded);
        CHECK(probe.calls == calls);
        auto bad_camera = camera();
        bad_camera.extent.height = 0;
        CHECK(!gs::prepare_raster_frame(probe, items, bad_camera));
        CHECK(probe.calls == calls);
        items[1].stable_id = 20;
        CHECK(!gs::prepare_raster_frame(probe, items, camera()));
        CHECK(probe.calls == calls);
        items[1].stable_id = 10;
        items[1].model = glm::scale(glm::mat4{1}, glm::vec3{1, 2, 1});
        CHECK(!gs::prepare_raster_frame(probe, items, camera()));
        CHECK(probe.calls == calls);
        items[1].model = glm::mat4{1};
        probe.fail_at = probe.calls + 2;
        int const before = probe.releases;
        CHECK(!gs::prepare_raster_frame(probe, items, camera()));
        CHECK(probe.releases == before + 1);
        probe.fail_at = 0;
        probe.address.unaligned_at = probe.calls + 1;
        CHECK(!gs::prepare_raster_frame(probe, items, camera()));
        probe.address.unaligned_at = 0;
        probe.address.zero_at = probe.calls + 2;
        auto invalid_address = gs::prepare_raster_frame(probe, items, camera());
        CHECK(!invalid_address && invalid_address.error().code == gs::error_code::invalid_address);
        probe.address.zero_at = 0;
        probe.undersized_at = probe.calls + 2;
        auto short_upload = gs::prepare_raster_frame(probe, items, camera());
        CHECK(!short_upload && short_upload.error().code == gs::error_code::allocation_failure);
        probe.undersized_at = 0;
        std::weak_ptr<gs::raster_asset const> weak = *asset;
        (*asset).reset();
        for (auto& item : items) {
            item.asset.reset();
        }
        CHECK(!weak.expired());
        (*frame).reset();
        CHECK(weak.expired());
        raster_probe_core empty_probe;
        empty_probe.supported = false;
        auto empty = gs::prepare_raster_frame(empty_probe, {}, camera());
        CHECK(empty && (*empty)->params().visible_count == 0);
        CHECK(empty_probe.calls == 0);
    }
    struct push_probe {
        bool refused = false;
        std::vector<std::byte> bytes;
    };
    bool push_frame(void* owner, rhi::command_buffer&, std::span<std::byte const> bytes, std::uint32_t) {
        auto& out = *static_cast<push_probe*>(owner);
        out.bytes.assign(bytes.begin(), bytes.end());
        return !out.refused;
    }
    void test_raster_pass() {
        namespace pass = deren::engine::pass;
        raster_probe_core core;
        pass::gaussian_splatting_pass p;
        std::array<std::uint8_t, 4> shader = {3, 2, 35, 7};
        pass::pass_context context{.face = &core, .shader = [](void* owner, std::string_view) { return std::span<std::uint8_t const>{*static_cast<std::array<std::uint8_t, 4>*>(owner)}; }, .depth_format = rhi::image_format::depth, .owner = &shader};
        p.create(context);
        CHECK(p.ready());
        CHECK(core.first_stage == rhi::shader_stage::vertex);
        CHECK(core.depth_test);
        CHECK(core.blends.size() == 1 && core.blends[0] == rhi::blend_mode::alpha);
        CHECK(p.io().name == "gaussian");
        auto asset = gs::make_raster_asset(core, raster_source());
        CHECK(asset);
        if (!asset) {
            return;
        }
        std::array<gs::raster_instance, 1> items = {gs::raster_instance{*asset, glm::mat4{1}, 1}};
        auto frame = gs::prepare_raster_frame(core, items, camera());
        CHECK(frame);
        if (!frame) {
            return;
        }
        p.set_frame(*frame);
        push_probe push;
        raster_probe_commands commands;
        pass::resolved_io io;
        io.list = &commands;
        io.extent = {200, 200};
        io.target_storage[0] = {.image_handle = reinterpret_cast<rhi::image*>(std::uintptr_t{1}), .view_handle = reinterpret_cast<rhi::image_view*>(std::uintptr_t{2})};
        io.target_storage[1] = {.image_handle = reinterpret_cast<rhi::image*>(std::uintptr_t{3}), .view_handle = reinterpret_cast<rhi::image_view*>(std::uintptr_t{4})};
        io.targets = std::span{io.target_storage}.first(2);
        io.push_block = {&push, push_frame};
        p.record(io);
        CHECK(p.last_error() == rhi::error::ok);
        CHECK(commands.draws == 1 && commands.vertices == 6 && commands.instances == 2);
        CHECK(commands.color_load == rhi::load_op::load && commands.depth_load == rhi::load_op::load);
        CHECK(commands.cull == rhi::cull_mode::none && !commands.depth_write);
        CHECK(push.bytes.size() == 112);
        CHECK(commands.transitions.size() == 3);
        if (commands.transitions.size() == 3) {
            CHECK((commands.transitions[0] == std::pair{rhi::image_use::shader_read, rhi::image_use::depth_attachment}));
            CHECK((commands.transitions[2] == std::pair{rhi::image_use::depth_attachment, rhi::image_use::shader_read}));
        }
        commands.reset();
        push.refused = true;
        p.record(io);
        CHECK(commands.draws == 0 && commands.barriers == 0 && p.last_error() != rhi::error::ok);
        push.refused = false;
        commands.reset();
        commands.fail_bind = true;
        p.record(io);
        CHECK(commands.draws == 0 && commands.barriers == 0);
        commands.reset();
        commands.fail_begin = true;
        p.record(io);
        CHECK(commands.draws == 0 && commands.begins == 1 && commands.ends == 0 && commands.barriers == 2);
        commands.reset();
        commands.fail_barrier = 1;
        p.record(io);
        CHECK(commands.draws == 0 && commands.begins == 0);
        p.set_frame({});
        commands.reset();
        p.record(io);
        CHECK(commands.draws == 0 && commands.barriers == 0);
    }
} // namespace
int main() {
    test_layout();
    test_projection();
    test_camera_jacobian();
    test_sh_kernel();
    test_frame_upload();
    test_raster_pass();
    return deren::vk_test::finish("gaussian_raster");
}
