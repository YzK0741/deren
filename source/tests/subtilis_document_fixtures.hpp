#pragma once
// Include after document.hpp and vk_test.h. Shared generated document fixtures.
namespace subtilis_document_fixture {
    inline void put(sub::vector<std::byte>& bytes, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(std::byte((value >> (i * 8)) & 255));
    }
    inline std::uint32_t get(sub::vector<std::byte> const& bytes, size_t at) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t(std::to_integer<unsigned>(bytes[at + i])) << (i * 8);
        return value;
    }
    inline void replace(sub::string& text, std::string_view from, std::string_view to) {
        auto at = text.find(from);
        CHECK(at != sub::string::npos);
        if (at != sub::string::npos)
            text.replace(at, from.size(), to);
    }
    inline sub::vector<std::byte> glb(sub::string json, sub::vector<std::byte> bin) {
        while (json.size() % 4)
            json += ' ';
        while (bin.size() % 4)
            bin.push_back(std::byte{0});
        sub::vector<std::byte> bytes;
        put(bytes, 0x46546c67);
        put(bytes, 2);
        put(bytes, static_cast<std::uint32_t>(28 + json.size() + bin.size()));
        put(bytes, static_cast<std::uint32_t>(json.size()));
        put(bytes, 0x4e4f534a);
        auto chars = std::as_bytes(std::span(json));
        bytes.insert(bytes.end(), chars.begin(), chars.end());
        put(bytes, static_cast<std::uint32_t>(bin.size()));
        put(bytes, 0x004e4942);
        bytes.insert(bytes.end(), bin.begin(), bin.end());
        return bytes;
    }
    inline void write(tool::path const& p, sub::vector<std::byte> const& bytes) {
        std::ofstream file(p, std::ios::binary);
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        CHECK(file.good());
    }
    struct fixture {
        sub::string json;
        sub::vector<std::byte> bin;
    };
    template <typename Edit>
    sub::string edit_json(sub::string const& text, Edit edit) {
        auto* parsed = yyjson_read(text.data(), text.size(), 0);
        CHECK(parsed);
        auto* doc = yyjson_doc_mut_copy(parsed, nullptr);
        yyjson_doc_free(parsed);
        edit(doc, yyjson_mut_doc_get_root(doc));
        size_t size = 0;
        auto* bytes = yyjson_mut_write(doc, 0, &size);
        sub::string out(bytes, size);
        std::free(bytes);
        yyjson_mut_doc_free(doc);
        return out;
    }
    inline fixture make_fixture(sub::source_geometry const& s) {
        fixture f;
        for (auto const& row : s.positions)
            for (float v : row)
                put(f.bin, std::bit_cast<std::uint32_t>(v));
        for (auto const& row : s.normals)
            for (float v : row)
                put(f.bin, std::bit_cast<std::uint32_t>(v));
        for (auto const& row : s.uv0)
            for (float v : row)
                put(f.bin, std::bit_cast<std::uint32_t>(v));
        for (auto v : s.indices)
            put(f.bin, v);
        f.json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":@B@}],"bufferViews":[{"buffer":0,"byteLength":@P@},{"buffer":0,"byteOffset":@P@,"byteLength":@P@},{"buffer":0,"byteOffset":@N@,"byteLength":@U@},{"buffer":0,"byteOffset":@I@,"byteLength":@L@}],"accessors":[{"bufferView":0,"componentType":5126,"count":@V@,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},{"bufferView":1,"componentType":5126,"count":@V@,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":@V@,"type":"VEC2"},{"bufferView":3,"componentType":5125,"count":@C@,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"materials":[{}],"nodes":[{"mesh":0,"translation":[3,0,0]},{"mesh":0,"scale":[2,2,2]}],"scenes":[{"nodes":[0,1]}],"scene":0,"extensionsUsed":["VENDOR_metadata"],"extensions":{"VENDOR_metadata":{"nested":{"u":18446744073709551615,"text":"keep me"}}},"extras":{"custom":[true,null,{"x":7}]}})";
        auto substitute = [&](std::string_view token, size_t value) { auto text=deren::utility::vessel::to_string(value); while(f.json.find(token)!=sub::string::npos) replace(f.json,token,text); };
        substitute("@B@", f.bin.size());
        substitute("@P@", s.positions.size() * 12);
        substitute("@N@", s.positions.size() * 24);
        substitute("@U@", s.positions.size() * 8);
        substitute("@I@", s.positions.size() * 32);
        substitute("@L@", s.indices.size() * 4);
        substitute("@V@", s.positions.size());
        substitute("@C@", s.indices.size());
        return f;
    }
} // namespace subtilis_document_fixture
