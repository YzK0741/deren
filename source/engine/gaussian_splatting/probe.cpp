module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <span>
#include <string>
#include <vector>
module deren.engine.gaussian_splatting.probe;
import deren.engine.pass;
import deren.engine.pass.gaussian_splatting;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    namespace pass = deren::engine::pass;
    namespace {
        constexpr float tolerance = 0.005f;
        auto failure(std::string message, error_code code = error_code::invalid_argument) {
            return std::unexpected(foundation_error{code, std::move(message)});
        }
        deren::gaussian::particle particle(glm::vec3 center, glm::vec3 color, glm::vec3 scale) {
            deren::gaussian::particle p;
            p.center = {center.x, center.y, center.z};
            p.scale = {scale.x, scale.y, scale.z};
            p.rotation = {1, 0, 0, 0};
            p.opacity = 0.5f;
            for (std::size_t i = 0; i < 3; ++i) {
                p.sh[i] = (color[static_cast<glm::length_t>(i)] - 0.5f) / 0.28209479177387814f;
            }
            return p;
        }
        raster_camera simple_camera() {
            raster_camera camera;
            camera.extent = {33, 33};
            camera.view_projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
            camera.view_projection[1][1] *= -1;
            return camera;
        }
        glm::vec3 read_pixel(std::span<std::byte const> bytes, std::size_t pixel) {
            std::uint32_t rg = 0;
            std::uint32_t ba = 0;
            std::memcpy(&rg, bytes.data() + pixel * 8, 4);
            std::memcpy(&ba, bytes.data() + pixel * 8 + 4, 4);
            auto const red_green = glm::unpackHalf2x16(rg);
            return {red_green.x, red_green.y, glm::unpackHalf2x16(ba).x};
        }
        struct reference_splat {
            projected_splat projection;
            glm::vec3 color;
            float opacity;
        };
        std::expected<std::vector<glm::vec3>, foundation_error> reference_image(
            std::span<raster_instance const> inputs, raster_frame const& frame, raster_camera const& camera, float depth) {
            std::vector<packed_asset> packed;
            for (auto const& input : inputs) {
                auto value = pack_asset(input.asset->source());
                if (!value) {
                    return std::unexpected(value.error());
                }
                packed.push_back(std::move(*value));
            }
            std::vector<reference_splat> splats;
            for (auto const& ref : frame.references()) {
                auto const& input = inputs[ref.instance_index];
                auto projected = project_splat(input.asset->geometry_records()[ref.local_gaussian_index], input.model, camera);
                if (!projected) {
                    return std::unexpected(projected.error());
                }
                auto inverse = validate_model(input.model);
                if (!inverse) {
                    return std::unexpected(inverse.error());
                }
                auto const& p = input.asset->source().particles[ref.local_gaussian_index];
                glm::vec3 const center{p.center[0], p.center[1], p.center[2]};
                glm::vec3 const eye{*inverse * glm::vec4{camera.position, 1}};
                auto color = evaluate_sh(packed[ref.instance_index].sh[ref.local_gaussian_index], input.asset->source().sh_degree, glm::normalize(center - eye));
                if (!color) {
                    return std::unexpected(color.error());
                }
                splats.push_back({*projected, *color, p.opacity});
            }
            std::vector<glm::vec3> pixels(static_cast<std::size_t>(camera.extent.width) * camera.extent.height, glm::vec3{0});
            for (std::uint32_t y = 0; y < camera.extent.height; ++y) {
                for (std::uint32_t x = 0; x < camera.extent.width; ++x) {
                    auto& color = pixels[static_cast<std::size_t>(y) * camera.extent.width + x];
                    for (auto const& splat : splats) {
                        if (splat.projection.depth > depth) {
                            continue;
                        }
                        glm::vec2 const offset = glm::vec2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f} - splat.projection.center_pixel;
                        float const u = offset.x / splat.projection.basis_x.x;
                        glm::vec2 const coordinate{u, (offset.y - u * splat.projection.basis_x.y) / splat.projection.basis_y.y};
                        float const alpha = fragment_alpha(splat.opacity, coordinate);
                        color = splat.color * alpha + color * (1 - alpha);
                    }
                }
            }
            return pixels;
        }
        struct shader_pair {
            std::span<std::uint8_t const> vertex, fragment;
        };
        std::expected<std::vector<std::byte>, foundation_error> render_image(
            rhi::api_core& core, rhi::descriptor_heap& heap, pass::gaussian_splatting_pass& raster,
            std::shared_ptr<raster_frame const> const& frame, raster_camera const& camera, float clear_depth) {
            rhi::image_desc desc;
            desc.extent = camera.extent;
            desc.format = rhi::image_format::r16g16b16a16_sfloat;
            desc.flags = rhi::to_bits(rhi::image_flag::color_attachment) | rhi::to_bits(rhi::image_flag::host_transfer);
            rhi::object_manager<rhi::image> color{core.create_image(desc)};
            desc.format = rhi::image_format::depth;
            desc.flags = rhi::to_bits(rhi::image_flag::depth_attachment) | rhi::to_bits(rhi::image_flag::sampled);
            rhi::object_manager<rhi::image> depth{core.create_image(desc)};
            if (!color || !depth) {
                return failure("Gaussian probe target creation failed", error_code::allocation_failure);
            }
            rhi::object_manager<rhi::image_view> color_view{color->make_view({.role = rhi::view_role::color_attachment})};
            rhi::object_manager<rhi::image_view> depth_view{depth->make_view({.role = rhi::view_role::depth_attachment})};
            auto commands = core.make_command_buffer({.kind = rhi::command_buffer_kind::primary});
            if (!color_view || !depth_view || !commands) {
                return failure("Gaussian probe recording resources failed", error_code::allocation_failure);
            }
            if (commands->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) != rhi::error::ok ||
                heap.bind({.commands = commands.get()}) != rhi::error::ok) {
                return failure("Gaussian probe recording/binding refused");
            }
            std::array<rhi::image_barrier, 2> const initial = {
                rhi::image_barrier{.resource = color.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::color_attachment},
                rhi::image_barrier{.resource = depth.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::depth_attachment}};
            if (commands->barrier({.images = initial}) != rhi::error::ok) {
                return failure("Gaussian probe target transition refused");
            }
            std::array<rhi::color_attachment, 1> const colors = {rhi::color_attachment{.view = color_view.get(), .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0, 0, 0, 0}}};
            rhi::rendering_info clear;
            clear.area = {.width = camera.extent.width, .height = camera.extent.height};
            clear.colors = colors;
            clear.has_depth = true;
            clear.depth = {.view = depth_view.get(), .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear_depth = clear_depth};
            if (commands->begin_rendering(clear) != rhi::error::ok) {
                return failure("Gaussian probe clear refused");
            }
            commands->end_rendering();
            if (commands->barrier(rhi::image_barrier{.resource = depth.get(), .from = rhi::image_use::depth_attachment, .to = rhi::image_use::shader_read}) != rhi::error::ok) {
                return failure("Gaussian probe depth transition refused");
            }
            raster.set_frame(frame);
            pass::resolved_io io;
            io.list = commands.get();
            io.extent = camera.extent;
            io.target_storage[0] = {.image_handle = color.get(), .view_handle = color_view.get()};
            io.target_storage[1] = {.image_handle = depth.get(), .view_handle = depth_view.get()};
            io.targets = std::span{io.target_storage}.first(2);
            io.push_block = {&heap, [](void* owner, rhi::command_buffer& list, std::span<std::byte const> bytes, std::uint32_t offset) {
                                 return static_cast<rhi::descriptor_heap*>(owner)->push_data({.commands = &list, .offset = offset, .data = bytes}) == rhi::error::ok;
                             }};
            raster.record(io);
            if (raster.last_error() != rhi::error::ok) {
                return failure("Gaussian probe raster recording refused");
            }
            if (commands->end_recording() != rhi::error::ok || core.submit(*commands) != rhi::error::ok) {
                return failure("Gaussian probe submission refused");
            }
            // All frame/asset/target owners remain in scope through completion.
            core.wait_idle();
            auto content = color->get_content();
            if (!content || content->bytes_per_pixel != 8 || content->extent.width != camera.extent.width || content->extent.height != camera.extent.height ||
                content->bytes.size() != static_cast<std::size_t>(camera.extent.width) * camera.extent.height * 8) {
                return failure("Gaussian probe HDR readback failed");
            }
            return std::move(content->bytes);
        }
    } // namespace
    std::expected<raster_probe_report, foundation_error> run_raster_probe(rhi::api_core& core, std::span<std::uint8_t const> vertex, std::span<std::uint8_t const> fragment) {
        if (vertex.empty() || fragment.empty() || vertex.size() % 4 != 0 || fragment.size() % 4 != 0) {
            return failure("Gaussian probe requires both SPIR-V stages");
        }
        auto* heap = rhi::query_extension<rhi::descriptor_heap>(core);
        if (heap == nullptr || !heap->ready()) {
            return failure("Gaussian probe requires push-data support", error_code::unsupported);
        }
        shader_pair pair{vertex, fragment};
        pass::pass_context context;
        context.face = &core;
        context.depth_format = rhi::image_format::depth;
        context.owner = &pair;
        context.shader = [](void* owner, std::string_view name) {
            auto const& shaders = *static_cast<shader_pair*>(owner);
            return name == "gaussian_raster.vert.spv" ? shaders.vertex : shaders.fragment;
        };
        pass::gaussian_splatting_pass raster;
        raster.create(context);
        if (!raster.ready()) {
            return failure("Gaussian probe pipeline creation failed", error_code::unsupported);
        }
        raster_probe_report report;
        auto check = [&](std::span<raster_instance const> inputs, raster_camera const& camera, float depth, char const* name, bool known_center = false, bool known_edge = false) -> std::expected<void, foundation_error> {
            auto frame = prepare_raster_frame(core, inputs, camera);
            if (!frame) {
                return std::unexpected(frame.error());
            }
            auto expected = reference_image(inputs, **frame, camera, depth);
            if (!expected) {
                return std::unexpected(expected.error());
            }
            auto actual = render_image(core, *heap, raster, *frame, camera, depth);
            if (!actual) {
                return std::unexpected(actual.error());
            }
            if (known_center) {
                glm::vec3 const center = read_pixel(*actual, 16 * 33 + 16);
                if (glm::any(glm::greaterThan(glm::abs(center - glm::vec3{0.5f, 0, 0.25f}), glm::vec3{tolerance}))) {
                    return failure(std::format("{}: center RGB {},{},{} expected0.5,0,0.25", name, center.x, center.y, center.z));
                }
            }
            if (known_edge) {
                glm::vec3 const edge = read_pixel(*actual, 16 * 33 + 32);
                glm::vec3 const wanted{0, 0.5f * std::exp(-0.5f / 0.3f), 0};
                if (glm::any(glm::greaterThan(glm::abs(edge - wanted), glm::vec3{tolerance}))) {
                    return failure(std::format("{}: edge RGB {},{},{} expected green{}", name, edge.x, edge.y, edge.z, wanted.y));
                }
            }
            for (std::size_t i = 0; i < expected->size(); ++i) {
                glm::vec3 const rgb = read_pixel(*actual, i);
                if (!std::isfinite(rgb.x) || !std::isfinite(rgb.y) || !std::isfinite(rgb.z)) {
                    return failure(std::format("{}: pixel{} contains nonfinite RGB", name, i));
                }
                glm::vec3 const delta = glm::abs(rgb - (*expected)[i]);
                float const error = std::max({delta.x, delta.y, delta.z});
                if (!std::isfinite(error) || error > tolerance) {
                    return failure(std::format("{}: pixel{} RGB {},{},{} expected {},{},{} (error{})", name, i, rgb.x, rgb.y, rgb.z, (*expected)[i].x, (*expected)[i].y, (*expected)[i].z, error));
                }
                report.maximum_error = std::max(report.maximum_error, error);
            }
            ++report.cases;
            report.pixels += static_cast<std::uint32_t>(expected->size());
            return {};
        };
        deren::gaussian::asset overlap;
        overlap.particles = {particle({0, 0, -3}, {0, 0, 1}, {0.12f, 0.12f, 0.06f}), particle({0, 0, -2}, {1, 0, 0}, {0.12f, 0.05f, 0.03f})};
        auto overlap_asset = make_raster_asset(core, std::move(overlap));
        if (!overlap_asset) {
            return std::unexpected(overlap_asset.error());
        }
        std::array<raster_instance, 1> const overlap_inputs = {raster_instance{*overlap_asset, glm::mat4{1}, 1}};
        auto camera = simple_camera();
        if (auto result = check(overlap_inputs, camera, 1, "overlap", true); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = check(overlap_inputs, camera, 0.5f, "opaque depth"); !result) {
            return std::unexpected(result.error());
        }
        deren::gaussian::asset harmonics;
        harmonics.sh_degree = 1;
        auto sign = particle({0, 0, -2}, {0.5f, 0.5f, 0.5f}, {0.09f, 0.09f, 0.09f});
        sign.sh[6] = 1;
        harmonics.particles.push_back(sign);
        auto sh_asset = make_raster_asset(core, std::move(harmonics));
        if (!sh_asset) {
            return std::unexpected(sh_asset.error());
        }
        std::array<raster_instance, 1> const sh_inputs = {raster_instance{*sh_asset, glm::mat4{1}, 2}};
        if (auto result = check(sh_inputs, camera, 1, "SH direction"); !result) {
            return std::unexpected(result.error());
        }
        deren::gaussian::asset transformed;
        transformed.sh_degree = 3;
        auto p = particle({0, 0.05f, 0}, {0.3f, 1.5f, 0.7f}, {0.10f, 0.26f, 0.06f});
        p.rotation = {0.9238795f, 0, 0, 0.3826834f};
        for (std::size_t i = 3; i < p.sh.size(); ++i) {
            p.sh[i] = static_cast<float>(static_cast<int>(i % 7) - 3) * 0.025f;
        }
        transformed.particles = {particle({40, 0, 0}, {1, 0, 0}, {0.1f, 0.1f, 0.1f}), p};
        auto transformed_asset = make_raster_asset(core, std::move(transformed));
        if (!transformed_asset) {
            return std::unexpected(transformed_asset.error());
        }
        auto model = glm::translate(glm::mat4{1}, glm::vec3{0.2f, -0.1f, 0.3f});
        model = glm::rotate(model, glm::radians(29.0f), glm::normalize(glm::vec3{1, 2, 3}));
        model = glm::scale(model, glm::vec3{1.3f});
        std::array<raster_instance, 2> const transformed_inputs = {raster_instance{*transformed_asset, model, 10}, raster_instance{*transformed_asset, glm::translate(model, glm::vec3{0.18f, 0, -0.12f}), 20}};
        camera.position = {2, 1, 3};
        camera.extent = {33, 25};
        auto projection = glm::perspectiveRH_ZO(glm::radians(67.0f), 1.4f, 0.1f, 50.0f);
        projection[1][1] *= -1;
        projection[2][0] = 0.13f;
        projection[2][1] = -0.07f;
        camera.view_projection = projection * glm::lookAtRH(camera.position, glm::vec3{0}, glm::vec3{0, 1, 0});
        if (auto result = check(transformed_inputs, camera, 1, "transformed anisotropic SH3"); !result) {
            return std::unexpected(result.error());
        }
        camera = simple_camera();
        deren::gaussian::asset edge_source;
        edge_source.particles = {particle({2.0f + 2.0f / 33.0f, 0, -2}, {0, 1, 0}, {1e-6f, 1e-6f, 1e-6f})};
        auto edge_asset = make_raster_asset(core, std::move(edge_source));
        if (!edge_asset) {
            return std::unexpected(edge_asset.error());
        }
        std::array<raster_instance, 1> const edge_inputs = {raster_instance{*edge_asset, glm::mat4{1}, 30}};
        if (auto result = check(edge_inputs, camera, 1, "regularized edge", false, true); !result) {
            return std::unexpected(result.error());
        }
        return report;
    }
} // namespace deren::engine::gaussian_splatting
