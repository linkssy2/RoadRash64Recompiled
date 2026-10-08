add_executable(RoadRash64Recompiled
    "${RR64_REPLAY_FRAME_SOURCE}"
    "${RR64_REPLAY_RESOURCE_SOURCE}"
    src/main.cpp
    src/rr64_weapon_render.cpp
    src/register_overlays.cpp
    src/rr64_achievement_audio.cpp
    src/rr64_music.cpp
    src/rr64_achievements.cpp
    src/rr64_actor_pose.cpp
    src/rr64_actor_presentation.cpp
    src/rr64_actor_render_snapshot.cpp
    src/rr64_actor_render_runtime.cpp
    src/rr64_actor_held_pose.cpp
    src/rr64_shadow_tags.cpp
    src/rr64_world_render.cpp
    src/rr64_world_camera.cpp
    src/rr64_video_mode.cpp
    src/rr64_custom_cop.cpp
    src/rr64_custom_cop_ui.cpp
    src/rr64_custom_cop_runtime.cpp
    src/rr64_local_race_options.cpp
    src/rr64_ai_bike_selection.cpp
    src/rr64_thrash_options.cpp
    src/rr64_campaign_completion.cpp
    src/rr64_campaign_bonus_save.cpp
    src/rr64_character_preferences.cpp
    src/rr64_character_menu.cpp
    src/rr64_rider_skins.cpp
    src/rr64_rider_skin_menu.cpp
    src/rr64_rider_skin_mod_ui.cpp
    src/rr64_rider_skin_preferences.cpp
    src/rr64_rider_skin_render.cpp
    src/rr64_race_end_trace.cpp
    src/rr64_world_terrain_assets.cpp
    src/rr64_world_terrain.cpp
    src/rr64_world_object_assets.cpp
    src/rr64_world_objects.cpp
    src/rr64_audio_output.cpp
    src/rr64_engine_contract.cpp
    src/rr64_engine_snapshot.cpp
    src/rr64_input_prompts.cpp
    src/rr64_menu_navigation.cpp
    src/rr64_name_entry.cpp
    src/rr64_profile_names.cpp
    src/rr64_terrain_residency.cpp
    src/rr64_terrain_graphics_pool.cpp
    src/rr64_terrain_snapshot.cpp
    src/rr64_netplay.cpp
    src/rr64_online_menu.cpp
    src/rr64_online_race_sync.cpp
    src/rr64_online_audio.cpp
    src/rr64_rival_engine.cpp
    src/rr64_rival_engine_config.cpp
    src/rr64_online_terrain.cpp
    src/rr64_highlights.cpp
    src/rr64_highlight_audio.cpp
    src/rr64_offline_modifiers.cpp
    src/rr64_offline_modifiers_ui.cpp
    src/rr64_offline_modifiers_health.cpp
    src/rr64_offline_modifiers_weapons.cpp
    src/rr64_offline_modifiers_bikes.cpp
    src/rr64_race_pack_mod.cpp
    src/rr64_race_pack_mod_ui.cpp
    src/rr64_mk64_import.cpp
    src/rr64_import_process.cpp
    src/rr64_import_install.cpp
    src/rr64_highlight_recording.cpp
    src/rr64_highlight_pose.cpp
    src/rr64_highlight_weapon.cpp
    src/rr64_highlight_traffic.cpp
    src/rr64_highlight_traffic_assets.cpp
    src/rr64_highlight_camera.cpp
    src/rr64_highlight_camera_terrain.cpp
    src/rr64_highlight_render_boundary.cpp
    src/rr64_highlight_network.cpp
    src/rr64_online_traffic.cpp
    src/rr64_online_combat.cpp
    src/rr64_authoritative_native.cpp
    src/rr64_authoritative_step.cpp
    src/rr64_prediction_capture.cpp
    src/rr64_prediction_reconcile.cpp
    src/rr64_prediction_reconcile_entry.cpp
    src/rr64_prediction_verify.cpp
    src/rr64_online_guest_flow.cpp
    src/rr64_online_ready.cpp
    src/rr64_runtime_shims.cpp
    src/rr64_sky_sprites.cpp
    src/rr64_hud_widgets.cpp
    src/rr64_roaming_route.cpp
    src/rr64_voice_chat.cpp
    src/rr64_race_pack_menu.cpp
)

if(RR64_EXPERIMENTAL_COURSE)
    target_sources(RoadRash64Recompiled PRIVATE
        src/rr64_experimental_course.cpp src/rr64_experimental_course_route.cpp
        src/rr64_experimental_course_policy.cpp src/rr64_course_material.cpp
        src/rr64_course_items.cpp src/rr64_course_item_render.cpp src/rr64_course_sky.cpp
        src/rr64_mk64_items.cpp src/rr64_mk64_item_kernel.cpp
        src/rr64_mk64_item_render.cpp src/rr64_mk64_item_hud.cpp src/rr64_mk64_item_audio.cpp
        src/rr64_mk64_item_assets.cpp src/rr64_mk64_item_audio_runtime.cpp
        src/rr64_mk64_item_material.cpp
        src/rr64_course_audio.cpp src/rr64_course_audio_world.cpp
        src/rr64_course_music.cpp
        src/rr64_course_pickups.cpp
        src/rr64_course_walls.cpp src/rr64_course_impact.cpp src/rr64_course_guardrail.cpp
        src/rr64_course_hazards.cpp src/rr64_course_hazard_motion.cpp
        src/rr64_course_sprite_motion.cpp src/rr64_course_scene_motion.cpp
        src/rr64_course_effect_motion.cpp
        src/rr64_course_hazard_collision.cpp src/rr64_course_hazard_render.cpp
        src/rr64_course_ai.cpp src/rr64_course_boost.cpp src/rr64_course_diagnostics.cpp)
    target_compile_definitions(RoadRash64Recompiled PRIVATE RR64_EXPERIMENTAL_COURSE=1)
    target_compile_definitions(RecompiledFuncs PRIVATE RR64_EXPERIMENTAL_COURSE=1)
    target_include_directories(RoadRash64Recompiled PRIVATE "${N64MODERN_RUNTIME_ROOT}/thirdparty")
