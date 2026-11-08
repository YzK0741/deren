# First-party utility targets.
include_guard(GLOBAL)

# THE UTILITY TOOLKIT IS TWO TARGETS (batch ④, the dynamic-backend flip's prerequisite).
#
# WHY TWO, and it is a measured hazard rather than tidiness: the toolkit is STATIC and BOTH halves
# of the RHI boundary link it, so one process holds one copy of everything - the log sink included.
# THE FLIP BREAKS THAT: `deren_vulkan` becomes SHARED, the DLL carries its own copy of every
# library it links, and the process then holds TWO sinks - two `ofstream`s on the same deren-debug.log,
# two startup rotations, and `rotate_previous_log()` TRUNCATES the file the other copy still has
# open and appends to (interleaved lines, holes, two sessions in one deren-debug.log.old). The boundary
# gate is blind to it by construction: it measures deren_vulkan ∩ deren_engine, and this is a
# third library that defines no backend symbol.
#
# THE SEAM IS STATE, NOT DIRECTORY:
#   shared_utility  - the PROCESS-WIDE state: the log sink (queue, worker thread, file handle), the
#                     startup rotation, panic convergence, and the mimalloc allocation/free service.
#   static_utility  - everything per-object or pure, plus the loader: data_block, BVH, frame_clock,
#                     frame_stats, thread_pool, the platform_* TUs, deren.utility.dynamic_link, and
#                     the printing / byte-writer TEMPLATES (`log` / `error` / `panic` format in the
#                     caller's instantiation and hand the result over as a std::string_view).
#
# BOTH ARE STATIC IN THIS BATCH, on purpose: the split lands and is verified first with ZERO
# behaviour change (one sink today, exactly as before), and `shared_utility` becomes SHARED in the
# same batch as `deren_vulkan`. Flipping the two together would make the split itself unverifiable.
#
# ONE TARGET PER MODULE NAME, EVEN THOUGH BOTH HALVES COMPILE EACH MODULE: `shared_utility` owns
# `deren.utility.shared` (including the vessel allocator interface); `static_utility` owns `deren.utility`
# (with `:data_block` / `:bvh` / `:frame_clock` / `:frame_stats` / `:thread_pool`) and
# `deren.utility.dynamic_link`. Listing one .cppm in two targets would produce two BMIs of one
# module name, which a single translation unit cannot arbitrate - the same ruling that moved
# deren.vulkan.constant_init into a target of its own (see that target's comment above).
#
# `deren.utility` RE-EXPORTS `deren.utility.shared`, so the 52 existing `import deren.utility;`
# sites keep working unchanged: a call site does not know which half a name came from.
#
# DEPENDENCY DIRECTION: static_utility MAY depend on shared_utility (it formats text and calls the
# sink); the reverse is FORBIDDEN - the shared module writes its own file routes instead of calling this
# module's `println`; logging/fatal routes have no console fallback. Neither utility target imports or links the RHI contract;
# the contract depends on shared_utility. RHI failure types (`error`, `error_info`) belong to the contract.
#
# `dynamic_link` STAYS ON THE STATIC SIDE FOR A LOAD-BEARING REASON: it is the key that loads the
# backend, so putting it in the process-wide library would build a bootstrap chain ("the executable
# must load shared_utility before it can load the backend") - and it needs no shared state.
#
# THE EXPORT SURFACE IS C++, AND THIS BATCH USES THE RELAXED RULES THE USER SET (batch ④). The three
# of them, written here because a later reader will otherwise "fix" this split back into a C shim:
#   1. NO C SHIM. `deren.utility.shared` exports C++ directly (std::string_view, std::span,
#      std::source_location, std::format_string) - an opaque C ABI for a library both halves are built
#      from, by one toolchain, in one commit, buys nothing here.
#   2. STL IS ALLOWED: std::string / std::vector / std::function / std::span / std::pmr may cross, and
#      `log(fmt, args...)` may be a template in a module interface. THE MEASURED BASIS, not a hope:
#      the executable imports libc++.dll and uses the UCRT heap, so both images share ONE C++ runtime
#      and ONE heap - which is what makes an allocation on one side and a free on the other safe in
#      this configuration. That premise is now MACHINE-CHECKED rather than assumed: see the
#      C++-runtime check in scripts/checks/check_backend_boundary.py (dll imports enumerated, no static
#      libc++ in any image).
#   3. THE REASON THE SPLIT EXISTS IS STATE, NOT MEMORY. One log sink per process: two copies mean two
#      `ofstream`s on one deren-debug.log and two startup rotations, and `rotate_previous_log()` TRUNCATES
#      the file the other copy still has open (interleaved lines, holes, two sessions in one
#      deren-debug.log.old). The heap/STL question is NOT the reason for this split and is deliberately not
#      argued here - that is the `shared_object` line of work (abi 18).
# WHERE THIS BATCH STILL PASSES A VIEW (`log_text(std::string_view)`), the reason is a design choice
# rather than a prohibition: the sink copies what it queues on its own side, and the formatting stays
# in the caller's template instantiation, so no template has to be an exported symbol. Nothing in the
# rules above requires that - it is simply the shape the sink's door settled on.
# THE PROCESS-WIDE GUARD IS THE ROTATION CLAIM (source/utility/shared/log_rotation_claim.cpp): an OS-named
# object keyed by the process id, so even if a second copy of the sink ever appears in one process the
# second one does NOT rotate the file the first one is writing.
# =============================================================================================
# SHARED, IN THE SAME BATCH AS deren_vulkan, AND THIS IS THE MOMENT THE SPLIT WAS FOR: until now both
# halves of the toolkit were STATIC, so the process happened to hold one sink even with two copies of
# the code. A DLL carries its own copy of what it links, so with deren_vulkan SHARED the sink WOULD be
# duplicated - two `ofstream`s on one deren-debug.log, two startup rotations, and
# `rotate_previous_log()` TRUNCATING the file the other copy still has open. `shared_utility` being a
# DLL is what prevents it: the executable and deren_vulkan.dll both import THIS image, so the OS loads
# one copy of the sink for the whole process. The rotation claim
# (source/utility/shared/log_rotation_claim.cpp, an OS-named object keyed by the process id) stays as the
# second line of defence, and this batch is where it is first genuinely needed - and where it was
# MEASURED, on the artifacts: one rotation for the whole process, not one per image.
add_library(shared_utility SHARED)
# THE FILE NAME IS PART OF THE DESIGN: the process-wide half ships as `shared_utility.dll`, because
# BOTH the executable and deren_vulkan.dll import it BY THAT NAME. MinGW's default `lib` prefix would
# produce `libshared_utility.dll`, so it is removed on purpose (the import library keeps its
# conventional spelling through IMPORT_PREFIX).
set_target_properties(shared_utility PROPERTIES PREFIX "")
get_filename_component(_trace_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(VR_STACKTRACE_SYMBOLIZER NAMES llvm-symbolizer HINTS "${_trace_compiler_dir}")
if(VR_STACKTRACE_SYMBOLIZER)
    target_compile_definitions(shared_utility PRIVATE DEREN_LLVM_SYMBOLIZER="${VR_STACKTRACE_SYMBOLIZER}")
endif()
target_link_libraries(shared_utility PRIVATE ${CMAKE_DL_LIBS})

target_sources(shared_utility
        PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT}
        FILES
        # The sink, the rotation, panic convergence, the allocator hook. Its tools go into
        # `deren.utility` below, which re-exports this module.
        ${DEREN_REPO_ROOT}/source/utility/shared/shared_utility.cppm
        PRIVATE
        ${DEREN_REPO_ROOT}/source/utility/shared/shared_utility.cpp
        # The allocator hook's definitions. IT IS NOT A PARTITION, AND THAT IS MEASURED: a partition
        # of a module that is itself re-exported (`deren.utility` re-exports `deren.utility.shared`)
        # makes clang 22.1.8 crash while semantically analysing a consumer's std::format call
        # (utility.cpp's write_png, reproduced with -fsyntax-only; the crash disappears when the
        # partition re-export becomes a plain import). So the hook is declared in the primary
        # interface and defined here - no partition, nothing lost.
        ${DEREN_REPO_ROOT}/source/utility/shared/memory.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/allocation_failure.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace_native.cpp
        ${DEREN_REPO_ROOT}/source/utility/shared/stacktrace_posix.cpp
        # The once-per-process rotation claim, a plain (non-module) TU for the platform_* reason:
        # <windows.h> does not belong in a module's global module fragment.
        ${DEREN_REPO_ROOT}/source/utility/shared/log_rotation_claim.cpp
)

