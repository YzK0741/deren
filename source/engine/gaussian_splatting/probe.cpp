module;
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <numeric>
#include <span>
#include <vector>
module deren.engine.gaussian_splatting.probe;
import deren.utility.shared;
import deren.engine.pass;
import deren.engine.pass.gaussian_splatting;
import deren.engine.gaussian_splatting.gpu_frame;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    namespace pass = deren::engine::pass;
    namespace {
        constexpr float tolerance = 0.005f;
        auto failure(deren::utility::vessel::string message, error_code code = error_code::invalid_argument) {
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
        std::expected<deren::utility::vessel::vector<glm::vec3>, foundation_error> reference_image(
            std::span<raster_instance const> inputs, raster_frame const& frame, raster_camera const& camera, float depth) {
            deren::utility::vessel::vector<packed_asset> packed;
            for (auto const& input : inputs) {
                auto value = pack_asset(input.asset->source());
                if (!value) {
                    return std::unexpected(value.error());
                }
                packed.push_back(std::move(*value));
            }
            deren::utility::vessel::vector<reference_splat> splats;
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
            deren::utility::vessel::vector<glm::vec3> pixels(static_cast<std::size_t>(camera.extent.width) * camera.extent.height, glm::vec3{0});
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
            deren::utility::vessel::array<std::span<std::uint8_t const>, 3> compute;
        };
        std::expected<deren::utility::vessel::vector<std::byte>, foundation_error> render_image(
            rhi::api_core& core, rhi::descriptor_heap& heap, pass::gaussian_splatting_pass& raster,
            std::shared_ptr<raster_frame const> const& frame, raster_camera const& camera, float clear_depth, std::shared_ptr<gpu_frame const> const& gpu = {}) {
            rhi::image_desc desc;
            desc.extent = camera.extent;
            desc.format = rhi::image_format::r16g16b16a16_sfloat;
            desc.flags = rhi::to_bits(rhi::image_flag::color_attachment) | rhi::to_bits(rhi::image_flag::host_transfer);
            rhi::unique_object<rhi::image> color{core.resources->create_image(desc)};
            desc.format = rhi::image_format::depth;
            desc.flags = rhi::to_bits(rhi::image_flag::depth_attachment) | rhi::to_bits(rhi::image_flag::sampled);
            rhi::unique_object<rhi::image> depth{core.resources->create_image(desc)};
            if (!color || !depth) {
                return failure("Gaussian probe target creation failed", error_code::allocation_failure);
            }
            rhi::unique_object<rhi::image_view> color_view{color->make_view({.role = rhi::view_role::color_attachment})};
            rhi::unique_object<rhi::image_view> depth_view{depth->make_view({.role = rhi::view_role::depth_attachment})};
            auto commands = core.commands->make_command_buffer({.kind = rhi::command_buffer_kind::primary});
            if (!color_view || !depth_view || !commands) {
                return failure("Gaussian probe recording resources failed", error_code::allocation_failure);
            }
            if (commands->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) != rhi::error::ok ||
                heap.bind({.commands = commands.get()}) != rhi::error::ok) {
                return failure("Gaussian probe recording/binding refused");
            }
            deren::utility::vessel::array<rhi::image_barrier, 2> const initial = {
                rhi::image_barrier{.resource = color.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::color_attachment},
                rhi::image_barrier{.resource = depth.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::depth_attachment},
            };
            if (commands->barrier({.images = initial}) != rhi::error::ok) {
                return failure("Gaussian probe target transition refused");
            }
            deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {rhi::color_attachment{.view = color_view.get(), .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0, 0, 0, 0}}};
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
            if (gpu) {
                raster.set_gpu_frame(gpu);
            } else {
                raster.set_frame(frame);
            }
            pass::resolved_io io;
            io.list = commands.get();
            io.extent = camera.extent;
            io.target_storage[0] = {.image_handle = color.get(), .view_handle = color_view.get()};
            io.target_storage[1] = {.image_handle = depth.get(), .view_handle = depth_view.get()};
            io.targets = std::span{io.target_storage}.first(2);
            io.push_block = {
                nullptr,
                [](void*, rhi::command_buffer& list, std::span<std::byte const> bytes, std::uint32_t extra_lane) {
                    // Match runtime::push_stage_block, including all three heap index lanes.
                    deren::utility::vessel::vector<std::byte> transmitted(bytes.begin(), bytes.end());
                    deren::utility::vessel::array<std::uint32_t, 3> const lanes = {0u, 0u, extra_lane};
                    auto const lane_bytes = std::as_bytes(std::span{lanes});
                    transmitted.insert(transmitted.end(), lane_bytes.begin(), lane_bytes.end());
                    return list.push_constants(0u, transmitted) == rhi::error::ok;
                },
            };
            raster.record_owned(io);
            if (raster.last_error() != rhi::error::ok) {
                return failure("Gaussian probe raster recording refused");
            }
            if (commands->end_recording() != rhi::error::ok || core.commands->submit(*commands) != rhi::error::ok) {
                return failure("Gaussian probe submission refused");
            }
            // All frame/asset/target owners remain in scope through completion.
            core.wait_idle();
            auto content = color->get_content();
            if (!content || content->bytes_per_pixel != 8 || content->extent.width != camera.extent.width || content->extent.height != camera.extent.height ||
                content->bytes.size() != static_cast<std::size_t>(camera.extent.width) * camera.extent.height * 8) {
                return failure("Gaussian probe HDR readback failed");
            }
            // RHI readback keeps its existing ABI container; the diagnostic owns a vessel copy.
            return deren::utility::vessel::vector<std::byte>(content->bytes.begin(), content->bytes.end());
        }
        std::expected<void, foundation_error> verify_gpu_references(rhi::api_core& core, gpu_frame const& frame, std::span<raster_instance const> inputs, raster_camera const& camera, std::span<draw_reference const> cpu_references, raster_probe_report& report) {
            auto const count = frame.candidate_count();
            if (!count) {
                return {};
            }
            std::uint64_t const lane_bytes = static_cast<std::uint64_t>(count) * 4u;
            rhi::buffer_desc desc;
            desc.size = lane_bytes * 4u + 16u;
            desc.usage = rhi::buffer_usage::readback_coherent;
            rhi::unique_object<rhi::buffer> readback{core.resources->create_buffer(desc)};
            auto list = core.commands->make_command_buffer({.kind = rhi::command_buffer_kind::primary});
            if (!readback || !list || list->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) != rhi::error::ok) {
                return failure("GPU reference readback allocation failed");
            }
            deren::utility::vessel::array<rhi::buffer*, 4> const sources = {frame.keys_buffer(), frame.values_buffer(), frame.references_buffer(), frame.draw_buffer()};
            deren::utility::vessel::array<std::uint64_t, 4> const sizes = {lane_bytes, lane_bytes, lane_bytes * 2u, 16u};
            std::uint64_t offset = 0;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                rhi::buffer_barrier const barrier{.resource = sources[i], .from = rhi::buffer_use::shader_read_write, .to = rhi::buffer_use::transfer_source};
                if (list->barrier(rhi::barrier_group{.buffers = std::span{&barrier, 1}}) != rhi::error::ok ||
                    list->copy_buffer(*readback, *sources[i], sizes[i], 0u, offset) != rhi::error::ok) {
                    return failure("GPU reference copy refused");
                }
                offset += sizes[i];
            }
            rhi::buffer_barrier const host{.resource = readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
            if (list->barrier(rhi::barrier_group{.buffers = std::span{&host, 1}}) != rhi::error::ok || list->end_recording() != rhi::error::ok || core.commands->submit(*list) != rhi::error::ok) {
                return failure("GPU reference readback submission failed");
            }
            core.wait_idle(); // isolated diagnostic, never used by the viewer
            if (readback->mapped().size() < desc.size) {
                return failure("GPU reference readback is not mapped");
            }
            deren::utility::vessel::vector<std::uint32_t> keys(count);
            deren::utility::vessel::vector<std::uint32_t> values(count);
            deren::utility::vessel::vector<std::uint32_t> raw_keys(count);
            deren::utility::vessel::vector<std::uint32_t> expected(count);
            deren::utility::vessel::vector<std::uint32_t> indices(inputs.size());
            deren::utility::vessel::vector<draw_reference> references(count);
            deren::utility::vessel::vector<draw_reference> candidates;
            rhi::draw_indirect_command draw;
            auto const* bytes = readback->mapped().data();
            std::memcpy(keys.data(), bytes, static_cast<std::size_t>(lane_bytes));
            std::memcpy(values.data(), bytes + lane_bytes, static_cast<std::size_t>(lane_bytes));
            std::memcpy(references.data(), bytes + 2u * lane_bytes, static_cast<std::size_t>(2u * lane_bytes));
            std::memcpy(&draw, bytes + 4u * lane_bytes, sizeof(draw));
            deren::utility::vessel::vector<bool> seen(count, false);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (values[i] >= count || seen[values[i]]) {
                    return failure("GPU sort values are not a permutation");
                }
                seen[values[i]] = true;
                raw_keys[values[i]] = keys[i];
            }
            std::iota(expected.begin(), expected.end(), 0u);
            std::stable_sort(expected.begin(), expected.end(), [&](auto a, auto b) { return raw_keys[a] < raw_keys[b]; });
            if (expected != values) {
                return failure("GPU actual depth keys disagree with CPU stable_sort");
            }
            std::iota(indices.begin(), indices.end(), 0u);
            std::sort(indices.begin(), indices.end(), [&](auto a, auto b) { return inputs[a].stable_id < inputs[b].stable_id; });
            for (auto index : indices) {
                for (std::uint32_t local = 0; local < inputs[index].asset->gpu()->particle_count(); ++local) {
                    candidates.push_back({index, local});
                }
            }
            auto const stats = frame.completed_stats(0u);
            auto const sort_view = validate_raster_camera(camera);
            if (!sort_view) {
                return std::unexpected(sort_view.error());
            }
            if (draw.vertex_count != 6u || draw.instance_count != stats.visible || draw.first_vertex || draw.first_instance) {
                return failure("GPU indirect parameters disagree with visibility");
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                if ((keys[i] != UINT32_MAX) != (i < stats.visible)) {
                    return failure("GPU visible prefix is not contiguous");
                }
                if (i < stats.visible && (references[i].instance_index != candidates[values[i]].instance_index || references[i].local_gaussian_index != candidates[values[i]].local_gaussian_index)) {
                    return failure("GPU reference expansion disagrees with sorted indices");
                }
                if (i < stats.visible) {
                    auto const ref = references[i];
                    auto const& input = inputs[ref.instance_index];
                    auto const& center = input.asset->source().particles[ref.local_gaussian_index].center;
                    glm::vec3 const world{input.model * glm::vec4{center[0], center[1], center[2], 1}};
                    float const depth = glm::dot(world - camera.position, sort_view->forward);
                    std::uint32_t const cpu_bits = std::bit_cast<std::uint32_t>(depth);
                    std::uint32_t const negative_bits = (keys[i] & 0x80000000u) ? keys[i] ^ 0x80000000u : ~keys[i];
                    std::uint32_t const gpu_bits = std::bit_cast<std::uint32_t>(-std::bit_cast<float>(negative_bits));
                    std::uint32_t const ulp = cpu_bits > gpu_bits ? cpu_bits - gpu_bits : gpu_bits - cpu_bits;
                    report.maximum_depth_ulp = std::max(report.maximum_depth_ulp, ulp);
                    if (ulp > 4u) {
                        return failure("GPU forward depth exceeds four float32 ULP from CPU");
                    }
                    if (i < cpu_references.size() && (ref.instance_index != cpu_references[i].instance_index || ref.local_gaussian_index != cpu_references[i].local_gaussian_index)) {
                        ++report.depth_order_differences;
                    }
                }
            }
            return {};
        }
    } // namespace
    std::expected<raster_probe_report, foundation_error> run_raster_probe(rhi::api_core& core, std::span<std::uint8_t const> vertex, std::span<std::uint8_t const> fragment,
                                                                          deren::utility::vessel::array<std::span<std::uint8_t const>, 3> compute, bool gpu) {
        if (vertex.empty() || fragment.empty() || vertex.size() % 4 != 0 || fragment.size() % 4 != 0) {
            return failure("Gaussian probe requires both SPIR-V stages");
        }
        auto* heap = rhi::query_extension<rhi::descriptor_heap>(core);
        if (heap == nullptr || !heap->ready()) {
            return failure("Gaussian probe requires push-data support", error_code::unsupported);
        }
        shader_pair pair{vertex, fragment, compute};
        pass::pass_context context;
        context.face = &core;
        context.depth_format = rhi::image_format::depth;
        context.owner = &pair;
        context.shader = [](void* owner, std::string_view name) {
            auto const& shaders = *static_cast<shader_pair*>(owner);
            if (name == "gaussian_reset.comp.spv") {
                return shaders.compute[0];
            }
            if (name == "gaussian_prepare.comp.spv") {
                return shaders.compute[1];
            }
            if (name == "gaussian_finalize.comp.spv") {
                return shaders.compute[2];
            }
            if (name == "gaussian_raster.vert.spv") {
                return shaders.vertex;
            }
            if (name == "gaussian_raster.frag.spv") {
                return shaders.fragment;
            }
            return std::span<std::uint8_t const>{};
        };
        pass::gaussian_splatting_pass raster;
        raster.create(context);
        if (!raster.ready()) {
            return failure("Gaussian probe pipeline creation failed", error_code::unsupported);
        }
        if (gpu && !raster.gpu_ready()) {
            return failure("Gaussian GPU preparation pipelines unavailable", error_code::unsupported);
        }
        raster_probe_report report;
        std::shared_ptr<gpu_frame> completed_probe_slot;
        auto check = [&](std::span<raster_instance const> inputs, raster_camera const& camera, float depth, char const* name, bool known_center = false, bool known_edge = false) -> std::expected<void, foundation_error> {
            auto frame = prepare_raster_frame(core, inputs, camera);
            if (!frame) {
                return std::unexpected(frame.error());
            }
            auto expected = reference_image(inputs, **frame, camera, depth);
            if (!expected) {
                return std::unexpected(expected.error());
            }
            std::shared_ptr<gpu_frame> gpu_snapshot;
            if (gpu) {
                foundation_limits const limits{};
                auto prepared = prepare_gpu_frame(core, inputs, camera, limits, completed_probe_slot.get());
                if (!prepared) {
                    return std::unexpected(prepared.error());
                }
                gpu_snapshot = *prepared;
                completed_probe_slot = gpu_snapshot;
            }
            auto actual = render_image(core, *heap, raster, *frame, camera, depth, gpu_snapshot);
            if (!actual) {
                return std::unexpected(actual.error());
            }
            if (gpu_snapshot) {
                auto const stats = gpu_snapshot->completed_stats(report.cases);
                if (stats.errors || stats.visible != (*frame)->params().visible_count || stats.outside != (*frame)->outside_count() || stats.near_omitted != (*frame)->near_omitted_count()) {
                    return failure(deren::utility::vessel::format("{}: GPU statistics visible{} outside{} near{} errors{} differ from CPU", name, stats.visible, stats.outside, stats.near_omitted, stats.errors));
                }
                // Re-record the same completed slot to detect stale reset counts and unstable duplicate keys.
                auto repeat = render_image(core, *heap, raster, *frame, camera, depth, gpu_snapshot);
                if (!repeat || *repeat != *actual) {
                    return failure(deren::utility::vessel::format("{}: GPU repeat is not deterministic", name));
                }
                if (auto output = verify_gpu_references(core, *gpu_snapshot, inputs, camera, (*frame)->references(), report); !output) {
                    return std::unexpected(output.error());
                }
            }
            if (known_center) {
                glm::vec3 const center = read_pixel(*actual, 16 * 33 + 16);
                if (glm::any(glm::greaterThan(glm::abs(center - glm::vec3{0.5f, 0, 0.25f}), glm::vec3{tolerance}))) {
                    return failure(deren::utility::vessel::format("{}: center RGB {},{},{} expected0.5,0,0.25", name, center.x, center.y, center.z));
                }
            }
            if (known_edge) {
                glm::vec3 const edge = read_pixel(*actual, 16 * 33 + 32);
                glm::vec3 const wanted{0, 0.5f * std::exp(-0.5f / 0.3f), 0};
                if (glm::any(glm::greaterThan(glm::abs(edge - wanted), glm::vec3{tolerance}))) {
                    return failure(deren::utility::vessel::format("{}: edge RGB {},{},{} expected green{}", name, edge.x, edge.y, edge.z, wanted.y));
                }
            }
            for (std::size_t i = 0; i < expected->size(); ++i) {
                glm::vec3 const rgb = read_pixel(*actual, i);
                if (!std::isfinite(rgb.x) || !std::isfinite(rgb.y) || !std::isfinite(rgb.z)) {
                    return failure(deren::utility::vessel::format("{}: pixel{} contains nonfinite RGB", name, i));
                }
                glm::vec3 const delta = glm::abs(rgb - (*expected)[i]);
                float const error = std::max({delta.x, delta.y, delta.z});
                if (!std::isfinite(error) || error > tolerance) {
                    return failure(deren::utility::vessel::format("{}: pixel{} RGB {},{},{} expected {},{},{} (error{})", name, i, rgb.x, rgb.y, rgb.z, (*expected)[i].x, (*expected)[i].y, (*expected)[i].z, error));
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
        deren::utility::vessel::array<raster_instance, 1> const overlap_inputs = {raster_instance{*overlap_asset, glm::mat4{1}, 1}};
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
        deren::utility::vessel::array<raster_instance, 1> const sh_inputs = {raster_instance{*sh_asset, glm::mat4{1}, 2}};
        if (auto result = check(sh_inputs, camera, 1, "SH direction"); !result) {
            return std::unexpected(result.error());
        }
        deren::gaussian::asset transformed;
        transformed.sh_degree = 3;
        auto p = particle({0, 0.05f, 0}, {0.3f, 1.5f, 0.7f}, {0.10f, 0.26f, 0.06f});
        p.rotation = {0.9238795f, 0, 0, 0.3826834f};
        for (std::size_t i = 3; i < p.sh.size(); ++i) {
            p.sh[i] = static_cast<float>(static_cast<int32_t>(i % 7) - 3) * 0.025f;
        }
        transformed.particles = {particle({40, 0, 0}, {1, 0, 0}, {0.1f, 0.1f, 0.1f}), p};
        auto transformed_asset = make_raster_asset(core, std::move(transformed));
        if (!transformed_asset) {
            return std::unexpected(transformed_asset.error());
        }
        auto model = glm::translate(glm::mat4{1}, glm::vec3{0.2f, -0.1f, 0.3f});
        model = glm::rotate(model, glm::radians(29.0f), glm::normalize(glm::vec3{1, 2, 3}));
        model = glm::scale(model, glm::vec3{1.3f});
        deren::utility::vessel::array<raster_instance, 2> const transformed_inputs = {raster_instance{*transformed_asset, model, 10}, raster_instance{*transformed_asset, glm::translate(model, glm::vec3{0.18f, 0, -0.12f}), 20}};
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
        deren::utility::vessel::array<raster_instance, 1> const edge_inputs = {raster_instance{*edge_asset, glm::mat4{1}, 30}};
        if (auto result = check(edge_inputs, camera, 1, "regularized edge", false, true); !result) {
            return std::unexpected(result.error());
        }
        if (gpu) {
            auto degree2 = (*sh_asset)->source();
            degree2.sh_degree = 2u;
            degree2.particles.front().sh[12] = .25f;
            auto degree2_asset = make_raster_asset(core, std::move(degree2));
            if (!degree2_asset) {
                return std::unexpected(degree2_asset.error());
            }
            deren::utility::vessel::array<raster_instance, 1> const degree2_inputs = {raster_instance{*degree2_asset, glm::mat4{1}, 1}};
            if (auto result = check(degree2_inputs, camera, 1, "SH degree two"); !result) {
                return std::unexpected(result.error());
            }
            deren::utility::vessel::array<raster_instance, 2> const ties = {raster_instance{*overlap_asset, glm::mat4{1}, 20}, raster_instance{*overlap_asset, glm::mat4{1}, 10}};
            if (auto result = check(ties, camera, 1, "tied reordered instances"); !result) {
                return std::unexpected(result.error());
            }
            deren::utility::vessel::array<raster_instance, 1> const invisible = {raster_instance{*overlap_asset, glm::translate(glm::mat4{1}, glm::vec3{1000, 0, 0}), 1}};
            if (auto result = check(invisible, camera, 1, "all outside"); !result) {
                return std::unexpected(result.error());
            }
            deren::utility::vessel::array<raster_instance, 1> const far = {raster_instance{*overlap_asset, glm::translate(glm::mat4{1}, glm::vec3{0, 0, -200}), 1}};
            if (auto result = check(far, camera, 1, "far plane"); !result) {
                return std::unexpected(result.error());
            }
            deren::gaussian::asset near_source;
            near_source.particles = {particle({0, 0, -.11f}, {1, 0, 0}, {.02f, .02f, .02f})};
            auto near_asset = make_raster_asset(core, std::move(near_source));
            if (!near_asset) {
                return std::unexpected(near_asset.error());
            }
            deren::utility::vessel::array<raster_instance, 1> const near_inputs = {raster_instance{*near_asset, glm::mat4{1}, 1}};
            if (auto result = check(near_inputs, camera, 1, "near support omitted"); !result) {
                return std::unexpected(result.error());
            }
            // CPU projection rejects this overflow. GPU must count the error and skip the entire draw.
            deren::utility::vessel::array<raster_instance, 1> const overflow = {raster_instance{*overlap_asset, glm::scale(glm::mat4{1}, glm::vec3{3e20f}), 1}};
            auto failed_frame = prepare_gpu_frame(core, overflow, camera);
            auto empty = prepare_raster_frame(core, {}, camera);
            if (!failed_frame || !empty) {
                return failure("overflow probe frame setup failed");
            }
            auto blank = render_image(core, *heap, raster, *empty, camera, 1, *failed_frame);
            auto const errors = (*failed_frame)->completed_stats(report.cases).errors;
            if (!blank || !errors || !std::all_of(blank->begin(), blank->end(), [](auto byte) { return byte == std::byte{0}; })) {
                return failure("GPU overflow did not skip the entire frame");
            }
            ++report.cases;
        }
        return report;
    }
} // namespace deren::engine::gaussian_splatting
