# Keep the original dispatch instructions and current hooks in the regression.
# The replay runtime differs only by a deterministic clock; no window is opened.
set(_rr64_highlight_transition_dir "${CMAKE_CURRENT_BINARY_DIR}/highlight-transition-fixture")
set(_rr64_highlight_transition_dispatch "${_rr64_highlight_transition_dir}/rr64_highlight_transition_dispatch.cpp")
set(_rr64_highlight_transition_runtime "${_rr64_highlight_transition_dir}/rr64_highlight_transition_runtime.cpp")
set(_rr64_highlight_transition_generator "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_highlight_transition_fixture.py")
add_custom_command(OUTPUT "${_rr64_highlight_transition_dispatch}" "${_rr64_highlight_transition_runtime}"
    COMMAND "${Python3_EXECUTABLE}" "${_rr64_highlight_transition_generator}"
        --recompiled "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_11.c"
        --config "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
        --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/rr64_highlights.cpp"
        --output-dir "${_rr64_highlight_transition_dir}"
    DEPENDS "${_rr64_highlight_transition_generator}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_11.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/rr64_highlights.cpp"
    VERBATIM)
add_executable(RR64HighlightTransitionSmoke EXCLUDE_FROM_ALL
    tests/rr64_highlight_transition_smoke.cpp
    "${_rr64_highlight_transition_dispatch}" "${_rr64_highlight_transition_runtime}"
    src/rr64_highlight_recording.cpp src/rr64_highlight_pose.cpp
    src/rr64_actor_pose.cpp src/rr64_highlight_camera.cpp src/rr64_highlight_render_boundary.cpp
    src/rr64_shadow_tags.cpp
    tests/rr64_highlight_camera_terrain_unavailable.cpp
    src/rr64_highlight_network.cpp src/rr64_highlight_audio.cpp)
target_include_directories(RR64HighlightTransitionSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    "${RT64_ROOT}/src"
    $<TARGET_PROPERTY:RR64NetplaySmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64HighlightTransitionSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64HighlightTransitionSmoke PRIVATE libzstd_static)
if(MSVC)
    target_compile_definitions(RR64HighlightTransitionSmoke PRIVATE NOMINMAX)
    target_link_options(RR64HighlightTransitionSmoke PRIVATE /STACK:8388608)
endif()
