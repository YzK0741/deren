module;
#include <cstddef>
#include <meshoptimizer.h>
#include <mutex>
module deren.engine.subtilis.builder;
namespace deren::engine::subtilis::builder_detail {
    void initialize_meshoptimizer() {
        static std::once_flag once;
        std::call_once(once, [] {
            meshopt_setAllocator([](std::size_t size) { return deren::utility::memory::allocate(size, alignof(std::max_align_t)); },
                                 [](void* pointer) { deren::utility::memory::deallocate(pointer); });
        });
    }
    std::expected<void, build_error> simplify(source const&, vector<logical_node>&, vector<std::uint32_t>&,
                                              build_statistics&, build_options const&) {
        return {};
    }
} // namespace deren::engine::subtilis::builder_detail
