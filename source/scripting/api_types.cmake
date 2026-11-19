include_guard(GLOBAL)
add_library(deren_script_api STATIC)
target_sources(deren_script_api PUBLIC FILE_SET CXX_MODULES FILES
    source/scripting/api_types.cppm source/scripting/audio_api.cppm source/scripting/rml_ui_api.cppm)
target_link_libraries(deren_script_api PUBLIC vstd_lib)
