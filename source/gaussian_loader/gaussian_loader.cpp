module;
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
module deren.gaussian_loader;
namespace deren::gaussian {
    namespace {
        struct property {
            std::string name;
            unsigned bytes;
            bool floating;
            bool signed_value;
        };
        bool space(char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }
        std::string_view take(std::string_view& s) {
            while (!s.empty() && space(s.front()))
                s.remove_prefix(1);
            auto n = s.find_first_of(" \t\r\n");
            auto token = s.substr(0, n);
            s.remove_prefix(n == s.npos ? s.size() : n);
            return token;
        }
        bool unsigned_number(std::string_view s, std::uint64_t& value) {
            auto result = std::from_chars(s.data(), s.data() + s.size(), value);
            return !s.empty() && result.ec == std::errc{} && result.ptr == s.data() + s.size();
        }
        bool kind(std::string_view t, property& p) {
            p.floating = false;
            p.signed_value = false;
            if (t == "float" || t == "float32") {
                p.bytes = 4;
                p.floating = true;
            } else if (t == "double" || t == "float64") {
                p.bytes = 8;
                p.floating = true;
            } else if (t == "char" || t == "int8") {
                p.bytes = 1;
                p.signed_value = true;
            } else if (t == "uchar" || t == "uint8")
                p.bytes = 1;
            else if (t == "short" || t == "int16") {
                p.bytes = 2;
                p.signed_value = true;
            } else if (t == "ushort" || t == "uint16")
                p.bytes = 2;
            else if (t == "int" || t == "int32") {
                p.bytes = 4;
                p.signed_value = true;
            } else if (t == "uint" || t == "uint32")
                p.bytes = 4;
            else
                return false;
            return true;
        }
        bool scalar(std::string_view token, property const& p, double& value) {
            if (p.floating) {
                auto r = std::from_chars(token.data(), token.data() + token.size(), value);
                if (r.ec != std::errc{} || r.ptr != token.data() + token.size() || !std::isfinite(value))
                    return false;
                return p.bytes != 4 || std::isfinite(static_cast<float>(value));
            }
            if (p.signed_value) {
                std::int64_t v = 0;
                auto r = std::from_chars(token.data(), token.data() + token.size(), v);
                auto max = (std::int64_t(1) << (p.bytes * 8 - 1)) - 1;
                if (r.ec != std::errc{} || r.ptr != token.data() + token.size() || v < -max - 1 || v > max)
                    return false;
                value = static_cast<double>(v);
            } else {
                std::uint64_t v = 0;
                if (!unsigned_number(token, v) || v > ((std::uint64_t(1) << (p.bytes * 8)) - 1))
                    return false;
                value = static_cast<double>(v);
            }
            return !token.empty();
        }
    } // namespace
    load_result parse_ply(std::span<std::byte const> bytes, load_options const& limits) {
        auto fail = [](error_code code, std::size_t offset, std::string property_name, std::string message) -> load_result {
            return std::unexpected(diagnostic{code, offset, std::move(property_name), std::move(message)});
        };
        if (bytes.size() > limits.max_file_bytes)
            return fail(error_code::limit_exceeded, 0, {}, "file budget exceeded");
        if (bytes.empty())
            return fail(error_code::invalid_header, 0, {}, "missing PLY header");
        std::string_view text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        std::size_t offset = 0;
        bool binary = false, format = false, vertex = false, ended = false;
        std::uint64_t count = 0;
        std::vector<property> properties;
        while (offset < text.size()) {
            auto end = text.find('\n', offset);
            if (end == text.npos)
                return fail(error_code::truncated, offset, {}, "unterminated header");
            if (end >= 65536)
                return fail(error_code::limit_exceeded, offset, {}, "header exceeds 64 KiB");
            auto line_offset = offset;
            auto line = text.substr(offset, end - offset);
            offset = end + 1;
            if (line_offset == 0) {
                if (line != "ply" && line != "ply\r")
                    return fail(error_code::invalid_header, 0, {}, "expected ply");
                continue;
            }
            auto command = take(line);
            if (command == "comment" || command == "obj_info")
                continue;
            if (command == "format") {
                auto encoding = take(line);
                auto version = take(line);
                if (format || vertex || version != "1.0" || !take(line).empty())
                    return fail(error_code::invalid_header, line_offset, {}, "invalid format declaration");
                if (encoding != "ascii" && encoding != "binary_little_endian")
                    return fail(error_code::unsupported_format, line_offset, {}, "unsupported PLY encoding");
                binary = encoding == "binary_little_endian";
                format = true;
            } else if (command == "element") {
                auto name = take(line);
                auto number = take(line);
                if (!format || vertex || name != "vertex" || !unsigned_number(number, count) || !take(line).empty())
                    return fail(error_code::invalid_header, line_offset, {}, "only one vertex element is supported");
                vertex = true;
            } else if (command == "property") {
                auto type = take(line);
                auto name = take(line);
                property p{};
                if (!vertex || name.empty() || !take(line).empty() || !kind(type, p))
                    return fail(error_code::invalid_header, line_offset, std::string(name), "unsupported property declaration");
                if (properties.size() >= 256)
                    return fail(error_code::limit_exceeded, line_offset, {}, "too many properties");
                if (std::any_of(properties.begin(), properties.end(), [name](auto const& old) { return old.name == name; }))
                    return fail(error_code::invalid_header, line_offset, std::string(name), "duplicate property");
                p.name = name;
                properties.push_back(std::move(p));
            } else if (command == "end_header") {
                if (!format || !vertex || !take(line).empty())
                    return fail(error_code::invalid_header, line_offset, {}, "incomplete header");
                ended = true;
                break;
            } else
                return fail(error_code::invalid_header, line_offset, {}, "unknown header declaration");
        }
        if (!ended)
            return fail(error_code::truncated, offset, {}, "missing end_header");
        if (count > limits.max_particles || count > std::numeric_limits<std::size_t>::max() / sizeof(particle) ||
            limits.max_output_bytes < sizeof(asset) || count > (limits.max_output_bytes - sizeof(asset)) / sizeof(particle))
            return fail(error_code::limit_exceeded, offset, {}, "particle/output budget exceeded");
        auto find = [&properties](std::string_view name) -> std::size_t {
            auto it = std::find_if(properties.begin(), properties.end(), [name](auto const& p) { return p.name == name; });
            return static_cast<std::size_t>(it - properties.begin());
        };
        std::array<std::size_t, 14> required{};
        unsigned r = 0;
        for (auto name : {"x", "y", "z", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"}) {
            auto i = find(name);
            if (i == properties.size())
                return fail(error_code::missing_property, offset, name, "required attribute absent");
            required[r++] = i;
        }
        std::array<std::size_t, 45> rest{};
        rest.fill(properties.size());
        unsigned rest_count = 0;
        for (std::size_t i = 0; i < properties.size(); ++i) {
            auto const& name = properties[i].name;
            if (!name.starts_with("f_rest_"))
                continue;
            std::uint64_t n = 0;
            auto suffix = std::string_view(name).substr(7);
            if (!unsigned_number(suffix, n) || n >= 45 || suffix != std::to_string(n) || rest[n] != properties.size())
                return fail(error_code::invalid_header, offset, name, "invalid SH property index");
            rest[n] = i;
            ++rest_count;
        }
        if (rest_count != 0 && rest_count != 9 && rest_count != 24 && rest_count != 45)
            return fail(error_code::invalid_header, offset, {}, "incomplete SH degree");
        for (unsigned i = 0; i < rest_count; ++i)
            if (rest[i] == properties.size())
                return fail(error_code::missing_property, offset, "f_rest_" + std::to_string(i), "noncontiguous SH attributes");
        std::size_t stride = 0;
        for (auto const& p : properties)
            stride += p.bytes;
        if ((binary && count > (bytes.size() - offset) / stride) || (!binary && count > (bytes.size() - offset) / (properties.size() * 2 - 1)))
            return fail(error_code::truncated, offset, {}, "payload too short for declared count");
        asset answer{};
        answer.sh_degree = rest_count == 0 ? 0 : rest_count == 9 ? 1
                                             : rest_count == 24  ? 2
                                                                 : 3;
        answer.particles.reserve(static_cast<std::size_t>(count));
        std::vector<double> row(properties.size());
        for (std::uint64_t item = 0; item < count; ++item) {
            for (std::size_t i = 0; i < properties.size(); ++i) {
                auto const& p = properties[i];
                auto start = offset;
                double value = 0;
                if (binary) {
                    if (bytes.size() - offset < p.bytes)
                        return fail(error_code::truncated, offset, p.name, "truncated scalar");
                    std::uint64_t bits = 0;
                    for (unsigned b = 0; b < p.bytes; ++b)
                        bits |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset++])) << (b * 8);
                    if (p.floating)
                        value = p.bytes == 4 ? double(std::bit_cast<float>(static_cast<std::uint32_t>(bits))) : std::bit_cast<double>(bits);
                    else if (p.signed_value && (bits & (std::uint64_t(1) << (p.bytes * 8 - 1))))
                        value = double(bits) - std::ldexp(1.0, int(p.bytes * 8));
                    else
                        value = double(bits);
                    if (!std::isfinite(value))
                        return fail(error_code::invalid_data, start, p.name, "nonfinite scalar");
                } else {
                    auto tail = text.substr(offset);
                    auto token = take(tail);
                    if (token.empty())
                        return fail(error_code::truncated, offset, p.name, "missing scalar");
                    offset = text.size() - tail.size();
                    if (!scalar(token, p, value))
                        return fail(error_code::invalid_data, start, p.name, "invalid scalar");
                }
                row[i] = value;
            }
            particle point{};
            for (unsigned i = 0; i < 14; ++i)
                if (!std::isfinite(static_cast<float>(row[required[i]])))
                    return fail(error_code::invalid_data, offset, properties[required[i]].name, "attribute exceeds float32");
            for (unsigned i = 0; i < 3; ++i) {
                point.center[i] = float(row[required[i]]);
                point.scale[i] = float(std::exp(row[required[i + 3]]));
            }
            double norm = 0;
            for (unsigned i = 0; i < 4; ++i)
                norm = std::hypot(norm, row[required[i + 6]]);
            if (!(norm > 0))
                return fail(error_code::invalid_data, offset, "rot_0", "zero quaternion");
            for (unsigned i = 0; i < 4; ++i)
                point.rotation[i] = float(row[required[i + 6]] / norm);
            double logit = row[required[10]];
            point.opacity = float(logit >= 0 ? 1 / (1 + std::exp(-logit)) : std::exp(logit) / (1 + std::exp(logit)));
            for (unsigned i = 0; i < 3; ++i)
                point.sh[i] = float(row[required[11 + i]]);
            unsigned bands = rest_count / 3;
            for (unsigned c = 0; c < 3; ++c)
                for (unsigned k = 0; k < bands; ++k) {
                    auto value = float(row[rest[c * bands + k]]);
                    if (!std::isfinite(value))
                        return fail(error_code::invalid_data, offset, properties[rest[c * bands + k]].name, "SH exceeds float32");
                    point.sh[(k + 1) * 3 + c] = value;
                }
            double w = point.rotation[0], x = point.rotation[1], y = point.rotation[2], z = point.rotation[3];
            double matrix[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)}, {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)}, {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
            for (unsigned i = 0; i < 3; ++i)
                if (!(point.scale[i] > 0) || !std::isfinite(point.scale[i]))
                    return fail(error_code::invalid_data, offset, "scale_" + std::to_string(i), "invalid activated scale");
            for (unsigned i = 0; i < 3; ++i) {
                double variance = 0;
                for (unsigned j = 0; j < 3; ++j) {
                    double v = matrix[i][j] * point.scale[j];
                    variance += v * v;
                }
                double radius = 3 * std::sqrt(variance);
                double lower = double(point.center[i]) - radius, upper = double(point.center[i]) + radius;
                float lo = float(lower), hi = float(upper);
                if (double(lo) > lower)
                    lo = std::nextafter(lo, -std::numeric_limits<float>::infinity());
                if (double(hi) < upper)
                    hi = std::nextafter(hi, std::numeric_limits<float>::infinity());
                if (!std::isfinite(float(variance)) || float(variance) == 0 || !std::isfinite(lo) || !std::isfinite(hi))
                    return fail(error_code::invalid_data, offset, {}, "covariance/support outside float32");
                if (item == 0) {
                    answer.bounds_min[i] = lo;
                    answer.bounds_max[i] = hi;
                } else {
                    answer.bounds_min[i] = std::min(answer.bounds_min[i], lo);
                    answer.bounds_max[i] = std::max(answer.bounds_max[i], hi);
                }
            }
            answer.particles.push_back(point);
        }
        auto tail = text.substr(offset);
        if ((binary && !tail.empty()) || (!binary && !take(tail).empty()))
            return fail(error_code::invalid_data, offset, {}, "unexpected trailing payload");
        return answer;
    }
    load_result load_ply(std::string_view path, load_options const& limits) {
        std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
        if (!file)
            return std::unexpected(diagnostic{error_code::io, 0, {}, "cannot open file"});
        auto end = file.tellg();
        if (end < 0)
            return std::unexpected(diagnostic{error_code::io, 0, {}, "cannot read file size"});
        auto size = static_cast<std::uint64_t>(end);
        if (size > limits.max_file_bytes || size > std::numeric_limits<std::size_t>::max() || size > std::uint64_t(std::numeric_limits<std::streamsize>::max()))
            return std::unexpected(diagnostic{error_code::limit_exceeded, 0, {}, "file budget exceeded"});
        std::vector<std::byte> data(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
            return std::unexpected(diagnostic{error_code::io, 0, {}, "file read failed"});
        return parse_ply(data, limits);
    }
} // namespace deren::gaussian
