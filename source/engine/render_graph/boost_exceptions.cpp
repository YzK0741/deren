import deren.vstd;
#include <boost/throw_exception.hpp>

import deren.utility.shared;
namespace boost {
    BOOST_NORETURN void throw_exception(std::exception const&) {
        deren::utility::fast_panic("internal invariant: unexpected Boost exception");
    }
    BOOST_NORETURN void throw_exception(std::exception const&, boost::source_location const& location) {
        char message[512];
        std::snprintf(message, sizeof(message), "internal invariant: unexpected Boost exception at %s:%u in %s",
                      location.file_name() != nullptr ? location.file_name() : "<unknown>",
                      static_cast<unsigned>(location.line()),
                      location.function_name() != nullptr ? location.function_name() : "<unknown>");
        deren::utility::fast_panic(message);
    }
} // namespace boost
