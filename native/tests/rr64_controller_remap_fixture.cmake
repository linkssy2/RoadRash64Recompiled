# Exercise editor routing without opening a window. Production methods are
# extracted verbatim; the fixture substitutes rendering/services only.
set(_rr64_controls_source "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompui/src/config/ui_config_page_controls.cpp")
set(_rr64_controls_fixture "${CMAKE_CURRENT_BINARY_DIR}/ui-profile-production.inc")
add_custom_command(OUTPUT "${_rr64_controls_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_controller_ui_fixture.py"
        "${_rr64_controls_source}" "${_rr64_controls_fixture}"
    DEPENDS "${_rr64_controls_source}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_controller_ui_fixture.py"
    VERBATIM)
add_executable(RR64ControllerUiSmoke EXCLUDE_FROM_ALL
    tests/rr64_controller_ui_smoke.cpp "${_rr64_controls_fixture}")
target_compile_features(RR64ControllerUiSmoke PRIVATE cxx_std_20)
target_include_directories(RR64ControllerUiSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Real profile serialization, binding scanner and SDL polling. Only UI/window
# services and atomic backup-file transport are substituted by the fixture.
set(_rr64_input_root "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompinput")
set(_rr64_remap_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_controller_remap_fixture.inc")
add_custom_command(OUTPUT "${_rr64_remap_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_controller_remap_fixture.py"
        --frontend-root "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend" --output "${_rr64_remap_fixture}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_controller_remap_fixture.py"
        "${_rr64_input_root}/src/players.cpp" "${_rr64_input_root}/src/input_events.cpp"
    VERBATIM)
add_executable(RR64ControllerRemapSmoke EXCLUDE_FROM_ALL
    tests/rr64_controller_remap_smoke.cpp "${_rr64_remap_fixture}"
    "${_rr64_input_root}/src/profiles.cpp" "${_rr64_input_root}/src/input_mapping.cpp"
    "${_rr64_input_root}/src/input_types.cpp" "${_rr64_input_root}/src/input_state.cpp"
    "${_rr64_input_root}/src/input_binding.cpp")
target_compile_features(RR64ControllerRemapSmoke PRIVATE cxx_std_20)
target_include_directories(RR64ControllerRemapSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${_rr64_input_root}/include/recompinput"
    $<TARGET_PROPERTY:recompinput,INCLUDE_DIRECTORIES> "${RR64_SDL_INCLUDE_DIRS}")
target_link_libraries(RR64ControllerRemapSmoke PRIVATE ${RR64_SDL_TARGET})
# InputState also contains host services the fixture never calls.
if(MSVC)
    target_compile_definitions(RR64ControllerRemapSmoke PRIVATE NOMINMAX)
    target_compile_options(RR64ControllerRemapSmoke PRIVATE /Gy /Gw)
    target_link_options(RR64ControllerRemapSmoke PRIVATE /OPT:REF)
else()
    target_compile_options(RR64ControllerRemapSmoke PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(RR64ControllerRemapSmoke PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(RR64ControllerRemapSmoke PRIVATE -Wl,--gc-sections)
    endif()
endif()
