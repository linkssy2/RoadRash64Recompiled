# ROM-free validation for the revision-specific guest-memory contract. This
# catches range arithmetic and live-mode classification regressions without
# launching the game.
add_executable(RR64EngineContractSmoke EXCLUDE_FROM_ALL
    tests/rr64_engine_contract_smoke.cpp
    src/rr64_actor_pose.cpp
    src/rr64_actor_presentation.cpp
    src/rr64_engine_snapshot.cpp
    src/rr64_terrain_residency.cpp
    src/rr64_terrain_snapshot.cpp
)
target_include_directories(RR64EngineContractSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# ROM-free checks for opponent ownership and offline-only freeze boundaries.
add_executable(RR64OfflineOpponentsSmoke EXCLUDE_FROM_ALL
    tests/rr64_offline_opponents_smoke.cpp
    src/rr64_offline_modifiers.cpp
)
target_include_directories(RR64OfflineOpponentsSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# ROM-free checks for the optional full 3/4-player HUD pass and placement.
add_executable(RR64QuadrantHudSmoke EXCLUDE_FROM_ALL
    tests/rr64_quadrant_hud_smoke.cpp
    src/rr64_quadrant_hud.cpp
)
target_include_directories(RR64QuadrantHudSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# ROM-free validation for the Big Game name-entry grid navigation shim.
add_executable(RR64NameEntryNavigationSmoke EXCLUDE_FROM_ALL
    tests/rr64_name_entry_navigation_smoke.cpp
    src/rr64_name_entry.cpp
)
target_include_directories(RR64NameEntryNavigationSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# ROM-free validation for the shared stock-menu edge/repeat filter. Gameplay
# steering bypasses this filter and is covered by the runtime mode gate.
include(tests/rr64_online_menu_refresh_fixture.cmake)
include(tests/rr64_online_audio_fixture.cmake)
include(tests/rr64_rival_engine_fixture.cmake)
include(tests/rr64_rival_engine_config_fixture.cmake)
include(tests/rr64_campaign_bonus_fixture.cmake)
include(tests/rr64_mk64_item_native_fixture.cmake)
include(tests/rr64_campaign_bonus_save_fixture.cmake)
include(tests/rr64_course_ai_fixture.cmake)

# Save labels must not change the base chapter subsequently used for unlocks.
set(_rr64_bonus_label_generated "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs")
set(_rr64_bonus_label_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_campaign_bonus_label_native_fixture.cpp")
add_custom_command(OUTPUT "${_rr64_bonus_label_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_campaign_bonus_label_fixture.py"
        "${_rr64_bonus_label_generated}" "${_rr64_bonus_label_fixture}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_campaign_bonus_label_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../config/roadrash64.us.toml"
        "${_rr64_bonus_label_generated}/funcs_6.c"
        "${_rr64_bonus_label_generated}/funcs_7.c"
        "${_rr64_bonus_label_generated}/funcs_15.c"
    VERBATIM)
add_executable(RR64CampaignBonusLabelSmoke EXCLUDE_FROM_ALL
    tests/rr64_campaign_bonus_label_smoke.cpp src/rr64_campaign_bonus_save.cpp
    "${_rr64_bonus_label_fixture}")
target_compile_features(RR64CampaignBonusLabelSmoke PRIVATE cxx_std_20)
target_include_directories(RR64CampaignBonusLabelSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include")
add_executable(RR64MenuNavigationSmoke EXCLUDE_FROM_ALL
    tests/rr64_menu_navigation_smoke.cpp
    src/rr64_menu_navigation.cpp
)
target_include_directories(RR64MenuNavigationSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ROM-free validation for exact Big Game promotion/completion achievement
# boundaries. The runtime hooks themselves are emitted by N64Recomp.
add_executable(RR64AchievementTriggerSmoke EXCLUDE_FROM_ALL
    tests/rr64_achievement_trigger_smoke.cpp
)
target_include_directories(RR64AchievementTriggerSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ROM-free procedural sting validation. This guarantees the achievement sound
# is audible, bounded, and self-terminating without opening an audio device.
add_executable(RR64AchievementAudioSmoke EXCLUDE_FROM_ALL
    tests/rr64_achievement_audio_smoke.cpp
    src/rr64_achievement_audio.cpp
)
target_include_directories(RR64AchievementAudioSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Test-only fourteen-peer capacity and canonical rider-snapshot validation.
# This does not create a renderer or require a ROM.
add_executable(RR64NetplayCapacity EXCLUDE_FROM_ALL
    tests/rr64_netplay_capacity.cpp
    ${RR64_NETPLAY_TEST_SOURCES}
)
target_link_libraries(RR64NetplayCapacity PRIVATE libzstd_static)
target_include_directories(RR64NetplayCapacity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)
if(WIN32)
    target_link_libraries(RR64NetplayCapacity PRIVATE Ws2_32.lib)
endif()

# ROM-free validation for the proximity attenuation curve. Codec, microphone,
# and network transport are exercised by the normal build and netplay smoke.
add_executable(RR64VoiceChatSmoke EXCLUDE_FROM_ALL
    tests/rr64_voice_chat_smoke.cpp
)
target_include_directories(RR64VoiceChatSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ROM-free policy validation for peer-local online rendering. Online races use
# one full-screen camera per machine while original local multiplayer retains
# the stock split-screen layout.
add_executable(RR64OnlineViewportSmoke EXCLUDE_FROM_ALL
    tests/rr64_online_viewport_smoke.cpp
)
target_include_directories(RR64OnlineViewportSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(RR64OnlineViewportSmoke PRIVATE cxx_std_20)

if(WIN32)
    add_executable(RR64MusicSmoke EXCLUDE_FROM_ALL tests/rr64_music_smoke.cpp)
else()
    add_executable(RR64MusicSmoke EXCLUDE_FROM_ALL tests/rr64_music_linux_smoke.cpp)
endif()
add_executable(RR64ConnectionSmoke EXCLUDE_FROM_ALL tests/rr64_connection_smoke.cpp)
target_compile_features(RR64ConnectionSmoke PRIVATE cxx_std_20)
if(WIN32)
    target_link_libraries(RR64ConnectionSmoke PRIVATE Ws2_32.lib)
endif()
target_include_directories(RR64MusicSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(RR64MusicSmoke PRIVATE ${RR64_SDL2_TARGET})
target_include_directories(RR64MusicSmoke PRIVATE "${RR64_SDL2_INCLUDE_DIRS}")
target_include_directories(RR64MusicSmoke PRIVATE "${N64MODERN_RUNTIME_ROOT}/librecomp/include")
target_include_directories(RR64MusicSmoke PRIVATE "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include")
target_link_directories(RR64MusicSmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
if(WIN32)
    target_link_libraries(RR64MusicSmoke PRIVATE mfplat mfreadwrite mfuuid ole32)
else()

    add_dependencies(RR64MusicSmoke rr64_ffmpeg)
    target_include_directories(RR64MusicSmoke PRIVATE "${RR64_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(RR64MusicSmoke PRIVATE ${RR64_FFMPEG_LIBS} z pthread)
endif()
add_executable(RR64NetplayLimitSmoke EXCLUDE_FROM_ALL
    tests/rr64_netplay_limit_smoke.cpp ${RR64_NETPLAY_TEST_SOURCES})
target_link_libraries(RR64NetplayLimitSmoke PRIVATE libzstd_static)
target_include_directories(RR64NetplayLimitSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(RR64NetplayLimitSmoke PRIVATE Ws2_32.lib)
endif()

add_executable(RR64SettingsSmoke EXCLUDE_FROM_ALL tests/rr64_settings_smoke.cpp)
target_compile_features(RR64SettingsSmoke PRIVATE cxx_std_20)

add_executable(RR64CharacterPreferencesSmoke EXCLUDE_FROM_ALL
    tests/rr64_character_preferences_smoke.cpp src/rr64_character_preferences.cpp)
target_include_directories(RR64CharacterPreferencesSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(RR64CharacterPreferencesSmoke PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
add_executable(RR64SyncLogSmoke EXCLUDE_FROM_ALL tests/rr64_sync_log_smoke.cpp)
target_include_directories(RR64SyncLogSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(RR64SyncLogSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64SyncLogSmoke PRIVATE Threads::Threads)
target_link_libraries(RR64CharacterPreferencesSmoke PRIVATE Threads::Threads)

add_executable(RR64CharacterMenuSmoke EXCLUDE_FROM_ALL
    tests/rr64_character_menu_smoke.cpp src/rr64_character_menu.cpp src/rr64_character_preferences.cpp)
target_include_directories(RR64CharacterMenuSmoke PRIVATE $<TARGET_PROPERTY:RR64WeaponRenderSmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64CharacterMenuSmoke PRIVATE cxx_std_20)

add_executable(RR64RaceEndTraceSmoke EXCLUDE_FROM_ALL
    tests/rr64_race_end_trace_smoke.cpp src/rr64_race_end_trace.cpp)
target_include_directories(RR64RaceEndTraceSmoke PRIVATE $<TARGET_PROPERTY:RR64WeaponRenderSmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64RaceEndTraceSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64RaceEndTraceSmoke PRIVATE Threads::Threads)

add_executable(RR64RacerViewSmoke EXCLUDE_FROM_ALL tests/rr64_racer_view_smoke.cpp)
target_compile_features(RR64RacerViewSmoke PRIVATE cxx_std_20)

add_executable(RR64PopupInputSmoke EXCLUDE_FROM_ALL tests/rr64_popup_input_smoke.cpp)
target_compile_features(RR64PopupInputSmoke PRIVATE cxx_std_20)

add_executable(RR64ActionBindingsSmoke EXCLUDE_FROM_ALL tests/rr64_action_bindings_smoke.cpp)
target_compile_features(RR64ActionBindingsSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64ActionBindingsSmoke PRIVATE recompinput)
target_include_directories(RR64ActionBindingsSmoke PRIVATE $<TARGET_PROPERTY:recompinput,INCLUDE_DIRECTORIES>)

add_executable(RR64WeaponRenderSmoke EXCLUDE_FROM_ALL tests/rr64_weapon_render_smoke.cpp src/rr64_weapon_render.cpp)
add_executable(RR64SkySpritesSmoke EXCLUDE_FROM_ALL tests/rr64_sky_sprites_smoke.cpp src/rr64_sky_sprites.cpp)
add_executable(RR64HUDWidgetsSmoke EXCLUDE_FROM_ALL tests/rr64_hud_widgets_smoke.cpp src/rr64_hud_widgets.cpp)
add_executable(RR64RoamingRouteSmoke EXCLUDE_FROM_ALL tests/rr64_roaming_route_smoke.cpp
    src/rr64_roaming_route.cpp src/rr64_experimental_course_route.cpp)
target_include_directories(RR64RoamingRouteSmoke PRIVATE $<TARGET_PROPERTY:RR64WeaponRenderSmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64RoamingRouteSmoke PRIVATE cxx_std_20)
# Exercise the actual generated progress accumulator, not a second translation
# of its arithmetic. Only curve-length lookup is supplied by the straight-road
# fixture; native progress/segment functions are extracted at configure.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_16.c" RR64_ROUTE_NATIVE)
set(RR64_ROUTE_FIXTURE "#include \"funcs.h\"\n")
foreach(RR64_ROUTE_FUNC 80066D70 80066DA0 8006736C 800674C4 80068D0C 80068C70)
    string(FIND "${RR64_ROUTE_NATIVE}" "RECOMP_FUNC void func_${RR64_ROUTE_FUNC}(" RR64_ROUTE_START)
    if(RR64_ROUTE_START LESS 0)
        message(FATAL_ERROR "Missing native roaming test function ${RR64_ROUTE_FUNC}; regenerate source")
    endif()
    string(SUBSTRING "${RR64_ROUTE_NATIVE}" ${RR64_ROUTE_START} -1 RR64_ROUTE_TAIL)
    string(SUBSTRING "${RR64_ROUTE_TAIL}" 1 -1 RR64_ROUTE_REST)
    string(FIND "${RR64_ROUTE_REST}" "RECOMP_FUNC" RR64_ROUTE_END)
    math(EXPR RR64_ROUTE_END "${RR64_ROUTE_END}+1")
    string(SUBSTRING "${RR64_ROUTE_TAIL}" 0 ${RR64_ROUTE_END} RR64_ROUTE_BODY)
    string(APPEND RR64_ROUTE_FIXTURE "${RR64_ROUTE_BODY}\n")
endforeach()
# Native lap evaluation clamps the curve parameter before comparing progress
# with the finish threshold. Keep that stock scalar helper in the fixture too.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_4.c" RR64_ROUTE_PLANE_NATIVE)
string(FIND "${RR64_ROUTE_PLANE_NATIVE}" "RECOMP_FUNC void func_8001A250(" RR64_ROUTE_PLANE_START)
if(RR64_ROUTE_PLANE_START LESS 0)
    message(FATAL_ERROR "Missing native parameter-clamp test function 8001A250; regenerate source")
endif()
string(SUBSTRING "${RR64_ROUTE_PLANE_NATIVE}" ${RR64_ROUTE_PLANE_START} -1 RR64_ROUTE_PLANE_TAIL)
string(SUBSTRING "${RR64_ROUTE_PLANE_TAIL}" 1 -1 RR64_ROUTE_PLANE_REST)
string(FIND "${RR64_ROUTE_PLANE_REST}" "RECOMP_FUNC" RR64_ROUTE_PLANE_END)
math(EXPR RR64_ROUTE_PLANE_END "${RR64_ROUTE_PLANE_END}+1")
string(SUBSTRING "${RR64_ROUTE_PLANE_TAIL}" 0 ${RR64_ROUTE_PLANE_END} RR64_ROUTE_PLANE_BODY)
string(APPEND RR64_ROUTE_FIXTURE "${RR64_ROUTE_PLANE_BODY}\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rr64_roaming_native_fixture.cpp" CONTENT "${RR64_ROUTE_FIXTURE}" @ONLY)
target_sources(RR64RoamingRouteSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/rr64_roaming_native_fixture.cpp")
target_include_directories(RR64RoamingRouteSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs")
target_include_directories(RR64HUDWidgetsSmoke PRIVATE $<TARGET_PROPERTY:RR64WeaponRenderSmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64HUDWidgetsSmoke PRIVATE cxx_std_20)
target_include_directories(RR64SkySpritesSmoke PRIVATE $<TARGET_PROPERTY:RR64WeaponRenderSmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64SkySpritesSmoke PRIVATE cxx_std_20)
add_executable(RR64MenuEjectSmoke EXCLUDE_FROM_ALL tests/rr64_menu_eject_smoke.cpp)
add_executable(RR64AutotestControlSmoke EXCLUDE_FROM_ALL tests/rr64_autotest_control_smoke.cpp)
target_include_directories(RR64AutotestControlSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(RR64AutotestControlSmoke PRIVATE cxx_std_20)
# Developer-only traffic recorder; no production hooks or recorder thread.
add_executable(RR64TrafficTraceSmoke EXCLUDE_FROM_ALL tests/rr64_traffic_trace_smoke.cpp src/rr64_traffic_trace.cpp)
target_include_directories(RR64TrafficTraceSmoke PRIVATE $<TARGET_PROPERTY:RR64MenuEjectSmoke,INCLUDE_DIRECTORIES>)
target_include_directories(RR64MenuEjectSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include" "${N64MODERN_RUNTIME_ROOT}/ultramodern/include" "${N64MODERN_RUNTIME_ROOT}/librecomp/include" "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon")
target_include_directories(RR64WeaponRenderSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include" "${N64MODERN_RUNTIME_ROOT}/ultramodern/include" "${N64MODERN_RUNTIME_ROOT}/librecomp/include" "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon")

if(NOT WIN32)
    # Release player builds keep optimization, but offline assertions must run.
    foreach(RR64_CHECK RR64MenuEjectSmoke RR64WeaponRenderSmoke RR64ActionBindingsSmoke RR64MusicSmoke RR64TrafficTraceSmoke)
        target_compile_options(${RR64_CHECK} PRIVATE -UNDEBUG)
    endforeach()
endif()
include(tests/rr64_mk64_item_material_call_fixture.cmake)
