module;

export module deren.app.application:environment_loader;

import deren.vstd;
import deren.utility;
import deren.utility.shared;
import deren.engine.math;
import deren.engine.lighting.environment_controls;
import deren.app.image_loader;

// THE FOUR IBL RESOLUTIONS ARE COMPILE-TIME CONSTANTS, not settings: the precompute runs before the device
// exists, the baked sizes are part of what the reference captures measure, and a config could only ever
// re-shape them by making every capture incomparable. `--bake-environment` writes these same numbers into
// the .ibl header, which is why a stale file is rejected instead of silently uploaded.
export inline constexpr std::int32_t environment_cubemap_size = 256;
export inline constexpr std::int32_t environment_mip_count = 5;
export inline constexpr std::int32_t environment_irradiance_size = 32;
export inline constexpr std::int32_t environment_brdf_lut_size = 256;

// A baked environment is the IBL the renderer actually uploads, not the equirectangular source:
// the GGX-prefiltered cube mip chain, the irradiance cube and the BRDF LUT. Baking it once offline
// removes the HDR decode plus all three convolutions from every launch; the header pins the layout
// so a stale or hand-edited file is rejected instead of being uploaded as garbage.
export struct environment_ibl_data {
    std::uint32_t env_size = 0;
    std::uint32_t env_mip_count = 0;
    std::uint32_t irr_size = 0;
    std::uint32_t lut_size = 0;
    float input_intensity = 1.0f;
    deren::utility::vessel::vector<std::uint8_t> prefiltered;
    deren::utility::vessel::vector<std::uint8_t> irradiance;
    deren::utility::vessel::vector<std::uint8_t> brdf_lut;
};

export inline bool is_environment_ibl_path(std::filesystem::path const& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".ibl";
}

export inline std::uint64_t environment_ibl_prefiltered_bytes(std::uint32_t env_size, std::uint32_t mip_count) {
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
        std::uint64_t const size = std::max<std::uint64_t>(1, env_size >> mip);
        total += 6 * size * size * 8; // RGBA16F
    }
    return total;
}

export inline std::uint64_t environment_ibl_irradiance_bytes(std::uint32_t irr_size) {
    return 6ull * irr_size * irr_size * 8; // RGBA16F
}

export inline std::uint64_t environment_ibl_lut_bytes(std::uint32_t lut_size) {
    return static_cast<std::uint64_t>(lut_size) * lut_size * 4; // RG16F
}

// Header layout (little-endian, no padding):
//   char magic[8]; u32 version; u32 header_size; u32 env_size; u32 env_mip_count;
//   u32 irr_size; u32 lut_size; u32 intensity_bits; u64 prefiltered_bytes;
//   u64 irradiance_bytes; u64 lut_bytes
export inline constexpr char environment_ibl_magic[8] = {'D', 'E', 'R', 'E', 'N', 'I', 'B', 'L'};
export inline constexpr std::uint32_t environment_ibl_version = 1;
export inline constexpr std::uint32_t environment_ibl_header_size = 8 + 7 * 4 + 3 * 8;

