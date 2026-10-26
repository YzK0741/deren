module;
#include <cstdint>
#include <new>
module deren.utility;
import deren.utility.shared;

namespace deren::utility {
    std::string_view file_error_name(file_error_code const code) noexcept {
        switch (code) {
        case file_error_code::status_failed:
            return "status_failed";
        case file_error_code::not_regular_file:
            return "not_regular_file";
        case file_error_code::open_failed:
            return "open_failed";
        case file_error_code::size_unavailable:
            return "size_unavailable";
        case file_error_code::too_large:
            return "too_large";
        case file_error_code::read_failed:
            return "read_failed";
        case file_error_code::file_changed:
            return "file_changed";
        }
        return "unknown";
    }

    namespace {
        template <typename Container>
        std::expected<Container, file_error> read_bounded_file(std::filesystem::path const& path,
                                                               std::uintmax_t const max_bytes) {
            std::error_code cause;
            auto const status = std::filesystem::status(path, cause);
            if (cause || !std::filesystem::exists(status)) {
                if (!cause) {
                    cause = std::make_error_code(std::errc::no_such_file_or_directory);
                }
                return std::unexpected(file_error{file_error_code::status_failed, cause});
            }
            if (!std::filesystem::is_regular_file(status)) {
                return std::unexpected(file_error{file_error_code::not_regular_file});
            }
            std::ifstream file(path, std::ios::binary);
            auto const stream_error = std::make_error_code(std::io_errc::stream);
            if (!file) {
                return std::unexpected(file_error{file_error_code::open_failed, stream_error});
            }
            // Measure the opened stream, not an earlier file_size(path) lookup.
            file.seekg(0, std::ios::end);
            auto const end = file.tellg();
            if (end < std::streampos(0)) {
                return std::unexpected(file_error{file_error_code::size_unavailable, stream_error});
            }
            auto const size = static_cast<std::uintmax_t>(static_cast<std::streamoff>(end));
            Container data;
            if (size > max_bytes || size > data.max_size()) {
                return std::unexpected(file_error{file_error_code::too_large});
            }
            file.seekg(0, std::ios::beg);
            if (!file) {
                return std::unexpected(file_error{file_error_code::read_failed, stream_error});
            }
            data.resize(static_cast<std::size_t>(size)); // checked against container max_size above
            std::size_t offset = 0;
            constexpr std::size_t chunk_bytes = std::size_t{1024} * 1024;
            while (offset < data.size()) {
                auto const count = std::min(chunk_bytes, data.size() - offset);
                auto const requested = static_cast<std::streamsize>(count);
                file.read(reinterpret_cast<char*>(data.data()) + offset, requested);
                if (file.bad() || (file.fail() && !file.eof())) {
                    return std::unexpected(file_error{file_error_code::read_failed, stream_error});
                }
                if (file.gcount() != requested) {
                    return std::unexpected(file_error{file_error_code::file_changed});
                }
                offset += count;
            }
            // The initial size is our allocation bound. An observed append is an error, not growth.
            auto const extra = file.peek();
            if (file.bad() || (file.fail() && !file.eof())) {
                return std::unexpected(file_error{file_error_code::read_failed, stream_error});
            }
            if (extra != std::char_traits<char>::eof()) {
                return std::unexpected(file_error{file_error_code::file_changed});
            }
            return data;
        }
    } // namespace

    std::expected<vessel::vector<uint8_t>, file_error> read_binary_to_vector(std::filesystem::path const& path,
                                                                             std::uintmax_t const max_bytes) {
        return read_bounded_file<vessel::vector<uint8_t>>(path, max_bytes);
    }
    std::expected<vessel::string, file_error> read_binary_to_string(std::filesystem::path const& path,
                                                                    std::uintmax_t const max_bytes) {
        return read_bounded_file<vessel::string>(path, max_bytes);
    }
} // namespace deren::utility
