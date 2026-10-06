// ============================================================================
// module: deren.utility.shared
// module version: 0.1.0a
//
// THE PROCESS-WIDE HALF OF utility (batch ④, the dynamic-backend flip's prerequisite).
//
// WHY A MODULE OF ITS OWN: this toolkit is STATIC and BOTH halves of the RHI boundary link it, so
// today one process holds one copy of everything - the log sink included. TURNING `deren_vulkan`
// INTO A SHARED LIBRARY BREAKS THAT: a DLL carries its own copy of whatever it links, so the
// process would hold TWO sinks - two `ofstream`s on the same debug.log and two startup rotations,
// and `rotate_previous_log()` TRUNCATES the file the other copy is still writing. The boundary gate
// cannot see it either: that gate measures deren_vulkan ∩ vulkancorekit, and this is a third
// library defining no backend symbol. Hence a target of its own, which the flip makes SHARED.
//
// WHAT BELONGS HERE is exactly the state that must exist once per PROCESS:
//   - the log sink: the queue, its worker thread, and the file handle;
//   - the startup rotation of the previous session's debug.log (and its once-per-process claim);
//   - panic convergence: one `panic` for the whole process, flushing that one sink;
//   - the allocator hook: `:better_pmr` sets the process's default `std::pmr` resource.
// What stays in `deren.utility` (static_utility) is everything per-object or pure, plus the loader:
// data_block / BVH / frame_clock / frame_stats / thread_pool / the platform_* TUs /
// deren.utility.dynamic_link / every template. `dynamic_link` in particular stays on the
// executable's side: it is the key that loads the backend, so putting it behind another library
// would build a bootstrap chain ("load shared_utility before you can load the backend").
//
// `deren.utility` RE-EXPORTS this module, so an existing `import deren.utility;` still names the
// sink, `panic`, `log`, `error` and `init_pmr` - the call sites do not know which half a name
// came from, and this batch changes no call site.
//
// THE EXPORT SURFACE IS C++ AND MAY CARRY STL (batch ④'s relaxed rules, set by the user after the first
// draft argued itself into a C shim): std::string, std::vector, std::function, std::span, std::pmr and a
// `std::format_string` template are all allowed across this module's interface. THE MEASURED BASIS is
// that the executable imports libc++.dll and uses the UCRT heap, so ONE C++ runtime and ONE heap are
// shared by both images - an allocation here and a free there is safe in this configuration, and
// scripts/check_backend_boundary.py now machine-checks that premise (libc++.dll imported, no static
// libc++ linked anywhere).
//
// WHERE THIS BATCH STILL HANDS OVER A VIEW, it is a design choice rather than a rule: the sink's door is
// `log_text(std::string_view)` because the COPY the queue owns is made on the sink's side, and because
// the formatting stays in the caller's template instantiation (`deren.utility`'s `log`), so no template
// needs to be an exported symbol. Nothing above forbids the alternatives.
//
// THE REASON THIS MODULE EXISTS IS ONE PIECE OF STATE PER PROCESS, which is the whole point of the
// split: the sink. Two copies of it would mean two `ofstream`s on one debug.log and two startup
// rotations, and `rotate_previous_log()` truncates the file the other copy still has open. That - not
// the heap, and not STL - is what the seam is for.
//
// The dependency direction is one-way and enforced by this file's contents: `deren.utility` may
// depend on this module, and this module depends on NOTHING of it - it does not even import
// `deren.promise.rhi` (the verdict/result templates that carry `rhi::error_info` are static-side).
// ============================================================================
module;

#include <cstdint>

export module deren.utility.shared;
export import deren.vstd;

namespace deren::utility {
    /**
     * @ingroup utility
     * @brief THE sink's one door: queue a pre-formatted line for this process's log worker
     * @param text the line, without its newline (the worker adds the blank-line spacing)
     * @note
     *     - FORMATTING STAYS IN THE CALLER: `log(fmt, args...)` in `deren.utility` is an inline
     *       template whose instantiation formats into a temporary `std::string` on the caller's
     *       side and hands it here as a view. That is a design choice, not a restriction (STL is
     *       allowed across this interface - see the header): it keeps the template in the caller's
     *       instantiation and needs no exported symbol.
     *     - the copy this function queues is made on THIS side of the boundary
     *     - thread safe; a message that arrives while the sink is shutting down is dropped (see
     *       `deren.utility`'s `log()` doc for why that is the correct late-write behaviour)
     */
    export void log_text(std::string_view text) noexcept;

