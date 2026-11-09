# The RHI contract targets.
include_guard(GLOBAL)

# The RHI boundary's contract (m03159): a module of virtual base classes,
# deren.promise.rhi, which lives in source/promise/rhi/ - the outer segment names the family of contract
# interfaces and the inner one the graphics-API instance of it (m03716, corrected by m03931). It is a
# module rather than a directory of headers because BOTH
# sides of the boundary compile it - the engine's targets and a backend DLL each read the same
# interface unit instead of a shared header - which is what keeps the two compilations from drifting,
# and it is listed here, once, for the same reason. There is no .cpp: every declaration below this
# line is an interface, so the STATIC target exists only to carry the module's file set (the shape
# vstd_lib has for deren.vstd). The C ABI surface is deliberately NOT here (m03159): it is
# source/promise/rhi/backend_entry.hpp - in the same directory, but a plain header, not a partition and not
# in the file set below, because an entry point is not a virtual base class. The keyword it is
# declared with is a build concern and lives in source/utility/abi_export.hpp (m04074).
add_library(promise STATIC)

# Handle-free synchronization data; independent of the existing virtual recording ABI.
add_library(rhi_sync_contract STATIC)
target_sources(rhi_sync_contract PUBLIC FILE_SET CXX_MODULES FILES
    source/promise/rhi/rhi.synchronization.cppm)
target_link_libraries(promise PUBLIC shared_utility)
target_link_libraries(rhi_sync_contract PUBLIC vstd_lib)
target_link_libraries(promise PUBLIC rhi_sync_contract)

target_sources(promise
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/promise/rhi/rhi.cppm
        source/promise/rhi/rhi.contract.cppm
        source/promise/rhi/contract/rhi.error.cppm
        source/promise/rhi/contract/rhi.object.cppm
        source/promise/rhi/contract/rhi.interop.cppm
        # The creation parameters, as a contract type: the program fills it, the backend builds its
        # context from it, and the factory that takes it (S2) needs no backend-specific structure.
        source/promise/rhi/rhi.core_desc.cppm
        source/promise/rhi/rhi.extension.cppm
        source/promise/rhi/resource/rhi.resource.cppm
        source/promise/rhi/pipeline/rhi.pipeline.cppm
        source/promise/rhi/recording/rhi.recording.cppm
        source/promise/rhi/frame/rhi.frame.cppm
        source/promise/rhi/core/rhi.core.cppm
        source/promise/rhi/rhi.api_core.cppm
        source/promise/rhi/render_operations.cppm
        source/promise/gui/gui.cppm
)

# THE CONTRACT IS UNINSTRUMENTED IN THE SANITIZER JOB, AND THAT IS A LINK REQUIREMENT, NOT A PREFERENCE.
# Measured in a Debug tree configured exactly like .github/workflows/ci.yml's sanitizers job
# (-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined ...", -DCMAKE_EXE_LINKER_FLAGS carrying the runtime):
# linking `probe_backend` failed with
#     ld.lld: error: undefined symbol: __asan_init / __ubsan_handle_type_mismatch_v1 / ...
# The DLL is compiled with -fno-sanitize=all on purpose (an instrumented DLL cannot be hosted by an
# instrumented process - the rule deren_vulkan and probe_backend follow), but the OBJECTS it links out
# of this archive were instrumented, and the DLL's link line carries no sanitizer runtime. So the
# two rules meet here: a DLL that must be clean cannot be built out of instrumented objects.
#
# The cost is this archive's coverage in that job - virtual declarations, PODs and the
# validate_structure helpers - and it is the cheaper half of the trade: the alternative is an
# instrumented DLL, which is the shape that aborts when the instrumented test loads it.
if(NOT MSVC)
    target_compile_options(promise PRIVATE -fno-sanitize=all)
endif()
