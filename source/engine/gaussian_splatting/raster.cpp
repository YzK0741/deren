module;
#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <glm/glm.hpp>
#include <limits>
module deren.engine.gaussian_splatting.raster;
namespace deren::engine::gaussian_splatting {
    namespace {
        auto invalid(char const* message) {
            return std::unexpected(foundation_error{error_code::invalid_argument, message});
        }
        bool float_finite(double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        }
        glm::dvec4 row(glm::dmat4 const& m, glm::length_t i) {
            return {m[0][i], m[1][i], m[2][i], m[3][i]};
        }
        bool valid_covariance(geometry_record const& g) {
            for (auto const& lane : {g.center_opacity, g.covariance_x, g.covariance_yz}) {
                for (float v : lane) {
                    if (!std::isfinite(v)) {
                        return false;
                    }
                }
            }
            if (g.center_opacity[3] < 0 || g.center_opacity[3] > 1 || g.covariance_x[3] != 0 || g.covariance_yz[3] != 0) {
                return false;
            }
            double const a = g.covariance_x[0];
            double const b = g.covariance_yz[0];
            double const c = g.covariance_yz[2];
            double const d = g.covariance_x[1];
            double const e = g.covariance_x[2];
            double const f = g.covariance_yz[1];
            if (a < 0 || b < 0 || c < 0) {
                return false;
            }
            double const scale = std::max({a, b, c, std::abs(d), std::abs(e), std::abs(f)});
            if (scale == 0) {
                return true;
            }
            double const x = a / scale + 1e-6;
            double const y = b / scale + 1e-6;
            double const z = c / scale + 1e-6;
            double const xy = d / scale;
            double const xz = e / scale;
            double const yz = f / scale;
            return x * y - xy * xy >= 0 && x * z - xz * xz >= 0 && y * z - yz * yz >= 0 && x * y * z + 2 * xy * xz * yz - x * yz * yz - y * xz * xz - z * xy * xy >= 0;
        }
        glm::dmat3 covariance(geometry_record const& g) {
            return {{g.covariance_x[0], g.covariance_x[1], g.covariance_x[2]},
                    {g.covariance_x[1], g.covariance_yz[0], g.covariance_yz[1]},
                    {g.covariance_x[2], g.covariance_yz[1], g.covariance_yz[2]}};
        }
    } // namespace
    std::expected<sort_camera, foundation_error> validate_raster_camera(raster_camera const& camera) {
        if (camera.extent.width == 0 || camera.extent.height == 0 || camera.extent.depth != 1) {
            return invalid("raster viewport must be nonempty and 2D");
        }
        for (glm::length_t i = 0; i < 4; ++i) {
            for (glm::length_t j = 0; j < 4; ++j) {
                if (!std::isfinite(camera.view_projection[i][j])) {
                    return invalid("nonfinite view projection");
                }
            }
        }
        for (glm::length_t i = 0; i < 3; ++i) {
            if (!std::isfinite(camera.position[i])) {
                return invalid("nonfinite camera position");
            }
        }
        glm::dmat4 const vp{camera.view_projection};
        glm::dvec3 const n{row(vp, 3)};
        double const length = glm::length(n);
        if (!std::isfinite(length) || length == 0 || glm::determinant(vp) == 0) {
            return invalid("raster camera requires a nonsingular perspective projection");
        }
        glm::dvec3 const forward = n / length;
        glm::dvec4 const eye = vp * glm::dvec4{camera.position, 1};
        double const tolerance = 1e-5 * std::max(1.0, glm::length(glm::dvec3{camera.position})) * length;
        if (std::abs(eye.w) > tolerance || std::abs(eye.x) > tolerance || std::abs(eye.y) > tolerance) {
            return invalid("camera position differs from perspective eye");
        }
        if (glm::dot(glm::dvec3{row(vp, 2)}, forward) <= 0 || glm::dot(glm::dvec3{row(vp, 3) - row(vp, 2)}, forward) >= 0) {
            return invalid("raster camera requires finite forward depth [0,1]");
        }
        return sort_camera{camera.position, glm::vec3{forward}};
    }
    std::expected<projected_splat, foundation_error> project_splat(geometry_record const& g, glm::mat4 const& transform, raster_camera const& camera) {
        auto valid_camera = validate_raster_camera(camera);
        if (!valid_camera) {
            return std::unexpected(valid_camera.error());
        }
        auto inverse = validate_model(transform);
        if (!inverse) {
            return std::unexpected(inverse.error());
        }
        if (!valid_covariance(g)) {
            return invalid("invalid raster geometry or covariance");
        }
        glm::dmat4 const model{transform};
        glm::dmat4 const vp{camera.view_projection};
        glm::dvec4 const world = model * glm::dvec4{g.center_opacity[0], g.center_opacity[1], g.center_opacity[2], 1};
        glm::dvec4 const clip = vp * world;
        for (glm::length_t i = 0; i < 4; ++i) {
            if (!float_finite(world[i]) || !float_finite(clip[i])) {
                return invalid("raster center exceeds float32 range");
            }
        }
        projected_splat out;
        if (clip.w <= 0) {
            return out;
        }
        glm::dmat3 const a{model};
        glm::dmat3 const cov = a * covariance(g) * glm::transpose(a);
        for (glm::length_t i = 0; i < 3; ++i) {
            for (glm::length_t j = 0; j < 3; ++j) {
                if (!float_finite(cov[i][j])) {
                    return invalid("world covariance exceeds float32 range");
                }
            }
        }
        std::array<glm::dvec4, 6> const planes = {row(vp, 3) + row(vp, 0), row(vp, 3) - row(vp, 0), row(vp, 3) + row(vp, 1), row(vp, 3) - row(vp, 1), row(vp, 2), row(vp, 3) - row(vp, 2)};
        double near_distance = 0;
        double near_radius = 0;
        for (std::size_t i = 0; i < planes.size(); ++i) {
            glm::dvec3 const n{planes[i]};
            double const distance = glm::dot(planes[i], world);
            double const radius = 3 * std::sqrt(std::max(0.0, glm::dot(n, cov * n)));
            if (!float_finite(distance) || !float_finite(radius)) {
                return invalid("raster support exceeds float32 range");
            }
            if (distance + radius < 0) {
                return out;
            }
            if (i == 4) {
                near_distance = distance;
                near_radius = radius;
            }
        }
        if (near_distance - near_radius <= 0) {
            out.visibility = projection_visibility::near_omitted;
            return out;
        }
        glm::dvec2 const ndc{clip.x / clip.w, clip.y / clip.w};
        glm::dvec3 const jx = 0.5 * camera.extent.width * (glm::dvec3{row(vp, 0)} - ndc.x * glm::dvec3{row(vp, 3)}) / clip.w;
        glm::dvec3 const jy = 0.5 * camera.extent.height * (glm::dvec3{row(vp, 1)} - ndc.y * glm::dvec3{row(vp, 3)}) / clip.w;
        double const xx = glm::dot(jx, cov * jx) + 0.3;
        double const xy = glm::dot(jx, cov * jy);
        double const yy = glm::dot(jy, cov * jy) + 0.3;
        if (xx <= 0 || yy <= 0 || xx * yy - xy * xy <= 0) {
            return invalid("projected covariance is not positive definite");
        }
        double const bx = std::sqrt(xx);
        double const by = xy / bx;
        double const cy = std::sqrt(std::max(0.3, yy - by * by));
        glm::dvec2 const pixel{(ndc.x + 1) * 0.5 * camera.extent.width, (ndc.y + 1) * 0.5 * camera.extent.height};
        if (!float_finite(xx) || !float_finite(xy) || !float_finite(yy) || !float_finite(bx) || !float_finite(by) || !float_finite(cy) || !float_finite(pixel.x) || !float_finite(pixel.y)) {
            return invalid("projected covariance exceeds float32 range");
        }
        out.visibility = projection_visibility::visible;
        out.center_pixel = glm::vec2{pixel};
        out.covariance = {static_cast<float>(xx), static_cast<float>(xy), static_cast<float>(yy)};
        out.basis_x = {static_cast<float>(bx), static_cast<float>(by)};
        out.basis_y = {0, static_cast<float>(cy)};
        out.depth = static_cast<float>(std::min(1.0, clip.z / clip.w));
        return out;
    }
    std::expected<glm::vec3, foundation_error> evaluate_sh(sh_record const& sh, std::uint32_t degree, glm::vec3 const& direction) {
        if (degree > 3) {
            return invalid("SH degree exceeds three");
        }
        for (float value : sh.coefficients) {
            if (!std::isfinite(value)) {
                return invalid("nonfinite SH coefficient");
            }
        }
        glm::dvec3 const v{direction};
        double const norm = glm::length(v);
        if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-5) {
            return invalid("SH direction must be finite and normalized");
        }
        double const x = v.x;
        double const y = v.y;
        double const z = v.z;
        double const xx = x * x;
        double const yy = y * y;
        double const zz = z * z;
        std::array<double, 16> const basis = {
            0.28209479177387814,
            -0.4886025119029199 * y, 0.4886025119029199 * z, -0.4886025119029199 * x,
            1.0925484305920792 * x * y, -1.0925484305920792 * y * z, 0.31539156525252005 * (2 * zz - xx - yy),
            -1.0925484305920792 * x * z, 0.5462742152960396 * (xx - yy),
            -0.5900435899266435 * y * (3 * xx - yy), 2.890611442640554 * x * y * z,
            -0.4570457994644658 * y * (4 * zz - xx - yy), 0.3731763325901154 * z * (2 * zz - 3 * xx - 3 * yy),
            -0.4570457994644658 * x * (4 * zz - xx - yy), 1.445305721320277 * z * (xx - yy), -0.5900435899266435 * x * (xx - 3 * yy)};
        glm::dvec3 color{0.5};
        std::uint32_t const used = (degree + 1) * (degree + 1);
        for (std::uint32_t i = 0; i < used; ++i) {
            for (glm::length_t channel = 0; channel < 3; ++channel) {
                color[channel] += basis[i] * sh.coefficients[i * 3 + static_cast<std::uint32_t>(channel)];
                if (!float_finite(color[channel])) {
                    return invalid("SH radiance exceeds float32 range");
                }
            }
        }
        return glm::vec3{glm::max(color, glm::dvec3{0})};
    }
    float fragment_alpha(float opacity, glm::vec2 const& coordinate) noexcept {
        if (!std::isfinite(opacity) || opacity < 0 || opacity > 1 || !std::isfinite(coordinate.x) || !std::isfinite(coordinate.y)) {
            return 0;
        }
        double const radius = glm::dot(glm::dvec2{coordinate}, glm::dvec2{coordinate});
        if (radius > 9) {
            return 0;
        }
        float const alpha = std::min(0.99f, static_cast<float>(opacity * std::exp(-0.5 * radius)));
        return alpha < 1.0f / 255.0f ? 0 : alpha;
    }
} // namespace deren::engine::gaussian_splatting
