add_executable(RR64AuthoredFrameSmoke EXCLUDE_FROM_ALL
    tests/rr64_authored_frame_smoke.cpp
)
add_executable(RR64FrameOverwriteSmoke EXCLUDE_FROM_ALL
    tests/rr64_frame_overwrite_smoke.cpp
)
add_executable(RR64GeometryMatchSmoke EXCLUDE_FROM_ALL
    tests/rr64_geometry_match_smoke.cpp
)
target_include_directories(RR64GeometryMatchSmoke PRIVATE
    "${RT64_ROOT}/src"
)
set_target_properties(RR64GeometryMatchSmoke PROPERTIES CXX_STANDARD 17)
add_executable(RR64WorldMatchingSmoke EXCLUDE_FROM_ALL tests/rr64_world_matching_smoke.cpp
    tests/rr64_matching_test_callbacks.cpp)
target_include_directories(RR64WorldMatchingSmoke PRIVATE "${RT64_ROOT}/src"
    "${RT64_ROOT}/src/contrib" "${RT64_ROOT}/src/contrib/hlslpp/include"
    "${RT64_ROOT}/src/contrib/dxc/inc")
target_link_libraries(RR64WorldMatchingSmoke PRIVATE rt64)
target_link_directories(RR64WorldMatchingSmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
if(MSVC)
    target_compile_options(RR64WorldMatchingSmoke PRIVATE /clang:-march=nehalem)
endif()
add_executable(RR64HighlightCameraBoundarySmoke EXCLUDE_FROM_ALL
    tests/rr64_highlight_camera_boundary_smoke.cpp tests/rr64_matching_test_callbacks.cpp
    src/rr64_highlight_render_boundary.cpp src/rr64_shadow_tags.cpp)
target_include_directories(RR64HighlightCameraBoundarySmoke PRIVATE src
    $<TARGET_PROPERTY:RR64WorldMatchingSmoke,INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:RR64NetplaySmoke,INCLUDE_DIRECTORIES>)
target_compile_features(RR64HighlightCameraBoundarySmoke PRIVATE cxx_std_20)
target_link_libraries(RR64HighlightCameraBoundarySmoke PRIVATE rt64)
target_link_directories(RR64HighlightCameraBoundarySmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
if(MSVC)
    target_compile_options(RR64HighlightCameraBoundarySmoke PRIVATE /clang:-march=nehalem)
endif()
add_executable(RR64AuthoredCadenceSmoke EXCLUDE_FROM_ALL
    tests/rr64_authored_cadence_smoke.cpp
)
set(_shadow_fixture "${CMAKE_CURRENT_BINARY_DIR}/rr64_shadow_tags_fixture.c")
file(GLOB _shadow_functions "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs/funcs_*.c")
add_custom_command(OUTPUT "${_shadow_fixture}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_shadow_tags_fixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/RecompiledFuncs" "${_shadow_fixture}"
    DEPENDS scripts/extract_shadow_tags_fixture.py ${_shadow_functions}
    VERBATIM)
add_executable(RR64ShadowTagsSmoke EXCLUDE_FROM_ALL tests/rr64_shadow_tags_smoke.cpp
    tests/rr64_matching_test_callbacks.cpp src/rr64_shadow_tags.cpp "${_shadow_fixture}")
target_include_directories(RR64ShadowTagsSmoke PRIVATE src "${RT64_ROOT}"
    $<TARGET_PROPERTY:RR64WorldMatchingSmoke,INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:RR64NetplaySmoke,INCLUDE_DIRECTORIES> "${RR64_SDL2_INCLUDE_DIRS}")
target_compile_features(RR64ShadowTagsSmoke PRIVATE cxx_std_20)
target_link_libraries(RR64ShadowTagsSmoke PRIVATE rt64)
target_link_directories(RR64ShadowTagsSmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
if(MSVC)
    target_compile_options(RR64ShadowTagsSmoke PRIVATE /clang:-march=nehalem)
endif()
add_executable(RR64InterpolationPressureSmoke EXCLUDE_FROM_ALL
    tests/rr64_interpolation_pressure_smoke.cpp
)
target_include_directories(RR64InterpolationPressureSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src"
)
foreach(RR64_CPU_TEST RR64OwnedTextureReuseSmoke RR64RSPVertexSmoke RR64RSPTransformIndexSmoke RR64WorldMatchingPerformanceSmoke RR64TMEMLoadSmoke RR64MatchingPreflightSmoke RR64RSPTriangleBatchSmoke RR64TranslationRejectionSmoke)
    if(RR64_CPU_TEST STREQUAL "RR64OwnedTextureReuseSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_owned_texture_reuse_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64RSPVertexSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_rsp_vertex_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64RSPTransformIndexSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_rsp_transform_index_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64TMEMLoadSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_tmem_load_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64MatchingPreflightSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_matching_preflight_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64RSPTriangleBatchSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_rsp_triangle_batch_smoke.cpp)
    elseif(RR64_CPU_TEST STREQUAL "RR64TranslationRejectionSmoke")
        set(RR64_CPU_FIXTURE tests/rr64_translation_rejection_smoke.cpp)
    else()
        set(RR64_CPU_FIXTURE tests/rr64_world_matching_performance_smoke.cpp)
    endif()
    add_executable(${RR64_CPU_TEST} EXCLUDE_FROM_ALL ${RR64_CPU_FIXTURE}
        tests/rr64_matching_test_callbacks.cpp)
    target_include_directories(${RR64_CPU_TEST} PRIVATE "${RT64_ROOT}" "${RT64_ROOT}/src"
        "${RT64_ROOT}/src/contrib" "${RT64_ROOT}/src/contrib/hlslpp/include"
        "${RT64_ROOT}/src/contrib/dxc/inc" "${RR64_SDL2_INCLUDE_DIRS}")
    target_link_libraries(${RR64_CPU_TEST} PRIVATE rt64)
    target_link_directories(${RR64_CPU_TEST} PRIVATE ${RR64_SDL2_LIB_DIRS})
    set_target_properties(${RR64_CPU_TEST} PROPERTIES CXX_STANDARD 17)
endforeach()
add_executable(RR64CoordinateConsistencySmoke EXCLUDE_FROM_ALL
    tests/rr64_coordinate_consistency_smoke.cpp
)
add_executable(RR64TMEMHashSmoke EXCLUDE_FROM_ALL tests/rr64_tmem_hash_smoke.cpp)
target_include_directories(RR64TMEMHashSmoke PRIVATE "${RT64_ROOT}/src" "${RT64_ROOT}/src/contrib")
target_compile_definitions(RR64TMEMHashSmoke PRIVATE NOMINMAX)
set_target_properties(RR64TMEMHashSmoke PROPERTIES CXX_STANDARD 17)
foreach(RR64_PRESENTATION_TEST RR64AuthoredCadenceSmoke RR64CoordinateConsistencySmoke)
    target_include_directories(${RR64_PRESENTATION_TEST} PRIVATE "${RT64_ROOT}/src")
    set_target_properties(${RR64_PRESENTATION_TEST} PROPERTIES CXX_STANDARD 17)
endforeach()
target_include_directories(RR64FrameOverwriteSmoke PRIVATE
    "${RT64_ROOT}/src"
)
target_include_directories(RR64AuthoredFrameSmoke PRIVATE
    "${RT64_ROOT}/src"
)
target_include_directories(RR64FrameMetadataSmoke PRIVATE
    "${RT64_ROOT}/src"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
)

# ROM-free renderer-owned Max LOD snapshot, allocation and rollback contract.
add_executable(RR64ActorRenderSnapshotSmoke EXCLUDE_FROM_ALL
    tests/rr64_actor_render_snapshot_smoke.cpp
    src/rr64_actor_render_snapshot.cpp
    src/rr64_actor_pose.cpp
)
target_include_directories(RR64ActorRenderSnapshotSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# Exercises the actual Max LOD adapter with synthetic RDRAM and bounded helper
# substitutes. This never calls original guest code or starts a renderer.
add_executable(RR64ActorRenderRuntimeSmoke EXCLUDE_FROM_ALL
    tests/rr64_actor_render_runtime_smoke.cpp
    src/rr64_actor_render_runtime.cpp
    src/rr64_actor_held_pose.cpp
    src/rr64_actor_render_snapshot.cpp
    src/rr64_actor_pose.cpp
)
target_include_directories(RR64ActorRenderRuntimeSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)

# Exercise the current adapter with original generated root, vector, quaternion,
# renderer and fixed-matrix helpers. The fixture bounds animation/shadow routing;
# it verifies coordinate and matrix parity without a ROM or a game launch.
find_program(RR64_TEST_POWERSHELL NAMES pwsh powershell)
if(RR64_TEST_POWERSHELL)
    set(RR64_ACTOR_MATH_GENERATED_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/actor_math/rr64_actor_math_functions.c")
    add_custom_command(
        OUTPUT "${RR64_ACTOR_MATH_GENERATED_SOURCE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_math_fixture.ps1"
            -OutputPath "${RR64_ACTOR_MATH_GENERATED_SOURCE}"
            -GeneratedDirectory "${RECOMPILED_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_math_fixture.ps1"
            ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original actor renderer and math helpers for headless regression"
        VERBATIM
    )
    add_executable(RR64ActorRenderMathSmoke EXCLUDE_FROM_ALL
        tests/rr64_actor_render_math_smoke.cpp
        tests/world_hooks_disabled.cpp
        src/rr64_actor_render_runtime.cpp
        src/rr64_actor_held_pose.cpp
        src/rr64_actor_render_snapshot.cpp
        src/rr64_actor_pose.cpp
        "${RR64_ACTOR_MATH_GENERATED_SOURCE}"
    )
    target_include_directories(RR64ActorRenderMathSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${RECOMPILED_DIR}"
        "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
        "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
        "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
        "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
    )
    if(MSVC)
        target_compile_options(RR64ActorRenderMathSmoke PRIVATE
            /clang:-march=nehalem /clang:-fno-strict-aliasing
            /clang:-Wno-unused-variable /clang:-Wno-implicit-function-declaration)
    else()
        target_compile_options(RR64ActorRenderMathSmoke PRIVATE
            -fno-strict-aliasing -Wno-unused-variable -Wno-implicit-function-declaration)
    endif()

    # Compare the private held-wheel evaluator to the original generated wheel
    # update at zero elapsed time. Extraction keeps the reference tied to the
    # configured game output instead of copying production arithmetic here.
    set(RR64_ACTOR_WHEEL_GENERATED_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/actor_wheel/rr64_actor_wheel_functions.c")
    add_custom_command(
        OUTPUT "${RR64_ACTOR_WHEEL_GENERATED_SOURCE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_wheel_fixture.ps1"
            -OutputPath "${RR64_ACTOR_WHEEL_GENERATED_SOURCE}"
            -GeneratedDirectory "${RECOMPILED_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_wheel_fixture.ps1"
            ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original wheel pose helpers for headless regression"
        VERBATIM
    )
    add_executable(RR64ActorHeldWheelSmoke EXCLUDE_FROM_ALL
        tests/rr64_actor_held_wheel_smoke.cpp
        src/rr64_actor_held_pose.cpp
        src/rr64_actor_render_snapshot.cpp
        src/rr64_actor_pose.cpp
        "${RR64_ACTOR_WHEEL_GENERATED_SOURCE}"
    )
    get_target_property(RR64_ACTOR_MATH_INCLUDES RR64ActorRenderMathSmoke INCLUDE_DIRECTORIES)
    get_target_property(RR64_ACTOR_MATH_OPTIONS RR64ActorRenderMathSmoke COMPILE_OPTIONS)
    target_include_directories(RR64ActorHeldWheelSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES})
    target_compile_options(RR64ActorHeldWheelSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})

    # Actual held-pose evaluator and generated quaternion math, exercised
    # through the runtime's stage/publication path. Other visual helpers are
    # bounded substitutes; the wheel-only target covers original B948 parity.
    set(RR64_HELD_PAIR_GENERATED_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/actor_held_pair/rr64_actor_held_math_functions.c")
    add_custom_command(
        OUTPUT "${RR64_HELD_PAIR_GENERATED_SOURCE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_wheel_fixture.ps1"
            -OnlyMath
            -OutputPath "${RR64_HELD_PAIR_GENERATED_SOURCE}"
            -GeneratedDirectory "${RECOMPILED_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_wheel_fixture.ps1"
            ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original math for runtime held-pair integration"
        VERBATIM
    )
    add_executable(RR64ActorHeldPairSmoke EXCLUDE_FROM_ALL
        tests/rr64_actor_render_runtime_smoke.cpp
        src/rr64_actor_render_runtime.cpp
        src/rr64_actor_held_pose.cpp
        src/rr64_actor_render_snapshot.cpp
        src/rr64_actor_pose.cpp
        "${RR64_HELD_PAIR_GENERATED_SOURCE}"
    )
    target_include_directories(RR64ActorHeldPairSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES})
    target_compile_options(RR64ActorHeldPairSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    target_compile_definitions(RR64ActorHeldPairSmoke PRIVATE RR64_TEST_REAL_HELD_MATH=1)

    set(RR64_FULL_WEIGHT_GENERATED_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/actor_full_weight/rr64_actor_full_weight_functions.c")
    add_custom_command(
        OUTPUT "${RR64_FULL_WEIGHT_GENERATED_SOURCE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_full_weight_fixture.ps1"
            -OutputPath "${RR64_FULL_WEIGHT_GENERATED_SOURCE}"
            -GeneratedDirectory "${RECOMPILED_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_full_weight_fixture.ps1"
            ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original full-weight animation helpers for headless regression"
        VERBATIM
    )
    add_executable(RR64ActorFullWeightSmoke EXCLUDE_FROM_ALL
        tests/rr64_actor_full_weight_smoke.cpp
        "${RR64_FULL_WEIGHT_GENERATED_SOURCE}"
    )
    target_include_directories(RR64ActorFullWeightSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES})
    target_compile_options(RR64ActorFullWeightSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})

    set(RR64_CRASH_ANIMATION_GENERATED_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/actor_crash/rr64_actor_crash_functions.c")
    add_custom_command(
        OUTPUT "${RR64_CRASH_ANIMATION_GENERATED_SOURCE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_crash_fixture.ps1"
            -OutputPath "${RR64_CRASH_ANIMATION_GENERATED_SOURCE}"
            -GeneratedDirectory "${RECOMPILED_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_actor_crash_fixture.ps1"
            ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original crash animation routing for headless regression"
        VERBATIM
    )
    add_executable(RR64ActorCrashAnimationSmoke EXCLUDE_FROM_ALL
        tests/rr64_actor_crash_animation_smoke.cpp
        "${RR64_CRASH_ANIMATION_GENERATED_SOURCE}"
    )
    target_include_directories(RR64ActorCrashAnimationSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES})
    target_compile_options(RR64ActorCrashAnimationSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})

    function(rr64_generated_world_test target fixture test_source)
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/original_functions.c")
        add_custom_command(
            OUTPUT "${generated}"
            COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/${fixture}"
                -OutputPath "${generated}" -GeneratedDirectory "${RECOMPILED_DIR}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${fixture}"
                ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
            COMMENT "Extracting original helpers for ${target}"
            VERBATIM)
        add_executable(${target} EXCLUDE_FROM_ALL "tests/${test_source}" "${generated}" ${ARGN})
        target_include_directories(${target} PRIVATE ${RR64_ACTOR_MATH_INCLUDES})
        target_include_directories(${target} PRIVATE "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue"
            "${N64MODERN_RUNTIME_ROOT}/thirdparty")
        target_compile_options(${target} PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    endfunction()
    rr64_generated_world_test(RR64CombatCreditSmoke generate_combat_credit_fixture.ps1
        rr64_combat_credit_smoke.cpp)
    rr64_generated_world_test(RR64TerrainGraphicsPoolSmoke generate_terrain_graphics_pool_fixture.ps1
        rr64_terrain_graphics_pool_smoke.cpp src/rr64_terrain_graphics_pool.cpp)
    target_compile_features(RR64TerrainGraphicsPoolSmoke PRIVATE cxx_std_20)
    set(RR64_STATIC_COLLISION_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/static_collision/original_functions.c")
    add_custom_command(OUTPUT "${RR64_STATIC_COLLISION_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_static_collision_fixture.py"
            --recompiled-dir "${RECOMPILED_DIR}" --output "${RR64_STATIC_COLLISION_SOURCE}"
        DEPENDS scripts/extract_static_collision_fixture.py ${RR64_RECOMP_C} "${RECOMPILED_DIR}/funcs.h"
        COMMENT "Extracting original static contacts for resident versus unloaded collision checks"
        VERBATIM)
    add_executable(RR64StaticCollisionSmoke EXCLUDE_FROM_ALL tests/rr64_static_collision_smoke.cpp
        src/rr64_online_terrain.cpp "${RR64_STATIC_COLLISION_SOURCE}")
    target_include_directories(RR64StaticCollisionSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES}
        "${N64MODERN_RUNTIME_ROOT}/thirdparty" "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue")
    target_compile_options(RR64StaticCollisionSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    target_compile_features(RR64StaticCollisionSmoke PRIVATE cxx_std_20)
    target_compile_definitions(RR64StaticCollisionSmoke PRIVATE RR64_EXPERIMENTAL_COURSE NOMINMAX)
    rr64_generated_world_test(RR64ProfileNamesSmoke generate_profile_names_fixture.ps1
        rr64_profile_names_smoke.cpp src/rr64_profile_names.cpp src/rr64_name_entry.cpp)
    rr64_generated_world_test(RR64CampaignBikeSmoke generate_campaign_bike_fixture.ps1
        rr64_campaign_bike_smoke.cpp src/rr64_campaign_completion.cpp src/rr64_campaign_bonus_save.cpp
        src/rr64_local_race_options.cpp src/rr64_thrash_options.cpp
        src/rr64_custom_cop.cpp src/rr64_custom_cop_runtime.cpp
        src/rr64_character_menu.cpp src/rr64_character_preferences.cpp)
    target_compile_features(RR64CampaignBikeSmoke PRIVATE cxx_std_20)
    set(RR64_ACHIEVEMENT_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/generated/achievements/rr64_achievement_flush.hpp")
    add_custom_command(OUTPUT "${RR64_ACHIEVEMENT_FIXTURE}"
        COMMAND "${RR64_TEST_POWERSHELL}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_achievement_persistence_fixture.ps1"
            -OutputPath "${RR64_ACHIEVEMENT_FIXTURE}"
        DEPENDS tests/generate_achievement_persistence_fixture.ps1 src/rr64_achievements.cpp VERBATIM)
    add_executable(RR64AchievementPersistenceSmoke EXCLUDE_FROM_ALL
        tests/rr64_achievement_persistence_smoke.cpp "${RR64_ACHIEVEMENT_FIXTURE}")
    target_include_directories(RR64AchievementPersistenceSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/achievements")
    rr64_generated_world_test(RR64ActorFinishBlendSmoke generate_actor_finish_fixture.ps1
        rr64_actor_finish_blend_smoke.cpp src/rr64_actor_render_runtime.cpp
        src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp src/rr64_actor_held_pose.cpp)
    rr64_generated_world_test(RR64VideoModeSmoke generate_video_mode_fixture.ps1
        rr64_video_mode_smoke.cpp src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp)
    rr64_generated_world_test(RR64WorldTerrainAssetsSmoke generate_world_terrain_assets_fixture.ps1
        rr64_world_terrain_assets_smoke.cpp src/rr64_world_terrain_assets.cpp)
    rr64_generated_world_test(RR64WorldTerrainSmoke generate_world_terrain_assets_fixture.ps1
        rr64_world_terrain_smoke.cpp src/rr64_world_terrain_assets.cpp src/rr64_world_terrain.cpp
        src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp)
    # Run both terrain paths through the real RSP and strict frame matcher.
    add_executable(RR64TerrainHandoffRSPSmoke EXCLUDE_FROM_ALL
        tests/rr64_terrain_handoff_rsp_smoke.cpp tests/rr64_world_terrain_smoke.cpp
        tests/rr64_matching_test_callbacks.cpp src/rr64_world_terrain_assets.cpp
        src/rr64_world_terrain.cpp src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/RR64WorldTerrainSmoke/original_functions.c")
    target_include_directories(RR64TerrainHandoffRSPSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES}
        "${N64MODERN_RUNTIME_ROOT}/thirdparty" "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue"
        "${RT64_ROOT}" "${RR64_SDL2_INCLUDE_DIRS}"
        $<TARGET_PROPERTY:RR64WorldMatchingSmoke,INCLUDE_DIRECTORIES>)
    target_compile_options(RR64TerrainHandoffRSPSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    target_compile_definitions(RR64TerrainHandoffRSPSmoke PRIVATE RR64_TERRAIN_RSP_HANDOFF NOMINMAX)
    target_compile_features(RR64TerrainHandoffRSPSmoke PRIVATE cxx_std_20)
    target_link_libraries(RR64TerrainHandoffRSPSmoke PRIVATE rt64)
    target_link_directories(RR64TerrainHandoffRSPSmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
    rr64_generated_world_test(RR64WorldObjectAssetsSmoke generate_world_object_assets_fixture.ps1
        rr64_world_object_assets_smoke.cpp src/rr64_world_object_assets.cpp)
    rr64_generated_world_test(RR64HighlightTrafficSmoke generate_highlight_traffic_fixture.ps1
        rr64_highlight_traffic_smoke.cpp src/rr64_highlight_traffic.cpp
        src/rr64_highlight_traffic_assets.cpp src/rr64_world_object_assets.cpp
        src/rr64_world_camera.cpp src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp)
    target_compile_features(RR64HighlightTrafficSmoke PRIVATE cxx_std_20)
    add_executable(RR64WorldAssetBatchSmoke EXCLUDE_FROM_ALL
        tests/rr64_world_asset_batch_smoke.cpp src/rr64_world_terrain_assets.cpp
        src/rr64_world_object_assets.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/RR64WorldTerrainAssetsSmoke/original_functions.c"
        tests/rr64_world_asset_batch_object_oracle.c)
    add_custom_target(RR64WorldAssetBatchObjectOracle DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/generated/RR64WorldObjectAssetsSmoke/original_functions.c")
    add_dependencies(RR64WorldAssetBatchSmoke RR64WorldAssetBatchObjectOracle)
    target_include_directories(RR64WorldAssetBatchSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES}
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${N64MODERN_RUNTIME_ROOT}/thirdparty" "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue")
    target_compile_options(RR64WorldAssetBatchSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    add_executable(RR64WorldObjectsSmoke EXCLUDE_FROM_ALL tests/rr64_world_objects_smoke.cpp
        src/rr64_world_objects.cpp src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp)
    target_include_directories(RR64WorldObjectsSmoke PRIVATE ${RR64_ACTOR_MATH_INCLUDES}
        "${N64MODERN_RUNTIME_ROOT}/thirdparty" "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue")
    target_compile_options(RR64WorldObjectsSmoke PRIVATE ${RR64_ACTOR_MATH_OPTIONS})
    rr64_generated_world_test(RR64WorldRenderSmoke generate_world_render_fixture.ps1
        rr64_world_render_smoke.cpp src/rr64_world_render.cpp
        src/rr64_world_camera.cpp src/rr64_actor_render_snapshot.cpp src/rr64_actor_pose.cpp)
else()
    message(STATUS "Skipping optional generated actor math tests: PowerShell was not found")
endif()
