#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "document.hpp"
namespace deren::tools::subtilis::package_detail {
    result<void> move_path(path const& from, path const& to) {
#ifdef _WIN32
        bool success = MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
#else
        bool success = ::rename(from.c_str(), to.c_str()) == 0;
#endif
        if (!success)
            return std::unexpected(asset::error{asset::error_code::io, "cannot move staged output into place", {}, {}});
        return {};
    }
} // namespace deren::tools::subtilis::package_detail
