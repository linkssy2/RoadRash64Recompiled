set(_rr64_startup_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_startup_recovery_fixture.inc")
add_custom_command(OUTPUT "${_rr64_startup_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_startup_recovery_fixture.py"
        "${N64MODERN_RUNTIME_ROOT}/librecomp/src/recomp.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/rr64_experimental_course.cpp"
        "${_rr64_startup_fixture}"
    DEPENDS scripts/extract_startup_recovery_fixture.py src/rr64_experimental_course.cpp
        "${N64MODERN_RUNTIME_ROOT}/librecomp/src/recomp.cpp"
    VERBATIM)
add_executable(RR64StartupRecoverySmoke EXCLUDE_FROM_ALL
    tests/rr64_startup_recovery_smoke.cpp src/rr64_race_pack_mod.cpp
    "${_rr64_startup_fixture}")
target_include_directories(RR64StartupRecoverySmoke PRIVATE
    src "${CMAKE_CURRENT_BINARY_DIR}" "${N64MODERN_RUNTIME_ROOT}/thirdparty"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon")
target_compile_features(RR64StartupRecoverySmoke PRIVATE cxx_std_20)
target_compile_definitions(RR64StartupRecoverySmoke PRIVATE RR64_EXPERIMENTAL_COURSE)
