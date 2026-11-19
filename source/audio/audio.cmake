# Audio loading/playback is auto-imported by the application; codecs stay inside the DLL.
include_guard(GLOBAL)
add_library(miniaudio_vendored STATIC source/third_party/miniaudio/miniaudio.c)
target_compile_definitions(miniaudio_vendored PUBLIC MA_NO_ENCODING MA_NO_GENERATION)
target_include_directories(miniaudio_vendored SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/miniaudio")
add_library(deren_audio SHARED)
# Match deren_assets: same-toolchain C++ module exports, with an import library for consumers.
set_target_properties(deren_audio PROPERTIES PREFIX "" WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_sources(deren_audio PUBLIC FILE_SET CXX_MODULES FILES
    source/audio/audio.cppm source/audio/declarations.cppm
    source/audio/audio_load.cppm source/audio/miniaudio_load.cppm)
target_link_libraries(deren_audio PUBLIC vstd_lib PRIVATE miniaudio_vendored)

# Keep upstream options scoped: no shared libraries, upstream tests or HTTP support.
function(deren_audio_codecs)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    set(OPUS_PACKAGE_VERSION 1.5.2)
    # Older upstream cmake_minimum_required must still honor the scoped BUILD_TESTING=OFF.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(source/third_party/ogg-1.3.5 "${CMAKE_BINARY_DIR}/audio-codecs/ogg" EXCLUDE_FROM_ALL SYSTEM)
    add_subdirectory(source/third_party/opus-1.5.2 "${CMAKE_BINARY_DIR}/audio-codecs/opus" EXCLUDE_FROM_ALL SYSTEM)
endfunction()
deren_audio_codecs()
add_library(opusfile_vendored STATIC
    source/third_party/opusfile-0.12/src/info.c source/third_party/opusfile-0.12/src/internal.c
    source/third_party/opusfile-0.12/src/opusfile.c source/third_party/opusfile-0.12/src/stream.c)
target_compile_definitions(opusfile_vendored PRIVATE OP_DISABLE_HTTP)
target_include_directories(opusfile_vendored SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/opusfile-0.12/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/opus-1.5.2/include")
target_link_libraries(opusfile_vendored PUBLIC opus ogg)
if(UNIX)
    target_link_libraries(opusfile_vendored PRIVATE m)
endif()
add_library(vorbis_vendored STATIC source/third_party/stb/stb_vorbis.c)
target_include_directories(vorbis_vendored SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/stb")
add_library(qoa_vendored STATIC source/third_party/qoa/qoa.c)
target_include_directories(qoa_vendored SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/qoa")
target_compile_definitions(qoa_vendored PUBLIC QOA_NO_STDIO)
target_sources(deren_audio PUBLIC FILE_SET CXX_MODULES FILES
    source/audio/decode_helpers.cppm source/audio/vorbis_load.cppm
    source/audio/opus_load.cppm source/audio/qoa_load.cppm)
target_link_libraries(deren_audio PUBLIC static_utility PRIVATE vorbis_vendored opusfile_vendored qoa_vendored)

target_sources(deren_audio PUBLIC FILE_SET CXX_MODULES FILES
    source/audio/engine.cppm source/audio/playback.cppm)
if(WIN32)
    target_link_libraries(miniaudio_vendored PUBLIC ole32 winmm uuid)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio_vendored PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# These archives feed the shared library on POSIX too. Consumers never link a second codec copy.
set_target_properties(miniaudio_vendored vorbis_vendored qoa_vendored opusfile_vendored opus ogg
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
