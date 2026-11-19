// module version: 0.1.0
export module deren.ui.rml_overlay;

export import deren.promise.rhi;
export import deren.ui.rml_document;
import deren.utility;
export import deren.vstd;

export namespace deren::ui {
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
        bool prepare(std::uint32_t width, std::uint32_t height,
                     deren::promise::rhi::image_format format, std::uint32_t slot,
                     float density_independent_pixel_ratio);
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
        };
        deren::utility::vessel::array<frame_resources, deren::promise::rhi::max_frames_in_flight> frames;
        deren::utility::vessel::map<std::tuple<deren::utility::xxh3_digest, std::uint32_t, std::uint32_t>,
                                    deren::promise::rhi::resource_group_ref>
            textures;
        std::uint32_t current_slot = 0;
        std::uint32_t width = 0, height = 0;
        deren::promise::rhi::image_format format = deren::promise::rhi::image_format::unknown;
    };
} // namespace deren::ui