export inline bool read_environment_ibl(std::filesystem::path const& path, environment_ibl_data& data, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open baked environment '" + path.string() + "'";
        return false;
    }
    char magic[8]{};
    input.read(magic, 8);
    if (!input || !std::equal(std::begin(magic), std::end(magic), std::begin(environment_ibl_magic))) {
        error = "not a deren baked environment (bad magic): '" + path.string() + "'";
        return false;
    }
    auto read_u32 = [&](std::uint32_t& value) {
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        return static_cast<bool>(input);
    };
    auto read_u64 = [&](std::uint64_t& value) {
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        return static_cast<bool>(input);
    };
    std::uint32_t version = 0;
    std::uint32_t header_size = 0;
    std::uint32_t intensity_bits = 0;
    std::uint64_t prefiltered_bytes = 0;
    std::uint64_t irradiance_bytes = 0;
    std::uint64_t lut_bytes = 0;
    if (!read_u32(version) || !read_u32(header_size) || !read_u32(data.env_size) || !read_u32(data.env_mip_count) ||
        !read_u32(data.irr_size) || !read_u32(data.lut_size) || !read_u32(intensity_bits) ||
        !read_u64(prefiltered_bytes) || !read_u64(irradiance_bytes) || !read_u64(lut_bytes)) {
        error = "truncated baked environment header: '" + path.string() + "'";
        return false;
    }
    if (version != environment_ibl_version || header_size != environment_ibl_header_size) {
        error = "unsupported baked environment version/header in '" + path.string() + "'";
        return false;
    }
    if (data.env_size == 0 || data.env_size > 4096 || data.env_mip_count < 2 || data.env_mip_count > 16 ||
        data.irr_size == 0 || data.irr_size > 4096 || data.lut_size == 0 || data.lut_size > 4096) {
        error = "invalid baked environment dimensions in '" + path.string() + "'";
        return false;
    }
    std::uint64_t const expected_prefiltered = environment_ibl_prefiltered_bytes(data.env_size, data.env_mip_count);
    std::uint64_t const expected_irradiance = environment_ibl_irradiance_bytes(data.irr_size);
    std::uint64_t const expected_lut = environment_ibl_lut_bytes(data.lut_size);
    if (prefiltered_bytes != expected_prefiltered || irradiance_bytes != expected_irradiance || lut_bytes != expected_lut) {
        error = "baked environment payload sizes do not match its header: '" + path.string() + "'";
        return false;
    }
    std::uint64_t const expected_file_size = environment_ibl_header_size + prefiltered_bytes + irradiance_bytes + lut_bytes;
    std::error_code size_error;
    std::uint64_t const file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size != expected_file_size) {
        error = "baked environment file size does not match its header: '" + path.string() + "'";
        return false;
    }
    data.input_intensity = std::bit_cast<float>(intensity_bits);
    if (!std::isfinite(data.input_intensity) || data.input_intensity < 0.0f) {
        error = "baked environment has an invalid intensity: '" + path.string() + "'";
        return false;
    }
    data.prefiltered.resize(static_cast<std::size_t>(prefiltered_bytes));
    data.irradiance.resize(static_cast<std::size_t>(irradiance_bytes));
    data.brdf_lut.resize(static_cast<std::size_t>(lut_bytes));
    input.read(reinterpret_cast<char*>(data.prefiltered.data()), static_cast<std::streamsize>(data.prefiltered.size()));
    input.read(reinterpret_cast<char*>(data.irradiance.data()), static_cast<std::streamsize>(data.irradiance.size()));
    input.read(reinterpret_cast<char*>(data.brdf_lut.data()), static_cast<std::streamsize>(data.brdf_lut.size()));
    if (!input) {
        error = "truncated baked environment payload: '" + path.string() + "'";
        return false;
    }
    return true;
}

export inline bool write_environment_ibl(std::filesystem::path const& path, environment_ibl_data const& data, std::string& error) {
    std::error_code directory_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
    }
    if (directory_error) {
        error = "could not create the baked environment directory '" + path.parent_path().string() + "': " + directory_error.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open baked environment for writing: '" + path.string() + "'";
        return false;
    }
    auto write_u32 = [&](std::uint32_t const value) {
        output.write(reinterpret_cast<char const*>(&value), sizeof(value));
    };
    auto write_u64 = [&](std::uint64_t const value) {
        output.write(reinterpret_cast<char const*>(&value), sizeof(value));
    };
    output.write(environment_ibl_magic, sizeof(environment_ibl_magic));
    write_u32(environment_ibl_version);
    write_u32(environment_ibl_header_size);
    write_u32(data.env_size);
    write_u32(data.env_mip_count);
    write_u32(data.irr_size);
    write_u32(data.lut_size);
    write_u32(std::bit_cast<std::uint32_t>(data.input_intensity));
    write_u64(static_cast<std::uint64_t>(data.prefiltered.size()));
    write_u64(static_cast<std::uint64_t>(data.irradiance.size()));
    write_u64(static_cast<std::uint64_t>(data.brdf_lut.size()));
    output.write(reinterpret_cast<char const*>(data.prefiltered.data()), static_cast<std::streamsize>(data.prefiltered.size()));
    output.write(reinterpret_cast<char const*>(data.irradiance.data()), static_cast<std::streamsize>(data.irradiance.size()));
    output.write(reinterpret_cast<char const*>(data.brdf_lut.data()), static_cast<std::streamsize>(data.brdf_lut.size()));
    if (!output) {
        error = "failed while writing the baked environment: '" + path.string() + "'";
        return false;
    }
    return true;
}

