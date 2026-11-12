set(_rr64_item_hud_generated "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs")
set(_rr64_item_hud_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_mk64_item_hud_fixture.c")
file(GLOB _rr64_item_hud_functions "${_rr64_item_hud_generated}/funcs_*.c")
add_custom_command(OUTPUT "${_rr64_item_hud_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_mk64_item_hud_fixture.py"
        "${_rr64_item_hud_generated}" "${_rr64_item_hud_fixture}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_mk64_item_hud_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml" ${_rr64_item_hud_functions}
    VERBATIM)
add_executable(RR64Mk64ItemHudSmoke EXCLUDE_FROM_ALL tests/rr64_mk64_item_hud_smoke.cpp
    src/rr64_mk64_item_hud.cpp "${_rr64_item_hud_fixture}")
target_include_directories(RR64Mk64ItemHudSmoke PRIVATE src
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include")
target_compile_features(RR64Mk64ItemHudSmoke PRIVATE cxx_std_20 c_std_17)
target_link_libraries(RR64Mk64ItemHudSmoke PRIVATE Threads::Threads)
target_compile_definitions(RR64Mk64ItemHudSmoke PRIVATE RR64_EXPERIMENTAL_COURSE=1)
if(MSVC)
    target_compile_options(RR64Mk64ItemHudSmoke PRIVATE /fp:strict)
else()
    target_compile_options(RR64Mk64ItemHudSmoke PRIVATE -fno-fast-math -ffp-contract=off)
endif()
