# ROM-free host-audio queue policy and telemetry validation.
add_executable(RR64AudioQueueSmoke EXCLUDE_FROM_ALL
    tests/rr64_audio_queue_smoke.cpp
    src/rr64_audio_output.cpp
)
target_include_directories(RR64AudioQueueSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
)

# ROM-free Vulkan surface limits and swapchain recreation checks.
add_executable(RR64VulkanSwapChainSmoke EXCLUDE_FROM_ALL
    tests/rr64_vulkan_swapchain_smoke.cpp)
target_compile_features(RR64VulkanSwapChainSmoke PRIVATE cxx_std_17)
target_include_directories(RR64VulkanSwapChainSmoke PRIVATE "${RT64_ROOT}/src")

# ROM-free regression coverage for long-session 60/120 FPS presentation.
# In particular, this reproduces the former self-sustaining 60-to-30 FPS
# collapse after a transient late interpolation frame.
add_executable(RR64FramePacingSmoke EXCLUDE_FROM_ALL
    tests/rr64_frame_pacing_smoke.cpp
    "${RT64_ROOT}/src/common/rt64_timer.cpp"
)
target_include_directories(RR64FramePacingSmoke PRIVATE
    "${RT64_ROOT}/src"
)

# Exercise the actual message-queue implementation with a small stub scheduler.
# This target never starts a game, renderer or host event thread.
set(RR64_SCHEDULER_EVENTS_SOURCE "${N64MODERN_RUNTIME_ROOT}/ultramodern/src/events.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RR64_SCHEDULER_EVENTS_SOURCE}")
file(READ "${RR64_SCHEDULER_EVENTS_SOURCE}" RR64_SCHEDULER_EVENTS_TEXT)
string(REGEX MATCHALL "void[ \t]+ultramodern::send_si_message[ \t]*\\([ \t]*\\)[ \t]*\\{"
    RR64_SI_SIGNATURES "${RR64_SCHEDULER_EVENTS_TEXT}")
list(LENGTH RR64_SI_SIGNATURES RR64_SI_SIGNATURE_COUNT)
string(REGEX MATCH "void[ \t]+ultramodern::send_si_message[ \t]*\\([ \t]*\\)[ \t]*\\{[^{}]*\\}"
    RR64_SI_PRODUCER "${RR64_SCHEDULER_EVENTS_TEXT}")
string(REGEX REPLACE "//[^\n]*" "" RR64_SI_PRODUCER_SHAPE "${RR64_SI_PRODUCER}")
if (NOT RR64_SI_SIGNATURE_COUNT EQUAL 1 OR NOT RR64_SI_PRODUCER_SHAPE MATCHES
    "^void[ \t]+ultramodern::send_si_message[ \t]*\\([ \t]*\\)[ \t]*\\{[ \t\r\n]*ultramodern::enqueue_external_message\\(events_context\\.si\\.mq,[ \t\r\n]*events_context\\.si\\.msg,[ \t\r\n]*false,[ \t\r\n]*(true|false)\\);[ \t\r\n]*\\}$")
    message(FATAL_ERROR "SI queue smoke extraction requires review: send_si_message shape changed")
endif()
set(RR64_SCHEDULER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/scheduler_queue")
file(MAKE_DIRECTORY "${RR64_SCHEDULER_GENERATED_DIR}")
file(WRITE "${RR64_SCHEDULER_GENERATED_DIR}/rr64_si_message_producer.inc" "${RR64_SI_PRODUCER}\n")
# Negative control changes only the retry flag in the extracted producer. It
# must fail the same regression; production source is never modified.
string(REGEX REPLACE ",[ \t\r\n]*(true|false)\\);" ", true);"
    RR64_SI_LEGACY_PRODUCER "${RR64_SI_PRODUCER}")
file(WRITE "${RR64_SCHEDULER_GENERATED_DIR}/rr64_si_message_producer_legacy.inc" "${RR64_SI_LEGACY_PRODUCER}\n")
foreach(RR64_QUEUE_TEST RR64SchedulerQueueSmoke RR64SchedulerQueueLegacySiSmoke)
    add_executable(${RR64_QUEUE_TEST} EXCLUDE_FROM_ALL tests/rr64_scheduler_queue_smoke.cpp)
    target_include_directories(${RR64_QUEUE_TEST} PRIVATE
        "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
        "${N64MODERN_RUNTIME_ROOT}/thirdparty"
        "${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue"
        "${RR64_SCHEDULER_GENERATED_DIR}"
    )
    set_target_properties(${RR64_QUEUE_TEST} PROPERTIES CXX_STANDARD 20)
    if (WIN32)
        target_compile_definitions(${RR64_QUEUE_TEST} PRIVATE NOMINMAX)
    endif()
endforeach()
target_compile_definitions(RR64SchedulerQueueLegacySiSmoke PRIVATE
    RR64_SI_PRODUCER_INCLUDE="rr64_si_message_producer_legacy.inc")

add_executable(RR64FrameMetadataSmoke EXCLUDE_FROM_ALL
    tests/rr64_frame_metadata_smoke.cpp
)

add_executable(RR64PipelineDiagnosticsSmoke EXCLUDE_FROM_ALL
    tests/rr64_pipeline_diagnostics_smoke.cpp)
target_compile_features(RR64PipelineDiagnosticsSmoke PRIVATE cxx_std_17)
target_include_directories(RR64PipelineDiagnosticsSmoke PRIVATE "${RT64_ROOT}/src")
add_executable(RR64BufferCapacitySmoke EXCLUDE_FROM_ALL tests/rr64_buffer_capacity_smoke.cpp)
target_compile_features(RR64BufferCapacitySmoke PRIVATE cxx_std_17)
target_include_directories(RR64BufferCapacitySmoke PRIVATE "${RT64_ROOT}/src")
add_executable(RR64LocalPlayersSmoke EXCLUDE_FROM_ALL tests/rr64_local_players_smoke.cpp)
add_executable(RR64ControllerSmoke EXCLUDE_FROM_ALL tests/rr64_controller_smoke.cpp
    lib/RecompFrontend/recompinput/src/input_binding.cpp)
target_compile_features(RR64ControllerSmoke PRIVATE cxx_std_20)
target_include_directories(RR64ControllerSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompinput/include/recompinput"
    $<TARGET_PROPERTY:recompinput,INCLUDE_DIRECTORIES> "${RR64_SDL2_INCLUDE_DIRS}")
target_link_directories(RR64ControllerSmoke PRIVATE ${RR64_SDL2_LIB_DIRS})
target_link_libraries(RR64ControllerSmoke PRIVATE ${RR64_SDL2_TARGET})
include(tests/rr64_controller_remap_fixture.cmake)
add_executable(RR64LocalRaceOptionsSmoke EXCLUDE_FROM_ALL tests/rr64_local_race_options_smoke.cpp src/rr64_local_race_options.cpp src/rr64_thrash_options.cpp src/rr64_custom_cop.cpp src/rr64_custom_cop_runtime.cpp)
include(tests/rr64_mk64_item_options_fixture.cmake)
target_compile_features(RR64LocalRaceOptionsSmoke PRIVATE cxx_std_20)
target_include_directories(RR64LocalRaceOptionsSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include")
target_compile_features(RR64LocalPlayersSmoke PRIVATE cxx_std_17)
target_include_directories(RR64LocalPlayersSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompinput/include")
