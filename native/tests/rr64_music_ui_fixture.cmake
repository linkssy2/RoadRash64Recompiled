# Actual config control methods with headless widget services, not a game/window.
set(_rr64_music_ui_source "${RECOMP_FRONTEND_ROOT}/recompui/src/config/ui_config_option.cpp")
set(_rr64_music_ui_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_music_ui_fixture.inc")
add_custom_command(OUTPUT "${_rr64_music_ui_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_music_ui_fixture.py"
        "${_rr64_music_ui_source}" "${_rr64_music_ui_fixture}"
    DEPENDS "${_rr64_music_ui_source}" "${RECOMP_FRONTEND_ROOT}/recompui/src/config/ui_config_option.h"
        scripts/extract_music_ui_fixture.py scripts/extract_controller_ui_fixture.py
    VERBATIM)
add_executable(RR64MusicUiSmoke EXCLUDE_FROM_ALL tests/rr64_music_ui_smoke.cpp "${_rr64_music_ui_fixture}")
target_compile_features(RR64MusicUiSmoke PRIVATE cxx_std_20)
target_include_directories(RR64MusicUiSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Exercise native popup layout and scrolling rather than substituting widgets.
add_executable(RR64MusicDropdownSmoke EXCLUDE_FROM_ALL tests/rr64_music_dropdown_smoke.cpp)
target_compile_features(RR64MusicDropdownSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64MusicDropdownSmoke PRIVATE RmlUi::Core)
