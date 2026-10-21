cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d" CACHE STRING "CMake import std gate")
project(deren-subtilis VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_definitions(__SANITIZER_DISABLE_CONTAINER_OVERFLOW__)
endif()
include(../../cmake/deren_cpu_utility.cmake)
include(../../cmake/deren_asset_dependencies.cmake)

add_library(subtilis_codec STATIC)
target_sources(subtilis_codec PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT} FILES
    ${DEREN_REPO_ROOT}/source/asset/subtilis/types.cppm
    ${DEREN_REPO_ROOT}/source/asset/subtilis/codec.cppm
    PRIVATE ${DEREN_REPO_ROOT}/source/asset/subtilis/codec.cpp
    ${DEREN_REPO_ROOT}/source/asset/subtilis/digest.cpp)
target_link_libraries(subtilis_codec PUBLIC shared_utility vstd_lib PRIVATE yyjson_vendored)

enable_testing()
add_library(subtilis_dag STATIC)
target_sources(subtilis_dag PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${DEREN_REPO_ROOT} FILES
    ${DEREN_REPO_ROOT}/source/asset/subtilis/dag.cppm
    PRIVATE ${DEREN_REPO_ROOT}/source/asset/subtilis/dag.cpp
    ${DEREN_REPO_ROOT}/source/asset/subtilis/cut.cpp)
target_link_libraries(subtilis_dag PUBLIC subtilis_codec)
add_executable(test_subtilis_dag ${DEREN_REPO_ROOT}/source/tests/test_subtilis_dag.cpp)
target_link_libraries(test_subtilis_dag PRIVATE subtilis_dag)
add_test(NAME test_subtilis_dag COMMAND test_subtilis_dag)
add_library(subtilis_document STATIC document.cpp accessor.cpp)
target_link_libraries(subtilis_document PUBLIC subtilis_dag PRIVATE fastgltf_vendored yyjson_vendored)
target_include_directories(subtilis_document SYSTEM PRIVATE ${DEREN_REPO_ROOT}/source/third_party/fastgltf/include)
add_executable(test_subtilis_document ${DEREN_REPO_ROOT}/source/tests/test_subtilis_document.cpp)
target_link_libraries(test_subtilis_document PRIVATE subtilis_document yyjson_vendored)
add_test(NAME test_subtilis_document COMMAND test_subtilis_document)
add_executable(test_subtilis_codec ${DEREN_REPO_ROOT}/source/tests/test_subtilis_codec.cpp)
target_link_libraries(test_subtilis_codec PRIVATE subtilis_codec)
add_test(NAME test_subtilis_codec COMMAND test_subtilis_codec)
# Use one canonical option list across public module providers and consumers.
foreach(target vstd_lib shared_utility subtilis_codec test_subtilis_codec subtilis_dag test_subtilis_dag subtilis_document test_subtilis_document)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /bigobj /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-reserved-module-identifier -fno-exceptions -fno-rtti)
    endif()
endforeach()
if(MSVC)
    set_source_files_properties(${DEREN_REPO_ROOT}/source/vstd/vstd_msvc.cppm PROPERTIES COMPILE_OPTIONS /wd4996)
endif()
