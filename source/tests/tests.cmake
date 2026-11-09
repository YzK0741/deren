# Headless test registration.
include_guard(GLOBAL)

if(VR_BUILD_TESTS)
    enable_testing()
    # Test-only hooks: compiled when VR_BUILD_TESTS=ON (which is also the Release package config),
    # inert without a caller and absent from the module interface. This follows the existing
    # DEREN_MEMORY_TESTING precedent; a dedicated test-support option is a possible future split.
    target_compile_definitions(shared_utility PRIVATE DEREN_MEMORY_TESTING DEREN_LOG_TESTING)
    target_sources(shared_utility PRIVATE source/utility/shared/log_test_hooks.cpp)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME test_clang_tidy_driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_clang_tidy_driver.py")
    add_test(NAME test_progressive_measure
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_progressive_measure.py")
    add_test(NAME test_software_raster_probe_verifier
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure/verify_software_raster.py" --self-test)
    add_test(NAME test_software_raster_image_verifier
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure/verify_software_images.py" --self-test)
    # M4 Task 3 closeout: the deterministic fixture generator and the GPU-route acceptance capture harness have their
    # own headless self-tests, so a broken fixture recipe or a capture whose markers/manifest expectations no longer
    # match the runtime fails in CI instead of in a GPU-only acceptance run.
    add_test(NAME test_software_raster_fixture_generator
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure/make_software_raster_fixture.py" --self-test)
    add_test(NAME test_software_gpu_capture_harness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure/capture_software_gpu_route.py" --self-test)
    add_test(NAME test_boost_graph_vendor
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/vendor_boost_graph.py"
                --output "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/boost" --verify)
    add_test(NAME test_boost_int128_vendor
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tools/vendor_boost_int128.py"
                --output "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/boost" --verify)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads REQUIRED)
        add_executable(stacktrace_posix_probe
            source/tests/stacktrace_posix_probe.cpp
            source/utility/shared/stacktrace_native.cpp
            source/utility/shared/stacktrace_posix.cpp)
        target_link_libraries(stacktrace_posix_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
        add_test(NAME test_stacktrace_posix
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_stacktrace_posix.py"
                    $<TARGET_FILE:stacktrace_posix_probe> "${VR_STACKTRACE_SYMBOLIZER}")
    endif()
    add_test(NAME test_stacktrace_offline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_stacktrace_offline.py")
    add_test(NAME test_native_boundary_zero
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/checks/check_native_boundary.py"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}" --require-zero)

    add_test(NAME test_backend_boundary
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/source/tests" -p "test_*boundary.py" -v)
    set_tests_properties(test_backend_boundary PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run"
        ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/test-run;TEMP=${CMAKE_CURRENT_BINARY_DIR}/test-run;VR_TARGET_MANIFEST=${CMAKE_BINARY_DIR}/target_manifest.txt")
    # 完整产品先构建，再量 main/app_support/引擎归档，避免半构建产生假性下降。
    add_custom_target(check-backend-boundary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/checks/check_backend_boundary.py"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
                --report "${CMAKE_CURRENT_BINARY_DIR}/backend_boundary_report.json"
                --require-zero
        DEPENDS deren
        VERBATIM)

    set(VR_TEST_TARGETS test_acceleration_structures test_app_config test_asset_library test_subtilis test_subtilis_asset test_subtilis_dag_asset test_subtilis_dag_lod test_subtilis_import test_subtilis_lod test_subtilis_builder test_subtilis_residency test_subtilis_page_streaming test_subtilis_page_gpu test_subtilis_upload test_subtilis_selection test_subtilis_frame test_subtilis_cut_gpu test_rhi_retention test_utility test_morton test_vessel test_stacktrace test_math test_profiling test_filters test_animation test_gltf_loader test_gaussian_loader test_gaussian_splatting test_gaussian_raster test_gaussian_gpu_frame test_toon_material_sidecar test_shadow_fit test_render_resources test_resource_cluster test_boost_graph test_render_graph test_render_graph_ranges test_render_graph_parameters test_render_graph_sync test_render_graph_dump test_render_graph_lowering test_render_graph_execute test_render_graph_bloom test_render_graph_frame test_render_graph_post test_render_graph_compose test_render_graph_lighting test_pass test_shader_sources test_meshlet test_docs test_dynamic_link test_vulkan_capabilities test_progressive_texture test_texture_upload_layout test_error_mapping test_runtime_injection test_image_formats test_visibility_buffer test_render_graph_visibility test_visibility_raster_pass test_visibility_resolve_pass test_hzb test_visibility_software test_visibility_software_pass test_visibility_merge_pass test_render_operations)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run")
    # EACH TEST GETS ITS OWN WORKING DIRECTORY (planned X5 housekeeping): several tests write and read files
    # there - test_runtime_injection reads the panic text a CHILD process left behind - and with ONE shared
    # directory `ctest -j` made two of them read each other's files (measured: 18/19 in parallel, 19/19
    # serial). They cannot collide now.

    # NO CONDITIONAL TEST TARGETS. A test whose add_executable sits behind `if(EXISTS ...)` disappears from
    # CTest silently when its source file is gone - measured: `test_radiance_capture` was dropped in
    # d770719f and the guard outlived it, so the suite was one test smaller and nothing anywhere said so.
    # A target that is meant to exist is declared unconditionally; a missing source is then a configure error.
    foreach(test_target IN ITEMS ${VR_TEST_TARGETS})
        add_executable(${test_target} source/tests/${test_target}.cpp)
        target_link_libraries(${test_target} shared_utility vstd_lib)
        add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run/${test_target}")
        set_tests_properties(${test_target} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-run/${test_target}"
                TIMEOUT 120)
    endforeach()

    target_link_libraries(test_morton boost_int128_headers)
    target_link_libraries(test_rhi_retention promise vulkan_core_support)
    # The injection test's single entry point carries the local objects for every child mode; Debug+ASan
    # builds that frame above the Windows default 1 MiB reserve. Give this test executable room to run.
    if(WIN32)
        target_link_options(test_runtime_injection PRIVATE -Wl,--stack,8388608)
    endif()

    # Execute the real module-partition query bodies with driver query doubles.
    # This test links no Vulkan loader and needs no GPU or window.
    set(VR_VULKAN_QUERY_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/test-generated/vulkan_query_fixture.h")
    set(VR_TEXTURE_REGIONS_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/test-generated/texture_regions_fixture.h")
    add_custom_command(OUTPUT "${VR_VULKAN_QUERY_FIXTURE}" "${VR_TEXTURE_REGIONS_FIXTURE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/generate_vulkan_query_fixture.py"
                --source "${CMAKE_CURRENT_SOURCE_DIR}/source/backends/vulkan/core/init_utils/init_utils.cppm"
                --output "${VR_VULKAN_QUERY_FIXTURE}"
                --vma-source "${CMAKE_CURRENT_SOURCE_DIR}/source/backends/vulkan/core/vma/vma.cppm"
                --regions-output "${VR_TEXTURE_REGIONS_FIXTURE}"
        DEPENDS source/tests/generate_vulkan_query_fixture.py source/backends/vulkan/core/init_utils/init_utils.cppm source/backends/vulkan/core/vma/vma.cppm
        VERBATIM)
    target_sources(test_vulkan_capabilities PRIVATE "${VR_VULKAN_QUERY_FIXTURE}")
    target_link_libraries(test_vulkan_capabilities vulkan_constant_init)
    target_include_directories(test_vulkan_capabilities PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/test-generated" "${Vulkan_INCLUDE_DIRS}")
    target_include_directories(test_texture_upload_layout PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source/backends/vulkan/core/vma" "${Vulkan_INCLUDE_DIRS}" "${CMAKE_CURRENT_BINARY_DIR}/test-generated")
    target_sources(test_texture_upload_layout PRIVATE "${VR_TEXTURE_REGIONS_FIXTURE}")
    target_link_libraries(test_texture_upload_layout vulkan_constant_init)

    # source root for fixture/model paths (tests no longer assume the repo root is the cwd)
    target_compile_definitions(test_app_config PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(test_gltf_loader PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_toon_material_sidecar reads the sidecars under source/tests/fixtures/toon/, so it needs the source root
    # for the same reason test_gltf_loader does - and it asserts the MISSING-file path too, which is a path
    # that must NOT exist, so the root has to be the real one rather than the build tree.
    target_compile_definitions(test_toon_material_sidecar PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_render_resources reads source/shaders/common/heap_slots.slang and source/backends/vulkan/core/declarations/core.declarations.cppm to compare the slot grid's two
    # sources of truth (see the last block of that test): both are text, neither is generated from the other.
    target_compile_definitions(test_render_resources PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # test_shader_sources parses the FOUR places a .spv is named (VR_SLANG_SOURCES, the two compile scripts, and
    # source/app/app_config.cpp's and app_pipelines.cpp's shader registration) and compares them: one shader built by the build and not by the scripts is
    # the drift that test exists for, and it is the mesh path's CI gate (see the test's header).
    target_compile_definitions(test_shader_sources PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_deferred_push_layout
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_deferred_push_layout.py"
                "${CMAKE_CURRENT_BINARY_DIR}/shaders/deferred.frag.spv")
    add_test(NAME test_motion_converter
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_motion_converter.py")
    # test_docs parses the WRITTEN artifacts the manual and CI are built from - Doxyfile's INPUT, the
    # documents themselves, the generated target manifest and the workflow that mirrors it - so it needs
    # the source root and the manifest path, but no library at all (see the test's header for the invariants).
    target_compile_definitions(test_docs PRIVATE
            VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            VR_TARGET_MANIFEST="${CMAKE_BINARY_DIR}/target_manifest.txt")
    # deren.engine.meshlet is pure CPU alongside vulkan.math: the split's coverage/bounds/determinism checks need no
    # device, which is the only way they can run in CI (the capture gate's references are machine-specific).
    target_link_libraries(test_meshlet
            deren_engine
            vstd_lib
    )

    target_link_libraries(test_app_config
            app_config
            vstd_lib
    )
    target_link_libraries(test_asset_library
            asset_library
            vstd_lib
    )
    target_link_libraries(test_utility
            static_utility
            glm::glm
            vstd_lib
    )
    # deren.engine.math lives in deren_engine; the module itself is pure CPU (see math.cppm), the
    # link chain just rides the library's PUBLIC dependencies (Vulkan/glfw are not pulled in
    # unless the math code actually references them).
    target_link_libraries(test_math
            deren_engine
            vstd_lib
    )
    # deren.engine.profiling and deren.engine.filters (the pass-side table; user_filter needs a device and
    # is not exercised) are pure CPU, so both tests ride deren_engine for the same reason test_math does.
    target_link_libraries(test_profiling deren_engine vstd_lib)
    target_link_libraries(test_filters deren_engine promise vstd_lib)
    # deren.engine.render_resource is pure CPU on purpose (its own enums, no Vulkan type): the invariants it exists
    # for - usage is a subset of the schema, kind and access fit, a pass's own bindings are contiguous, a pool
    # count is derivable - are checkable WITHOUT a device, which is the only way they can be checked in CI.
    target_link_libraries(test_render_resources
            deren_engine
            vstd_lib
    )
    # deren.engine.pass is the framework: its base class and behaviour kinds name Vulkan handle types but never call
    # Vulkan, so its contract - resolve, apply the behaviour, record, mark, recreate - is testable with a fake
    # host and no device at all.
    target_link_libraries(test_pass
            deren_engine
            vstd_lib
    )
    # the format vocabulary is a contract fact (its numbers cross the C ABI); the engine link is
    # only there so the test can import deren.promise.rhi
    target_link_libraries(test_image_formats
            deren_engine
            vstd_lib
    )
    # the visibility vocabulary is pure CPU (packing + record layout); the engine link carries the module
    target_link_libraries(test_subtilis deren_engine vstd_lib)
    target_link_libraries(test_subtilis_builder deren_engine vstd_lib)
    target_link_libraries(test_subtilis_residency deren_engine asset_subtilis_core vstd_lib)
    target_link_libraries(test_subtilis_page_streaming deren_engine asset_subtilis_core vstd_lib)
    target_link_libraries(test_subtilis_page_gpu deren_engine backend_loader asset_subtilis_core static_utility vstd_lib)
    set_tests_properties(test_subtilis_page_gpu PROPERTIES SKIP_RETURN_CODE 77)
    target_link_libraries(test_subtilis_upload deren_engine probe_backend_static promise)
    target_link_libraries(test_subtilis_selection deren_engine probe_backend_static promise)
    target_link_libraries(test_subtilis_frame deren_engine asset_subtilis_core probe_backend_static promise)
    target_link_libraries(test_subtilis_cut_gpu deren_engine backend_loader static_utility vstd_lib)
    target_compile_definitions(test_subtilis_cut_gpu PRIVATE VR_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
    add_dependencies(test_subtilis_cut_gpu deren_vulkan)
    set_tests_properties(test_subtilis_cut_gpu PROPERTIES SKIP_RETURN_CODE 77)
    target_link_libraries(test_subtilis_asset deren_engine vstd_lib)
    target_link_libraries(test_subtilis_dag_asset deren_engine vstd_lib)
    target_link_libraries(test_subtilis_dag_lod deren_engine asset_subtilis_core yyjson_vendored)
    target_link_libraries(test_subtilis_import deren_engine asset_subtilis_core deren_assets probe_backend_static promise)
    target_compile_definitions(test_subtilis_import PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(test_subtilis_dag_lod PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_subtilis_lod deren_engine vstd_lib)
    target_link_libraries(test_visibility_buffer
            deren_engine
            vstd_lib
    )
    # deren.engine.visibility.hzb is the CPU reference for the HZB build/cull passes: pure arithmetic (the pyramid
    # layout, §6's conservative math, the phase-B gate, the compaction rule), no device and no shader compiler, which
    # is the only way the formulas can be checked in CI (a shader's version of them is checked only by a picture).
    target_link_libraries(test_hzb
            deren_engine
            vstd_lib
    )
    target_compile_definitions(test_visibility_buffer PRIVATE VR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_render_graph_visibility render_graph_visibility)
    target_link_libraries(test_visibility_raster_pass
            deren_engine
            vstd_lib
    )
    target_link_libraries(test_visibility_resolve_pass
            deren_engine
            vstd_lib
    )
    # THE TWO SOFTWARE RASTER PASSES' HEADLESS DECLARATIONS (M4 Task 2): the tile workgroup shape, the full-extent
    # rule, the one depth attachment and the pipeline gating are device-free facts - the same reason the raster and
    # resolve passes have a test apiece. They need the engine's modules (the passes, rhi, the software CPU module) and
    # no device, which is what lets them run in CI.
    target_link_libraries(test_visibility_software_pass
            deren_engine
            vstd_lib
    )
    target_link_libraries(test_visibility_merge_pass
            deren_engine
            vstd_lib
    )
    # deren.engine.visibility.software is the M4 hybrid producer's PURE CPU front half (the conservative subpixel
    # criterion, the stable [hardware prefix | software suffix] split and the 16x16 tile binning): it decides which
    # clusters the hardware raster will NOT dispatch, so a too-eager classification is a hole, not a slow frame.
    # Those properties are checkable without a device and without a shader compiler, which is the only way they can
    # run in CI - the shader side is checked by fixtures in Task 3 (the capture gate never runs in CI). The links are
    # the same pure pair test_meshlet uses; no Vulkan type is named by the module.
    target_link_libraries(test_visibility_software
            deren_engine
            vstd_lib
            glm::glm
    )
    # deren.engine.animation is format-neutral / runtime-agnostic; the sampling tests only touch the
    # pure functions, so nothing Vulkan is pulled in.
    add_library(vessel_probe SHARED source/tests/vessel_probe.cpp)
    target_link_libraries(vessel_probe PRIVATE shared_utility vstd_lib)
    set_target_properties(vessel_probe PROPERTIES PREFIX "")
    if(NOT MSVC)
        target_compile_options(vessel_probe PRIVATE -fno-sanitize=all)
        target_link_options(vessel_probe PRIVATE -fno-sanitize=all)
    endif()
    target_link_libraries(test_vessel static_utility shared_utility vstd_lib)
    target_sources(test_utility PRIVATE source/tests/vessel_child.cpp)
    target_sources(test_vessel PRIVATE source/tests/vessel_child.cpp)
    target_link_libraries(test_boost_graph boost_graph_headers render_graph_rhi)
    target_link_libraries(test_render_graph render_graph_core)
    target_link_libraries(test_render_graph_ranges render_graph_core)
    target_link_libraries(test_render_graph_parameters render_graph_core)
    target_link_libraries(test_render_graph_sync render_graph_core)
    target_link_libraries(test_render_graph_execute render_graph_execute probe_backend_static)
    target_link_libraries(test_render_graph_frame render_graph_execute probe_backend_static backend_loader)
    target_link_libraries(test_render_graph_frame deren_engine)

    target_link_libraries(test_render_graph_compose render_graph_compose probe_backend_static)
    target_link_libraries(test_render_graph_lighting render_graph_lighting render_graph_cluster render_graph_shadow render_graph_post deren_engine)
    target_link_libraries(test_render_graph_post render_graph_post deren_engine probe_backend_static)
    target_link_libraries(test_render_graph_bloom render_graph_bloom deren_engine probe_backend_static)
    target_link_libraries(test_render_graph_dump render_graph_core)
    target_link_libraries(test_render_graph_lowering render_graph_rhi vulkan_synchronization)
    add_test(NAME test_render_graph_dump_json
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/tests/test_render_graph_dump_json.py" $<TARGET_FILE:test_render_graph_dump>)
    target_sources(test_render_graph PRIVATE source/tests/vessel_child.cpp)
    target_sources(test_render_graph_parameters PRIVATE source/tests/vessel_child.cpp)
    target_sources(test_boost_graph PRIVATE source/tests/vessel_child.cpp)
    target_sources(test_stacktrace PRIVATE source/tests/vessel_child.cpp)
    target_sources(test_stacktrace PRIVATE source/tests/stacktrace_fault.cpp)
    target_link_libraries(test_stacktrace static_utility)
    add_executable(stacktrace_symbolizer_fixture source/tests/stacktrace_symbolizer_fixture.cpp)
    if(NOT MSVC)
        target_compile_options(stacktrace_symbolizer_fixture PRIVATE -fno-sanitize=all)
        target_link_options(stacktrace_symbolizer_fixture PRIVATE -fno-sanitize=all)
    endif()
    # A DLL whose DllMain BLOCKS, holding the loader lock: loaded on one thread while another
    # thread crashes, it is the instrument for "the fatal path must not call back into the loader".
    add_library(loader_lock_fixture SHARED source/tests/loader_lock_fixture.cpp)
    if(NOT MSVC)
        target_compile_options(loader_lock_fixture PRIVATE -fno-sanitize=all)
        target_link_options(loader_lock_fixture PRIVATE -fno-sanitize=all)
    endif()
    target_compile_definitions(test_stacktrace PRIVATE
        STACKTRACE_FIXTURE="$<TARGET_FILE:stacktrace_symbolizer_fixture>"
        STACKTRACE_LOADER_FIXTURE="$<TARGET_FILE:loader_lock_fixture>"
        STACKTRACE_PROBE="$<TARGET_FILE:vessel_probe>"
        STACKTRACE_DIAGNOSTICS=$<BOOL:${VR_DIAGNOSTICS}>
        STACKTRACE_HAVE_SYMBOLIZER=$<BOOL:${VR_STACKTRACE_SYMBOLIZER}>
        # The CROSS-CHECK against the offline decoder: the same raw record has to decode to the same
        # fault line in both decoders, and nothing else ties the two rules together (see the review).
        STACKTRACE_DECODER="${CMAKE_CURRENT_SOURCE_DIR}/scripts/diagnostics/symbolize_stacktrace.py"
        STACKTRACE_PYTHON="${Python3_EXECUTABLE}")
    add_dependencies(test_stacktrace stacktrace_symbolizer_fixture vessel_probe loader_lock_fixture)
    target_compile_definitions(test_vessel PRIVATE VESSEL_PROBE_FILENAME="$<TARGET_FILE_NAME:vessel_probe>")
    add_dependencies(test_vessel vessel_probe)
    target_link_libraries(test_animation
            deren_engine
            glm::glm
            vstd_lib
    )
    # deren_assets is pure CPU (fastgltf + stb vendored, linked PRIVATE into that DLL); the
    # test loads the repo's gltf_model/DamagedHelmet sample from the working directory.
    target_link_libraries(test_gaussian_splatting deren_engine)
    target_link_libraries(test_gaussian_raster deren_engine)
    target_link_libraries(test_gaussian_gpu_frame deren_engine)
    target_link_libraries(test_gaussian_loader gaussian_loader)
    target_link_libraries(test_gltf_loader
            yyjson_vendored
            deren_assets
            glm::glm
            vstd_lib
    )
    # The sidecar reader lives in the deren_assets TARGET (it is a model-loading concern) but not in the
    # gltf_loader MODULE, so this test links the same library and imports a different module.
    target_link_libraries(test_toon_material_sidecar
            deren_assets
            vstd_lib
    )
    # deren.engine.shadow_fit is pure CPU arithmetic (no Vulkan object is touched), and the module lives in
    # deren_engine next to deren.engine.math; the link chain brings Vulkan/glfw in, which the tests do not
    # use. This is the first test of the shadow system, which had none: the fit had no device
    # dependency, it just lived where nothing could reach it.
    target_link_libraries(test_shadow_fit
            deren_engine
            glm::glm
            vstd_lib
    )

    # ---- the RHI boundary's loader, and the fixture its test loads ----
    # probe_backend is a stand-in for the C ABI over source/promise/rhi/: ONE source file compiled twice, as a DLL (the
    # export macro in its "building a DLL" mode) and as a static library (macro inert). test_dynamic_link
    # therefore sees both halves of DEREN_API_EXPORT at once: it opens the DLL by path and calls the
    # static half directly, and the two must agree. The duplication of the source is the point - it is
    # the only way to exercise both modes without a second copy to drift from the first.
    #
    # THE SANITIZER JOB IS WHY -fno-sanitize=all IS HERE. That job compiles with
    # CMAKE_CXX_FLAGS=-fsanitize=address,undefined, but only CMAKE_EXE_LINKER_FLAGS carries the runtime:
    # an instrumented DLL would not link at all, and if it did it would put a SECOND ASan runtime into
    # the instrumented process, which aborts on load. The DLL is loaded BY a sanitized process and must
    # not be sanitized itself; the static half is linked into the test and IS instrumented.
    add_library(probe_backend SHARED source/tests/probe_backend.cpp)
    target_compile_definitions(probe_backend PRIVATE DEREN_API_SHARED DEREN_API_BUILD)
    # The project's compile options are NOT repeated here: the project-wide sweep near the end of this
    # file covers every first-party target, this one included. Only the sanitizer exception stays local.
    target_link_libraries(probe_backend PRIVATE promise)
    if(NOT MSVC)
        target_compile_options(probe_backend PRIVATE -fno-sanitize=all)
        target_link_options(probe_backend PRIVATE -fno-sanitize=all)
    endif()
    # Next to the test executables, which is where test_dynamic_link looks for it: the test asks argv[0]
    # for its own directory at run time, so no path is ever spelled inside a string literal (a Windows
    # separator in one is exactly the bug that only shows up on the machine that has one).
    set_target_properties(probe_backend PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    add_library(probe_backend_static STATIC source/tests/probe_backend.cpp)
    # The same promise module the DLL half reads: the two halves compile one interface unit each,
    # which is the "both sides compile the contract" rule in §4.1, not a shared binary.
    target_link_libraries(probe_backend_static PRIVATE promise)

    target_link_libraries(test_dynamic_link
            promise
            static_utility
            probe_backend_static
            vstd_lib
    )
    # THE INJECTION TEST (batch ⑥): the engine builds a renderer from a PROBE root, with no loader, no
    # DLL and no device - which is the layering's own evidence - and covers the consumption-side
    # handshake by running the panic case in a CHILD process of itself (CreateProcessW, so the exit
    # status is not translated by a shell). It links the SAME probe TU the C entry lives in, so the knob
    # (`probe_make_core` with a chosen `api_version()`) cannot drift from the entry's own behaviour.
    target_link_libraries(test_resource_cluster deren_engine probe_backend_static promise)
    target_link_libraries(test_runtime_injection
            promise
            deren_engine
            static_utility
            probe_backend_static
            vstd_lib
    )
    # The file NAME, not a path: the test composes the path from its own location (see above), so the
    # name stays correct whichever prefix/suffix the platform's shared-library convention uses.
    target_compile_definitions(test_dynamic_link PRIVATE
            VR_TEST_PROBE_BACKEND_FILE_NAME="$<TARGET_FILE_NAME:probe_backend>")
    # `cmake --build --target test_dynamic_link` has to build the DLL it opens, or ctest would run a
    # test whose fixture is missing and report a loader error as a test failure.
    add_dependencies(test_dynamic_link probe_backend)

    # The error mechanism's translation tables (source/backends/vulkan/core/core.error.cpp), fed every plausible
    # VkResult and asserted per call-site family. Pure CPU: the backend archive is linked for the
    # translators' definitions only - the test creates no device, no window, no instance - and the
    # contract module supplies the unified values (rhi::error / error_info / zone_of).
    target_link_libraries(test_error_mapping
        vulkan_error_tables
            promise
            # NO deren_vulkan: this test imports no backend module any more (the flip moved the
            # tables into `vulkan_error_tables`, linked below), so it neither links the backend
            # nor loads its DLL.
            vstd_lib
    )

    # ---- the boundary spike ---------------------------------------------------------------------
    # NOT in VR_TEST_TARGETS on purpose: it loads a DLL and (with --with-device) creates a real
    # context, so it cannot belong to the headless ctest set - and source/tests/test_docs.cpp compares that
    # list against the workflow, so putting it there would be a lie in CI. It is registered as a test
    # only in the spike tree, where the option is on and CI is not the consumer.
    add_executable(test_backend_boundary_spike source/tests/spike_backend_boundary.cpp)
    # The ABSOLUTE path of the library under test: the spike's whole Q5 is that loading by
    # absolute path uses LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS and
    # therefore never searches %PATH%.
    target_compile_definitions(test_backend_boundary_spike PRIVATE
            VR_SPIKE_BACKEND_DLL="$<TARGET_FILE:deren_vulkan>")
    target_link_libraries(test_backend_boundary_spike
            promise
            static_utility
            vstd_lib
            vulkan_main_context
    )
    # The library the test opens has to exist first, or the run reports a loader error as a test
    # failure (the same rule test_dynamic_link lives by).
    add_dependencies(test_backend_boundary_spike deren_vulkan)
    # NOT `add_test`: this spike loads a DLL and, with --with-device, builds a real context, so it
    # cannot be in the headless ctest set (source/tests/test_docs.cpp compares that set against the
    # workflow). It is RUN by the batch that touches the boundary, which is what its report is for.
endif()