// Each background task keeps its decoded pixels and joins both convolution tasks before returning.
export struct prepared_environment {
    deren::utility::vessel::vector<std::uint8_t> environment;
    deren::utility::vessel::vector<std::uint8_t> irradiance;
    deren::utility::vessel::string error;
};

export prepared_environment prepare_environment(std::filesystem::path const& path,
                                                deren::utility::vessel::array<float, 3> sun, int env_size, int mip_count, int irr_size, float input_scale) {
    // A baked file is already in the exact form a per-object override uploads: its prefiltered
    // chain and irradiance cube. Reuse them instead of decoding and convolving the source again.
    if (is_environment_ibl_path(path)) {
        environment_ibl_data data;
        std::string error;
        if (!read_environment_ibl(path, data, error)) {
            return {{}, {}, deren::utility::vessel::string(error)};
        }
        if (data.env_size != static_cast<std::uint32_t>(env_size) ||
            data.env_mip_count != static_cast<std::uint32_t>(mip_count) ||
            data.irr_size != static_cast<std::uint32_t>(irr_size)) {
            return {{}, {}, "Baked environment dimensions do not match this config"};
        }
        return {std::move(data.prefiltered), std::move(data.irradiance), {}};
    }
    deren::utility::vessel::vector<float> cube;
    if (path.empty()) {
        cube = deren::engine::generate_environment_cubemap(env_size, sun);
    } else {
        deren::app::image_loader::hdr_image decoded;
        std::string error;
        // HDR-only: the loader refuses an LDR file instead of gamma-decoding it as a panorama.
        if (!deren::app::image_loader::load_hdr(path, decoded, error)) {
            return {{}, {}, deren::utility::vessel::string(error)};
        }
        auto const count = decoded.rgba.size();
        bool finite = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(decoded.rgba[i]) || decoded.rgba[i] < 0) {
                finite = false;
                break;
            }
        }
        if (!finite) {
            return {{}, {}, "Environment contains invalid radiance"};
        }
        cube = deren::engine::generate_environment_cubemap_from_equirect(std::span<float const>(decoded.rgba.data(), count), decoded.width, decoded.height, env_size, input_scale);
    }
    if (!deren::engine::environment_detail::valid_half_radiance(cube)) {
        return {{}, {}, "Environment exceeds finite half-float radiance; reduce input intensity"};
    }
    auto specular = deren::engine::prefilter_environment_async(cube, env_size, mip_count);
    auto diffuse = deren::engine::generate_irradiance_map_async(cube, env_size, irr_size);
    auto specular_pixels = specular.get();
    auto diffuse_pixels = diffuse.get();
    if (!deren::engine::environment_detail::valid_half_radiance(specular_pixels) || !deren::engine::environment_detail::valid_half_radiance(diffuse_pixels)) {
        return {{}, {}, "Prepared environment exceeds finite half-float radiance; reduce input intensity"};
    }
    return {deren::engine::to_half_rgba(specular_pixels), deren::engine::to_half_rgba(diffuse_pixels), {}};
}