target_link_libraries(shared_utility
        vstd_lib
        mimalloc_vendored
)
add_library(static_utility STATIC)

target_sources(static_utility
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/utility/utility.cppm
        source/utility/data_block.cppm
        source/utility/BVH/bvh.cppm
        source/utility/frame_clock/frame_clock.cppm
        source/utility/frame_stats/frame_stats.cppm
        source/utility/thread_pool/thread_pool.cppm
        # The loader the RHI boundary is built on: the only place in the tree that
        # asks the platform for a library. It is a module of its own rather than a `utility:` partition
        # because a partition cannot be imported by another target's module without dragging `deren.utility`
        # in, and the boundary rule is that ONE module owns the platform load path. It declares the
        # platform entry points itself (no <windows.h>, no <dlfcn.h> in a module's global fragment),
        # which is what keeps its importers platform-agnostic.
        # IT STAYS HERE, NOT IN shared_utility: it is the key that loads the backend, so putting it
        # behind the process-wide library would make "load shared_utility" a prerequisite of "load
        # the backend" - a bootstrap chain with nothing to gain.
        source/utility/dynamic_link.cppm
        PRIVATE
        source/utility/utility.cpp
        source/utility/file_io.cpp
)

# static_utility: the per-half toolkit. utility.cpp implements the xxHash-based hash functions
# (deren::utility::xxh3_128bits) on top of the vendored xxhash library, so the dependency is
# declared here, on the library that actually uses the XXH3_128bits symbol. It propagates to
# consumers (deren_engine / deren_assets / deren) at link time, so the executable does not need to
# link xxhash itself. The platform_* TUs live here too - they are per-half by nature (no state).
target_link_libraries(static_utility PUBLIC
        vstd_lib
        xxhash
        # PUBLIC because this module re-exports deren.utility.shared: every consumer that imports
        # `deren.utility` needs that module's BMI (and, at the flip, its import library) as well.
        shared_utility
)
if(WIN32)
    target_link_libraries(static_utility PUBLIC comdlg32)
endif()
target_link_libraries(static_utility PRIVATE boost_int128_headers)
