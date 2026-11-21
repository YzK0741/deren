// module version: 0.2.0
export module deren.ui.rml_overlay;

export import deren.promise.rhi;
export import deren.ui.rml_document;
import deren.utility;
export import deren.vstd;

export namespace deren::ui {
    // The frame's 3D image, when the document asked for it (`deren://scene`). Passed in rather than pulled
    // from a runtime type so this module stays a plain RHI consumer; an empty `image` means this frame has
    // no resolve target and the scene draws are skipped rather than shown stale.
    struct scene_texture {
        deren::promise::rhi::shared_object<deren::promise::rhi::image> image;
    };

    class rml_overlay {
    public:
        explicit rml_overlay(deren::promise::rhi::api_core& core) noexcept;
        ~rml_overlay();
        rml_overlay(rml_overlay const&) = delete;
        rml_overlay& operator=(rml_overlay const&) = delete;

        bool initialize(std::filesystem::path const& shaders);
        bool open(std::filesystem::path const& markup, std::filesystem::path const& font,
                  std::filesystem::path const& fallback_font, std::filesystem::path const& shaders);
        // `width`/`height` are framebuffer pixels, `density_independent_pixel_ratio` is the window's
        // content scale (RmlUi's own backend spelling): HiDPI windows draw the document at its
        // authored size instead of at half of it.
        // `scene_color` is the image scene draws sample; leave it empty on a frame that renders into the
        // swapchain, and the scene element simply draws nothing that frame.
        bool prepare(std::uint32_t width, std::uint32_t height,
                     deren::promise::rhi::image_format format, std::uint32_t slot,
                     float density_independent_pixel_ratio, scene_texture const& scene_color = {});
        rml_document& content() noexcept {
            return this->document;
        }
        static deren::promise::rhi::mergeable_command_buffer_ref record(
            void* context, deren::promise::rhi::command_buffer_begin_info const& begin);

    private:
        struct batch {
            deren::promise::rhi::resource_group_ref texture;
            deren::promise::rhi::rect clip;
            std::uint32_t first_vertex;
            std::uint32_t vertex_count;
            float offset_x, offset_y;
        };

        deren::promise::rhi::api_core& core;
        rml_document document;
        deren::utility::vessel::vector<std::uint8_t> vertex_code, fragment_code;
        deren::promise::rhi::unique_object<deren::promise::rhi::pipeline> pipeline;
        deren::promise::rhi::shared_object<deren::promise::rhi::sampler> sampler;
        struct frame_resources {
            deren::promise::rhi::unique_object<deren::promise::rhi::buffer> vertices;
            deren::utility::vessel::vector<batch> batches;
            std::uint64_t vertex_address = 0;
            // The scene image this slot sampled last, and the group made for it. A swapchain image is
            // stable for a slot until the swapchain is recreated, so the group is created once per
            // device generation instead of once per frame (the backend's group creation is not cheap).
            deren::promise::rhi::shared_object<deren::promise::rhi::image> scene_image;
            deren::promise::rhi::resource_group_ref scene_group;
        };
        deren::utility::vessel::array<frame_resources, deren::promise::rhi::max_frames_in_flight> frames;
        deren::utility::vessel::map<std::tuple<deren::utility::xxh3_digest, std::uint32_t, std::uint32_t>,
                                    deren::promise::rhi::resource_group_ref>
            textures;
        // A document asking for `deren://scene` on a frame with no resolved image would be blank with no
        // explanation at all: say why once, then stay quiet (this is a configuration mistake, not a state
        // the document can fix).
        bool scene_missing_logged = false;
        std::uint32_t current_slot = 0;
        std::uint32_t width = 0, height = 0;
        deren::promise::rhi::image_format format = deren::promise::rhi::image_format::unknown;
    };
} // namespace deren::ui
