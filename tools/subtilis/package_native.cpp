#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "document.hpp"
namespace deren::tools::subtilis {
    result<void> publish_replace(path const& staging, path const& target) {
#ifdef _WIN32
        bool success = MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(staging, target, ec);
        bool success = !ec;
#endif
        if (!success)
            return std::unexpected(asset::error{asset::error_code::io, "cannot atomically replace output", {}, {}});
        return {};
    }
    result<void> publish_no_replace(path const& staging, path const& target) {
#ifdef _WIN32
        bool success = MoveFileExW(staging.c_str(), target.c_str(), 0) != 0;
#else
        bool success = ::link(staging.c_str(), target.c_str()) == 0;
        if (success) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
        }
#endif
        if (!success)
            return std::unexpected(asset::error{asset::error_code::io, "cannot publish output (target may already exist)", {}, {}});
        return {};
    }
} // namespace deren::tools::subtilis
