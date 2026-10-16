#include <boost/throw_exception.hpp>
import deren.utility.shared;
namespace boost {
    BOOST_NORETURN void throw_exception(std::exception const&) {
        deren::utility::fast_panic("unexpected Boost exception");
    }
    BOOST_NORETURN void throw_exception(std::exception const&, boost::source_location const&) {
        deren::utility::fast_panic("unexpected Boost exception");
    }
} // namespace boost
