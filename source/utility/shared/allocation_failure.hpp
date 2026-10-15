#pragma once
#include <cstddef>

namespace deren::utility::memory::detail {
    [[noreturn]] void terminate_allocation(char const* reason, std::size_t bytes, std::size_t alignment) noexcept;
}
