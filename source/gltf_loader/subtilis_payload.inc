// Private implementation, included after get_source_bytes inside the loader's
// anonymous namespace. Only locates bytes; wire semantics belong to the engine.
using subtilis_cache = deren::utility::vessel::vector<deren::utility::vessel::vector<std::shared_ptr<deren::gltf::subtilis_payload const>>>;
subtilis_cache extract_subtilis_payloads(fastgltf::GltfDataGetter& file, Asset const& asset) {
    subtilis_cache cache;
    cache.resize(asset.meshes.size());
    for (size_t m = 0; m < asset.meshes.size(); ++m)
        cache[m].resize(asset.meshes[m].primitives.size());
    if (file.totalSize() > uint64_t(4) << 30)
        return cache;
    file.reset();
    auto input = file.read(file.totalSize(), 0);
    std::span<std::byte const> raw(input.data(), input.size()), json = raw;
    auto u32 = [&](size_t at) {uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(std::to_integer<unsigned>(raw[at+i]))<<(i*8);return n; };
    if (raw.size() >= 4 && u32(0) == 0x46546c67) {
        if (raw.size() < 20 || u32(16) != 0x4e4f534a || u32(12) > raw.size() - 20)
            return cache;
        json = raw.subspan(20, u32(12));
    }
    if (json.size() > uint64_t(64) << 20)
        return cache;
    yyjson_alc allocator{
        [](void*, size_t n) -> void* { return deren::utility::memory::allocate(n); },
        [](void*, void* old, size_t before, size_t n) -> void* {auto* next=deren::utility::memory::allocate(n);if(old)std::memcpy(next,old,std::min(before,n));deren::utility::memory::deallocate(old);return next; },
        [](void*, void* p) { deren::utility::memory::deallocate(p); }, nullptr};
    auto* document = yyjson_read_opts(reinterpret_cast<char*>(const_cast<std::byte*>(json.data())), json.size(), 0, &allocator, nullptr);
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> owner(document, yyjson_doc_free);
    if (!document)
        return cache;
    auto* meshes = yyjson_obj_get(yyjson_doc_get_root(document), "meshes");
    uint64_t total = 0;
    uint32_t available = 0, unavailable = 0;
    constexpr uint64_t payload_limit = uint64_t(1) << 30;
    for (size_t m = 0; m < asset.meshes.size(); ++m) {
        auto* primitives = yyjson_obj_get(yyjson_arr_get(meshes, m), "primitives");
        for (size_t p = 0; p < asset.meshes[m].primitives.size(); ++p) {
            auto* declaration = yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(primitives, p), "extensions"), "DEREN_subtilis");
            if (!declaration)
                continue;
            auto payload = std::make_shared<deren::gltf::subtilis_payload>();
            auto const& primitive = asset.meshes[m].primitives[p];
            bool position = false, normal = false;
            payload->profile_attributes = primitive.type == fastgltf::PrimitiveType::Triangles && primitive.targets.empty();
            for (auto const& attribute : primitive.attributes) {
                std::string_view name = attribute.name;
                position |= name == "POSITION";
                normal |= name == "NORMAL";
                if (name != "POSITION" && name != "NORMAL" && name != "TEXCOORD_0")
                    payload->profile_attributes = false;
            }
            payload->profile_attributes &= position && normal;
            auto* version = yyjson_obj_get(declaration, "version");
            auto* profile = yyjson_obj_get(declaration, "profile");
            auto* view = yyjson_obj_get(declaration, "bufferView");
            if (yyjson_is_uint(version) && yyjson_get_uint(version) <= UINT32_MAX)
                payload->version = static_cast<uint32_t>(yyjson_get_uint(version));
            if (yyjson_is_str(profile) && yyjson_get_len(profile) <= 64)
                payload->profile.assign(yyjson_get_str(profile), yyjson_get_len(profile));
            if (yyjson_is_uint(view) && yyjson_get_uint(view) < asset.bufferViews.size()) {
                payload->buffer_view = static_cast<uint32_t>(yyjson_get_uint(view));
                auto const& bv = asset.bufferViews[payload->buffer_view];
                if (bv.bufferIndex < asset.buffers.size() && !bv.target && !bv.byteStride) {
                    auto const& buffer = asset.buffers[bv.bufferIndex];
                    auto bytes = get_source_bytes(asset, buffer.data);
                    if (bv.byteOffset <= bytes.size() && bv.byteLength <= bytes.size() - bv.byteOffset && bv.byteOffset <= buffer.byteLength && bv.byteLength <= buffer.byteLength - bv.byteOffset && bv.byteLength <= payload_limit - total) {
                        auto data = bytes.subspan(bv.byteOffset, bv.byteLength);
                        auto* first = reinterpret_cast<uint8_t const*>(data.data());
                        payload->bytes.assign(first, first + data.size());
                        total += data.size();
                    }
                }
            }
            if (payload->bytes.empty())
                ++unavailable;
            else
                ++available;
            // Keep declaration presence on malformed metadata so import can count
            // invalid_payload while core LOD0 remains available.
            cache[m][p] = std::move(payload);
        }
    }
    if (available || unavailable)
        deren::utility::log("glTF subtilis: {} available payload(s), {} unavailable declaration(s)", available, unavailable);
    return cache;
}
