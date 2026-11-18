#include <cstdio>

import deren.vstd;
import deren.promise.rhi;
import deren.utility;
import deren.utility.shared;
import deren.gltf_loader;
import deren.engine.visibility.texture_cache;

int main(int argc, char** argv) {
    if (argc != 3) {
        deren::utility::println(stderr, "usage: deren_texture_export <model.gltf|model.glb> <output-directory>");
        return 2;
    }
    auto model = deren::gltf::load_model(argv[1]);
    if (!model) {
        deren::utility::println(stderr, "cannot load model '{}': {}", argv[1], static_cast<int>(model.error()));
        return 1;
    }
    auto materials = deren::gltf::resolve_materials(*model, true);
    std::filesystem::path directory(argv[2]);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        deren::utility::println(stderr, "cannot create output directory: {}", error.message());
        return 1;
    }
    std::ofstream manifest(directory / "textures.tsv");
    manifest << "key\twidth\theight\tlevels\tbytes\tformat\tusage\n";
    deren::utility::vessel::set<deren::utility::vessel::string> exported;
    for (auto const& material : materials) {
        if (material.toon_family) {
            continue;
        }
        for (std::size_t slot = 0; slot < 3; ++slot) {
            auto const& image = material.slots[slot];
            if (!image.valid || image.data.empty() ||
                (slot == 0 && (material.factors.alpha_mask || material.factors.alpha_blend))) {
                continue;
            }
            bool const srgb = slot == 0;
            auto const format = srgb ? deren::promise::rhi::image_format::bc7_srgb : (slot == 2 && deren::engine::texture_cache::normal_bc5_compatible(image.data) ? deren::promise::rhi::image_format::bc5_unorm : deren::promise::rhi::image_format::bc7_unorm);
            auto const key = deren::engine::texture_cache::source_key(image.data, image.width, image.height, image.mip_levels, srgb, format);
            if (!exported.insert(key).second) {
                continue;
            }
            std::ofstream output(directory / (key + ".rgba"), std::ios::binary);
            output.write(reinterpret_cast<char const*>(image.data.data()), static_cast<std::streamsize>(image.data.size()));
            if (!output) {
                deren::utility::println(stderr, "cannot export texture '{}'", key);
                return 1;
            }
            manifest << key << '\t' << image.width << '\t' << image.height << '\t' << image.mip_levels << '\t' << image.data.size()
                     << '\t' << deren::engine::texture_cache::dxgi_format(format)
                     << '\t' << (slot == 0 ? "albedo" : (slot == 1 ? "metallic_roughness" : "normal")) << '\n';
        }
    }
    manifest.close();
    if (!manifest) {
        return 1;
    }
    deren::utility::println("exported {} unique PBR texture mip chains", exported.size());
    return 0;
}
