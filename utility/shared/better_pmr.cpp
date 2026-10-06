module;

#include <mimalloc.h>

module deren.utility.shared;

// ============================================================================
// The allocator hook's DEFINITIONS (the declarations are in shared_utility.cppm, which explains why
// this is not a partition). A translation unit of its own so that <mimalloc.h> stays in one global
// module fragment, exactly as the sink's platform pieces keep <windows.h> out of the module purview.
// ============================================================================

void* mimalloc_memory_resource::do_allocate(std::size_t const size, std::size_t alignment) {
    return mi_aligned_alloc(alignment, size);
}

void mimalloc_memory_resource::do_deallocate(void* p, [[maybe_unused]] std::size_t size, std::size_t const alignment) {
    mi_free_aligned(p, alignment);
}

bool mimalloc_memory_resource::do_is_equal(memory_resource const& other) const noexcept {
    return this == &other;
}

namespace deren::utility {
    pmr_manager::pmr_manager() {
        this->memory_resource = std::make_unique<mimalloc_memory_resource>();
        std::pmr::set_default_resource(this->memory_resource.get());
    }

    pmr_manager::~pmr_manager() {
        std::pmr::set_default_resource(nullptr);
        this->memory_resource = nullptr;
    }

    pmr_manager& init_pmr() {
        static pmr_manager manager;
        return manager;
    }
} // namespace deren::utility
