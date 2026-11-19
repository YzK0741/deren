# Keep native adapter modules under the root compiler policy, like the other app support targets.
include_guard(GLOBAL)
add_library(deren_scripting_audio STATIC)
target_sources(deren_scripting_audio PUBLIC FILE_SET CXX_MODULES FILES source/scripting/audio_host.cppm)
target_link_libraries(deren_scripting_audio PUBLIC vstd_lib static_utility deren_script_api PRIVATE deren_audio)