// A BAKED ENVIRONMENT IS A FUNCTION OF ITS SOURCE AND OF THE BAKED RESOLUTIONS, so a cache entry for it
// can be keyed by exactly those inputs and reused across launches: the source file's identity (path,
// size, modification time), the sun the analytic sky is built from, the three resolutions the file
// header pins, and the input scale. Nothing else in the process may reach a bake, so a hit is the same
// pixels a miss would have produced. The layout version is part of the key: changing what a bake means
// re-keys every entry instead of silently reusing one.
export inline constexpr std::uint32_t environment_cache_version = 1;

// The identity of a source file, read once by the caller that already has to open it.
export struct environment_source_identity {
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

export inline std::filesystem::path environment_cache_file(std::filesystem::path const& cache_dir,
                                                           std::filesystem::path const& source,
                                                           environment_source_identity identity,
                                                           deren::utility::vessel::array<float, 3> sun,
                                                           std::int32_t env_size,
                                                           std::int32_t mip_count,
                                                           std::int32_t irr_size,
                                                           std::int32_t lut_size,
                                                           float input_scale) {
    std::string key;
    auto append = [&key](void const* data, std::size_t size) { key.append(static_cast<char const*>(data), size); };
    auto append_u32 = [&append](std::uint32_t value) { append(&value, sizeof(value)); };
    auto append_u64 = [&append](std::uint64_t value) { append(&value, sizeof(value)); };
    auto append_float = [&append](float value) { append(&value, sizeof(value)); };
    char const domain[] = "deren environment cache";
    append(domain, sizeof(domain) - 1);
    append_u32(environment_cache_version);
    append_u32(static_cast<std::uint32_t>(env_size));
    append_u32(static_cast<std::uint32_t>(mip_count));
    append_u32(static_cast<std::uint32_t>(irr_size));
    append_u32(static_cast<std::uint32_t>(lut_size));
    append_float(input_scale);
    // the source path is part of the identity even though the bytes decide the bake: two projects that
    // ship the same panorama under different names are different assets to the person debugging them.
    std::string const source_text = source.generic_string();
    append_u64(static_cast<std::uint64_t>(source_text.size()));
    append(source_text.data(), source_text.size());
    append_u64(identity.size);
    append_u64(static_cast<std::uint64_t>(identity.modified));
    // the analytic sky is the only bake the sun shapes; keying a decoded HDR by the sun would re-bake an
    // unchanged file every time the light moved.
    if (source.empty()) {
        append_float(sun[0]);
        append_float(sun[1]);
        append_float(sun[2]);
    }
    auto const digest = deren::utility::xxh3_128bits(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(key.data()), key.size()));
    std::string name;
    name.reserve(2 * deren::utility::xxh3_digest::size_byte + 4);
    char const digits[] = "0123456789abcdef";
    for (std::uint8_t const byte : digest.data) {
        name.push_back(digits[byte >> 4]);
        name.push_back(digits[byte & 0x0fu]);
    }
    name += ".ibl";
    return cache_dir / name;
}

// What a cached preparation did, for the log line the caller writes: a hit is the case this exists for.
export struct environment_cache_report {
    bool hit = false;
    bool write_failed = false;
    std::filesystem::path file;
    std::uint64_t bytes = 0;
};

