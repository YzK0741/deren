#include "vk_test.h"

#include <bit>
#include <cstdint>
#include <limits>
import deren.asset.subtilis.codec;
namespace sub = deren::asset::subtilis;
namespace v = deren::utility::vessel;
using std::byte;

namespace {
    uint64_t read(v::vector<byte> const& bytes, size_t at, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
        return value;
    }
    void put(v::vector<byte>& bytes, size_t at, uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i)
            bytes[at + i] = byte((value >> (8 * i)) & 255);
    }
    v::string hex(sub::digest const& hash) {
        v::string text;
        for (auto b : hash) {
            auto n = std::to_integer<unsigned>(b);
            text += "0123456789abcdef"[n >> 4];
            text += "0123456789abcdef"[n & 15];
        }
        return text;
    }
    sub::payload fixture() {
        sub::payload p;
        p.indices = {0, 1, 2, 0, 2, 3, 0, 1, 3};
        p.clusters = {{0, 3, sub::none, 0, {0, 0, 0, 1}}, {3, 3, sub::none, 0, {1, 0, 0, 2}}, {6, 3, 0, sub::none, {0, 0, 0, 3}}};
        p.groups = {{0, 2, 2, 1, {0, 0, 0, 4}, .25f}};
        p.references = {0, 1, 2};
        p.leaves = {0, 1};
        p.roots = {2};
        p.build_meta = "{\"algorithm\":\"subtilis-dag-v1\"}";
        p.source_hash[0] = byte{0xAB};
        p.build_hash[31] = byte{0xCD};
        return p;
    }
} // namespace
int main() {
    auto p = fixture();
    auto encoded = sub::encode(p, {});
    CHECK(encoded);
    if (!encoded)
        return deren::vk_test::finish("subtilis_codec");
    auto const& bytes = *encoded;
    CHECK(bytes.size() == 600); // 352 directory + 40 indices + 96 clusters + 48 groups + 16 refs + 8 leaves + 8 roots + 32 JSON
    CHECK(read(bytes, 0, 8) == 0x00474144534E5244ULL);
    CHECK(read(bytes, 8, 2) == 1 && read(bytes, 10, 2) == 0);
    CHECK(read(bytes, 12, 4) == 128 && read(bytes, 16, 8) == bytes.size());
    CHECK(read(bytes, 24, 4) == 7 && read(bytes, 28, 4) == 0);
    CHECK(bytes[32] == byte{0xAB} && bytes[95] == byte{0xCD});
    CHECK(read(bytes, 128 + 32 + 4, 4) == 32 && read(bytes, 128 + 64 + 4, 4) == 48);
    CHECK(read(bytes, 128 + 16, 8) == 352);
    CHECK(read(bytes, 352, 4) == 0 && read(bytes, 356, 4) == 1 && read(bytes, 360, 4) == 2);
    CHECK(read(bytes, 392 + 4, 4) == 3 && read(bytes, 392 + 8, 4) == 0xffffffffu && read(bytes, 392 + 12, 4) == 0);
    CHECK(read(bytes, 392 + 28, 4) == 0x3f800000u);
    CHECK(read(bytes, 488 + 4, 4) == 2 && read(bytes, 488 + 8, 4) == 2 && read(bytes, 488 + 12, 4) == 1);
    CHECK(read(bytes, 488 + 28, 4) == 0x40800000u && read(bytes, 488 + 32, 4) == 0x3e800000u);
    auto decoded = sub::decode(bytes, {});
    CHECK(decoded);
    if (decoded) {
        CHECK(decoded->indices == p.indices && decoded->clusters == p.clusters && decoded->groups == p.groups);
        CHECK(decoded->references == p.references && decoded->leaves == p.leaves && decoded->roots == p.roots);
        CHECK(decoded->source_hash == p.source_hash && decoded->build_hash == p.build_hash && decoded->build_meta == p.build_meta);
        auto again = sub::encode(*decoded, {});
        CHECK(again && *again == bytes);
    }
    auto rejects = [&](size_t at, uint64_t value, size_t width) { auto bad = bytes; put(bad, at, value, width); CHECK(!sub::decode(bad, {})); };
    rejects(0, 0, 1);
    rejects(8, 2, 2);
    rejects(10, 1, 2);
    rejects(12, 96, 4);
    rejects(16, bytes.size() - 1, 8);
    rejects(24, 6, 4);
    rejects(28, 1, 4);
    rejects(96, 1, 1);
    rejects(128, 8, 4);
    rejects(160, 1, 4);
    rejects(132, 8, 4);
    rejects(136, UINT64_MAX, 8);
    rejects(144, UINT64_MAX - 7, 8);
    rejects(152, UINT64_MAX, 8);
    rejects(176, 352, 8); // overlapping sections
    rejects(144, 128, 8);
    rejects(144, 353, 8);
    rejects(388, 1, 1);               // alignment padding after indices
    rejects(488 + 36, 1, 1);          // group reserved bytes
    rejects(488 + 32, 0x7f800000, 4); // nonfinite decoded error
    rejects(392 + 28, 0xbf800000, 4); // negative decoded sphere radius
    rejects(568, '[', 1);             // malformed decoded metadata
    rejects(128 + 6 * 32 + 8, 0, 8);  // count/length mismatch
    for (size_t length : {size_t(0), size_t(127), size_t(351), bytes.size() - 1})
        CHECK(!sub::decode(std::span(bytes).first(length), {}));
    v::vector<byte> unaligned{byte{0}};
    unaligned.insert(unaligned.end(), bytes.begin(), bytes.end());
    CHECK(sub::decode(std::span(unaligned).subspan(1), {})); // host pointer alignment is irrelevant
    sub::limits small;
    small.max_records = 2;
    CHECK(!sub::decode(bytes, small));
    CHECK(!sub::encode(p, small));
    small = {};
    small.max_payload_bytes = 599;
    CHECK(!sub::decode(bytes, small));
    CHECK(!sub::encode(p, small));
    auto bad_meta = p;
    bad_meta.build_meta = "{]";
    CHECK(!sub::encode(bad_meta, {}));
    bad_meta.build_meta = "[]";
    CHECK(!sub::encode(bad_meta, {}));
    bad_meta.build_meta = "{\"x\":\"\xC0\x80\"}";
    CHECK(!sub::encode(bad_meta, {}));
    auto bad_float = p;
    bad_float.groups[0].error = std::numeric_limits<float>::infinity();
    CHECK(!sub::encode(bad_float, {}));
    auto lod0 = p;
    lod0.groups.clear();
    auto lod0_wire = sub::encode(lod0, {});
    CHECK(lod0_wire && sub::decode(*lod0_wire, {})); // no DAG validation in codec

    CHECK(hex(sub::sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    char const abc[] = "abc";
    CHECK(hex(sub::sha256(std::as_bytes(std::span(abc, 3)))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    char const multi[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(sub::sha256(std::as_bytes(std::span(multi, 56)))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    v::vector<byte> million(1000000, byte{'a'});
    CHECK(hex(sub::sha256(million)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    sub::source_geometry geometry;
    geometry.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    geometry.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    geometry.uv0 = {{0, 0}, {1, 0}, {0, 1}};
    geometry.indices = {0, 1, 2};
    auto original = sub::source_digest(geometry);
    CHECK(original);
    CHECK(original && hex(*original) == "d69c2d5399f6166777eb4b68e6c80e68c85aafc1b669386408f349187b25eec4"); // independent Python struct/hashlib fixture
    geometry.positions[0][0] = -0.f;
    CHECK(sub::source_digest(geometry) == original);
    geometry.indices = {0, 2, 1};
    CHECK(sub::source_digest(geometry) != original);
    geometry.indices = {0, 1, 2};
    geometry.normals[0][0] = .25f;
    CHECK(sub::source_digest(geometry) != original);
    geometry.positions[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!sub::source_digest(geometry));
    geometry.positions[0][0] = 0;
    geometry.indices[0] = 3;
    CHECK(!sub::source_digest(geometry));
    sub::build_options options;
    auto identity = sub::build_digest(options);
    CHECK(identity);
    CHECK(identity && hex(*identity) == "41e594a24d335564646a5163c6ab897b0a6cae69bbcf065c5d38f7f035e78339");
    options.normal_weight = .2f;
    CHECK(sub::build_digest(options) != identity);
    options.normal_weight = .1f;
    options.triangle_limit = 86;
    CHECK(!sub::build_digest(options));
    return deren::vk_test::finish("subtilis_codec");
}
