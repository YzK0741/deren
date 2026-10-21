module;
#include <cstddef>
#include <cstdint>
module deren.asset.subtilis.codec;
namespace deren::asset::subtilis {
    namespace {
        class sha256_stream {
            array<uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            array<byte, 64> block{};
            uint64_t length = 0;
            size_t used = 0;
            void compress() {
                static constexpr uint32_t k[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                                                   0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                                                   0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                                                   0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                                                   0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                                                   0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                                                   0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                                                   0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
                uint32_t w[64]{};
                for (size_t i = 0; i < 16; ++i)
                    for (size_t j = 0; j < 4; ++j)
                        w[i] = (w[i] << 8) | std::to_integer<unsigned>(block[i * 4 + j]);
                auto rotr = [](uint32_t x, int n) { return std::rotr(x, n); };
                for (size_t i = 16; i < 64; ++i) {
                    auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (size_t i = 0; i < 64; ++i) {
                    auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    auto ch = (e & f) ^ (~e & g);
                    auto temp1 = hh + s1 + ch + k[i] + w[i];
                    auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    auto maj = (a & b) ^ (a & c) ^ (b & c);
                    hh = g;
                    g = f;
                    f = e;
                    e = d + temp1;
                    d = c;
                    c = b;
                    b = a;
                    a = temp1 + s0 + maj;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += hh;
            }

        public:
            void append(byte value) {
                block[used++] = value;
                ++length;
                if (used == block.size()) {
                    compress();
                    used = 0;
                }
            }
            void append(std::span<byte const> bytes) {
                for (auto b : bytes)
                    append(b);
            }
            void u32(uint32_t value) {
                for (unsigned shift = 0; shift < 32; shift += 8)
                    append(byte((value >> shift) & 255));
            }
            void f32(float value) {
                u32(value == 0 ? 0 : std::bit_cast<uint32_t>(value));
            }
            void text(std::string_view value) {
                u32(static_cast<uint32_t>(value.size()));
                append(std::as_bytes(std::span(value)));
            }
            digest finish() {
                uint64_t bits = length * 8;
                append(byte{0x80});
                while (used != 56)
                    append(byte{0});
                for (int shift = 56; shift >= 0; shift -= 8)
                    append(byte((bits >> shift) & 255));
                digest out{};
                for (size_t i = 0; i < 8; ++i)
                    for (size_t j = 0; j < 4; ++j)
                        out[i * 4 + j] = byte((h[i] >> (24 - 8 * j)) & 255);
                return out;
            }
        };
        auto failure(error_code code, char const* context) {
            return std::unexpected(error{code, context, {}, {}});
        }
    } // namespace
    digest sha256(std::span<byte const> bytes) {
        sha256_stream hash;
        hash.append(bytes);
        return hash.finish();
    }

    result<digest> source_digest(source_geometry const& source) {
        auto n = source.positions.size();
        if (!n || n > UINT32_MAX || source.normals.size() != n || source.uv0.size() != n || source.indices.empty() || source.indices.size() > UINT32_MAX || source.indices.size() % 3)
            return failure(error_code::invalid_geometry, "source attribute/index counts are invalid");
        auto finite = [](auto const& values) { for (auto const& row : values) for (float f : row) if (!std::isfinite(f)) return false; return true; };
        if (!finite(source.positions) || !finite(source.normals) || !finite(source.uv0))
            return failure(error_code::invalid_geometry, "nonfinite source attribute");
        for (auto id : source.indices)
            if (id >= n)
                return failure(error_code::invalid_geometry, "source index outside vertex array");
        sha256_stream hash;
        hash.text("DEREN_subtilis.source.v1");
        hash.text("shared-vertices");
        hash.u32(4);
        hash.u32(static_cast<uint32_t>(n));
        auto attribute = [&](std::string_view name, auto const& values) { hash.text(name); hash.u32(static_cast<uint32_t>(n)); for (auto const& row : values) for (float f : row) hash.f32(f); };
        attribute("POSITION", source.positions);
        attribute("NORMAL", source.normals);
        attribute("TEXCOORD_0", source.uv0);
        hash.u32(static_cast<uint32_t>(source.indices.size()));
        for (auto id : source.indices)
            hash.u32(id);
        hash.text("OPAQUE");
        hash.text("POSITION,NORMAL,TEXCOORD_0");
        hash.u32(0);
        return hash.finish();
    }
    result<digest> build_digest(build_options const& o) {
        if (!o.triangle_limit || o.triangle_limit > 85 || o.vertex_limit < 3 || o.vertex_limit > 256 || o.target_group_size < 2 || !o.max_rounds ||
            !std::isfinite(o.target_ratio) || o.target_ratio <= 0 || o.target_ratio >= 1 || !std::isfinite(o.relative_error) || o.relative_error < 0 ||
            !std::isfinite(o.normal_weight) || o.normal_weight < 0 || !std::isfinite(o.uv_weight) || o.uv_weight < 0)
            return failure(error_code::invalid_options, "invalid shared-vertices build options");
        sha256_stream hash;
        hash.text("DEREN_subtilis.build.v1");
        hash.text("shared-vertices");
        hash.text("subtilis-dag-v1");
        hash.u32(o.triangle_limit);
        hash.u32(o.vertex_limit);
        hash.u32(o.target_group_size);
        hash.u32(o.max_rounds);
        hash.f32(o.target_ratio);
        hash.f32(o.relative_error);
        hash.f32(o.normal_weight);
        hash.f32(o.uv_weight);
        hash.text("meshoptimizer-1.3");
        hash.text("9e1f07b159d3cb777f1c67ed31fc11fd117986f4");
        hash.text("fastgltf-0.9.0");
        hash.text("simdjson-4.6.9");
        hash.text("yyjson-0.12.0");
        return hash.finish();
    }
} // namespace deren::asset::subtilis
