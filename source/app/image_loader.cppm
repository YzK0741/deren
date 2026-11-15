module;

// This module owns the application's stb_image instance. STB_IMAGE_STATIC keeps its symbols
// file-local, so it does not clash with gltf_loader.cpp's separate implementation.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

export module deren.app.image_loader;

import deren.vstd;
import deren.utility.shared;

export namespace deren::app::image_loader {
    struct hdr_image {
        int width = 0;
        int height = 0;
        deren::utility::vessel::vector<float> rgba;
    };

    struct rgba_image {
        int width = 0;
        int height = 0;
        deren::utility::vessel::vector<std::uint8_t> rgba;
    };

    bool load_hdr(std::filesystem::path const& path, hdr_image& image, std::string& error);
    bool load_png(std::span<std::uint8_t const> bytes, rgba_image& image, std::string& error);
} // namespace deren::app::image_loader

namespace deren::app::image_loader {
    bool load_hdr(std::filesystem::path const& path, hdr_image& image, std::string& error) {
        image = {};
        if (path.empty()) {
            error = "empty HDR path";
            return false;
        }
        auto const path_text = path.string();
        if (!stbi_is_hdr(path_text.c_str())) {
            error = "not a Radiance .hdr image";
            return false;
        }
        int width = 0, height = 0, channels = 0;
        if (!stbi_info(path_text.c_str(), &width, &height, &channels)) {
            error = stbi_failure_reason();
            return false;
        }
        if (width <= 0 || height <= 0 || width > 16384 || height > 8192) {
            error = "HDR dimensions exceed 16384 x 8192";
            return false;
        }
        float* const decoded = stbi_loadf(path_text.c_str(), &width, &height, &channels, 4);
        if (decoded == nullptr) {
            error = stbi_failure_reason();
            return false;
        }
        auto const count = std::size_t(width) * std::size_t(height) * 4;
        image.width = width;
        image.height = height;
        image.rgba.assign(decoded, decoded + count);
        stbi_image_free(decoded);
        return true;
    }

    bool load_png(std::span<std::uint8_t const> bytes, rgba_image& image, std::string& error) {
        image = {};
        if (bytes.empty()) {
            error = "empty PNG data";
            return false;
        }
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "PNG data is too large";
            return false;
        }
        int width = 0, height = 0, channels = 0;
        stbi_uc* const decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
        if (decoded == nullptr) {
            error = stbi_failure_reason();
            return false;
        }
        if (width <= 0 || height <= 0 || width > 16384 || height > 8192) {
            stbi_image_free(decoded);
            error = "PNG dimensions exceed 16384 x 8192";
            return false;
        }
        auto const count = std::size_t(width) * std::size_t(height) * 4;
        image.width = width;
        image.height = height;
        image.rgba.assign(decoded, decoded + count);
        stbi_image_free(decoded);
        return true;
    }
} // namespace deren::app::image_loader