// prepare_environment() with a disk cache in front of it: a hit reads the .ibl the last miss wrote, a
// miss writes one. An empty cache_dir disables the cache (the caller asked for a one-off bake), and a
// cache that cannot be written is reported, never fatal - the prepared pixels are already in hand.
export prepared_environment prepare_environment_cached(std::filesystem::path const& path,
                                                       std::filesystem::path const& cache_dir,
                                                       deren::utility::vessel::array<float, 3> sun,
                                                       int env_size,
                                                       int mip_count,
                                                       int irr_size,
                                                       float input_scale,
                                                       environment_cache_report* report = nullptr) {
    if (report != nullptr) {
        *report = {};
    }
    environment_source_identity identity;
    std::error_code identity_error;
    if (!path.empty()) {
        identity.size = std::filesystem::file_size(path, identity_error);
        if (identity_error) {
            return {{}, {}, deren::utility::vessel::format("could not read the environment source '{}': {}",
                                                           path.string(), identity_error.message())};
        }
        auto const modified = std::filesystem::last_write_time(path, identity_error);
        if (identity_error) {
            return {{}, {}, deren::utility::vessel::format("could not read the environment source '{}': {}",
                                                           path.string(), identity_error.message())};
        }
        identity.modified = static_cast<std::int64_t>(modified.time_since_epoch().count());
    }
    // A baked file needs no bake, so it bypasses the cache entirely (the caller named the pixels).
    if (cache_dir.empty() || is_environment_ibl_path(path)) {
        return prepare_environment(path, sun, env_size, mip_count, irr_size, input_scale);
    }
    std::filesystem::path const file = environment_cache_file(cache_dir, path, identity, sun, env_size, mip_count,
                                                              irr_size, environment_brdf_lut_size, input_scale);
    std::error_code file_error;
    if (std::filesystem::is_regular_file(file, file_error) && !file_error) {
        environment_ibl_data data;
        std::string error;
        if (read_environment_ibl(file, data, error) && data.env_size == static_cast<std::uint32_t>(env_size) &&
            data.env_mip_count == static_cast<std::uint32_t>(mip_count) && data.irr_size == static_cast<std::uint32_t>(irr_size)) {
            if (report != nullptr) {
                report->hit = true;
                report->file = file;
                report->bytes = std::filesystem::file_size(file, file_error);
            }
            return {std::move(data.prefiltered), std::move(data.irradiance), {}};
        }
        // A truncated or foreign file at the key's path is not trusted: the bake below rewrites it.
    }
    prepared_environment prepared = prepare_environment(path, sun, env_size, mip_count, irr_size, input_scale);
    if (!prepared.error.empty()) {
        return prepared;
    }
    // The .ibl container carries the BRDF LUT next to the two cubes (that is the format the offline bake
    // writes); the caller regenerates its own LUT anyway, so this one only has to be a valid payload.
    auto const lut = deren::engine::generate_brdf_lut(environment_brdf_lut_size);
    environment_ibl_data data{
        .env_size = static_cast<std::uint32_t>(env_size),
        .env_mip_count = static_cast<std::uint32_t>(mip_count),
        .irr_size = static_cast<std::uint32_t>(irr_size),
        .lut_size = static_cast<std::uint32_t>(environment_brdf_lut_size),
        .input_intensity = input_scale,
        .prefiltered = prepared.environment,
        .irradiance = prepared.irradiance,
        .brdf_lut = deren::engine::to_half_rg(lut),
    };
    std::string write_error;
    if (write_environment_ibl(file, data, write_error)) {
        if (report != nullptr) {
            report->file = file;
            report->bytes = std::filesystem::file_size(file, file_error);
        }
    } else if (report != nullptr) {
        report->write_failed = true;
        report->file = file;
    }
    return prepared;
}

export inline bool bake_environment_ibl(std::filesystem::path const& input, std::filesystem::path const& output,
                                 std::uint32_t env_size, std::uint32_t mip_count, std::uint32_t irr_size, std::uint32_t lut_size,
                                 float intensity, std::string& error) {
    auto const prepared = prepare_environment(input, deren::utility::vessel::array<float, 3>{0.3f, 1.0f, 0.5f},
                                              static_cast<int>(env_size), static_cast<int>(mip_count), static_cast<int>(irr_size), intensity);
    if (!prepared.error.empty()) {
        error = prepared.error;
        return false;
    }
    auto const lut = deren::engine::generate_brdf_lut(static_cast<std::int32_t>(lut_size));
    environment_ibl_data data{
        .env_size = env_size,
        .env_mip_count = mip_count,
        .irr_size = irr_size,
        .lut_size = lut_size,
        .input_intensity = intensity,
        .prefiltered = prepared.environment,
        .irradiance = prepared.irradiance,
        .brdf_lut = deren::engine::to_half_rg(lut),
    };
    return write_environment_ibl(output, data, error);
}
