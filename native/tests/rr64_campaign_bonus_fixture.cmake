# Original campaign consumers; fixture reads private ROM metadata at execution.
set(_rr64_bonus_generated "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs")
set(_rr64_bonus_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_campaign_bonus_native_fixture.cpp")
add_custom_command(OUTPUT "${_rr64_bonus_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_campaign_bonus_fixture.py"
        "${_rr64_bonus_generated}" "${_rr64_bonus_fixture}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_campaign_bonus_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/rr64_local_race_options.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
        "${_rr64_bonus_generated}/funcs_4.c" "${_rr64_bonus_generated}/funcs_5.c"
        "${_rr64_bonus_generated}/funcs_6.c"
        "${_rr64_bonus_generated}/funcs_7.c" "${_rr64_bonus_generated}/funcs_10.c"
        "${_rr64_bonus_generated}/funcs_11.c" "${_rr64_bonus_generated}/funcs_15.c"
        "${_rr64_bonus_generated}/funcs_18.c"
    VERBATIM)
add_executable(RR64CampaignBonusSmoke EXCLUDE_FROM_ALL tests/rr64_campaign_bonus_smoke.cpp
    src/rr64_campaign_coop.cpp src/rr64_campaign_completion.cpp src/rr64_campaign_bonus_save.cpp
    src/rr64_ai_bike_selection.cpp "${_rr64_bonus_fixture}")
target_compile_features(RR64CampaignBonusSmoke PRIVATE cxx_std_20)
target_include_directories(RR64CampaignBonusSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include")