endif()

# Config::load_config exposes the runtime's JSON type in its callback ABI.
# RT64 ships a different JSON version; use librecomp's header for this caller
# in both course-enabled and ordinary builds, without changing RT64's headers.
set_property(SOURCE src/rr64_rival_engine_config.cpp src/rr64_race_pack_mod_ui.cpp src/rr64_mk64_import.cpp
    src/rr64_import_install.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES "${N64MODERN_RUNTIME_ROOT}/thirdparty")

target_include_directories(RoadRash64Recompiled PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/zstd/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
    "${RT64_ROOT}/src/contrib"
    "${RT64_ROOT}/src/contrib/hlslpp/include"
    "${RT64_ROOT}/src/contrib/dxc/inc"
    "${RT64_ROOT}/src"
    "${RT64_ROOT}/src/rhi"
    "${RT64_ROOT}/src/render"
    "${RT64_ROOT}/src/contrib/nativefiledialog-extended/src/include"
    "${RECOMP_FRONTEND_ROOT}/recompinput/include"
    "${RECOMP_FRONTEND_ROOT}/recompui/include"
    "${RECOMP_FRONTEND_ROOT}/recompui/src"
    "${RR64_SDL2_INCLUDE_DIRS}"
)

target_link_directories(RoadRash64Recompiled PRIVATE ${RR64_SDL2_LIB_DIRS})

if(MSVC)
    target_compile_options(RoadRash64Recompiled PRIVATE
        /clang:-march=nehalem
        /clang:-fno-strict-aliasing
        /clang:-fms-extensions
    )
    # Keep function identities distinct; useful for later patch/mod work.
    target_link_options(RoadRash64Recompiled PRIVATE /OPT:NOICF
        # Keep this as a linker argument: ClangCL's VS task drops the
        # GenerateMapFile property produced by CMake's /MAP translation.
        "-map:$<TARGET_FILE_DIR:RoadRash64Recompiled>/RoadRash64Recompiled.map"
        "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>"
        "$<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>")
else()
    target_compile_options(RoadRash64Recompiled PRIVATE -march=nehalem -fno-strict-aliasing -fms-extensions)
endif()

target_link_libraries(RoadRash64Recompiled PRIVATE
    libzstd_static
    RecompiledFuncs
    recompui
    recompinput
    librecomp
    ultramodern
    rt64
    nfd
    ${RR64_SDL2_TARGET}
    opus
)
if(WIN32)
    target_link_libraries(RoadRash64Recompiled PRIVATE Winmm.lib Ws2_32.lib)
endif()

if(WIN32)
    add_custom_command(TARGET RoadRash64Recompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxil.dll"
            "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "$<TARGET_FILE_DIR:RoadRash64Recompiled>"
    )
endif()

add_custom_command(TARGET RoadRash64Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RoadRash64Recompiled>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:RoadRash64Recompiled>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RECOMP_FRONTEND_ROOT}/recompui/lib/RmlUi/Samples/assets/LatoLatin-Regular.ttf"
        "$<TARGET_FILE_DIR:RoadRash64Recompiled>/assets/LatoLatin-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RECOMP_FRONTEND_ROOT}/recompui/lib/RmlUi/Samples/assets/LICENSE.txt"
        "$<TARGET_FILE_DIR:RoadRash64Recompiled>/assets/RmlUi-Sample-Fonts-LICENSE.txt"
)

set_property(TARGET RoadRash64Recompiled PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")

# Audio backends and the Windows entry point belong to playable builds too.
if(WIN32)
    target_link_libraries(RoadRash64Recompiled PRIVATE mfplat mfreadwrite mfuuid ole32)
else()
    add_dependencies(RoadRash64Recompiled rr64_ffmpeg)
    target_include_directories(RoadRash64Recompiled PRIVATE "${RR64_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(RoadRash64Recompiled PRIVATE ${RR64_FFMPEG_LIBS} z pthread)
endif()

if(WIN32)
    enable_language(RC)
    target_sources(RoadRash64Recompiled PRIVATE assets/RoadRashIcon.rc)
    add_executable(RoadRash64DirectStart WIN32 src/rr64_direct_start.cpp assets/RoadRashIcon.rc)
    target_compile_features(RoadRash64DirectStart PRIVATE cxx_std_20)
    target_link_libraries(RoadRash64DirectStart PRIVATE User32.lib)
endif()
