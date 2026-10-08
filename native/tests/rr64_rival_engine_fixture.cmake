# Real native initialization, producers and mixer logic; device/thread startup
# and sample-bank relocation are excluded from this isolated metadata fixture.
set(_rr64_rival_generated "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs")
set(_rr64_rival_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_rival_engine_native_fixture.cpp")
add_custom_command(OUTPUT "${_rr64_rival_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_rival_engine_fixture.py"
        "${_rr64_rival_generated}" "${_rr64_rival_fixture}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_rival_engine_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
        "${_rr64_rival_generated}/funcs_2.c" "${_rr64_rival_generated}/funcs_4.c"
        "${_rr64_rival_generated}/funcs_6.c" "${_rr64_rival_generated}/funcs_11.c"
        "${_rr64_rival_generated}/funcs_12.c" "${_rr64_rival_generated}/funcs_13.c"
        "${_rr64_rival_generated}/funcs_14.c" "${_rr64_rival_generated}/funcs_20.c"
        "${_rr64_rival_generated}/funcs_21.c" "${_rr64_rival_generated}/funcs_22.c"
        "${_rr64_rival_generated}/funcs_23.c" "${_rr64_rival_generated}/funcs_25.c"
    VERBATIM)
add_executable(RR64RivalEngineSmoke EXCLUDE_FROM_ALL tests/rr64_rival_engine_smoke.cpp
    tests/rr64_rival_engine_init_cases.hpp
    src/rr64_rival_engine.cpp src/rr64_online_audio.cpp "${_rr64_rival_fixture}")
target_compile_features(RR64RivalEngineSmoke PRIVATE cxx_std_20)
target_compile_definitions(RR64RivalEngineSmoke PRIVATE RR64_RIVAL_ENGINE_TEST_TRACE=1)
target_include_directories(RR64RivalEngineSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include")
