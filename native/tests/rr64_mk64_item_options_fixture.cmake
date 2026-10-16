set(RR64_ITEM_OPTIONS_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/mk64_item_options_native.c")
add_custom_command(OUTPUT "${RR64_ITEM_OPTIONS_NATIVE}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_mk64_item_options_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs" "${RR64_ITEM_OPTIONS_NATIVE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_mk64_item_options_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_7.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_19.c"
    VERBATIM)
add_executable(RR64Mk64ItemOptionsSmoke EXCLUDE_FROM_ALL
    tests/rr64_mk64_item_options_smoke.cpp "${RR64_ITEM_OPTIONS_NATIVE}"
    src/rr64_campaign_coop.cpp src/rr64_local_race_options.cpp src/rr64_thrash_options.cpp src/rr64_race_pack_menu.cpp)
target_compile_features(RR64Mk64ItemOptionsSmoke PRIVATE cxx_std_20)
target_compile_definitions(RR64Mk64ItemOptionsSmoke PRIVATE RR64_EXPERIMENTAL_COURSE=1)
target_include_directories(RR64Mk64ItemOptionsSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty")
