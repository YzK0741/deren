module;

#include <cstddef>

module deren.ui.rml_overlay;

namespace deren::ui {
    namespace rhi = deren::promise::rhi;
    namespace {
        struct draw_parameters {
            float offset_x, offset_y;
            float width, height;
            std::uint32_t srgb_output;
        };

        static_assert(sizeof(vertex) == 20 && offsetof(vertex, r) == 16);
        static_assert(sizeof(draw_parameters) == 20);
    } // namespace

    rml_overlay::rml_overlay(rhi::api_core& core) noexcept
        : core(core) {
    }

    rml_overlay::~rml_overlay() {
        if (this->pipeline) {
            this->core.wait_idle();
        }
    }

    bool rml_overlay::open(std::filesystem::path const& markup, std::filesystem::path const& font,
                           std::filesystem::path const& fallback_font, std::filesystem::path const& shaders) {
        return this->initialize(shaders) && this->document.open(markup, font, fallback_font);
    }

    bool rml_overlay::initialize(std::filesystem::path const& shaders) {
        constexpr std::uintmax_t max_shader_bytes = 1024 * 1024;
        auto vertex_data = utility::read_binary_to_vector(shaders / "rmlui.vert.spv", max_shader_bytes);
        auto fragment_data = utility::read_binary_to_vector(shaders / "rmlui.frag.spv", max_shader_bytes);
        if (!vertex_data || !fragment_data) {
            return false;
        }
        this->sampler = rhi::shared_object<rhi::sampler>(
            this->core.resources->create_sampler({.address_mode = rhi::sampler_address_mode::clamp_to_edge}));
        if (!this->sampler) {
            return false;
        }
        this->vertex_code = std::move(*vertex_data);
        this->fragment_code = std::move(*fragment_data);
        return true;
    }

    bool rml_overlay::prepare(std::uint32_t width, std::uint32_t height, rhi::image_format format, std::uint32_t slot,
                              float density_independent_pixel_ratio) {
        if (slot >= this->frames.size()) {
            return false;
        }
        this->current_slot = slot;
        auto& frame = this->frames[slot];
        if (width == 0 || height == 0) {
            frame.batches.clear();
            return true;
        }
        this->document.set_density_independent_pixel_ratio(density_independent_pixel_ratio);

        rhi::unique_object<rhi::pipeline> new_pipeline;
        if (!this->pipeline || this->format != format) {
            rhi::blend_mode const blend = rhi::blend_mode::alpha;
            rhi::resource_group_binding const layout[] = {
                {0, rhi::resource_kind::sampled_image, 8, 2},
                {1, rhi::resource_kind::sampler, 12, 2},
            };
            rhi::pipeline_desc desc{};
            desc.color_formats = std::span(&format, 1);
            desc.vertex_code = std::as_bytes(std::span(this->vertex_code));
            desc.fragment_code = std::as_bytes(std::span(this->fragment_code));
            desc.depth_test = false;
            desc.blend_modes = std::span(&blend, 1);
            desc.debug_name = "rmlui";
            desc.resource_group = layout;
            new_pipeline = this->core.resources->create_pipeline(desc);
            if (!new_pipeline) {
                return false;
            }
        }

        utility::vessel::vector<vertex> expanded;
        utility::vessel::vector<batch> new_batches;

        utility::vessel::set<std::tuple<utility::xxh3_digest, std::uint32_t, std::uint32_t>> used_textures;
        constexpr std::uint32_t white = 0xffffffff;
        for (auto const& draw : this->document.paint(width, height)) {
            texture_view const texture = draw.texture.pixels.empty()
                                             ? texture_view{1, 1, std::as_bytes(std::span(&white, 1))}
                                             : draw.texture;
            auto const source = std::span(reinterpret_cast<std::uint8_t const*>(texture.pixels.data()), texture.pixels.size());
            auto const key = std::tuple{utility::xxh3_128bits(source), texture.width, texture.height};
            used_textures.insert(key);
            auto [texture_entry, inserted] = this->textures.try_emplace(key);
            if (inserted) {
                auto image = this->core.resources->create_image({
                    .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                    .format = rhi::image_format::rgba8_unorm,
                    .flags = rhi::to_bits(rhi::image_flag::sampled),
                    .initial_bytes = texture.pixels,
                });
                if (!image) {
                    return false;
                }
                rhi::resource_group_entry const entries[] = {
                    {.binding = 0, .kind = rhi::resource_kind::sampled_image, .resource = rhi::shared_object<rhi::image>(std::move(image))},
                    {.binding = 1, .kind = rhi::resource_kind::sampler, .resource = this->sampler},
                };
                auto group = this->core.resources->create_resource_group({.entries = entries});
                if (!group) {
                    return false;
                }
                texture_entry->second = std::move(*group);
            }
            if (draw.indices.size() > std::numeric_limits<std::uint32_t>::max() - expanded.size()) {
                return false;
            }
            new_batches.push_back({
                .texture = texture_entry->second,
                .clip = {draw.clip_x, draw.clip_y, static_cast<std::uint32_t>(draw.clip_width), static_cast<std::uint32_t>(draw.clip_height)},
                .first_vertex = static_cast<std::uint32_t>(expanded.size()),
                .vertex_count = static_cast<std::uint32_t>(draw.indices.size()),
                .offset_x = draw.offset_x,
                .offset_y = draw.offset_y,
            });
            for (auto index : draw.indices) {
                if (index >= draw.vertices.size()) {
                    return false;
                }
                expanded.push_back(draw.vertices[index]);
            }
        }

        rhi::unique_object<rhi::buffer> new_vertices;
        std::uint64_t address = 0;
        if (!expanded.empty()) {
            auto const bytes = std::as_bytes(std::span(expanded));
            new_vertices = this->core.resources->create_buffer({
                .size = bytes.size(),
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address),
                .initial_bytes = bytes,
            });
            auto const* addresses = rhi::query_extension<rhi::device_address>(this->core);
            if (!new_vertices || !addresses || !(address = addresses->buffer_address(*new_vertices, 0))) {
                return false;
            }
        }