    /**
     * @ingroup utility
     * @brief the error route: Debug prints straight to stderr in red, Release queues `[ERROR] ...`
     * @param text the pre-formatted error text
     * @note NOT queued in Debug on purpose (error is usually followed by terminate, and the direct
     *       write is the one that survives it); in Release it joins the same queue as everything
     *       else, so there is still exactly ONE writer to debug.log
     */
    export void error_text(std::string_view text) noexcept;

    /**
     * @ingroup utility
     * @brief block until every line queued so far has been written by the log worker
     * @note this is what `panic()` flushes with before `std::terminate()`, which skips static
     *       destructors and would otherwise lose the panic's own lines
     */
    export void wait_log_all() noexcept;

    /**
     * @ingroup utility
     * @brief use when the program causes a terminating error
     * @param msg error message
     * @param source_location just use the default argument; it captures the caller's position
     * @note
     *     - thread safe, and TERMINATING: this is the process's one panic, so the report always
     *       goes through the single sink
     *     - `fatal` from a classified outcome is EXECUTED by the engine, not by the backend (see
     *       `deren::utility::enforce`): the backend reports, the engine panics here
     */
    export [[noreturn]] void panic(std::string_view msg = "",
                                   std::source_location source_location = std::source_location::current()) noexcept;
} // namespace deren::utility

// ---- the allocator hook, IN THIS INTERFACE (and not a partition - that is measured, not taste) ----
//
// IT WAS A PARTITION (`deren.utility.shared:better_pmr`) AND IT CANNOT BE ONE. Measured: a partition
// of a module that is ITSELF re-exported (`deren.utility` re-exports `deren.utility.shared`, which
// re-exported `:better_pmr`) makes clang 22.1.8 die with `clang frontend command failed due to
// signal` in a mere CONSUMER's translation unit - utility.cpp, while instantiating std::format in
// write_png, with `-fsyntax-only` (so it is Sema, not codegen). Replacing the partition re-export
// with a plain `import` makes it compile; the same file compiled against master's BMI compiles.
// The state is process-wide either way, so the declarations live here and the definitions live in
// better_pmr.cpp of this same target: no partition, no nesting, nothing lost.
class mimalloc_memory_resource : public std::pmr::memory_resource // NOLINT
{
    void* do_allocate(std::size_t size, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t size, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(memory_resource const& other) const noexcept override;
};

namespace deren::utility {
    /**
     * @defgroup better_pmr PMR Allocation Routing
     * @ingroup utility
     * @brief process-wide routing of std::pmr allocations through the vendored mimalloc
     */
    export class pmr_manager // NOLINT
    {
        std::unique_ptr<mimalloc_memory_resource> memory_resource = nullptr;
        pmr_manager();

    public:
        pmr_manager(pmr_manager const&) = delete;
        pmr_manager& operator=(pmr_manager const&) = delete;
        ~pmr_manager();
        friend pmr_manager& init_pmr();
    };

    /**
     * @ingroup better_pmr
     * @brief call this function to replace default memory resource to mimalloc memory resource
     * @warning DO NOT use any pmr container (including static) before init_pmr(), it will lead memory fault
     * @note in use: main.cpp and runtime/runtime.cpp keep a file-scope
     *     @c [[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); whose dynamic
     *     initialization runs before main(), so the runtime's per-frame std::pmr vectors
     *     (cull_visible / frame_leaves / frame_visible) already allocate via mimalloc
     * @note it sets the PROCESS's default resource, which is why it lives in this module rather than
     *     in deren.utility (static_utility) - the same reason the log sink does
     */
    export pmr_manager& init_pmr();
} // namespace deren::utility
