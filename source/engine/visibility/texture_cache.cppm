// module version: 0.1.0
export module deren.engine.visibility.texture_cache;
export import deren.vstd;
export import deren.utility.shared;
export import deren.promise.rhi;
import deren.utility;

export namespace deren::engine::texture_cache {
    namespace rhi = deren::promise::rhi;

    inline std::uint32_t dxgi_format(rhi::image_format format) {
        switch (format) {
        case rhi::image_format::bc5_unorm:
            return 83;
        case rhi::image_format::bc7_unorm:
            return 98;
        case rhi::image_format::bc7_srgb:
            return 99;
        default:
            return 0;
        }
    }

    /// BC5 normal decoding reconstructs positive Z. Keep all three components for negative-Z sources.
    inline bool normal_bc5_compatible(std::span<std::uint8_t const> pixels) {
        if (pixels.empty() || pixels.size() % 4 != 0) {
            return false;
        }
        for (std::size_t at = 2; at < pixels.size(); at += 4) {
            if (pixels[at] < 128) {
                return false;
            }
        }
        return true;
    }

    inline deren::utility::vessel::string source_key(std::span<std::uint8_t const> source,
                                                     std::uint32_t width, std::uint32_t height,
                                                     std::uint32_t levels, bool srgb, rhi::image_format format = rhi::image_format::unknown) {
        auto const digest = deren::utility::xxh3_128bits(source);
        return deren::utility::vessel::format("{}-{}x{}-{}-{}-{}", digest.to_hex_string(), width, height, levels, srgb ? "srgb" : "linear",
                                              format == rhi::image_format::bc5_unorm ? "bc5-normal-rg-v1" : "bc7-basic-v1");
    }

    inline std::uint32_t word(std::span<std::uint8_t const> bytes, std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8u) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16u) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24u);
    }

    inline std::uint64_t payload_size(std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
        if (!width || !height || !levels || levels > static_cast<std::uint32_t>(std::bit_width(std::max(width, height)))) {
            return 0;
        }
        std::uint64_t size = 0;
        for (std::uint32_t mip = 0; mip < levels; ++mip) {
            auto const bytes = rhi::image_mip_bytes(rhi::image_format::bc7_unorm, std::max(1u, width >> mip), std::max(1u, height >> mip));
            if (!bytes || bytes > std::numeric_limits<std::uint64_t>::max() - size) {
                return 0;
            }
            size += bytes;
        }
        return size;
    }

    /// Accept only one 2D DX10 BC7 image with all declared source mip levels. BC5 normals use DXGI format 83.
    inline bool valid_dds(std::span<std::uint8_t const> file, std::uint32_t width,
                          std::uint32_t height, std::uint32_t levels, bool srgb, rhi::image_format format = rhi::image_format::unknown) {
        auto const size = payload_size(width, height, levels);
        if (!size || size > std::numeric_limits<std::size_t>::max() - 148 || file.size() != size + 148) {
            return false;
        }
        auto const expected_format = dxgi_format(format == rhi::image_format::unknown ? (srgb ? rhi::image_format::bc7_srgb : rhi::image_format::bc7_unorm) : format);
        if (!expected_format) {
            return false;
        }
        return word(file, 0) == 0x20534444u && word(file, 4) == 124 &&
               word(file, 12) == height && word(file, 16) == width && word(file, 24) <= 1 && word(file, 28) == levels &&
               word(file, 76) == 32 && (word(file, 80) & 4u) != 0 && word(file, 84) == 0x30315844u &&
               word(file, 112) == 0 && word(file, 128) == expected_format &&
               word(file, 132) == 3 && word(file, 136) == 0 && word(file, 140) == 1 && word(file, 144) == 0;
    }

    inline std::shared_ptr<deren::utility::vessel::vector<std::uint8_t> const>
    load(std::filesystem::path const& path, std::uint32_t width, std::uint32_t height, std::uint32_t levels, bool srgb,
         rhi::image_format format = rhi::image_format::unknown) {
        auto const size = payload_size(width, height, levels);
        std::error_code error;
        auto const file_size = std::filesystem::file_size(path, error);
        if (error || !size || size > std::numeric_limits<std::size_t>::max() - 148 || file_size != size + 148) {
            return {};
        }
        deren::utility::vessel::vector<std::uint8_t> file(static_cast<std::size_t>(file_size));
        std::ifstream stream(path, std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size())) ||
            !valid_dds(file, width, height, levels, srgb, format)) {
            deren::utility::log("texture cache: invalid compressed entry '{}', using source pixels", path.string());
            return {};
        }
        return std::make_shared<deren::utility::vessel::vector<std::uint8_t> const>(file.begin() + 148, file.end());
    }
} // namespace deren::engine::texture_cache