        // Old geometry belongs to this completed frame slot. Only a format change replaces a shared pipeline.
        if (new_pipeline && this->pipeline) {
            this->core.wait_idle();
        }
        if (new_pipeline) {
            this->pipeline = std::move(new_pipeline);
        }
        // Older in-flight slots retain their resource groups when an atlas is replaced or hidden.
        for (auto it = this->textures.begin(); it != this->textures.end();) {
            if (!used_textures.contains(it->first)) {
                it = this->textures.erase(it);
            } else {
                ++it;
            }
        }
        frame.vertices = std::move(new_vertices);
        frame.batches = std::move(new_batches);
        frame.vertex_address = address;
        this->format = format;
        this->width = width;
        this->height = height;
        return true;
    }

    rhi::mergeable_command_buffer_ref rml_overlay::record(void* context, rhi::command_buffer_begin_info const& begin) {
        auto const& overlay = *static_cast<rml_overlay*>(context);
        auto const& frame = overlay.frames[overlay.current_slot];
        if (frame.batches.empty()) {
            return {};
        }
        auto segment = overlay.core.commands->make_mergeable_command_buffer({});
        if (!segment || segment->begin_recording(begin) != rhi::error::ok) {
            utility::error("rmlui: could not begin overlay recording");
            return {};
        }
        auto const width = static_cast<float>(overlay.width);
        auto const height = static_cast<float>(overlay.height);
        auto const srgb = overlay.format == rhi::image_format::rgba8_srgb || overlay.format == rhi::image_format::bgra8_srgb;
        bool recorded = segment->bind_pipeline(*overlay.pipeline) == rhi::error::ok;
        if (recorded) {
            segment->set_viewport({.width = width, .height = height});
            segment->set_cull_mode(rhi::cull_mode::none);
            segment->set_depth_write(false);
            for (auto const& batch : frame.batches) {
                segment->set_scissor(batch.clip);
                auto const address = frame.vertex_address + sizeof(vertex) * batch.first_vertex;
                draw_parameters const parameters{batch.offset_x, batch.offset_y, width, height, srgb ? 1u : 0u};
                if (segment->load_resource_group(batch.texture) != rhi::error::ok ||
                    segment->push_constants(0, std::as_bytes(std::span(&address, 1))) != rhi::error::ok ||
                    segment->push_constants(16, std::as_bytes(std::span(&parameters, 1))) != rhi::error::ok) {
                    recorded = false;
                    break;
                }
                segment->draw(batch.vertex_count, 1, 0, 0);
            }
        }
        if (segment->end_recording() != rhi::error::ok || !recorded) {
            utility::error("rmlui: could not record overlay draws");
            return {};
        }
        return segment;
    }
} // namespace deren::ui
