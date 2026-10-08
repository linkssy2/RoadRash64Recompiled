#include "rr64_controller_mappings.hpp"
#include "rr64_custom_cop.hpp"
#include "recompui/startup_diagnostics.h"
#include "rr64_popup_input.hpp"
#include "rr64_view_width.hpp"
#include "rr64_weapon_diagnostics.hpp"
#include "rr64_local_players.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_character_preferences.hpp"
#include "rr64_rider_skins.hpp"
#include "rr64_rider_skin_mod_ui.hpp"
#include "rr64_rider_skin_preferences.hpp"
#include "rr64_rider_skin_render.hpp"
#include "rr64_rider_skin_diagnostics.hpp"
#include "rr64_race_end_trace.hpp"
#include "composites/ui_player_card.h"
#include "rr64_log_batch.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_presentation_options.hpp"
#include "rr64_master_volume.hpp"
#include "rr64_rival_engine_config.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "nfd.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/audio_policy.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_syswm.h"
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef Success
#undef Success
#endif
#ifdef LockMask
#undef LockMask
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "util/file.h"
#include "elements/ui_theme.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_text_input.h"
#include "hle/rt64_rsp.h"
#include "hle/rt64_rr64_frame_pacing.h"
#include "hle/rt64_rr64_pipeline_diagnostics.h"
#include "hle/rt64_rr64_matching_evidence.h"
#include "hle/rt64_rr64_command_profile.h"
#include "ultramodern/rr64_scheduler_diagnostics.hpp"
#include "contrib/plume/plume_present_outcome.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <timeapi.h>
#endif

#include "rr64_msvc_crt_compat.hpp"
#include "rr64_native.hpp"
#include "rr64_actor_render_diagnostics.hpp"
#include "rr64_world_terrain.hpp"
#include "rr64_world_objects.hpp"
#include "rr64_highlight_traffic.hpp"
#include "rr64_world_render.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_achievement_audio.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_course_audio.hpp"
#include "rr64_course_music.hpp"
#include "rr64_mk64_item_audio.hpp"
#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_hud.hpp"
#endif
#include "rr64_music.hpp"
#include "rr64_achievements.hpp"
#include "rr64_audio_output.hpp"
#include "rr64_menu_navigation.hpp"
#include "rr64_netplay.hpp"
#include "rr64_online_menu.hpp"
#include "rr64_online_terrain.hpp"
#include "rr64_highlights.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_race_pack_mod.hpp"
#include "rr64_race_pack_mod_ui.hpp"
#include "rr64_mk64_import.hpp"
#include "rr64_voice_chat.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#endif

constexpr const char* kVersion = "1.4.5";
constexpr uint64_t kRoadRash64UsXxh3 = 0x517F53BCD9D13BF2ULL;
constexpr const char* kProgramName = "ROAD RASH 64 RECOMPILED";
constexpr const char* kRemoveDistanceFogOption = "rr64_remove_distance_fog";
constexpr const char* kExtendedHorizonHazeOption = "rr64_extended_horizon_haze";
constexpr const char* kAchievementsEnabledOption = "rr64_achievements_enabled";
constexpr const char* kControllerRumbleEnabledOption = "rr64_controller_rumble_enabled";
constexpr const char* kProximityVoiceEnabledOption = "rr64_proximity_voice_enabled";
const std::u8string kProgramId = u8"RoadRash64Recompiled";

// RecompFrontend expects these port globals to have external linkage.
SDL_Window* window = nullptr;
std::atomic_bool rr64_dynamic_ultrawide{false};

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
RspExitReason rr64_audio_rsp(uint8_t* rdram, uint32_t ucode_addr);

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = kRoadRash64UsXxh3,
        .internal_name = "ROAD RASH 64",
        .display_name = "Road Rash 64",
        .game_id = u8"rr64.n64.us.1.0",
        .mod_game_id = "rr64",
        // Road Rash 64 uses a Controller Pak rather than cartridge EEPROM/SRAM.
        // PFS/Controller Pak virtualization is a later Road Rash-specific runtime task.
        .save_type = recomp::SaveType::None,
        .thumbnail_bytes = {},
        .is_enabled = false,
        .decompression_routine = nullptr,
        .has_compressed_code = false,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = [](uint8_t* memory, recomp_context* context) {
            // Mod content is loaded after on_init, before this entrypoint.
            rr64::rider_skins::begin_session();
            recomp_entrypoint(memory, context);
        },
        .on_init_callback = [](uint8_t* memory, recomp_context*) {
            rr64::rider_skins::prepare_session();
            rr64::rider_skins::reset_render_session();
            rr64::offline_modifiers::reset(memory);
            rr64_local_bike_profiles_reset();
            rr64_online_bike_profiles_reset();
            rr64::highlights::reset();
            rr64::online_terrain::reset();
#ifdef RR64_EXPERIMENTAL_COURSE
            rr64::experimental_course::initialize();
#endif
            rr64::world::terrain_reset_session(); rr64::world::objects_reset_session();
            rr64::highlights::reset_traffic_session();
        },
    },
};

namespace {
std::atomic<float> g_master_volume_gain{1.0f};
SDL_AudioDeviceID g_audio_device = 0;
uint32_t g_audio_rate = 48000;
rr64::audio::QueueMonitor g_audio_queue_monitor{};
FILE* g_audio_trace_file = nullptr;
bool g_audio_playback_started = false;
std::uint32_t g_audio_startup_prebuffer_frames = 0;
std::uint32_t g_audio_host_period_frames = 0;
std::uint32_t g_audio_timeline_epoch_seen = 0;

std::mutex g_runtime_log_mutex;
FILE* g_runtime_log = nullptr;
std::filesystem::path g_runtime_log_path;
bool g_create_gfx_seen = false;
bool g_window_created = false;
bool g_renderer_created = false;
bool g_update_gfx_seen = false;
bool g_launcher_init_seen = false;
std::filesystem::path g_auto_rom_path;
std::atomic_bool g_auto_start_pending = false;
std::atomic_bool g_auto_start_dispatched = false;
std::atomic_bool g_maximum_view_distance_active = false;
std::atomic_bool g_extended_horizon_haze_requested = true;
std::atomic_uint64_t g_present_interval_samples = 0;
std::atomic_uint64_t g_present_interval_total_us = 0;
std::atomic_uint64_t g_present_interval_over_budget = 0;
std::atomic_uint32_t g_present_interval_worst_us = 0;
std::atomic_uint32_t g_present_target_rate = 0;
std::atomic_uint32_t g_present_original_rate = 0;
std::atomic_uint64_t g_present_late_before_timer = 0;
std::atomic_uint64_t g_present_late_present_call = 0;
std::atomic_uint64_t g_present_late_timer = 0;
std::atomic_uint64_t g_present_late_other = 0;
std::atomic_uint32_t g_present_worst_before_timer_us = 0;
std::atomic_uint32_t g_present_worst_timer_overrun_us = 0;
std::atomic_uint32_t g_present_worst_present_call_us = 0;
std::atomic_uint64_t g_interpolation_workloads = 0;
std::array<std::atomic_uint64_t, 3> g_production_image_counts{};
std::atomic_uint64_t g_interpolation_planned_frames = 0;
std::atomic_uint64_t g_interpolation_rendered_frames = 0;
std::atomic_uint64_t g_interpolation_backlog_dropped_frames = 0;
std::atomic_uint64_t g_interpolation_unavailable_workloads = 0;
std::atomic_uint64_t g_interpolation_stock_flag_workloads = 0;
std::atomic_uint64_t g_interpolation_history_target_workloads = 0;
std::atomic_uint32_t g_interpolation_target_rate = 0;
std::atomic_uint32_t g_interpolation_original_rate = 0;
std::atomic_uint32_t g_source_raw_factor = 0;
std::atomic_uint32_t g_source_stable_factor = 0;
std::atomic_uint32_t g_source_pending_factor = 0;
std::atomic_uint32_t g_source_pending_samples = 0;
std::atomic_uint32_t g_source_rate = 0;
std::atomic_uint32_t g_source_race_active = 0;
std::atomic_uint64_t g_consumer_interpolated_batches = 0;
std::atomic_uint64_t g_consumer_repeated_batches = 0;
std::atomic_uint64_t g_consumer_rejected_batches = 0;
std::array<std::atomic_uint64_t, 5> g_consumer_rejection_reasons{};
std::array<std::atomic_uint64_t, 2> g_owned_produced{};
std::array<std::atomic_uint64_t, 2> g_owned_presented{};
std::array<std::atomic_uint64_t, 2> g_geometry_compatible{};
std::array<std::atomic_uint64_t, 6> g_geometry_reasons{};
std::array<std::atomic_uint64_t,
    static_cast<std::size_t>(plume::D3D12PresentOutcome::Count)> g_d3d12_present_outcomes{};
std::atomic_uint32_t g_d3d12_last_present_error = 0;
std::array<std::atomic_uint64_t, 4> g_authored_timing_decisions{};
std::atomic_uint32_t g_authored_timing_source_rate = 0;
std::atomic_uint64_t g_authored_timing_interval_samples = 0;
std::atomic_uint64_t g_authored_timing_interval_total_ns = 0;
std::atomic_uint64_t g_authored_timing_interval_max_ns = 0;
std::array<std::atomic_uint64_t, 2> g_authored_camera_changes{};
std::array<std::atomic_uint64_t, 2> g_authored_world_changes{};
std::array<std::atomic_uint64_t, 4> g_present_sequence_modes{};
struct PipelineStageCounters {
    std::atomic_uint64_t samples{0}, total_ns{0}, maximum_ns{0};
};
std::array<PipelineStageCounters,
    static_cast<std::size_t>(RT64::RR64PipelineDiagnostics::Stage::Count)> g_pipeline_stages{};
std::array<std::atomic_uint64_t, 10> g_pipeline_resources{};
std::array<PipelineStageCounters,
    static_cast<std::size_t>(ultramodern::rr64_diagnostics::Stage::Count)> g_scheduler_stages{};
std::array<std::atomic_uint64_t,
    static_cast<std::size_t>(ultramodern::rr64_diagnostics::Event::Count)> g_scheduler_events{};
struct GuestQueueCounters {
    std::atomic_uint32_t queue{0};
    PipelineStageCounters timing;
    PipelineStageCounters external_delivery;
    std::array<std::atomic_uint64_t, 3> external_outcomes{};
};
std::array<GuestQueueCounters, 64> g_guest_queue_counters{};
std::atomic_uint64_t g_guest_queue_overflow_samples{0};

void record_stage_sample(PipelineStageCounters& stage, std::uint64_t nanoseconds) {
    stage.total_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto maximum = stage.maximum_ns.load(std::memory_order_relaxed);
    while (nanoseconds > maximum && !stage.maximum_ns.compare_exchange_weak(
        maximum, nanoseconds, std::memory_order_relaxed)) { }
    stage.samples.fetch_add(1, std::memory_order_relaxed);
}
std::chrono::steady_clock::time_point g_last_present_health_log =
    std::chrono::steady_clock::now();
#ifdef _WIN32
std::atomic<DWORD> g_event_thread_id = 0;
std::atomic<DWORD> g_ui_thread_id = 0;
#endif

void record_d3d12_present_outcome(plume::D3D12PresentOutcome outcome,
    std::uint32_t result_code) noexcept {
    const auto index = static_cast<std::size_t>(outcome);
    if (index >= g_d3d12_present_outcomes.size()) {
        return;
    }
    g_d3d12_present_outcomes[index].fetch_add(1, std::memory_order_relaxed);
    if (outcome == plume::D3D12PresentOutcome::Error) {
        g_d3d12_last_present_error.store(result_code, std::memory_order_relaxed);
    }
}

void apply_extended_horizon_haze() {
    const bool enabled = g_maximum_view_distance_active.load(std::memory_order_relaxed) &&
        g_extended_horizon_haze_requested.load(std::memory_order_relaxed);
    RT64::setUserExtendedHorizonHazeEnabled(enabled);
}

// One user setting drives cached geometry and the original terrain tier.
// Keep the camera/actor world policy stable; only terrain submission and the
// safe stock tier change live. Haze follows whether extension is requested.
void rr64_log(const char* format, ...);
void apply_draw_distance(double requested) {
    const int distance=std::isfinite(requested)?int(std::clamp(requested,0.0,100.0)):0;
    rr64::presentation_options::draw_distance.store(distance);
    rr64_set_maximum_view_distance_enabled(distance>0?1:0);
    g_maximum_view_distance_active.store(distance>0,std::memory_order_relaxed);
    apply_extended_horizon_haze();
    rr64_log("[RR64-GFX] Draw distance=%d%%; stock terrain extension=%d.\n",distance,distance>0?1:0);
}

enum class ForcedGraphicsApi {
    Auto,
    D3D12,
    Vulkan,
};

ForcedGraphicsApi g_forced_graphics_api = ForcedGraphicsApi::Auto;

FILE* audio_trace_file() {
    static bool initialized = false;
    if (initialized) {
        return g_audio_trace_file;
    }
    initialized = true;

    char* path = nullptr;
    std::size_t path_length = 0;
    if (_dupenv_s(&path, &path_length, "RR64_AUDIO_TRACE") != 0 || path == nullptr || path[0] == '\0') {
        std::free(path);
        return nullptr;
    }

#ifdef _WIN32
    const errno_t result = fopen_s(&g_audio_trace_file, path, "w");
#else
    g_audio_trace_file = std::fopen(path, "w");
    const errno_t result = (g_audio_trace_file != nullptr) ? 0 : errno;
#endif
    std::free(path);
    if (result != 0 || g_audio_trace_file == nullptr) {
        std::fprintf(stderr, "[RR64-AUDIO] Could not open RR64_AUDIO_TRACE.\n");
        return nullptr;
    }

    std::fprintf(g_audio_trace_file,
        "sequence,sample_rate,host_period_frames,submitted_frames,queued_before_frames,queued_after_frames,gap_us,expected_us,discontinuity,empty_before,below_host_period,late_submit,playback_started_before,playback_started_after,startup_prebuffer_frames\n");
    std::fflush(g_audio_trace_file);
    return g_audio_trace_file;
}

const char* forced_graphics_api_name() {
    switch (g_forced_graphics_api) {
    case ForcedGraphicsApi::D3D12:
        return "D3D12";
    case ForcedGraphicsApi::Vulkan:
        return "Vulkan";
    case ForcedGraphicsApi::Auto:
    default:
        return "Auto";
    }
}

// Detailed performance reports are a developer opt-in, not release disk traffic.
bool detailed_diagnostics_enabled() {
    return rr64::diagnostics::detailed_enabled();
}
thread_local rr64::LogBatch* g_diagnostic_batch = nullptr;
void write_runtime_log(std::string_view text) {
    std::lock_guard<std::mutex> lock(g_runtime_log_mutex);
    // Open a report only for fatal failures in normal player runs.
    if (!g_runtime_log && !g_runtime_log_path.empty() &&
        (text.find("[RR64-CRASH]") != std::string_view::npos ||
         text.find("[RR64-EXIT]") != std::string_view::npos)) {
#ifdef _WIN32
        g_runtime_log = _wfsopen(g_runtime_log_path.c_str(), L"w", _SH_DENYNO);
#else
        g_runtime_log = _fsopen(g_runtime_log_path.c_str(), "w", _SH_DENYNO);
#endif
    }
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (g_runtime_log) {
        std::fwrite(text.data(), 1, text.size(), g_runtime_log);
        std::fflush(g_runtime_log);
    }
}
struct DiagnosticReport {
    rr64::LogBatch batch{write_runtime_log};
    DiagnosticReport() { g_diagnostic_batch = &batch; }
    ~DiagnosticReport() { g_diagnostic_batch = nullptr; }
};
void rr64_log(const char* format, ...) {
    if (!detailed_diagnostics_enabled() &&
        !std::strstr(format, "[RR64-CRASH]") && !std::strstr(format, "[RR64-EXIT]") &&
        !std::strstr(format, "[RR64-STACK]")) {
        return;
    }
    char buffer[4096]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (g_diagnostic_batch) {
        g_diagnostic_batch->append(buffer);
    } else {
        write_runtime_log(buffer);
    }
}

void report_preparation_trace() {
    if (!RT64::RR64PreparationTrace::enabled()) { return; }
    // The UI/shutdown consumer formats records; rendering only publishes fixed
    // timing data. Nested stage durations overlap and must not be added together.
    RT64::RR64PreparationTrace::buffer.drain([](const auto& record) {
        std::string stages;
        for (std::size_t i = 0; i < record.stageNs.size(); ++i) {
            if (!record.stageNs[i]) { continue; }
            char text[128];
            std::snprintf(text, sizeof(text), " %s=%.3fms",
                RT64::RR64PipelineDiagnostics::Names[i], double(record.stageNs[i]) / 1.0e6);
            stages += text;
        }
        rr64_log("[RR64-PREPARATION] ordinal=%" PRIu64 " first-writer=%" PRIu64
            " last-writer=%" PRIu64 " start-ns=%" PRIu64 " end-ns=%" PRIu64
            " total-ns=%" PRIu64 " display-list-address=%08X"
            " buffer-pair-growths=%" PRIu64 " buffer-pair-old-bytes=%" PRIu64
            " buffer-pair-new-bytes=%" PRIu64 " output-buffer-growths=%" PRIu64
            " output-buffer-old-bytes=%" PRIu64 " output-buffer-new-bytes=%" PRIu64 "%s\n",
            record.ordinal, record.firstWriter, record.lastWriter, record.startNs,
            record.endNs, record.totalNs, record.displayListAddress,
            record.bufferPairGrowthCount, record.bufferPairOldCapacityBytes, record.bufferPairNewCapacityBytes,
            record.outputBufferGrowthCount, record.outputBufferOldCapacityBytes, record.outputBufferNewCapacityBytes,
            stages.c_str());
    });
    rr64_log("[RR64-PREPARATION] attempts=%" PRIu64 " dropped=%" PRIu64 " counters=cumulative\n",
        RT64::RR64PreparationTrace::buffer.attempts.load(std::memory_order_relaxed),
        RT64::RR64PreparationTrace::buffer.dropped.load(std::memory_order_relaxed));
}
#ifdef _WIN32
std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}
#else
std::filesystem::path executable_directory() {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return self.parent_path();
}
#endif

#ifdef _WIN32
LONG WINAPI rr64_unhandled_exception_filter(EXCEPTION_POINTERS* exception_info) {
    DWORD code = 0;
    void* address = nullptr;
    ULONG_PTR access_operation = static_cast<ULONG_PTR>(-1);
    ULONG_PTR access_address = 0;
    if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr) {
        code = exception_info->ExceptionRecord->ExceptionCode;
        address = exception_info->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_ACCESS_VIOLATION && exception_info->ExceptionRecord->NumberParameters >= 2) {
            access_operation = exception_info->ExceptionRecord->ExceptionInformation[0];
            access_address = exception_info->ExceptionRecord->ExceptionInformation[1];
        }
    }

    const uintptr_t module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t exception_address = reinterpret_cast<uintptr_t>(address);
    const uintptr_t exception_rva = (exception_address >= module_base) ? (exception_address - module_base) : 0;
    const char* access_name = (access_operation == 0) ? "read" :
        (access_operation == 1) ? "write" :
        (access_operation == 8) ? "execute" : "n/a";

    const DWORD crash_thread_id = GetCurrentThreadId();
    rr64_log(
        "\n[RR64-CRASH] Unhandled Windows exception 0x%08lX at %p thread=%lu\n"
        "[RR64-CRASH] module_base=%p rva=0x%" PRIXPTR " access=%s target=0x%" PRIXPTR "\n"
        "[RR64-CRASH] create_gfx=%d window=%d renderer=%d launcher=%d update=%d event_thread=%lu ui_thread=%lu\n"
        "[RR64-CRASH] Runtime log: " RR64_PATH_FORMAT "\n",
        static_cast<unsigned long>(code),
        address,
        static_cast<unsigned long>(crash_thread_id),
        reinterpret_cast<void*>(module_base),
        exception_rva,
        access_name,
        static_cast<uintptr_t>(access_address),
        g_create_gfx_seen ? 1 : 0,
        g_window_created ? 1 : 0,
        g_renderer_created ? 1 : 0,
        g_launcher_init_seen ? 1 : 0,
        g_update_gfx_seen ? 1 : 0,
        static_cast<unsigned long>(g_event_thread_id.load()),
        static_cast<unsigned long>(g_ui_thread_id.load()),
        g_runtime_log_path.c_str()
    );

    // Walk directly from the exception context. Logging module-relative PCs
    // keeps the trace useful with ASLR and lets llvm-symbolizer resolve every
    // statically linked frontend/runtime frame after the process exits.
    if (exception_info != nullptr && exception_info->ContextRecord != nullptr) {
        CONTEXT unwind_context = *exception_info->ContextRecord;
        rr64_log("[RR64-STACK] Native exception stack (module-relative):\n");
        for (unsigned frame_index = 0; frame_index < 32 && unwind_context.Rip != 0; ++frame_index) {
            const DWORD64 pc = unwind_context.Rip;
            MEMORY_BASIC_INFORMATION memory_info{};
            const SIZE_T query_result = VirtualQuery(
                reinterpret_cast<const void*>(static_cast<uintptr_t>(pc)),
                &memory_info,
                sizeof(memory_info)
            );
            const uintptr_t frame_module_base = (query_result != 0)
                ? reinterpret_cast<uintptr_t>(memory_info.AllocationBase)
                : 0;
            const uintptr_t frame_rva = (frame_module_base != 0 && pc >= frame_module_base)
                ? static_cast<uintptr_t>(pc - frame_module_base)
                : 0;
            rr64_log(
                "[RR64-STACK] #%02u pc=%p module=%p rva=0x%" PRIXPTR "\n",
                frame_index,
                reinterpret_cast<void*>(static_cast<uintptr_t>(pc)),
                reinterpret_cast<void*>(frame_module_base),
                frame_rva
            );

            DWORD64 unwind_image_base = 0;
            PRUNTIME_FUNCTION function_entry = RtlLookupFunctionEntry(pc, &unwind_image_base, nullptr);
            const DWORD64 previous_pc = unwind_context.Rip;
            if (function_entry != nullptr) {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(
                    UNW_FLAG_NHANDLER,
                    unwind_image_base,
                    pc,
                    function_entry,
                    &unwind_context,
                    &handler_data,
                    &establisher_frame,
                    nullptr
                );
            }
            else if (unwind_context.Rsp != 0) {
                unwind_context.Rip = *reinterpret_cast<const DWORD64*>(unwind_context.Rsp);
                unwind_context.Rsp += sizeof(DWORD64);
            }
            else {
                break;
            }

            if (unwind_context.Rip == previous_pc) {
                break;
            }
        }
    }

    // Automatic bring-up runs are unattended. Preserve the first crash in the
    // logs and terminate immediately instead of leaving multiple modal dialogs
    // and cascading faults across runtime threads.
    if (g_auto_start_dispatched.load() && !g_auto_rom_path.empty()) {
        std::_Exit(EXIT_FAILURE);
    }

    std::wstring message =
        L"Road Rash 64 Recompiled encountered a runtime error.\n\n"
        L"Crash code: 0x";
    wchar_t code_text[16]{};
    swprintf_s(code_text, 16, L"%08lX", static_cast<unsigned long>(code));
    message += code_text;
    message += L"\n\nA diagnostic log was written to:\n";
    message += g_runtime_log_path.wstring();
    MessageBoxW(nullptr, message.c_str(), L"Road Rash 64 Recompiled - Runtime Crash", MB_OK | MB_ICONERROR | MB_TOPMOST);
    return EXCEPTION_EXECUTE_HANDLER;
}

void rr64_quick_exit_handler() {
    rr64_log(
        "\n[RR64-EXIT] std::quick_exit was invoked during runtime initialization.\n"
        "[RR64-EXIT] create_gfx=%d window=%d renderer=%d launcher=%d update=%d\n",
        g_create_gfx_seen ? 1 : 0,
        g_window_created ? 1 : 0,
        g_renderer_created ? 1 : 0,
        g_launcher_init_seen ? 1 : 0,
        g_update_gfx_seen ? 1 : 0
    );
    std::wstring message =
        L"Road Rash 64 Recompiled exited during runtime initialization.\n\n"
        L"See the diagnostic log:\n";
    message += g_runtime_log_path.wstring();
    MessageBoxW(nullptr, message.c_str(), L"Road Rash 64 Recompiled - Early Exit", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

void rr64_terminate_handler() {
    rr64_log(
        "\n[RR64-CRASH] std::terminate was invoked.\n"
        "[RR64-CRASH] create_gfx=%d window=%d renderer=%d launcher=%d update=%d\n",
        g_create_gfx_seen ? 1 : 0,
        g_window_created ? 1 : 0,
        g_renderer_created ? 1 : 0,
        g_launcher_init_seen ? 1 : 0,
        g_update_gfx_seen ? 1 : 0
    );
    std::wstring message =
        L"Road Rash 64 Recompiled terminated unexpectedly.\n\n"
        L"See the diagnostic log:\n";
    message += g_runtime_log_path.wstring();
    MessageBoxW(nullptr, message.c_str(), L"Road Rash 64 Recompiled - Runtime Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
    std::_Exit(EXIT_FAILURE);
}
#endif

void initialize_runtime_diagnostics() {
#ifdef _WIN32
    g_runtime_log_path = executable_directory() / "RoadRash64Recompiled-runtime.log";
    FILE* opened = detailed_diagnostics_enabled() ? _wfsopen(g_runtime_log_path.c_str(), L"w", _SH_DENYNO) : nullptr;
    if (opened != nullptr) {
        g_runtime_log = opened;
    }
    SetUnhandledExceptionFilter(rr64_unhandled_exception_filter);
    std::set_terminate(rr64_terminate_handler);
    std::at_quick_exit(rr64_quick_exit_handler);
#else
    g_runtime_log_path = std::filesystem::current_path() / "RoadRash64Recompiled-runtime.log";
    // Normal releases do not create a session log; capture is explicitly opt-in.
    g_runtime_log = detailed_diagnostics_enabled() ? std::fopen(g_runtime_log_path.string().c_str(), "w") : nullptr;
#endif

    rr64_log("[RR64-DIAG] Runtime diagnostics enabled.\n");
#ifdef _WIN32
    rr64_log("[RR64-DIAG] Executable directory: " RR64_PATH_FORMAT "\n", executable_directory().c_str());
    rr64_log("[RR64-DIAG] Runtime log: " RR64_PATH_FORMAT "\n", g_runtime_log_path.c_str());
#else
    rr64_log("[RR64-DIAG] Runtime log: %s\n", g_runtime_log_path.string().c_str());
#endif
}

void fatal_sdl(const char* what) {
    rr64_log("[RR64-CRASH] %s: %s\n", what, SDL_GetError());
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    g_create_gfx_seen = true;
    rr64_log("[RR64-STAGE] create_gfx entered.\n");

    SDL_version compiled_version{};
    SDL_version runtime_version{};
    SDL_VERSION(&compiled_version);
    SDL_GetVersion(&runtime_version);
    rr64_log(
        "[RR64-STAGE] SDL compiled=%u.%u.%u runtime=%u.%u.%u\n",
        static_cast<unsigned>(compiled_version.major),
        static_cast<unsigned>(compiled_version.minor),
        static_cast<unsigned>(compiled_version.patch),
        static_cast<unsigned>(runtime_version.major),
        static_cast<unsigned>(runtime_version.minor),
        static_cast<unsigned>(runtime_version.patch)
    );

    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    rr64_log("[RR64-STAGE] Calling SDL_Init(video/input).\n");
    const int init_result = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
    rr64_log("[RR64-STAGE] SDL_Init returned %d (%s).\n", init_result, SDL_GetError());
    if (init_result < 0) {
        fatal_sdl("SDL video/input initialization failed");
    }

    // Register before the first device-added event is processed, including
    // controllers already connected when the launcher starts.
    if (!rr64_register_controller_mappings()) {
        rr64_log("[RR64-INPUT] Controller mapping registration failed: %s\n", SDL_GetError());
    }

    const char* video_driver = SDL_GetCurrentVideoDriver();
    rr64_log("[RR64-STAGE] SDL video driver: %s\n", video_driver != nullptr ? video_driver : "<null>");
    return {};
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    recompui::startup_checkpoint("window-begin");
    rr64_log("[RR64-STAGE] create_window entered.\n");
    uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(PLUME_SDL_VULKAN_ENABLED)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow(
        kProgramName,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        960,
        540,
        flags
    );
    if (window == nullptr) {
        fatal_sdl("SDL window creation failed");
    }
    g_window_created = true;
    rr64_log("[RR64-STAGE] SDL window created: %p flags=0x%08" PRIX32 "\n", static_cast<void*>(window), flags);

    SDL_SysWMinfo wm_info{};
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fatal_sdl("SDL_GetWindowWMInfo failed");
    }

#ifdef _WIN32
    rr64_log("[RR64-STAGE] HWND acquired: %p thread=%lu\n", static_cast<void*>(wm_info.info.win.window), static_cast<unsigned long>(GetCurrentThreadId()));
    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#else
    rr64_log("[RR64-STAGE] SDL window handle acquired: %p\n", static_cast<void*>(window));
    return window;
#endif
}

void update_gfx(void*) {
    if (!g_update_gfx_seen) {
        g_update_gfx_seen = true;
#ifdef _WIN32
        g_event_thread_id.store(GetCurrentThreadId());
#endif
        rr64_log("[RR64-STAGE] update_gfx event loop entered.\n");
    }

    // Keep exceptions from the frontend/input event thread from being converted
    // into an opaque std::terminate while the renderer is starting. The renderer
    // thread has its own diagnostics; this identifies an independent event-loop
    // failure if one exists.
    try {
        recompinput::handle_events();
    }
    catch (const std::exception& ex) {
        rr64_log("[RR64-EVENT] C++ exception in recompinput::handle_events: %s\n", ex.what());
        SDL_Delay(10);
    }
    catch (...) {
        rr64_log("[RR64-EVENT] Unknown exception in recompinput::handle_events.\n");
        SDL_Delay(10);
    }

    // Direct host/client sessions continue while the in-game menus and race are
    // active. rr64_netplay internally throttles packet intervals.
    rr64::netplay::update();

    // The remaining work only drains opt-in reports, not game or network state.
    if (!detailed_diagnostics_enabled()) { return; }
    // Aggregate presentation timing off the renderer thread.
    const auto now = std::chrono::steady_clock::now();
    if (rr64::rider_skins::preview_trace_enabled()) {
        rr64::rider_skins::PreviewTrace sample;
        // At most 128 records per process; no formatting/file I/O on the
        // native render worker and no extra per-frame disk flushing.
        while (rr64::rider_skins::take_preview_trace(sample)) {
            rr64_log("[RR64-SKIN-PREVIEW] reason=%u root=%08X appearance=%u mode=%u handler=%08X epoch=%u gfx=%u pool=%08X head=%08X base=%08X active=%08X capacity=%u start=%08X end=%08X commands=%u rejected=%08X replacements=%u list=%08X dropped=%u\n",
                sample.reason,sample.root,sample.appearance,sample.mode,sample.handler,sample.epoch,
                sample.gfx,sample.pool,sample.head,sample.base,sample.active_base,sample.capacity,
                sample.start,sample.end,sample.commands,sample.rejected_opcode,sample.replacements,
                sample.list,rr64::rider_skins::preview_trace_dropped());
            for (unsigned i=0;i<4;++i)
                rr64_log("[RR64-SKIN-PREVIEW] root=%08X slot=%u choice=%u donor=%u header=%08X\n",
                    sample.root,i,sample.choices[i],sample.donors[i],sample.headers[i]);
            for (const auto &actor:sample.actors) if (actor[0])
                rr64_log("[RR64-SKIN-PREVIEW] root=%08X actor=%08X type=%u entity=%08X slot=%u graph=%08X\n",
                    sample.root,actor[0],actor[1],actor[2],actor[3],actor[4]);
            for (unsigned i=0;i<sample.image_count;++i)
                rr64_log("[RR64-SKIN-PREVIEW] root=%08X image=%08X address=%08X\n",
                    sample.root,sample.images[i][0],sample.images[i][1]);
        }
    }
    static bool previous_gameplay_feedback=false;
    static auto boundary_log_at=std::chrono::steady_clock::time_point::max();
    const bool gameplay_feedback=rr64_is_gameplay_feedback_active()!=0;
    if(previous_gameplay_feedback&&!gameplay_feedback)boundary_log_at=now+std::chrono::seconds(1);
    previous_gameplay_feedback=gameplay_feedback;
    if (detailed_diagnostics_enabled() && ((now - g_last_present_health_log) >= std::chrono::seconds(10) || now>=boundary_log_at)) {
        DiagnosticReport report;
        boundary_log_at=std::chrono::steady_clock::time_point::max();
        const double health_interval_seconds =
            std::chrono::duration<double>(now - g_last_present_health_log).count();
        g_last_present_health_log = now;
        for (std::size_t i = 0; i < g_scheduler_stages.size(); ++i) {
            auto& stage = g_scheduler_stages[i];
            const auto samples = stage.samples.exchange(0);
            const auto total = stage.total_ns.exchange(0);
            const auto maximum = stage.maximum_ns.exchange(0);
            if (samples > 0) {
                rr64_log("[RR64-SCHEDULER] stage=%s samples=%" PRIu64
                    " avg=%.3fms max=%.3fms total=%.3fms window=%.3fs\n",
                    ultramodern::rr64_diagnostics::StageNames[i], samples,
                    double(total) / samples / 1.0e6, double(maximum) / 1.0e6,
                    double(total) / 1.0e6, health_interval_seconds);
            }
        }
        for (std::size_t i = 0; i < g_scheduler_events.size(); ++i) {
            rr64_log("[RR64-SCHEDULER] event=%s count=%" PRIu64 " window=%.3fs\n",
                ultramodern::rr64_diagnostics::EventNames[i],
                g_scheduler_events[i].exchange(0), health_interval_seconds);
        }
        for (auto& queue : g_guest_queue_counters) {
            const auto samples = queue.timing.samples.exchange(0);
            const auto total = queue.timing.total_ns.exchange(0);
            const auto maximum = queue.timing.maximum_ns.exchange(0);
            if (samples > 0) {
                rr64_log("[RR64-SCHEDULER] guest-recv queue=%08" PRIX32
                    " samples=%" PRIu64 " avg=%.3fms max=%.3fms total=%.3fms window=%.3fs\n",
                    queue.queue.load(), samples, double(total) / samples / 1.0e6,
                    double(maximum) / 1.0e6, double(total) / 1.0e6, health_interval_seconds);
            }
            const auto delivered = queue.external_outcomes[0].exchange(0);
            const auto requeued = queue.external_outcomes[1].exchange(0);
            const auto dropped = queue.external_outcomes[2].exchange(0);
            const auto delivery_samples = queue.external_delivery.samples.exchange(0);
            const auto delivery_total = queue.external_delivery.total_ns.exchange(0);
            const auto delivery_maximum = queue.external_delivery.maximum_ns.exchange(0);
            if (delivered + requeued + dropped + delivery_samples > 0) {
                rr64_log("[RR64-SCHEDULER] external-queue=%08" PRIX32
                    " delivered=%" PRIu64 " requeued=%" PRIu64 " dropped=%" PRIu64
                    " latency-samples=%" PRIu64 " delivery-avg=%.3fms delivery-max=%.3fms window=%.3fs\n",
                    queue.queue.load(), delivered, requeued, dropped, delivery_samples,
                    delivery_samples ? double(delivery_total) / delivery_samples / 1.0e6 : 0.0,
                    double(delivery_maximum) / 1.0e6, health_interval_seconds);
            }
        }
        rr64_log("[RR64-SCHEDULER] queue-overflow-samples=%" PRIu64 "\n",
            g_guest_queue_overflow_samples.exchange(0));
        report_preparation_trace();
        for (std::size_t i = 0; i < g_pipeline_stages.size(); ++i) {
            auto& stage = g_pipeline_stages[i];
            const auto stage_samples = stage.samples.exchange(0);
            const auto stage_total = stage.total_ns.exchange(0);
            const auto stage_maximum = stage.maximum_ns.exchange(0);
            if (stage_samples > 0) {
                rr64_log("[RR64-PIPELINE] stage=%s samples=%" PRIu64
                    " avg=%.3fms max=%.3fms total=%.3fms window=%.3fs\n",
                    RT64::RR64PipelineDiagnostics::Names[i], stage_samples,
                    double(stage_total) / stage_samples / 1.0e6,
                    double(stage_maximum) / 1.0e6, double(stage_total) / 1.0e6,
                    health_interval_seconds);
            }
        }
        rr64_log("[RR64-PIPELINE] resources targets=%" PRIu64 " target-base-mib=%.2f"
            " owned-batches=%" PRIu64 " owned-mib=%.2f textures=%" PRIu64
            " retired-textures=%" PRIu64 " estimates-not-total-vram=1\n",
            g_pipeline_resources[0].load(), double(g_pipeline_resources[1].load()) / 1048576.0,
            g_pipeline_resources[2].load(), double(g_pipeline_resources[3].load()) / 1048576.0,
            g_pipeline_resources[4].load(), g_pipeline_resources[5].load());
        rr64_log("[RR64-PIPELINE] saved-frame-reuse hits=%" PRIu64 " misses=%" PRIu64
            " pool-mib=%.2f pool-images=%" PRIu64 " counters=cumulative\n",
            g_pipeline_resources[6].load(), g_pipeline_resources[7].load(),
            double(g_pipeline_resources[8].load()) / 1048576.0, g_pipeline_resources[9].load());
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        DWORD handles = 0;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) &&
            GetProcessHandleCount(GetCurrentProcess(), &handles)) {
            rr64_log("[RR64-PIPELINE] process private-mib=%.2f working-set-mib=%.2f handles=%lu\n",
                double(memory.PrivateUsage) / 1048576.0,
                double(memory.WorkingSetSize) / 1048576.0, handles);
        }
#endif
        const std::uint64_t samples = g_present_interval_samples.exchange(0);
        const std::uint64_t total_us = g_present_interval_total_us.exchange(0);
        const std::uint64_t over_budget = g_present_interval_over_budget.exchange(0);
        const std::uint32_t worst_us = g_present_interval_worst_us.exchange(0);
        const std::uint64_t late_before_timer = g_present_late_before_timer.exchange(0);
        const std::uint64_t late_present_call =
            g_present_late_present_call.exchange(0);
        const std::uint64_t late_timer = g_present_late_timer.exchange(0);
        const std::uint64_t late_other = g_present_late_other.exchange(0);
        const std::uint32_t worst_before_timer_us =
            g_present_worst_before_timer_us.exchange(0);
        const std::uint32_t worst_timer_overrun_us =
            g_present_worst_timer_overrun_us.exchange(0);
        const std::uint32_t worst_present_call_us =
            g_present_worst_present_call_us.exchange(0);
        const std::uint64_t interpolation_workloads =
            g_interpolation_workloads.exchange(0);
        std::array<std::uint64_t, 3> production_image_counts{};
        for (std::size_t i = 0; i < production_image_counts.size(); ++i) {
            production_image_counts[i] = g_production_image_counts[i].exchange(0);
        }
        const std::uint64_t interpolation_planned_frames =
            g_interpolation_planned_frames.exchange(0);
        const std::uint64_t interpolation_rendered_frames =
            g_interpolation_rendered_frames.exchange(0);
        const std::uint64_t interpolation_backlog_dropped_frames =
            g_interpolation_backlog_dropped_frames.exchange(0);
        const std::uint64_t interpolation_unavailable_workloads =
            g_interpolation_unavailable_workloads.exchange(0);
        const std::uint64_t interpolation_stock_flag_workloads =
            g_interpolation_stock_flag_workloads.exchange(0);
        const std::uint64_t interpolation_history_target_workloads =
            g_interpolation_history_target_workloads.exchange(0);
        const auto consumer_interpolated = g_consumer_interpolated_batches.exchange(0);
        const auto consumer_repeated = g_consumer_repeated_batches.exchange(0);
        const auto consumer_rejected = g_consumer_rejected_batches.exchange(0);
        const auto owned_refused = g_owned_produced[0].exchange(0);
        const auto owned_published = g_owned_produced[1].exchange(0);
        const auto owned_missed = g_owned_presented[0].exchange(0);
        const auto owned_hits = g_owned_presented[1].exchange(0);
        const auto geometry_fallback = g_geometry_compatible[0].exchange(0);
        const auto geometry_accepted = g_geometry_compatible[1].exchange(0);
        std::array<std::uint64_t, 6> geometry_reasons{};
        for (std::size_t i = 0; i < geometry_reasons.size(); ++i) {
            geometry_reasons[i] = g_geometry_reasons[i].exchange(0);
        }
        if (geometry_fallback + geometry_accepted > 0) {
            rr64_log("[RR64-PERF] geometry compatible=%" PRIu64 " native-fallback=%" PRIu64
                " reasons=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
                geometry_accepted, geometry_fallback, geometry_reasons[0], geometry_reasons[1],
                geometry_reasons[2], geometry_reasons[3], geometry_reasons[4], geometry_reasons[5]);
        }
        RT64::RR64MatchingEvidence::buffer.drain([](const RT64::RR64MatchingEvidence::Record &r) {
            if(r.category==3) {
                rr64_log("[RR64-MATCH-CAMERA] marker=%08X round=%u layout=%u views=%u replay-key=%u valid-mask=%u modes=%u,%u,%u,%u owners=%u,%u,%u,%u flags=%u,%u,%u,%u generations=%u,%u,%u,%u bikes=%08X,%08X,%08X,%08X riders=%08X,%08X,%08X,%08X\n",
                    r.cameraMarker,r.cameraRound,r.cameraLayout,r.cameraViews,r.cameraReplayKey,r.cameraValidMask,
                    r.cameraModes[0],r.cameraModes[1],r.cameraModes[2],r.cameraModes[3],
                    r.cameraOwners[0],r.cameraOwners[1],r.cameraOwners[2],r.cameraOwners[3],
                    r.cameraFlags[0],r.cameraFlags[1],r.cameraFlags[2],r.cameraFlags[3],
                    r.cameraGenerations[0],r.cameraGenerations[1],r.cameraGenerations[2],r.cameraGenerations[3],
                    r.cameraBikes[0],r.cameraBikes[1],r.cameraBikes[2],r.cameraBikes[3],
                    r.cameraRiders[0],r.cameraRiders[1],r.cameraRiders[2],r.cameraRiders[3]);
                return;
            }
            rr64_log("[RR64-MATCH-EVIDENCE] category=%u submission=%llu workload=%llu fb=%u proj=%u view=%u world=%u previous=%u id=%08X previous-id=%08X occurrences=%u/%u mapped=%u policy=%u order=%u lerp=%u/%u vertices=%u indices=%u topology=%llu\n",
                r.category,(unsigned long long)r.submission,(unsigned long long)r.workload,r.framebuffer,r.projection,r.view,r.world,r.previousWorld,
                r.id,r.previousId,r.idOccurrences,r.previousIdOccurrences,unsigned(r.mapped),r.positionPolicy,r.ordering,unsigned(r.worldLerp),unsigned(r.viewLerp),r.vertices,r.indices,(unsigned long long)r.topologyHash);
            rr64_log("[RR64-MATCH-EVIDENCE-POS] category=%u submission=%llu world=%u world-before=%.9g,%.9g,%.9g world-after=%.9g,%.9g,%.9g view-before=%.9g,%.9g,%.9g view-after=%.9g,%.9g,%.9g\n",
                r.category,(unsigned long long)r.submission,r.world,r.worldBefore[0],r.worldBefore[1],r.worldBefore[2],r.worldAfter[0],r.worldAfter[1],r.worldAfter[2],
                r.viewBefore[0],r.viewBefore[1],r.viewBefore[2],r.viewAfter[0],r.viewAfter[1],r.viewAfter[2]);
            rr64_log("[RR64-MATCH-EVIDENCE-SOURCE] category=%u submission=%llu world=%u mask=%u physical=%08X segmented=%08X previous-physical=%08X previous-segmented=%08X view-id=%08X previous-view-id=%08X\n",
                r.category,(unsigned long long)r.submission,r.world,r.sourceMask,r.physicalAddress,r.segmentedAddress,r.previousPhysicalAddress,r.previousSegmentedAddress,r.viewId,r.previousViewId);
        });
        if (RT64::RR64CommandProfile::enabled()) {
            for (unsigned opcode=0;opcode<RT64::RR64CommandProfile::buckets.size();opcode++) {
                const auto &bucket=RT64::RR64CommandProfile::buckets[opcode];
                const auto samples=bucket.samples.load(std::memory_order_relaxed);
                if (!samples) { continue; }
                const auto ns=bucket.nanoseconds.load(std::memory_order_relaxed);
                rr64_log("[RR64-COMMAND-PROFILE] extended=%u opcode=%02X cumulative-samples=%llu cumulative-ns=%llu nominal-rate=1024 approximate-snapshot=1\n",
                    opcode>=256,opcode&255u,(unsigned long long)samples,(unsigned long long)ns);
            }
        }
        std::array<std::uint64_t,
            static_cast<std::size_t>(plume::D3D12PresentOutcome::Count)> present_outcomes{};
        std::uint64_t present_attempts = 0;
        for (std::size_t i = 0; i < present_outcomes.size(); ++i) {
            present_outcomes[i] = g_d3d12_present_outcomes[i].exchange(0);
            present_attempts += present_outcomes[i];
        }
        const std::uint32_t last_present_error = g_d3d12_last_present_error.exchange(0);
        if (present_attempts > 0) {
            const auto accepted = present_outcomes[static_cast<std::size_t>(plume::D3D12PresentOutcome::Accepted)];
            const auto busy = present_outcomes[static_cast<std::size_t>(plume::D3D12PresentOutcome::Busy)];
            const auto occluded = present_outcomes[static_cast<std::size_t>(plume::D3D12PresentOutcome::Occluded)];
            const auto focus_deferred = present_outcomes[static_cast<std::size_t>(plume::D3D12PresentOutcome::FocusDeferred)];
            const auto errors = present_outcomes[static_cast<std::size_t>(plume::D3D12PresentOutcome::Error)];
            rr64_log("[RR64-PERF] d3d12-present window=%.3fs attempts=%" PRIu64
                " dxgi-calls=%" PRIu64 " accepted=%" PRIu64 " busy=%" PRIu64
                " occluded=%" PRIu64 " focus-deferred=%" PRIu64 " errors=%" PRIu64
                " last-error=0x%08" PRIX32 " accepted-is-not-scanout=1\n",
                health_interval_seconds, present_attempts, present_attempts - focus_deferred,
                accepted, busy, occluded, focus_deferred, errors, last_present_error);
        }
        std::array<std::uint64_t, 4> authored_decisions{};
        std::uint64_t authored_samples = 0;
        for (std::size_t i = 0; i < authored_decisions.size(); ++i) {
            authored_decisions[i] = g_authored_timing_decisions[i].exchange(0);
            authored_samples += authored_decisions[i];
        }
        const auto authored_intervals = g_authored_timing_interval_samples.exchange(0);
        const auto authored_total_ns = g_authored_timing_interval_total_ns.exchange(0);
        const auto authored_max_ns = g_authored_timing_interval_max_ns.exchange(0);
        const auto authored_camera_same = g_authored_camera_changes[0].exchange(0);
        const auto authored_camera_changed = g_authored_camera_changes[1].exchange(0);
        const auto authored_world_same = g_authored_world_changes[0].exchange(0);
        const auto authored_world_changed = g_authored_world_changes[1].exchange(0);
        if (authored_samples > 0) {
            rr64_log("[RR64-PERF] authored-timing window=%.3fs source=%u samples=%" PRIu64
                " warmup=%" PRIu64 " cadence60=%" PRIu64 " cadence30=%" PRIu64
                " discontinuity=%" PRIu64
                " interval-samples=%" PRIu64 " interval-avg=%.3fms interval-max=%.3fms\n",
                health_interval_seconds, g_authored_timing_source_rate.load(std::memory_order_relaxed),
                authored_samples, authored_decisions[0], authored_decisions[1], authored_decisions[2],
                authored_decisions[3], authored_intervals,
                authored_intervals ? double(authored_total_ns) / double(authored_intervals) / 1.0e6 : 0.0,
                double(authored_max_ns) / 1.0e6);
            rr64_log("[RR64-PERF] authored-motion window=%.3fs camera-same=%" PRIu64
                " camera-changed=%" PRIu64 " world-same=%" PRIu64 " world-changed=%" PRIu64
                " comparison=consecutive-matrix-hashes\n",
                health_interval_seconds, authored_camera_same, authored_camera_changed,
                authored_world_same, authored_world_changed);
        }
        std::array<std::uint64_t, 4> present_sequence_modes{};
        std::uint64_t present_sequence_events = 0;
        for (std::size_t i = 0; i < present_sequence_modes.size(); ++i) {
            present_sequence_modes[i] = g_present_sequence_modes[i].exchange(0);
            present_sequence_events += present_sequence_modes[i];
        }
        if (present_sequence_events > 0) {
            rr64_log("[RR64-PERF] present-sequence window=%.3fs events=%" PRIu64
                " native-target=%" PRIu64 " owned-native=%" PRIu64
                " owned-fractional=%" PRIu64 " skipped-queued=%" PRIu64 "\n",
                health_interval_seconds, present_sequence_events, present_sequence_modes[0],
                present_sequence_modes[1], present_sequence_modes[2], present_sequence_modes[3]);
        }
        if (owned_refused + owned_published + owned_missed + owned_hits > 0) {
            rr64_log("[RR64-PERF] owned-batches published=%" PRIu64
                " refused=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64 "\n",
                owned_published, owned_refused, owned_hits, owned_missed);
        }
        std::array<std::uint64_t, 5> rejection_reasons{};
        for (std::size_t reason = 0; reason < rejection_reasons.size(); ++reason) {
            rejection_reasons[reason] = g_consumer_rejection_reasons[reason].exchange(0);
        }
        if (rr64_world_distance_enabled()) {
            const auto world = rr64::world::terrain_statistics();
            rr64_log("[RR64-WORLD] terrain cached=%u cached-triangles=%u bytes=%u visible=%u stock=%u stock-replaced=%u drawn-triangles=%u frames=%llu refused=%llu far-target=%.0f shadow-tags=%llu\n",
                world.cached_cells, world.cached_triangles, world.cached_bytes, world.visible_cells,
                world.stock_cells, world.replaced_stock_cells, world.drawn_triangles, world.frames, world.refusals, rr64::world::far_distance, rr64_shadow_tags_count());
            rr64_log("[RR64-WORLD] course-excluded-terrain-cells=%u\n",world.course_excluded_cells);
            rr64_log("[RR64-WORLD] stock-course-excluded=%u\n",world.stock_course_excluded);
            if(world.evidence.enabled) {
                const auto& evidence=world.evidence;
                rr64_log("[RR64-COURSE] schema=1 generation=%u ownership=unknown stock=post-filter-union extended=last-pass grid=70x70 bit=row*70+column\n",evidence.generation);
                std::string setup;
                for(auto value:evidence.raw_setup){char hex[16];std::snprintf(hex,sizeof(hex),"%08x,",value);setup+=hex;}
                rr64_log("[RR64-COURSE] raw-setup-mode-pending-engine-words=%s\n",setup.c_str());
                for(unsigned i=0;i<evidence.views.size();++i) {
                    const auto& v=evidence.views[i];if(!v.valid)continue;
                    rr64_log("[RR64-COURSE] view=%u epoch=%u origin=%.3f,%.3f stock=%u extended=%u triangles=%u island-excluded=%u\n",
                        i,v.epoch,v.x,v.y,v.stock,v.extended,v.triangles,v.excluded);
                    const auto bitmap=[](const auto& words){std::string out;out.reserve(1309);
                        for(auto word:words){char hex[20];std::snprintf(hex,sizeof(hex),"%016llx,",word);out+=hex;}return out;};
                    rr64_log("[RR64-COURSE] view=%u stock-union=%s\n",i,bitmap(v.stock_union).c_str());
                    rr64_log("[RR64-COURSE] view=%u extended-last=%s\n",i,bitmap(v.extended_last).c_str());
                    const auto float_bits=[](const auto& values){std::string out;
                        for(float value:values){uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));char hex[12];std::snprintf(hex,sizeof(hex),"%08x,",bits);out+=hex;}return out;};
                    rr64_log("[RR64-TERRAIN-REPLAY] schema=1 view=%u epoch=%u origin-width-f32=%s matrix-layout=row-major\n",
                        i,v.epoch,float_bits(std::array<float,3>{v.x,v.y,v.view_width}).c_str());
                    rr64_log("[RR64-TERRAIN-REPLAY] view=%u epoch=%u view-f32=%s projection-f32=%s\n",
                        i,v.epoch,float_bits(v.view).c_str(),float_bits(v.projection).c_str());
                    rr64_log("[RR64-TERRAIN-REPLAY] view=%u epoch=%u stock-last=%s\n",i,v.epoch,bitmap(v.stock_last).c_str());
                }
            }
            static std::array<unsigned long long, 5> previous_world{};
            std::array<unsigned long long, 5> current_world{};
            for (unsigned i = 0; i < current_world.size(); ++i) current_world[i] = rr64_world_counter(i);
            rr64_log("[RR64-WORLD] actors pedestrians=%llu traffic=%llu objects=%llu fallback=%llu refused=%llu\n",
                current_world[0]-previous_world[0], current_world[1]-previous_world[1],
                current_world[2]-previous_world[2], current_world[3]-previous_world[3],
                current_world[4]-previous_world[4]);
            previous_world = current_world;
            const auto objects = rr64::world::objects_statistics();
            rr64_log("[RR64-WORLD] scenery models=%u cached=%u bytes=%u visible=%u stock=%u triangles=%u frames=%llu refused=%llu unmatched=%llu texture-syncs=%llu\n",
                objects.cached_models, objects.cached_placements, objects.cached_bytes,
                objects.visible_placements, objects.stock_placements, objects.drawn_triangles,
                objects.frames, objects.refusals, objects.unmatched_stock, objects.texture_syncs);
        }
        std::array<unsigned long long,8> weaponCounts{};
        for(unsigned i=0;i<weaponCounts.size();++i)weaponCounts[i]=rr64_weapon_counter(i);
        if(weaponCounts[6])rr64_log("[RR64-WEAPON] per-view-root-refreshed=%llu\n",weaponCounts[6]);
        if(weaponCounts[7])rr64_log("[RR64-WEAPON] certified-rider-root=%llu\n",weaponCounts[7]);
        const auto weaponReport=rr64::weapon::take_report();
        if(weaponReport.examined)rr64_log("[RR64-WEAPON-POSE] examined=%u keys=%u omitted=%u\n",weaponReport.examined,weaponReport.size,weaponReport.omitted);
        for(unsigned i=0;i<weaponReport.size;++i){const auto& s=weaponReport.samples[i];
            rr64_log("[RR64-WEAPON-POSE] node=%08X graph=%08X root=%08X view=%u model=%08X lod=%u source=%u parent-source=%u dist2=%.3f parent=(%.3f,%.3f,%.3f) float=(%.3f,%.3f,%.3f) packed=(%.3f,%.3f,%.3f)\n",
                s.node,s.graph,s.record,s.view,s.model,s.lod,s.source,s.parentSource,s.distanceSquared,
                s.parent[0],s.parent[1],s.parent[2],s.translation[0],s.translation[1],s.translation[2],s.packed[0],s.packed[1],s.packed[2]);
        }
        if(weaponCounts[0])rr64_log("[RR64-WEAPON] calls=%llu accepted=%llu roots=%llu corrected=%llu alternate-calls=%llu source-converted=%llu\n",
            weaponCounts[0],weaponCounts[1],weaponCounts[2],weaponCounts[3],weaponCounts[4],weaponCounts[5]);
        const auto rootRanges=rr64::lod::take_root_range_report();
        if(rootRanges.examined){
            rr64_log("[RR64-ROOT-RANGE] pre-normalization examined=%llu near-limit=%llu replaced=%llu\n",
                (unsigned long long)rootRanges.examined,(unsigned long long)rootRanges.nearLimit,(unsigned long long)rootRanges.replaced);
            for(const auto& v:rootRanges.samples)if(v.node)rr64_log("[RR64-ROOT-RANGE] node=%08x record=%08x type=%u view=%u epoch=%u asset-source=%u xyz=%.3f,%.3f,%.3f maximum=%.3f\n",
                v.node,v.record,v.type,v.view,v.epoch,v.source,v.x,v.y,v.z,v.maximum);
        }
        if (rr64_render_only_max_lod_enabled()) {
            static std::array<unsigned long long, 4> previous_lod_stats{};
            std::array<unsigned long long, 4> lod_stats{};
            rr64_lod_read_stats(&lod_stats[0], &lod_stats[1], &lod_stats[2], &lod_stats[3]);
            rr64_log("[RR64-LOD] shadow-preparations=%llu published-pairs=%llu"
                " consumed-actors=%llu stock-fallbacks=%llu\n",
                lod_stats[0] - previous_lod_stats[0], lod_stats[1] - previous_lod_stats[1],
                lod_stats[2] - previous_lod_stats[2], lod_stats[3] - previous_lod_stats[3]);
            previous_lod_stats = lod_stats;
            static rr64::lod::ActivitySnapshot previous_activity;
            const auto lod_activity = rr64::lod::read_activity();
            const auto delta = [&](rr64::lod::ActivityCounter counter) -> unsigned long long {
                const auto index = static_cast<std::size_t>(counter);
                return lod_activity.counts[index] - previous_activity.counts[index];
            };
            using LodCounter = rr64::lod::ActivityCounter;
            rr64_log("[RR64-LOD] activity draws=%llu observed-pairs=%llu queued-pairs=%llu"
                " prepare-calls=%llu empty-prepares=%llu selector-calls=%llu inactive-selectors=%llu"
                " scene-rejected=%llu allocations=%llu allocations-accepted=%llu"
                " preflight-rejected=%llu stage-rejected=%llu publish-rejected=%llu\n",
                delta(LodCounter::Draw), delta(LodCounter::PairObserved), delta(LodCounter::PairQueued),
                delta(LodCounter::Prepare), delta(LodCounter::EmptyPrepare), delta(LodCounter::Select),
                delta(LodCounter::InactiveSelect), delta(LodCounter::SceneRejected),
                delta(LodCounter::Allocation), delta(LodCounter::AllocationAccepted),
                delta(LodCounter::PreflightRejected), delta(LodCounter::StageRejected),
                delta(LodCounter::PublishRejected));
            rr64_log("[RR64-LOD] last-scene mode=%u pending=%u views=%u race-players=%u"
                " menu-players=%u compiled-renderer=%u\n", lod_activity.mode, lod_activity.pending,
                lod_activity.view_count, lod_activity.race_players, lod_activity.setup_players,
                lod_activity.compiled_renderer);
            for (unsigned view = 0; view < 4; ++view) {
                rr64_log("[RR64-LOD-VIEW] camera=%u cumulative-pairs=%llu detailed-actors=%llu fallback-actors=%llu\n",
                    view + 1u, static_cast<unsigned long long>(lod_activity.view_published[view]),
                    static_cast<unsigned long long>(lod_activity.view_detailed[view]),
                    static_cast<unsigned long long>(lod_activity.view_fallback[view]));
            }
            rr64_log("[RR64-LOD] pose-stages missing-bike-root=%llu missing-rider-root=%llu"
                " missing-rider-animation=%llu missing-bike-animation=%llu\n",
                delta(LodCounter::MissingBikeRoot), delta(LodCounter::MissingRiderRoot),
                delta(LodCounter::MissingRiderAnimation), delta(LodCounter::MissingBikeAnimation));
            rr64_log("[RR64-LOD] consumption slot0=%llu slot1=%llu stock-lod-mismatch=%llu"
                " binding-rejected=%llu\n", delta(LodCounter::ConsumedSlot0),
                delta(LodCounter::ConsumedSlot1), delta(LodCounter::StockLodMismatch),
                delta(LodCounter::BindingRejected));
            rr64_log("[RR64-LOD] detailed-promotions bike-from-lod1=%llu bike-from-lod2=%llu"
                " rider-from-lod1=%llu rider-from-lod2=%llu\n",
                delta(LodCounter::BikeTier1ToMax), delta(LodCounter::BikeTier2ToMax),
                delta(LodCounter::RiderTier1ToMax), delta(LodCounter::RiderTier2ToMax));
            rr64_log("[RR64-LOD] source-normalized bike=%llu rider=%llu\n",
                delta(LodCounter::BikeFarNormalized), delta(LodCounter::RiderFarNormalized));
            rr64_log("[RR64-LOD] camera-clip near=%.3f far=%.3f\n",
                lod_activity.camera_near, lod_activity.camera_far);
            rr64_log("[RR64-LOD] rider-range observed=%llu restored=%llu rejected=%llu far-fallback-bike=%llu far-fallback-rider=%llu\n",
                delta(LodCounter::RiderRangeObserved), delta(LodCounter::RiderRangeRestored),
                delta(LodCounter::RiderRangeRejected), delta(LodCounter::FarBikeFallback), delta(LodCounter::FarRiderFallback));
            rr64_log("[RR64-LOD] held-bike-prepared=%llu full-weight-rider-prepared=%llu held-results-prepared=%llu finish-blend-prepared=%llu\n",
                delta(LodCounter::HeldBikePrepared), delta(LodCounter::FullWeightRiderPrepared),
                delta(LodCounter::HeldResultsPrepared), delta(LodCounter::FinishBlendPrepared));
            rr64_log("[RR64-LOD] finish-history-seeds=%llu\n",delta(LodCounter::FinishSeedPrepared));
            static constexpr std::array failure_names{"none", "context", "missing-pair", "generation", "view", "slot",
                "ownership", "capture", "stock-state", "resource", "allocation", "isolation", "root-plan", "source-bank"};
            static_assert(failure_names.size() == static_cast<std::size_t>(rr64::lod::FindFailure::Count));
            for (std::size_t i = 0; i < lod_activity.actor_details.size(); ++i) {
                const auto& detail = lod_activity.actor_details[i];
                const auto& previous = previous_activity.actor_details[i];
                const auto detailed = detail.detailed - previous.detailed;
                const auto fallback = detail.fallback - previous.fallback;
                if (detailed + fallback == 0u) { continue; }
                rr64_log("[RR64-LOD-ACTOR] racer=%zu type=%s node=%08X detailed=%llu fallback=%llu"
                    " last-detailed-distance=%.2f last-fallback-distance=%.2f last-fallback-lod=%u reason=%s"
                    " detailed-list=%08X fallback-list=%08X last-fallback-state-valid=%u"
                    " last-fallback-attached=%u last-fallback-ejected=%u last-fallback-bike-state=%u"
                    " last-fallback-contact=%u last-fallback-speed=%.2f last-fallback-transition=%.3f"
                    " rider-style=%u fallback-tier0=%llu fallback-tier1=%llu fallback-tier2=%llu\n",
                    i / 2u, (i % 2u) == 0u ? "bike" : "rider", detail.node,
                    static_cast<unsigned long long>(detailed), static_cast<unsigned long long>(fallback),
                    detail.detailed_distance, detail.fallback_distance, detail.stock_lod,
                    failure_names[static_cast<std::size_t>(detail.last_failure)], detail.detail_list, detail.fallback_list,
                    detail.fallback_state_valid, detail.fallback_attached, detail.fallback_ejected,
                    detail.fallback_bike_state, detail.fallback_contact_phase, detail.fallback_speed, detail.fallback_transition,
                    detail.rider_style,
                    static_cast<unsigned long long>(detail.fallback_by_lod[0] - previous.fallback_by_lod[0]),
                    static_cast<unsigned long long>(detail.fallback_by_lod[1] - previous.fallback_by_lod[1]),
                    static_cast<unsigned long long>(detail.fallback_by_lod[2] - previous.fallback_by_lod[2]));
            }
            for (std::size_t i = 0; i < failure_names.size(); ++i) {
                const auto count = lod_activity.find_failures[i] - previous_activity.find_failures[i];
                if (count) {
                    rr64_log("[RR64-LOD] consumer-rejected reason=%s count=%" PRIu64 "\n",
                        failure_names[i], count);
                }
            }
            previous_activity = lod_activity;
        }
        if (consumer_interpolated + consumer_repeated > 0) {
            rr64_log("[RR64-PERF] consumer batches-interpolated=%" PRIu64
                " batches-repeated=%" PRIu64 " metadata-rejected=%" PRIu64 "\n",
                consumer_interpolated, consumer_repeated, consumer_rejected);
            if (consumer_rejected > 0) {
                rr64_log("[RR64-PERF] rejected-batch workload=%" PRIu64
                    " scene=%" PRIu64 " target=%" PRIu64 " source-rate=%" PRIu64
                    " host-rate=%" PRIu64 "\n",
                    rejection_reasons[0], rejection_reasons[1], rejection_reasons[2],
                    rejection_reasons[3], rejection_reasons[4]);
            }
        }
        if (samples > 0) {
            rr64_log(
                "[RR64-PERF] present target=%u original=%u samples=%" PRIu64
                " avg=%.2fms worst=%.2fms late=%" PRIu64
                " cause-prepare=%" PRIu64 " cause-present=%" PRIu64
                " cause-timer=%" PRIu64 " cause-other=%" PRIu64
                " worst-prepare=%.2fms worst-present=%.2fms worst-over=%.2fms\n",
                g_present_target_rate.load(std::memory_order_relaxed),
                g_present_original_rate.load(std::memory_order_relaxed),
                samples,
                static_cast<double>(total_us) / static_cast<double>(samples) / 1000.0,
                static_cast<double>(worst_us) / 1000.0,
                over_budget,
                late_before_timer,
                late_present_call,
                late_timer,
                late_other,
                static_cast<double>(worst_before_timer_us) / 1000.0,
                static_cast<double>(worst_present_call_us) / 1000.0,
                static_cast<double>(worst_timer_overrun_us) / 1000.0);
        }
        if (interpolation_workloads > 0) {
            rr64_log(
                "[RR64-PERF] frame-production target=%u original=%u workloads=%" PRIu64
                " planned=%" PRIu64 " rendered=%" PRIu64
                " zero-image=%" PRIu64 " single-image=%" PRIu64 " multi-image=%" PRIu64
                " backlog-dropped=%" PRIu64 " stock=%" PRIu64
                " history=%" PRIu64 " unavailable=%" PRIu64
                " source-factor=%u stable=%u pending=%u/%u"
                " source-rate=%u race=%u\n",
                g_interpolation_target_rate.load(std::memory_order_relaxed),
                g_interpolation_original_rate.load(std::memory_order_relaxed),
                interpolation_workloads,
                interpolation_planned_frames,
                interpolation_rendered_frames,
                production_image_counts[0], production_image_counts[1], production_image_counts[2],
                interpolation_backlog_dropped_frames,
                interpolation_stock_flag_workloads,
                interpolation_history_target_workloads,
                interpolation_unavailable_workloads,
                g_source_raw_factor.load(std::memory_order_relaxed),
                g_source_stable_factor.load(std::memory_order_relaxed),
                g_source_pending_factor.load(std::memory_order_relaxed),
                g_source_pending_samples.load(std::memory_order_relaxed),
                g_source_rate.load(std::memory_order_relaxed),
                g_source_race_active.load(std::memory_order_relaxed));
        }
    }
}

bool reset_audio(uint32_t frequency) {
    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(frequency);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 0x400;
    desired.callback = nullptr;

    SDL_AudioSpec obtained{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (g_audio_device == 0) {
        rr64_log("[RR64-AUDIO] Device open failed at %" PRIu32 " Hz: %s\n", frequency, SDL_GetError());
        return false;
    }

    g_audio_rate = frequency;
    g_audio_host_period_frames = static_cast<std::uint32_t>(obtained.samples);
    g_audio_queue_monitor.reset(frequency, g_audio_host_period_frames);
    g_audio_playback_started = false;
    g_audio_timeline_epoch_seen = rr64_audio_timeline_epoch();
    g_audio_startup_prebuffer_frames = ultramodern::audio_startup_prebuffer_frames(
        frequency, g_audio_host_period_frames);
    SDL_PauseAudioDevice(g_audio_device, 1);
    rr64_log(
        "[RR64-AUDIO] Opened driver=%s device=%s requested=%" PRIu32 "Hz/S16/2ch obtained=%dHz/0x%04X/%uch samples=%u id=%u\n",
        SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "unknown",
        SDL_GetAudioDeviceName(0, 0) != nullptr ? SDL_GetAudioDeviceName(0, 0) : "default",
        frequency,
        obtained.freq,
        static_cast<unsigned>(obtained.format),
        static_cast<unsigned>(obtained.channels),
        static_cast<unsigned>(obtained.samples),
        static_cast<unsigned>(g_audio_device));
    rr64_log("[RR64-AUDIO] Prebuffering %u frames before playback.\n",
        g_audio_startup_prebuffer_frames);
    return true;
}

void resynchronize_audio_queue(const char* reason, std::uint32_t queued_frames) {
    if (g_audio_device == 0) {
        return;
    }

    SDL_PauseAudioDevice(g_audio_device, 1);
    SDL_ClearQueuedAudio(g_audio_device);
    g_audio_playback_started = false;
    g_audio_queue_monitor.reset(g_audio_rate, g_audio_host_period_frames);
    rr64_log(
        "[RR64-AUDIO] Timeline resync reason=%s discarded=%u frames.\n",
        reason,
        queued_frames);
}

void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (g_audio_device == 0 || audio_data == nullptr || sample_count == 0) {
        return;
    }

    // N64ModernRuntime exposes N64-native sample ordering; swap stereo channels
    // to account for the guest memory endianness convention used by recomp ports.
    static std::vector<int16_t> swapped;
    swapped.resize(sample_count);
    size_t i = 0;
    for (; i + 1 < sample_count; i += 2) {
        swapped[i + 0] = audio_data[i + 1];
        swapped[i + 1] = audio_data[i + 0];
    }
    if (i < sample_count) {
        swapped[i] = audio_data[i];
    }

    rr64::achievement_audio::mix_requested_guitar_sting(
        std::span<std::int16_t>(swapped.data(), swapped.size()),
        g_audio_rate);
#ifdef RR64_EXPERIMENTAL_COURSE
    rr64::course_music::mix(std::span<std::int16_t>(swapped.data(), swapped.size()),
                           g_audio_rate, rr64::music::volume_gain() * rr64::mk64_items::item_music_gain());
#endif
    rr64::music::mix(std::span<std::int16_t>(swapped.data(), swapped.size()), g_audio_rate);
#ifdef RR64_EXPERIMENTAL_COURSE
    rr64::course_audio::mix(std::span<std::int16_t>(swapped.data(), swapped.size()), g_audio_rate);
    rr64::mk64_items::mix_audio(std::span<std::int16_t>(swapped.data(), swapped.size()),
                               g_audio_rate, 1.f, rr64::music::volume_gain());
#endif
    rr64::voice_chat::mix(
        std::span<std::int16_t>(swapped.data(), swapped.size()),
        g_audio_rate);

    rr64::audio::apply_master_volume(swapped, g_master_volume_gain.load(std::memory_order_relaxed));

    constexpr std::uint32_t bytes_per_frame = sizeof(std::int16_t) * 2u;
    const bool playback_started_before = g_audio_playback_started;
    std::uint32_t queued_before_frames = SDL_GetQueuedAudioSize(g_audio_device) / bytes_per_frame;
    const std::uint32_t timeline_epoch = rr64_audio_timeline_epoch();
    if (timeline_epoch != g_audio_timeline_epoch_seen) {
        resynchronize_audio_queue("gameplay-boundary", queued_before_frames);
        g_audio_timeline_epoch_seen = timeline_epoch;
        queued_before_frames = 0;
    }
    else {
        const std::uint32_t maximum_queue_frames =
            rr64::audio::maximum_queue_latency_frames(
                g_audio_rate, g_audio_host_period_frames);
        if (rr64::audio::should_drop_overflow_submission(
                queued_before_frames, maximum_queue_frames)) {
            // Keep already queued, continuous audio playing and shed only
            // this newest block. The consumer drains the brief surplus by
            // the next producer interval without a device pause/rebuffer.
            return;
        }
    }
    const Uint32 byte_count = static_cast<Uint32>(swapped.size() * sizeof(int16_t));
    const int queue_result = SDL_QueueAudio(g_audio_device, swapped.data(), byte_count);
    const std::uint32_t queued_after_frames = SDL_GetQueuedAudioSize(g_audio_device) / bytes_per_frame;
    if (queue_result == 0 && !g_audio_playback_started &&
        queued_after_frames >= g_audio_startup_prebuffer_frames)
    {
        SDL_PauseAudioDevice(g_audio_device, 0);
        g_audio_playback_started = true;
        rr64_log("[RR64-AUDIO] Playback started with %u frames queued.\n", queued_after_frames);
    }
    if (FILE* trace = audio_trace_file()) {
        const rr64::audio::QueueObservation observation = g_audio_queue_monitor.observe(
            queued_before_frames,
            queued_after_frames,
            std::span<const std::int16_t>(swapped.data(), swapped.size()));
        std::fprintf(trace, "%llu,%u,%u,%u,%u,%u,%llu,%llu,%u,%u,%u,%u,%u,%u,%u\n",
            static_cast<unsigned long long>(observation.sequence),
            observation.sample_rate,
            observation.host_period_frames,
            observation.submitted_frames,
            observation.queued_before_frames,
            observation.queued_after_frames,
            static_cast<unsigned long long>(observation.gap_microseconds),
            static_cast<unsigned long long>(observation.expected_microseconds),
            observation.discontinuity,
            observation.empty_before_submit ? 1u : 0u,
            observation.below_host_period_before_submit ? 1u : 0u,
            observation.late_submit ? 1u : 0u,
            playback_started_before ? 1u : 0u,
            g_audio_playback_started ? 1u : 0u,
            g_audio_startup_prebuffer_frames);
        if (observation.sequence <= 8u || (observation.sequence % 120u) == 0u ||
            observation.empty_before_submit || observation.late_submit)
        {
            std::fflush(trace);
        }
    }
    if (!detailed_diagnostics_enabled()) { return; }
    static std::atomic_uint64_t buffer_sequence{0};
    const uint64_t sequence = buffer_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    // Keep diagnostics out of the steady-state audio producer. rr64_log flushes
    // two streams synchronously, and doing that every 120 buffers can briefly
    // hold up the mixer long enough for a marginal SDL queue to underrun.
    if (sequence <= 8 || queue_result != 0) {
        int peak = 0;
        size_t nonzero_samples = 0;
        for (const int16_t sample : swapped) {
            const int magnitude = sample == INT16_MIN ? 32768 : std::abs(static_cast<int>(sample));
            peak = std::max(peak, magnitude);
            nonzero_samples += sample != 0 ? 1 : 0;
        }
        rr64_log(
            "[RR64-AUDIO] buffer=%" PRIu64 " samples=%zu nonzero=%zu peak=%d first=(%d,%d,%d,%d) queued=%u result=%d error=%s\n",
            sequence,
            sample_count,
            nonzero_samples,
            peak,
            sample_count > 0 ? swapped[0] : 0,
            sample_count > 1 ? swapped[1] : 0,
            sample_count > 2 ? swapped[2] : 0,
            sample_count > 3 ? swapped[3] : 0,
            static_cast<unsigned>(queued_after_frames * bytes_per_frame),
            queue_result,
            queue_result != 0 ? SDL_GetError() : "");
    }
}

size_t get_frames_remaining() {
    if (g_audio_device == 0) {
        return 0;
    }
    const uint64_t bytes = SDL_GetQueuedAudioSize(g_audio_device);
    return static_cast<size_t>(bytes / (sizeof(int16_t) * 2));
}

void set_frequency(uint32_t frequency) {
    if (frequency == 0 || frequency == g_audio_rate) {
        return;
    }
    rr64_log("[RR64] Audio frequency -> %" PRIu32 " Hz\n", frequency);
    if (!reset_audio(frequency)) {
        recompui::message_box("Road Rash 64: failed to reset the host audio device.");
    }
}

uint32_t guest_pointer(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

uint32_t guest_pointer(int32_t p) {
    return static_cast<uint32_t>(p);
}

void log_rsp_task(const OSTask* task) {
    static const bool trace_enabled = [] {
        const char* value = std::getenv("RR64_RSP_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (!trace_enabled) {
        return;
    }

    static std::atomic_uint64_t task_sequence{0};

    if (task == nullptr) {
        std::fprintf(stderr, "[RR64-RSP] null OSTask\n");
        return;
    }

    const uint64_t sequence = task_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t ucode = guest_pointer(task->t.ucode);
    const uint32_t ucode_boot = guest_pointer(task->t.ucode_boot);
    const uint32_t ucode_data = guest_pointer(task->t.ucode_data);
    const uint32_t data_ptr = guest_pointer(task->t.data_ptr);
    const uint32_t yield_ptr = guest_pointer(task->t.yield_data_ptr);

    const bool known_audio = task->t.type == M_AUDTASK && ucode == 0x8007DC90u;
    if (known_audio && sequence > 4 && (sequence % 600) != 0) {
        return;
    }

    uint32_t initial_image_rom_candidate = 0;
    const uint32_t phys = ucode & 0x1FFFFFFFu;
    // The first 1 MiB is loaded from ROM 0x1000 at RAM 0x80000400.
    if (phys >= 0x00000400u && phys < 0x00100400u) {
        initial_image_rom_candidate = phys + 0xC00u;
    }

    std::fprintf(stderr,
        "[RR64-RSP] n=%" PRIu64 " type=%" PRIu32
        " flags=0x%08" PRIX32
        " boot=0x%08" PRIX32 "/0x%08" PRIX32
        " ucode=0x%08" PRIX32 "/0x%08" PRIX32
        " ucode_data=0x%08" PRIX32 "/0x%08" PRIX32
        " data=0x%08" PRIX32 "/0x%08" PRIX32
        " yield=0x%08" PRIX32 "/0x%08" PRIX32
        " rom_candidate=0x%08" PRIX32 "\n",
        sequence,
        static_cast<uint32_t>(task->t.type),
        static_cast<uint32_t>(task->t.flags),
        ucode_boot,
        static_cast<uint32_t>(task->t.ucode_boot_size),
        ucode,
        static_cast<uint32_t>(task->t.ucode_size),
        ucode_data,
        static_cast<uint32_t>(task->t.ucode_data_size),
        data_ptr,
        static_cast<uint32_t>(task->t.data_size),
        yield_ptr,
        static_cast<uint32_t>(task->t.yield_data_size),
        initial_image_rom_candidate
    );
    std::fflush(stderr);
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    log_rsp_task(task);

    if (task != nullptr && task->t.type == M_AUDTASK &&
        guest_pointer(task->t.ucode) == 0x8007DC90u) {
        return rr64_audio_rsp;
    }

    // Standard F3DEX.NoN graphics tasks are handled through RT64. Keep unknown
    // CPU-run RSP tasks explicit so a second microcode cannot be mistaken for
    // the verified Road Rash audio program.
    return nullptr;
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (rr64::online_menu::controls_online_players()) {
        const rr64::netplay::Status status = rr64::netplay::get_status();
        const bool connected = status.replicated_riders
                ? controller_num == 0
                : controller_num >= 0 &&
                controller_num < rr64::netplay::kMaximumLocalControllers &&
                status.players[static_cast<std::size_t>(controller_num)].connected;
        return ultramodern::input::connected_device_info_t{
            .connected_device = connected
                ? ultramodern::input::Device::Controller
                : ultramodern::input::Device::None,
            .connected_pak = connected
                ? ultramodern::input::Pak::RumblePak
                : ultramodern::input::Pak::None,
        };
    }

    // In single-player mode the shared keyboard/gamepad profile represents N64
    // controller port 1 only. Reporting it in every port makes Road Rash treat
    // the real player port as disconnected during its startup probe.
    const bool connected = recompinput::players::is_single_player_mode()
        ? controller_num == 0
        : recompinput::players::get_player_is_assigned(controller_num);
    if (connected) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

void apply_responsive_menu_navigation(int profile_index, uint16_t buttons, float* x, float* y) {
    static std::array<rr64::menu_navigation::State, 4> navigation_states{};
    if (profile_index < 0 || profile_index >= static_cast<int>(navigation_states.size()) ||
        x == nullptr || y == nullptr) {
        return;
    }

    auto& state = navigation_states[static_cast<std::size_t>(profile_index)];
    if (rr64_is_race_mode_active() && !rr64_online_host_pause_active()) {
        rr64::menu_navigation::reset(state);
        return;
    }

    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    rr64::menu_navigation::filter(state, buttons, *x, *y, now_ms);
}

bool get_local_input_with_road_rumble(
    int profile_index,
    int rumble_channel,
    uint16_t* buttons,
    float* x,
    float* y)
{
    const bool got_response =
        recompinput::profiles::get_n64_input(profile_index, buttons, x, y);
#ifdef RR64_EXPERIMENTAL_COURSE
    const std::uint16_t physical_buttons = got_response && buttons ? *buttons : 0;
#endif
    const bool dismissal_input_blocked = profile_index == 0
        ? rr64::popup_input::suppress(recompinput::game_input_disabled(),
            got_response, buttons ? *buttons : 0, x ? *x : 0, y ? *y : 0)
        : rr64::popup_input::wait_for_release.load(std::memory_order_acquire);
    if (dismissal_input_blocked) {
        if (buttons) *buttons = 0;
        if (x) *x = 0;
        if (y) *y = 0;
    }
    if (got_response && buttons != nullptr) {
        apply_responsive_menu_navigation(profile_index, *buttons, x, y);
    }
    if (got_response && buttons && !dismissal_input_blocked &&
        !recompinput::game_input_disabled() && recompinput::game_window_focused() &&
        rr64_are_gameplay_shortcuts_active() &&
        recompinput::profiles::get_action_input(profile_index, recompinput::GameInput::RR64_WEAPON_TRICK)) {
        // Remove the old RB/C-Down binding in existing profiles as well.
        *buttons &= ~0x0004;
        *buttons |= rr64_custom_cop_active() ? rr64_cop_weapon_trick_button : 0x0020;
    }
    // Edge state and requests belong to the controller slot, not player one.
    if (profile_index >= 0 && profile_index < 4) {
#ifdef RR64_EXPERIMENTAL_COURSE
        static std::array<bool, 4> item_was_held{};
        const bool item_held=got_response && recompinput::profiles::get_action_input(
            profile_index,recompinput::GameInput::RR64_MK64_USE_ITEM);
        const bool item_pressed=item_held && !item_was_held[profile_index];
        item_was_held[profile_index]=item_held;
        if(item_pressed && !dismissal_input_blocked && !recompinput::game_input_disabled() &&
           recompinput::game_window_focused() && rr64_are_gameplay_shortcuts_active() &&
           rr64::mk64_items::input_active()) {
            const auto online=rr64::netplay::get_status();
            rr64::mk64_items::request_use(rr64::online_flow::shortcut_slot(profile_index,
                online.active && online.connected && online.phase==rr64::netplay::Phase::Race,
                online.local_slot,online.replicated_riders));
        }
#endif
        static std::array<bool, 4> eject_was_held{};
        const bool held = got_response && recompinput::profiles::get_action_input(
            profile_index, recompinput::GameInput::RR64_EJECT);
        const bool pressed = held && !eject_was_held[profile_index];
        eject_was_held[profile_index] = held;
        if (pressed && !dismissal_input_blocked && !recompinput::game_input_disabled() &&
            recompinput::game_window_focused() && rr64_are_gameplay_shortcuts_active()) {
            const auto online=rr64::netplay::get_status();
            rr64_request_rider_eject(rr64::online_flow::shortcut_slot(profile_index,
                online.active && online.connected && online.phase==rr64::netplay::Phase::Race,
                online.local_slot,online.replicated_riders));
        }
    }
    if (profile_index >= 0 && profile_index < 4) {
        static std::array<bool, 4> right_stick_was_held{};
        const bool gameplay_shortcuts_allowed = rr64_are_gameplay_shortcuts_active() &&
            !dismissal_input_blocked && !recompinput::game_input_disabled() &&
            recompinput::game_window_focused();
        const bool right_stick_held = recompinput::profiles::get_action_input(profile_index, recompinput::GameInput::RR64_SPOKE_JAM);
        const bool right_stick_pressed = right_stick_held && !right_stick_was_held[profile_index];
        right_stick_was_held[profile_index] = right_stick_held;
        if (got_response && buttons != nullptr && right_stick_held && gameplay_shortcuts_allowed) {
            constexpr uint16_t n64_c_right_button = 0x0001;
            if (rr64_local_rider_has_fists_selected(static_cast<unsigned>(profile_index))) {
                // A tap of native C-Right is the game's punch/weapon-steal
                // path. Leave its opponent proximity and timing rules intact.
                if (right_stick_pressed) {
                    *buttons |= n64_c_right_button;
                }
            }
            else {
                // With a weapon selected, preserve the authored spoke-jam
                // chord and allow the player to hold R3 as before.
                constexpr uint16_t n64_c_down_button = 0x0004;
                *buttons |= n64_c_down_button | n64_c_right_button;
            }
        }
    }
    // Road Rash accelerates with N64 Z (0x2000). A is the wheelie button, so
    // treating A as throttle made the road-rumble channel fire for a wheelie
    // even while the bike was stopped or the rider was down.
    constexpr uint16_t n64_z_button = 0x2000;
    const bool accelerating =
        got_response && buttons != nullptr && ((*buttons & n64_z_button) != 0);
    const float steering_load = x != nullptr ? std::min(std::abs(*x), 1.0f) : 0.0f;
    const bool road_rumble_allowed =
        rr64_is_rumble_enabled() &&
        rr64_is_road_rumble_allowed() &&
        !recompinput::game_input_disabled() &&
        recompinput::game_window_focused();
    const float road_strength = road_rumble_allowed && accelerating
        ? 0.14f + (0.04f * steering_load)
        : 0.0f;
    recompinput::set_road_rumble(rumble_channel, road_strength);
#ifdef RR64_EXPERIMENTAL_COURSE
    // Filter the physical cycle hold before native input and netplay/history
    // receive it. This helper is used only for this machine's local profiles.
    if (profile_index >= 0 && profile_index < 4 && buttons) {
        *buttons = rr64::mk64_items::filter_cycle_input(unsigned(profile_index), physical_buttons,
            got_response ? *buttons : 0,
            got_response && !dismissal_input_blocked && !recompinput::game_input_disabled() &&
            recompinput::game_window_focused() && rr64_are_gameplay_shortcuts_active() &&
            rr64::mk64_items::input_active());
    }
#endif
    return got_response;
}

void set_gameplay_rumble(int controller_num, bool on) {
    const bool gameplay_rumble_allowed =
        rr64_is_rumble_enabled() &&
        rr64_is_gameplay_feedback_active() &&
        !recompinput::game_input_disabled() &&
        recompinput::game_window_focused();
    if (gameplay_rumble_allowed || !on) {
        // Honor stop commands at every boundary, but do not erase an authored
        // motor-on state merely because its output is temporarily suspended.
        recompinput::set_rumble(controller_num, on);
    }
}

void update_gameplay_rumble() {
    static bool gameplay_rumble_was_allowed = false;
    const bool gameplay_rumble_allowed =
        rr64_is_rumble_enabled() &&
        rr64_is_gameplay_feedback_active() &&
        !recompinput::game_input_disabled() &&
        recompinput::game_window_focused();
    if (!gameplay_rumble_allowed) {
        if (gameplay_rumble_was_allowed) {
            recompinput::suspend_all_rumble();
        }
        gameplay_rumble_was_allowed = false;
        return;
    }

    gameplay_rumble_was_allowed = true;
    recompinput::update_rumble();
}

bool get_input_with_trace(int controller_num, uint16_t* buttons, float* x, float* y) {
    // Keep actor edge visibility consistent with the renderer's expanding
    // ultrawide output when the window changes size.
    if (controller_num == 0 && window && rr64_dynamic_ultrawide.load()) {
        int width=0,height=0;SDL_GetWindowSize(window,&width,&height);
        if(width>0 && height>0) rr64::view_width.store(double(width)/height/(4.0/3.0));
    }
    if (rr64::online_menu::controls_online_players()) {
        const rr64::netplay::Status status = rr64::netplay::get_status();
        if (status.host_disconnected) {
            if (buttons) *buttons=0;
            if (x) *x=0;
            if (y) *y=0;
            return controller_num>=0 && controller_num<4 && status.players[controller_num].connected;
        }
        // Apply ownership to locally sampled controls too, not just packets.
        struct HostPauseOnly {
            std::uint16_t *buttons;
            bool strip;
            ~HostPauseOnly() { if (strip && buttons) *buttons &= ~std::uint16_t(0x1000); }
        } pause_guard{buttons, status.phase == rr64::netplay::Phase::Race &&
            (status.replicated_riders ? !status.is_host : controller_num != 0)};
        if (rr64::online_menu::host_controls_game_setup() ||
            (status.phase==rr64::netplay::Phase::Race && rr64_online_host_pause_active())) {
            if (controller_num != 0) {
                if (buttons != nullptr) *buttons = 0;
                if (x != nullptr) *x = 0.0f;
                if (y != nullptr) *y = 0.0f;
                return controller_num >= 0 && controller_num < rr64::netplay::kMaximumLocalControllers &&
                    !status.replicated_riders && status.players[controller_num].connected;
            }

            if (status.is_host) {
                const bool got_response =
                    get_local_input_with_road_rumble(0, 0, buttons, x, y);
                if (status.phase == rr64::netplay::Phase::TrackSelect) {
                    // The stock Rumble Pak prompt remains between selection
                    // and loading. Accept A or Start, latch the host command
                    // in snapshots, and let late clients advance it too.
                    if (got_response && buttons && (*buttons & 0x9000))
                        rr64::netplay::host_request_race_start();
                    if (buttons) *buttons=rr64::online_flow::start_prompt_buttons(rr64::netplay::get_status().game_setup.start_requested,
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (x) *x=0;
                    if (y) *y=0;
                }
                rr64::netplay::set_local_input(
                    got_response && buttons != nullptr ? *buttons : 0,
                    got_response && x != nullptr ? *x : 0.0f,
                    got_response && y != nullptr ? *y : 0.0f);
                return got_response;
            }

            std::uint16_t host_buttons = 0;
            float host_x = 0.0f;
            float host_y = 0.0f;
            const bool got_host = rr64::netplay::get_player_input(0, host_buttons, host_x, host_y);
            if (status.phase == rr64::netplay::Phase::CharacterSelect && status.game_setup.valid) {
                // Keep the host's final accept edge visible until this guest
                // reaches the same character/bike screen. This closes the
                // short UDP timing window around the setup transition.
                host_buttons |= status.game_setup.transition_buttons;
            }
            if (status.phase == rr64::netplay::Phase::TrackSelect) {
                host_buttons=rr64::online_flow::start_prompt_buttons(status.game_setup.start_requested,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                host_x=host_y=0;
            }
            if (buttons != nullptr) *buttons = host_buttons;
            if (x != nullptr) *x = host_x;
            if (y != nullptr) *y = host_y;
            return got_host || host_buttons != 0 || status.phase == rr64::netplay::Phase::TrackSelect;
        }

        if (status.phase == rr64::netplay::Phase::CharacterSelect ||
            status.phase == rr64::netplay::Phase::TrackSelect) {
            if (buttons) *buttons=0;
            if (x) *x=0;
            if (y) *y=0;
            if (controller_num != 0) return false;
            if (status.phase == rr64::netplay::Phase::TrackSelect) return true;
            return get_local_input_with_road_rumble(0,0,buttons,x,y);
        }

        if (status.replicated_riders) {
            if (controller_num != 0) {
                if (buttons != nullptr) *buttons = 0;
                if (x != nullptr) *x = 0.0f;
                if (y != nullptr) *y = 0.0f;
                return false;
            }
            const bool got_response =
                get_local_input_with_road_rumble(0, 0, buttons, x, y);
            rr64::netplay::set_local_input(
                got_response && buttons != nullptr ? *buttons : 0,
                got_response && x != nullptr ? *x : 0.0f,
                got_response && y != nullptr ? *y : 0.0f);
            return got_response;
        }

        if (controller_num < 0 || controller_num >= rr64::netplay::kMaximumLocalControllers ||
            !status.players[static_cast<std::size_t>(controller_num)].connected) {
            if (buttons != nullptr) *buttons = 0;
            if (x != nullptr) *x = 0.0f;
            if (y != nullptr) *y = 0.0f;
            return false;
        }

        if (static_cast<std::uint8_t>(controller_num) == status.local_slot) {
            const bool got_response =
                get_local_input_with_road_rumble(0, 0, buttons, x, y);
            rr64::netplay::set_local_input(
                got_response && buttons != nullptr ? *buttons : 0,
                got_response && x != nullptr ? *x : 0.0f,
                got_response && y != nullptr ? *y : 0.0f);
            return got_response;
        }

        std::uint16_t remote_buttons = 0;
        float remote_x = 0.0f;
        float remote_y = 0.0f;
        const bool got_remote = rr64::netplay::get_player_input(
            static_cast<std::uint8_t>(controller_num), remote_buttons, remote_x, remote_y);
        if (buttons != nullptr) *buttons = remote_buttons;
        if (x != nullptr) *x = remote_x;
        if (y != nullptr) *y = remote_y;
        return got_remote;
    }

    const bool connected = recompinput::players::is_single_player_mode()
        ? controller_num == 0
        : recompinput::players::get_player_is_assigned(controller_num);
    if (!connected) {
        recompinput::set_road_rumble(controller_num, 0.0f);
        if (buttons != nullptr) {
            *buttons = 0;
        }
        if (x != nullptr) {
            *x = 0.0f;
        }
        if (y != nullptr) {
            *y = 0.0f;
        }
        return false;
    }

    const bool got_response = get_local_input_with_road_rumble(
        controller_num, controller_num, buttons, x, y);
    if (controller_num == 0) {
        rr64::netplay::set_local_input(
            got_response && buttons != nullptr ? *buttons : 0,
            got_response && x != nullptr ? *x : 0.0f,
            got_response && y != nullptr ? *y : 0.0f);
    }
    if (!got_response || controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) {
        return got_response;
    }

    static const bool input_trace_enabled = [] {
        const char* value = std::getenv("RR64_INPUT_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (input_trace_enabled) {
        static uint16_t last_buttons = 0;
        static int last_x = 0;
        static int last_y = 0;
        const int current_x = static_cast<int>(std::lround(*x * 100.0f));
        const int current_y = static_cast<int>(std::lround(*y * 100.0f));
        if (*buttons != last_buttons || current_x != last_x || current_y != last_y) {
            rr64_log("[RR64-INPUT] port=1 buttons=0x%04X stick=(%.2f,%.2f)\n",
                static_cast<unsigned>(*buttons),
                static_cast<double>(*x),
                static_cast<double>(*y));
            last_buttons = *buttons;
            last_x = current_x;
            last_y = current_y;
        }
    }
    return got_response;
}

std::string get_game_thread_name(const OSThread* thread) {
    if (thread == nullptr) {
        return "[Road Rash 64] unknown";
    }
    return "[Road Rash 64] thread " + std::to_string(thread->id) + " pri " + std::to_string(thread->priority);
}

void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    recompui::renderer::enable_texture_pack(context, mod);
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    recompui::renderer::disable_texture_pack(mod);
}

void reorder_texture_packs(recomp::mods::ModContext&) {
    recompui::renderer::trigger_texture_pack_update();
}

void init_recompui_config() {
    rr64_log("[RR64-STAGE] Initializing RecompFrontend configuration tabs.\n");

    const auto config_dir = recompui::file::get_app_folder_path();
    if (!config_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config_dir, ec);
        if (ec) {
            rr64_log("[RR64-CONFIG] Warning: could not create config directory: %s\n", ec.message().c_str());
        }
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;

    rr64_log("[RR64-CONFIG] Creating General tab.\n");
    auto& general_config = recompui::config::create_general_tab(general_options, "Gameplay");
    for (unsigned slot = 0; slot < 4; ++slot) {
        const std::string key = "rr64_local_player_name_" + std::to_string(slot + 1);
        general_config.add_string_option(key, "Local Player " + std::to_string(slot + 1) + " Name",
            "Name shown in local multiplayer. Uses up to 11 letters, numbers or spaces; unsupported characters are omitted. Empty names use PLAYER 1â€“4. Controller bindings are configured separately in Controls.",
            "PLAYER " + std::to_string(slot + 1), true);
        general_config.add_option_change_callback(key,
            [slot](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rr64::local_players::set_name(slot, std::get<std::string>(value));
            });
    }
    general_config.add_bool_option(
        kControllerRumbleEnabledOption,
        "Controller Rumble",
        "Master switch for road, crash, combat, and Rumble Pak feedback. Rumble Strength below still controls intensity when enabled.",
        true
    );
    general_config.add_option_change_callback(
        kControllerRumbleEnabledOption,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            const bool enabled = std::get<bool>(value);
            rr64_set_rumble_enabled(enabled ? 1 : 0);
            rr64_log("[RR64-INPUT] Controller rumble enabled=%d.\n", enabled ? 1 : 0);
        }
    );
    general_config.add_bool_option(
        kAchievementsEnabledOption,
        "Achievements",
        "Enables local achievement tracking, Road Rash popup animations, and the unlock guitar riff. Existing progress is preserved while disabled.",
        true
    );
    general_config.add_option_change_callback(
        kAchievementsEnabledOption,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            const bool enabled = std::get<bool>(value);
            rr64::achievements::set_enabled(enabled);
            rr64_log("[RR64-ACH] Achievement system enabled=%d.\n", enabled ? 1 : 0);
        }
    );
    rr64::achievements::initialize();
    rr64::achievements::register_config_tab();
    rr64_log("[RR64-CONFIG] Creating Graphics tab.\n");
    auto& graphics_config = recompui::config::create_graphics_tab();
    graphics_config.add_option_change_callback(recompui::config::graphics::options::ar_option,
        [](recomp::config::ConfigValueVariant value,recomp::config::ConfigValueVariant,recomp::config::OptionChangeContext){
            const auto aspect=static_cast<ultramodern::renderer::AspectRatio>(std::get<uint32_t>(value));
            using A=ultramodern::renderer::AspectRatio;
            rr64_dynamic_ultrawide.store(aspect==A::Manual);
            rr64::view_width.store(aspect==A::Manual?7.0/4.0:(aspect==A::Original||aspect==A::Stretch?1.0:4.0/3.0));
        });
    graphics_config.add_bool_option("rr64_max_lod", "MAX LOD",
        "Keeps riders and bikes at maximum detail. Set before Start Game; changes during gameplay apply after restarting the application.", true);
    graphics_config.add_option_change_callback("rr64_max_lod",
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            if (!ultramodern::is_game_started()) rr64::presentation_options::max_lod.store(std::get<bool>(value));
        });
    graphics_config.add_number_option("rr64_draw_distance", "Draw Distance",
        "Terrain and roadside-object distance in all race modes. 0% keeps original drawing distance; 100% draws the full current map. Lower values reduce the added range and workload. Applies during play. MAX LOD controls rider and bike detail separately.",
        0, 100, 5, 0, true, 100);
    graphics_config.add_option_change_callback("rr64_draw_distance",
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            apply_draw_distance(std::get<double>(value));
        });
    graphics_config.add_bool_option(
        kRemoveDistanceFogOption,
        "Remove Distance Fog",
        "Disables all distance fog in 3D scenes. With increased Draw Distance this gives the clearest view, but terrain loading transitions can be visible.",
        false
    );
    graphics_config.add_option_change_callback(
        kRemoveDistanceFogOption,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            const bool remove_distance_fog = std::get<bool>(value);
            RT64::setUserFogEnabled(!remove_distance_fog);
            rr64_log("[RR64-GFX] Distance fog enabled=%d.\n", remove_distance_fog ? 0 : 1);
        }
    );
    graphics_config.add_bool_option(
        kExtendedHorizonHazeOption,
        "Extended Horizon Haze",
        "Softens the original terrain transition with horizon haze when Draw Distance is above 0%. Remains available when Distance Fog is removed. This haze follows the original terrain range, not the full-map slider boundary.",
        true
    );
    graphics_config.add_option_change_callback(
        kExtendedHorizonHazeOption,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            const bool extended_horizon_haze = std::get<bool>(value);
            g_extended_horizon_haze_requested.store(extended_horizon_haze, std::memory_order_relaxed);
            apply_extended_horizon_haze();
            rr64_log("[RR64-GFX] Extended horizon haze requested=%d.\n", extended_horizon_haze ? 1 : 0);
        }
    );
    rr64_log("[RR64-CONFIG] Creating Controls tab.\n");
    recompui::config::create_controls_tab();
    rr64_log("[RR64-CONFIG] Creating Sound tab.\n");
    // Group only the presentation; all existing values still live in sound.json.
    auto& sound_config = recompui::config::create_sound_tab("Audio", {
        {"Game Audio", "Adjust the overall game volume and nearby rival engines.",
            {recompui::config::sound::options::main_volume, rr64::rival_engine::enabled_option,
             rr64::rival_engine::volume_option}},
        {"Music", "Adjust music volume or choose your soundtrack. MK64 course music is selected in race options.",
            {"custom_music_volume", "custom_music_track"}},
        {"Voice Chat", "Hear nearby riders during online races. Set up your input in Microphone.",
            {kProximityVoiceEnabledOption, "rr64_voice_volume", "rr64_voice_flyby"}},
        {"Microphone", "Choose your voice input, then adjust its level and activation threshold.",
            {"rr64_microphone", "rr64_mic_mute", "rr64_mic_gain", "rr64_mic_threshold"}}
    });
    sound_config.add_option_change_callback(recompui::config::sound::options::main_volume,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            g_master_volume_gain.store(rr64::audio::volume_gain(std::get<double>(value)), std::memory_order_relaxed);
        });
    rr64::rival_engine::configure_volume(sound_config);
    auto music_directory = executable_directory() / "music";
#ifndef _WIN32
    // AppImage's mounted executable directory is read-only. Keep user tracks
    // beside the downloaded image, not inside its temporary mount.
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && appimage[0]) {
        music_directory = std::filesystem::path(appimage).parent_path() / "music";
    }
#endif
    rr64::music::configure(sound_config, music_directory);
    sound_config.add_bool_option(
        kProximityVoiceEnabledOption,
        "Proximity Voice Chat",
        "Directional voice during online races: nearby riders are clear and distant riders fade out. Configure your input in the Microphone section. Lobby and single-player audio are never transmitted.",
        true
    );
    sound_config.add_option_change_callback(
        kProximityVoiceEnabledOption,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            const bool enabled = std::get<bool>(value);
            rr64::voice_chat::set_enabled(enabled);
            rr64_log("[RR64-VOICE] Proximity voice enabled=%d.\n", enabled ? 1 : 0);
        }
    );
    rr64::voice_chat::configure(sound_config);
    rr64::race_pack_mod::configure_directory(music_directory.parent_path() / "race-packs" / "mk64");
    auto course_importer = executable_directory() / "tools" / "mk64-importer" /
#ifdef _WIN32
        "rr64-mk64-importer.exe";
#else
        "rr64-mk64-importer";
#endif
    rr64::mk64_import::configure(course_importer,
        recomp::get_config_path() / supported_games[0].stored_filename());
    rr64::race_pack_mod_ui::install();
    rr64::rider_skin_mod_ui::install();
    rr64_log("[RR64-CONFIG] Creating Mods tab.\n");
    recompui::config::create_mods_tab("Mods");
    // Keep optional offline cheats at the end of the settings navigation.
    rr64::offline_modifiers::register_config_tab();
    rr64_log("[RR64-CONFIG] Finalizing RecompFrontend configuration.\n");
    recompui::config::finalize();
    rr64::offline_modifiers::apply_config();
    // Registering another tab can relocate the frontend's config vector.
    auto& loaded_sound_config = recompui::config::get_config(recompui::config::sound::id);
    rr64::rival_engine::apply_volume_config(loaded_sound_config);
    rr64::voice_chat::apply_config(loaded_sound_config);
    rr64::voice_chat::set_enabled(std::get<bool>(loaded_sound_config.get_option_value(kProximityVoiceEnabledOption)));
    recompui::register_player_name_callbacks(
        [](int slot) { return rr64::local_players::snapshot().at(slot); },
        [](int slot, const std::string& name) {
            auto& config = recompui::config::get_general_config();
            config.set_option_value("rr64_local_player_name_" + std::to_string(slot + 1),
                rr64::local_players::display_name(name, slot));
            config.save_config();
        });
    rr64::presentation_options::max_lod.store(std::get<bool>(recompui::config::get_graphics_config().get_option_value("rr64_max_lod")));
    rr64::presentation_options::world_distance.store(1); // Draw Distance replaces the legacy world toggle.
    {using A=ultramodern::renderer::AspectRatio;
    const auto aspect=static_cast<A>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(recompui::config::graphics::options::ar_option)));
    rr64_dynamic_ultrawide.store(aspect==A::Manual);
            rr64::view_width.store(aspect==A::Manual?7.0/4.0:(aspect==A::Original||aspect==A::Stretch?1.0:4.0/3.0));}
    apply_draw_distance(std::get<double>(recompui::config::get_graphics_config().get_option_value("rr64_draw_distance")));
    g_master_volume_gain.store(rr64::audio::volume_gain(recompui::config::sound::get_main_volume()));
    rr64::netplay::configure({});
    // Controls owns loaded bindings; never rewrite a user's layout at startup.

    rr64_log("[RR64-STAGE] RecompFrontend configuration initialized and finalized.\n");
}

void configure_frontend_theme() {
    using namespace recompui;

    theme::set_theme_color(theme::color::Background1, Color{8, 8, 9, 255});
    theme::set_theme_color(theme::color::Background2, Color{21, 17, 16, 255});
    theme::set_theme_color(theme::color::Background3, Color{36, 27, 23, 255});
    theme::set_theme_color(theme::color::BGOverlay, Color{242, 101, 39, 26});
    theme::set_theme_color(theme::color::ModalOverlay, Color{8, 8, 9, 242});
    theme::set_theme_color(theme::color::BGShadow2, Color{5, 5, 6, 210});

    theme::set_theme_color(theme::color::Primary, Color{242, 91, 35, 255});
    theme::set_theme_color(theme::color::PrimaryL, Color{255, 177, 119, 255});
    theme::set_theme_color(theme::color::PrimaryD, Color{132, 37, 12, 255});
    theme::set_theme_color(theme::color::PrimaryA5, Color{242, 91, 35, 13});
    theme::set_theme_color(theme::color::PrimaryA20, Color{242, 91, 35, 51});
    theme::set_theme_color(theme::color::PrimaryA30, Color{242, 91, 35, 77});
    theme::set_theme_color(theme::color::PrimaryA50, Color{242, 91, 35, 128});
    theme::set_theme_color(theme::color::PrimaryA80, Color{242, 91, 35, 204});

    theme::set_theme_color(theme::color::Secondary, Color{246, 184, 55, 255});
    theme::set_theme_color(theme::color::SecondaryL, Color{255, 223, 146, 255});
    theme::set_theme_color(theme::color::SecondaryD, Color{157, 100, 12, 255});
    theme::set_theme_color(theme::color::SecondaryA5, Color{246, 184, 55, 13});
    theme::set_theme_color(theme::color::SecondaryA20, Color{246, 184, 55, 51});
    theme::set_theme_color(theme::color::SecondaryA30, Color{246, 184, 55, 77});
    theme::set_theme_color(theme::color::SecondaryA50, Color{246, 184, 55, 128});
    theme::set_theme_color(theme::color::SecondaryA80, Color{246, 184, 55, 204});

    theme::set_theme_color(theme::color::Elevated, Color{242, 101, 39, 32});
    theme::set_theme_color(theme::color::ElevatedSoft, Color{242, 101, 39, 18});
    theme::set_theme_color(theme::color::ElevatedBorder, Color{255, 177, 119, 180});
    theme::set_theme_color(theme::color::ElevatedBorderHard, Color{255, 177, 119, 255});

    theme::set_border_radius_sm(6.0f);
    theme::set_border_radius_md(10.0f);
    theme::set_border_radius_lg(14.0f);

    // LatoLatin is bundled under the SIL Open Font License. Keeping the
    // frontend presets at weight 400 lets RmlUi match every label to that face.
    theme::set_typography_preset(theme::Typography::Header1, 64.0f, 0.035f, 400);
    theme::set_typography_preset(theme::Typography::Header2, 50.0f, 0.025f, 400);
    theme::set_typography_preset(theme::Typography::Header3, 34.0f, 0.025f, 400);
    theme::set_typography_preset(theme::Typography::LabelLG, 34.0f, 0.04f, 400);
    theme::set_typography_preset(theme::Typography::LabelMD, 27.0f, 0.035f, 400);
    theme::set_typography_preset(theme::Typography::LabelSM, 20.0f, 0.035f, 400);
    theme::set_typography_preset(theme::Typography::LabelXS, 18.0f, 0.025f, 400);
    theme::set_typography_preset(theme::Typography::Body, 20.0f, 0.0f, 400);
}

void on_launcher_init(recompui::LauncherMenu* menu) {
    g_launcher_init_seen = true;
    rr64_log("[RR64-STAGE] RecompFrontend launcher initialization callback entered.\n");
    // The supplied launcher artwork already contains the full Road Rash 64
    // wordmark, so the generic text title would only duplicate it.
    menu->remove_default_title();
    auto* game_options = menu->init_game_options_menu(
        supported_games[0].game_id,
        supported_games[0].mod_game_id,
        supported_games[0].display_name,
        supported_games[0].thumbnail_bytes,
        recompui::GameOptionsMenuLayout::Left
    );
    constexpr const char* launcher_background_name = "RoadRashLauncher-v4.png";
    const std::filesystem::path launcher_background_path = recompui::file::get_asset_path(launcher_background_name);
    bool launcher_background_loaded = false;
    std::ifstream launcher_background_file(launcher_background_path, std::ios::binary | std::ios::ate);
    if (launcher_background_file) {
        const std::streamsize launcher_background_size = launcher_background_file.tellg();
        if (launcher_background_size > 0) {
            std::vector<char> launcher_background_bytes(static_cast<size_t>(launcher_background_size));
            launcher_background_file.seekg(0, std::ios::beg);
            if (launcher_background_file.read(launcher_background_bytes.data(), launcher_background_size)) {
                // RecompFrontend's RT64 renderer consumes images registered by
                // name, rather than opening the path supplied to an <img>.
                recompui::queue_image_from_bytes_file(launcher_background_name, launcher_background_bytes);
                menu->set_launcher_background_image(launcher_background_name);
                launcher_background_loaded = true;
            }
        }
    }
    if (!launcher_background_loaded) {
        rr64_log("[RR64-LAUNCHER] Could not load " RR64_PATH_FORMAT "; using vector fallback.\n", launcher_background_path.c_str());
        menu->set_launcher_background_svg("RoadRashLauncher.svg");
    }

    const bool stored_rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
    rr64_log(
        "[RR64-LAUNCHER] Validated ROM available: %s; primary action: %s.\n",
        stored_rom_valid ? "yes" : "no",
        stored_rom_valid ? "START GAME" : "SELECT GAME ROM"
    );
    game_options->add_start_game_or_load_rom_option("Select Game ROM", "Start Game")->get_label()->set_white_space(recompui::WhiteSpace::Nowrap);
    game_options->add_settings_option("Settings")->get_label()->set_white_space(recompui::WhiteSpace::Nowrap);
    game_options->add_exit_option("Exit")->get_label()->set_white_space(recompui::WhiteSpace::Nowrap);

    game_options->set_width(stored_rom_valid ? 320.0f : 400.0f);
    game_options->set_left(54.0f);
    game_options->set_bottom(62.0f);
    game_options->set_padding(16.0f);
    game_options->set_background_color(recompui::Color{ 0, 0, 0, 164 });
    game_options->set_border_radius(recompui::theme::border::radius_lg);

    // Custom contexts require RmlUi and RecompFrontend's document factory to
    // be initialized. The launcher callback is the first supported lifecycle
    // point where both are ready; creating the online context during config
    // registration dereferences an uninitialized RmlUi interface at startup.
    rr64::online_menu::initialize_ui();
    rr64::local_race_options::initialize(recompui::file::get_app_folder_path());
    rr64::character_preferences::initialize(recompui::file::get_app_folder_path());
    rr64::rider_skin_preferences::initialize(recompui::file::get_app_folder_path());
    rr64::achievements::initialize_toast_ui();
}

void on_launcher_update(recompui::LauncherMenu*) {
    if (rr64::mk64_import::busy()) return;
    // Run unattended start from the frontend's render/UI thread, matching the
    // normal launcher option callback. Starting from update_gfx races RmlUi's
    // element traversal because that callback belongs to the SDL event thread.
    if (!g_auto_start_pending.load() || !g_renderer_created || !g_launcher_init_seen) {
        return;
    }

    if (!recomp::is_rom_valid(supported_games[0].game_id)) {
        g_auto_start_pending.store(false);
        rr64_log("[RR64] Direct start needs a configured ROM; showing ROM selection.\n");
        return;
    }
    bool expected = false;
    if (g_auto_start_dispatched.compare_exchange_strong(expected, true)) {
        rr64_log("[RR64-AUTO] Renderer and launcher ready; starting staged ROM from UI thread.\n");
        recomp::start_game(supported_games[0].game_id, {});
        // Match the normal launcher option callback: once the game starts, the
        // launcher must stop drawing so it cannot cover the game's swap chain.
        recompui::hide_all_contexts();
        g_auto_start_pending.store(false);
        rr64_log("[RR64-AUTO] Launcher hidden after UI-thread start.\n");
    }
}

void on_ui_update() {
    static bool first_update = true;
    if (first_update) { recompui::startup_checkpoint("first-ui-update"); first_update = false; }
#ifdef _WIN32
    DWORD expected_ui_thread = 0;
    g_ui_thread_id.compare_exchange_strong(expected_ui_thread, GetCurrentThreadId());
#endif
    rr64::netplay::update();
    recompinput::players::refresh_connected_players(false); // Keyboard is assigned explicitly in Controls.
    rr64::online_menu::update_ui();
    rr64::offline_modifiers::update_ui();
    rr64::race_pack_mod_ui::update();
    rr64::local_race_options::flush();
    rr64::character_preferences::flush();
    rr64::rider_skin_preferences::flush();
    static const bool lap_trace_enabled = [] {
        const auto flag = [](const char* name) {
            const char* value = std::getenv(name);
            return value && value[0] && value[0] != '0';
        };
        const char* physics = std::getenv("RR64_COURSE_PHYSICS_TRACE");
        const bool physics_enabled = physics && physics[0] == '1' && physics[1] == '\0';
        return flag("RR64_RUNTIME_TRACE") || flag("RR64_DIAGNOSTICS") || physics_enabled;
    }();
    if (lap_trace_enabled) {
        // Transport status owns dynamic strings/rosters. Publish just the
        // diagnostic human mask here so the guest observer never allocates.
        const auto status = rr64::netplay::get_status();
        rr64::race_end_trace::set_authority_humans(
            status.active && status.connected && status.authoritative ? status.authority_humans : 0u);
    }
    rr64::race_end_trace::drain();
    rr64::music::update_ui();
    rr64::achievements::update_ui();

    // Record UI ownership changes separately from guest race modes. A report
    // that the game returned to its launcher is not necessarily an exception
    // or guest-main-menu transition. This runs on the UI owner after its updates;
    // it does not show/hide contexts, alter input, or log unchanged frames.
    if (detailed_diagnostics_enabled() && g_launcher_init_seen) {
        const bool started = ultramodern::is_game_started();
        const bool launcher = recompui::is_context_shown(recompui::get_launcher_context_id());
        const bool settings = recompui::is_context_shown(recompui::config::get_config_context_id());
        const bool prompt = recompui::is_prompt_open();
        const bool capturing = recompui::is_context_capturing_input();
        const unsigned state = unsigned(started) | (unsigned(launcher) << 1) |
            (unsigned(settings) << 2) | (unsigned(prompt) << 3) | (unsigned(capturing) << 4);
        static unsigned previous_state = ~0u;
        if (state != previous_state) {
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            rr64_log("[RR64-UI-STATE] time-ns=%lld game-started=%u launcher=%u settings=%u prompt=%u input-captured=%u\n",
                static_cast<long long>(now), unsigned(started), unsigned(launcher),
                unsigned(settings), unsigned(prompt), unsigned(capturing));
            previous_state = state;
        }
    }
}
} // namespace

extern "C" void rr64_course_progress_log(const char *format, ...) {
    if (!detailed_diagnostics_enabled() || !format) return;
    char buffer[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    rr64_log("%s", buffer);
}

extern "C" void rr64_record_guest_cadence(unsigned int frames, double seconds,
    double update_hz, float physics_delta, float update_ticks, float wait_ticks,
    float total_ticks, unsigned int mode, unsigned int pending_mode,
    unsigned int pause_state, unsigned int gameplay_active) {
    rr64_log("[RR64-RACE] frames=%u interval=%.3fs update-hz=%.2f physics-dt=%.6f"
        " update-ticks=%.3f wait-ticks=%.3f total-ticks=%.3f"
        " mode=%u pending=%u pause=%u gameplay=%u sample=%s\n",
        frames, seconds, update_hz, physics_delta, update_ticks, wait_ticks, total_ticks,
        mode, pending_mode, pause_state, gameplay_active, frames == 1 ? "start" : "interval");
}

extern "C" void rr64_record_pipeline_stage(unsigned int index,
    unsigned long long nanoseconds) {
    if (!detailed_diagnostics_enabled()) { return; }
    if (index >= g_pipeline_stages.size()) { return; }
    record_stage_sample(g_pipeline_stages[index], nanoseconds);
}

extern "C" void rr64_record_scheduler_stage(unsigned int index,
    unsigned long long nanoseconds) {
    if (!detailed_diagnostics_enabled()) { return; }
    if (index < g_scheduler_stages.size()) {
        record_stage_sample(g_scheduler_stages[index], nanoseconds);
    }
}

extern "C" void rr64_record_scheduler_event(unsigned int index,
    unsigned long long amount) {
    if (!detailed_diagnostics_enabled()) { return; }
    if (index < g_scheduler_events.size()) {
        g_scheduler_events[index].fetch_add(amount, std::memory_order_relaxed);
    }
}

GuestQueueCounters* find_queue_counters(unsigned int address) {
    if (address == 0) { return nullptr; }
    for (auto& slot : g_guest_queue_counters) {
        auto observed = slot.queue.load(std::memory_order_relaxed);
        if (observed == 0) {
            slot.queue.compare_exchange_strong(observed, address, std::memory_order_relaxed);
            observed = slot.queue.load(std::memory_order_relaxed);
        }
        if (observed == address) { return &slot; }
    }
    g_guest_queue_overflow_samples.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

extern "C" void rr64_record_guest_queue_wait(unsigned int address,
    unsigned long long nanoseconds) {
    if (!detailed_diagnostics_enabled()) { return; }
    if (auto* slot = find_queue_counters(address)) {
        record_stage_sample(slot->timing, nanoseconds);
    }
}

extern "C" void rr64_record_external_queue_event(unsigned int address,
    unsigned int outcome, unsigned long long nanoseconds) {
    if (!detailed_diagnostics_enabled()) { return; }
    if (outcome >= 3) { return; }
    if (auto* slot = find_queue_counters(address)) {
        slot->external_outcomes[outcome].fetch_add(1, std::memory_order_relaxed);
        if (outcome == 0) {
            record_stage_sample(slot->external_delivery, nanoseconds);
        }
    }
}

extern "C" void rr64_record_pipeline_resources(unsigned long long targets,
    unsigned long long target_bytes, unsigned long long batches,
    unsigned long long batch_bytes, unsigned long long textures,
    unsigned long long retired_textures, unsigned long long reuse_hits,
    unsigned long long reuse_misses, unsigned long long recycled_bytes, unsigned long long recycled_images) {
    const std::array<std::uint64_t, 10> values{
        targets, target_bytes, batches, batch_bytes, textures, retired_textures,
        reuse_hits, reuse_misses, recycled_bytes, recycled_images};
    for (std::size_t i = 0; i < values.size(); ++i) {
        g_pipeline_resources[i].store(values[i], std::memory_order_relaxed);
    }
}

extern "C" void rr64_record_authored_sample(unsigned long long writer,
    unsigned long long timestamp_ns, unsigned long long interval_ns,
    unsigned int source_rate, unsigned int timing_reason,
    unsigned long long camera_hash, unsigned long long world_hash) {
    // These classify parsed workload cadence, not distinct poses or successful
    // interpolation: 0 warmup, 1 cadence60, 2 cadence30, 3 discontinuity.
    const auto reason = timing_reason < g_authored_timing_decisions.size() ? timing_reason : 0u;
    const auto reason_sample = g_authored_timing_decisions[reason].fetch_add(1, std::memory_order_relaxed);
    g_authored_timing_source_rate.store(source_rate, std::memory_order_relaxed);
    if (interval_ns > 0) {
        g_authored_timing_interval_samples.fetch_add(1, std::memory_order_relaxed);
        g_authored_timing_interval_total_ns.fetch_add(interval_ns, std::memory_order_relaxed);
        std::uint64_t previous_max = g_authored_timing_interval_max_ns.load(std::memory_order_relaxed);
        while (interval_ns > previous_max && !g_authored_timing_interval_max_ns.compare_exchange_weak(
            previous_max, interval_ns, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
    struct PreviousAuthoredSample {
        unsigned long long writer = 0;
        unsigned long long timestamp_ns = 0;
        unsigned long long camera_hash = 0;
        unsigned long long world_hash = 0;
    };
    // The State producer owns each sequence. Window resets clear only counters,
    // keeping the preceding sample available for a comparison at the boundary.
    static thread_local PreviousAuthoredSample previous;
    const bool consecutive = (previous.writer != 0u) && (writer > previous.writer) &&
        ((writer - previous.writer) == 1u) && (timing_reason != 3u) &&
        (interval_ns > 0u) && (timestamp_ns > previous.timestamp_ns) &&
        ((timestamp_ns - previous.timestamp_ns) == interval_ns);
    const bool camera_changed = camera_hash != previous.camera_hash;
    const bool world_changed = world_hash != previous.world_hash;
    if (consecutive) {
        g_authored_camera_changes[camera_changed ? 1u : 0u].fetch_add(1, std::memory_order_relaxed);
        g_authored_world_changes[world_changed ? 1u : 0u].fetch_add(1, std::memory_order_relaxed);
    }
    // The health reporter exchanges the decision counters every ten seconds,
    // renewing eight consecutive samples per cadence instead of exhausting the
    // trace during a countdown. Hash equality alone never merges workloads.
    if (reason_sample < 8u) {
        rr64_log("[RR64-PERF] authored-sample writer=%llu timestamp-ns=%llu interval-ns=%llu"
            " previous-writer=%llu previous-ns=%llu source=%u reason=%u"
            " camera-hash=%016llX world-hash=%016llX consecutive=%u"
            " camera-changed=%u world-changed=%u\n",
            writer, timestamp_ns, interval_ns, previous.writer, previous.timestamp_ns,
            source_rate, timing_reason, camera_hash, world_hash,
            consecutive ? 1u : 0u, consecutive && camera_changed ? 1u : 0u,
            consecutive && world_changed ? 1u : 0u);
    }
    previous = {writer, timestamp_ns, camera_hash, world_hash};
}

extern "C" void rr64_record_present_sequence(unsigned long long presentId,
    unsigned long long writer, unsigned long long watermark,
    unsigned long long previousWriter, unsigned long long authoredNs,
    unsigned long long previousNs, unsigned int sourceRate,
    unsigned int targetRate, unsigned int images, unsigned int mode) {
    if (mode >= g_present_sequence_modes.size()) {
        return;
    }
    // Counts describe selected batches and dropped queue events. They are
    // separate from the backend's accepted/busy/occluded presentation outcomes.
    const auto mode_sample = g_present_sequence_modes[mode].fetch_add(1, std::memory_order_relaxed);
    if (mode_sample < 8u) {
        static constexpr std::array<const char *, 4> mode_names{
            "native-target", "owned-native", "owned-fractional", "skipped-queued"};
        rr64_log("[RR64-PERF] present-sequence-sample present=%llu writer=%llu watermark=%llu"
            " previous-writer=%llu authored-ns=%llu previous-ns=%llu"
            " source=%u target=%u images=%u mode=%u:%s\n",
            presentId, writer, watermark, previousWriter, authoredNs, previousNs,
            sourceRate, targetRate, images, mode, mode_names[mode]);
    }
}

extern "C" void rr64_record_present_interval(
    unsigned int target_rate,
    unsigned int original_rate,
    unsigned int interval_us,
    unsigned int pre_timer_us,
    unsigned int timer_overrun_us,
    unsigned int previous_present_call_us) {
    g_present_target_rate.store(target_rate, std::memory_order_relaxed);
    g_present_original_rate.store(original_rate, std::memory_order_relaxed);
    g_present_interval_samples.fetch_add(1, std::memory_order_relaxed);
    g_present_interval_total_us.fetch_add(interval_us, std::memory_order_relaxed);

    std::uint32_t previous_worst =
        g_present_interval_worst_us.load(std::memory_order_relaxed);
    while (interval_us > previous_worst &&
        !g_present_interval_worst_us.compare_exchange_weak(
            previous_worst,
            interval_us,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }

    std::uint32_t previous_worst_pre =
        g_present_worst_before_timer_us.load(std::memory_order_relaxed);
    while (pre_timer_us > previous_worst_pre &&
        !g_present_worst_before_timer_us.compare_exchange_weak(
            previous_worst_pre,
            pre_timer_us,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }

    std::uint32_t previous_worst_overrun =
        g_present_worst_timer_overrun_us.load(std::memory_order_relaxed);
    while (timer_overrun_us > previous_worst_overrun &&
        !g_present_worst_timer_overrun_us.compare_exchange_weak(
            previous_worst_overrun,
            timer_overrun_us,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }

    std::uint32_t previous_worst_present =
        g_present_worst_present_call_us.load(std::memory_order_relaxed);
    while (previous_present_call_us > previous_worst_present &&
        !g_present_worst_present_call_us.compare_exchange_weak(
            previous_worst_present,
            previous_present_call_us,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }

    const std::uint32_t frame_budget_us =
        target_rate > 0 ? (1'000'000u / target_rate) : 33'333u;
    const std::uint32_t late_threshold_us =
        frame_budget_us + frame_budget_us / 2u;
    if (interval_us > late_threshold_us) {
        g_present_interval_over_budget.fetch_add(1, std::memory_order_relaxed);
        if (previous_present_call_us > (frame_budget_us / 2u)) {
            g_present_late_present_call.fetch_add(1, std::memory_order_relaxed);
        }
        else if (pre_timer_us > late_threshold_us) {
            g_present_late_before_timer.fetch_add(1, std::memory_order_relaxed);
        }
        else if (timer_overrun_us > (frame_budget_us / 2u)) {
            g_present_late_timer.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            g_present_late_other.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

extern "C" void rr64_record_present_batch(int interpolated, int metadata_rejected) {
    (interpolated ? g_consumer_interpolated_batches : g_consumer_repeated_batches)
        .fetch_add(1, std::memory_order_relaxed);
    if (metadata_rejected) {
        g_consumer_rejected_batches.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" void rr64_record_owned_batch(unsigned long long writer,
    unsigned int address, unsigned int images, unsigned int published) {
    const unsigned int outcome = published != 0;
    g_owned_produced[outcome].fetch_add(1, std::memory_order_relaxed);
    static std::array<std::atomic_uint32_t, 2> samples{};
    if (samples[outcome].fetch_add(1, std::memory_order_relaxed) < 6u) {
        rr64_log("[RR64-PERF] owned-producer writer=%llu target=%08X images=%u published=%u\n",
            writer, address, images, outcome);
    }
}

extern "C" void rr64_record_geometry_compatibility(unsigned long long writer,
    unsigned int compatible, unsigned int reasons) {
    const unsigned int outcome = compatible != 0;
    g_geometry_compatible[outcome].fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < g_geometry_reasons.size(); ++i) {
        if ((reasons & (1u << i)) != 0) {
            g_geometry_reasons[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    static std::array<std::atomic_uint32_t, 2> samples{};
    if (samples[outcome].fetch_add(1, std::memory_order_relaxed) < 6u) {
        rr64_log("[RR64-PERF] geometry-sample writer=%llu compatible=%u reasons=%u\n",
            writer, outcome, reasons);
    }
}

extern "C" void rr64_record_owned_present(unsigned long long writer,
    unsigned long long watermark, unsigned int address, unsigned int hit) {
    const unsigned int outcome = hit != 0;
    g_owned_presented[outcome].fetch_add(1, std::memory_order_relaxed);
    static std::array<std::atomic_uint32_t, 2> samples{};
    if (samples[outcome].fetch_add(1, std::memory_order_relaxed) < 6u) {
        rr64_log("[RR64-PERF] owned-consumer writer=%llu watermark=%llu target=%08X hit=%u\n",
            writer, watermark, address, outcome);
    }
}

extern "C" void rr64_record_present_batch_mismatch(unsigned int reasons) {
    for (std::size_t reason = 0; reason < g_consumer_rejection_reasons.size(); ++reason) {
        if ((reasons & (1u << reason)) != 0u) {
            g_consumer_rejection_reasons[reason].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

extern "C" void rr64_record_present_target_sample(
    unsigned long long produced_workload_id,
    unsigned long long requested_workload_id,
    unsigned long long present_id,
    unsigned int produced_address, unsigned int produced_width, unsigned int produced_siz,
    unsigned int selected_address, unsigned int selected_width, unsigned int selected_siz,
    unsigned int vi_origin, unsigned int vi_address, unsigned int vi_width, unsigned int vi_siz,
    unsigned int frame_count, unsigned int presentation_mode)
{
    // A focused diagnostic for R2's persistent target-only rejection. The
    // first twelve failures are enough to expose rotating buffers/row offsets;
    // never create a per-frame capture or read additional guest memory here.
    static std::atomic_uint32_t samples{0};
    if (samples.load(std::memory_order_relaxed) >= 12u) { return; }
    const auto sample = samples.fetch_add(1u, std::memory_order_relaxed);
    if (sample >= 12u) { return; }
    rr64_log("[RR64-PERF] target-sample=%u produced-workload=%llu"
        " requested-workload=%llu present=%llu"
        " produced=%08X/%u/%u selected=%08X/%u/%u"
        " vi-origin=%08X vi-address=%08X vi-width=%u vi-siz=%u"
        " frames=%u presentation-mode=%u\n",
        sample + 1u, produced_workload_id, requested_workload_id, present_id,
        produced_address, produced_width, produced_siz,
        selected_address, selected_width, selected_siz,
        vi_origin, vi_address, vi_width, vi_siz, frame_count, presentation_mode);
}

extern "C" void rr64_record_source_cadence(
    unsigned int raw_factor,
    unsigned int stable_factor,
    unsigned int pending_factor,
    unsigned int pending_samples,
    unsigned int source_rate,
    unsigned int race_active) {
    g_source_raw_factor.store(raw_factor, std::memory_order_relaxed);
    g_source_stable_factor.store(stable_factor, std::memory_order_relaxed);
    g_source_pending_factor.store(pending_factor, std::memory_order_relaxed);
    g_source_pending_samples.store(pending_samples, std::memory_order_relaxed);
    g_source_rate.store(source_rate, std::memory_order_relaxed);
    g_source_race_active.store(race_active, std::memory_order_relaxed);
}

extern "C" void rr64_record_interpolation_workload(
    unsigned int target_rate,
    unsigned int original_rate,
    unsigned int planned_frames,
    unsigned int rendered_frames,
    unsigned int target_selection,
    unsigned int backlog_dropped_frames) {
    if (target_rate == 0) {
        return;
    }

    g_interpolation_target_rate.store(target_rate, std::memory_order_relaxed);
    g_interpolation_original_rate.store(original_rate, std::memory_order_relaxed);
    g_interpolation_workloads.fetch_add(1, std::memory_order_relaxed);
    // Count output quantity without asserting motion or unique picture content.
    // Single-image includes native 60 Hz and unresolved-cadence fallbacks.
    g_production_image_counts[std::min(rendered_frames, 2u)]
        .fetch_add(1, std::memory_order_relaxed);
    g_interpolation_planned_frames.fetch_add(planned_frames, std::memory_order_relaxed);
    g_interpolation_rendered_frames.fetch_add(rendered_frames, std::memory_order_relaxed);
    g_interpolation_backlog_dropped_frames.fetch_add(
        backlog_dropped_frames,
        std::memory_order_relaxed);
    switch (target_selection) {
    case 1:
        g_interpolation_stock_flag_workloads.fetch_add(1, std::memory_order_relaxed);
        break;
    case 2:
        g_interpolation_history_target_workloads.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        g_interpolation_unavailable_workloads.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

#include "rr64_sync_log.hpp"

int main(int argc, char** argv) {
    if(argc>=2 && std::strcmp(argv[1],"--replay-case")==0){
        if(argc!=4 && argc!=5)return EXIT_FAILURE;
        if(argc==5 && std::strcmp(argv[4],"--live-baseline")!=0)return EXIT_FAILURE;
        return rr64_prediction_run_case(argv[2],argv[3],argc==5);
    }
    rr64::sync_log::initialize();
    initialize_runtime_diagnostics();

#ifdef _WIN32
    // RecompFrontend resolves its assets relative to the working directory on
    // Windows. Anchor it to the executable so launchers can start from anywhere.
    const std::filesystem::path program_directory = executable_directory();
    std::error_code working_directory_error;
    std::filesystem::current_path(program_directory, working_directory_error);
    if (working_directory_error) {
        rr64_log("[RR64] Unable to set working directory to executable folder: %s\n", working_directory_error.message().c_str());
        return EXIT_FAILURE;
    }
    rr64_log("[RR64-DIAG] Working directory: " RR64_PATH_FORMAT "\n", program_directory.c_str());
#endif

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--force-d3d12") == 0) {
            g_forced_graphics_api = ForcedGraphicsApi::D3D12;
        }
        else if (std::strcmp(argv[i], "--force-vulkan") == 0) {
            g_forced_graphics_api = ForcedGraphicsApi::Vulkan;
        }
        else if (std::strcmp(argv[i], "--skip-launcher") == 0) {
            g_auto_start_pending.store(true);
        }
        else if (std::strcmp(argv[i], "--auto-rom") == 0) {
            if ((i + 1) >= argc) {
                rr64_log("[RR64-AUTO] --auto-rom requires a ROM path.\n");
                return EXIT_FAILURE;
            }
            g_auto_rom_path = std::filesystem::path(argv[++i]);
        }
    }
    rr64_log("[RR64-DIAG] Requested graphics API: %s\n", forced_graphics_api_name());

#ifdef _WIN32
    plume::setD3D12PresentOutcomeCallback(record_d3d12_present_outcome);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", 1);
#endif

    rr64_log("[RR64] Road Rash 64 playable checkpoint %s\n", kVersion);
    rr64_log("[RR64] Stable presentation policy: %s (RR64_STABLE_PRESENTATION)\n",
        RT64::RR64FramePacing::stablePresentationEnabled() ? "enabled" : "disabled control");
    rr64_log("[RR64] Render-only Max LOD candidate: %s (RR64_RENDER_ONLY_MAX_LOD)\n",
        rr64_render_only_max_lod_enabled() ? "enabled" : "disabled");
    rr64_log("[RR64] World distance candidate: %s (RR64_WORLD_DISTANCE)\n",
        rr64_world_distance_enabled() ? "enabled" : "disabled");
    rr64_log("[RR64] Expected ROM XXH3-64: 0x%016" PRIX64 "\n", kRoadRash64UsXxh3);

    recomp::Version project_version{};
    // Public releases retain the mod interfaces of the internal 1.0.6 builds.
    // Keep already-installed packs working without changing their manifests.
    recomp::Version legacy_mod_compatibility{};
    recomp::Version::from_string("1.0.6", legacy_mod_compatibility);
    recomp::set_mod_compatibility_version(legacy_mod_compatibility);
    if (!recomp::Version::from_string(kVersion, project_version)) {
        std::fprintf(stderr, "[RR64] Invalid project version string.\n");
        return EXIT_FAILURE;
    }

    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "[RR64] Native file dialog initialization failed.\n");
        return EXIT_FAILURE;
    }

    rr64_log("[RR64-STAGE] Program configuration.\n");
    recompui::programconfig::set_program_name(kProgramName);
    recompui::programconfig::set_program_id(kProgramId);

    // Use the same redistributable-font approach as established RecompFrontend
    // releases instead of copying a font from the local Windows installation.
    recompui::register_primary_font("LatoLatin-Regular.ttf", "LatoLatin");
    const auto app_folder = recompui::file::get_app_folder_path();
#ifdef _WIN32
    rr64_log("[RR64-STAGE] App/config folder: " RR64_PATH_FORMAT "\n", app_folder.c_str());
#else
    rr64_log("[RR64-STAGE] App/config folder: %s\n", app_folder.string().c_str());
#endif
    recomp::register_config_path(app_folder);

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    configure_frontend_theme();

    if (!g_auto_rom_path.empty()) {
        rr64_log("[RR64-AUTO] Validating developer ROM: %s\n", g_auto_rom_path.string().c_str());
        const recomp::RomValidationError rom_result = recomp::select_rom(g_auto_rom_path, supported_games[0].game_id);
        rr64_log("[RR64-AUTO] ROM validation result=%d.\n", static_cast<int>(rom_result));
        if (rom_result != recomp::RomValidationError::Good) {
            rr64_log("[RR64-AUTO] Developer ROM validation failed; game will not start.\n");
            return EXIT_FAILURE;
        }
        g_auto_start_pending.store(true);
    }

    recompui::register_ui_exports();
    rr64_register_overlays();
    recompinput::players::set_single_player_mode(true);

    // RecompFrontend's input/event and RT64 UI layers require the standard
    // configuration tabs to exist before recomp::start() launches the graphics
    // and event threads. Working RecompFrontend ports initialize and finalize
    // these tabs before registering the launcher callback.
    // Enumerate input devices only after SDL audio initialization; this opens
    // no microphone. Actual capture remains confined to enabled online races.
    const int audio_init_result = SDL_InitSubSystem(SDL_INIT_AUDIO);
    init_recompui_config();
    recompui::register_update_callback(on_ui_update);
    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback(on_launcher_update);

    rr64_log("[RR64-STAGE] Initializing SDL audio subsystem.\n");
    rr64_log("[RR64-STAGE] SDL audio init returned %d (%s).\n", audio_init_result, SDL_GetError());
    if (audio_init_result < 0) {
        rr64_log("[RR64] SDL audio initialization failed: %s\n", SDL_GetError());
    }
    else if (!reset_audio(48000)) {
        rr64_log("[RR64] Continuing without initialized audio output.\n");
    }
    else {
        rr64_log("[RR64-STAGE] Host audio device initialized.\n");
    }

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            rr64_log("[RR64-STAGE] Renderer context creation entered (developer_mode=%d).\n", developer_mode ? 1 : 0);

            // N64ModernRuntime exposes the active graphics configuration as const.
            // Make a copy, apply the diagnostic backend override, then commit it
            // through the supported setter before RecompFrontend constructs RT64.
            auto graphics_config = ultramodern::renderer::get_graphics_config();
            bool graphics_config_changed = false;
            switch (g_forced_graphics_api) {
            case ForcedGraphicsApi::D3D12:
                graphics_config.api_option = ultramodern::renderer::GraphicsApi::D3D12;
                graphics_config_changed = true;
                break;
            case ForcedGraphicsApi::Vulkan:
                graphics_config.api_option = ultramodern::renderer::GraphicsApi::Vulkan;
                graphics_config_changed = true;
                break;
            case ForcedGraphicsApi::Auto:
            default:
                break;
            }
            if (graphics_config_changed) {
                ultramodern::renderer::set_graphics_config(graphics_config);
            }
            const auto& active_graphics_config = ultramodern::renderer::get_graphics_config();
            rr64_log("[RR64-RT64] Graphics API request entering RT64: %s (enum=%d).\n",
                forced_graphics_api_name(), static_cast<int>(active_graphics_config.api_option));

            try {
                auto context = recompui::renderer::create_render_context(
                    rdram,
                    window_handle,
                    // Road Rash can reach a full-sync point while its displayed
                    // framebuffer is only partially composed. PresentEarly may
                    // expose that intermediary image for one host frame, with
                    // riders and HUD visible over a cleared world. Present only
                    // at the emulated console's VI boundary instead.
                    ultramodern::renderer::PresentationMode::Console,
                    developer_mode
                );
                if (!context) {
                    throw std::runtime_error("RecompFrontend returned a null RT64 renderer context");
                }
                recompui::startup_checkpoint("renderer-context-ready");
                g_renderer_created = true;
                rr64_log("[RR64-STAGE] Renderer context creation returned successfully.\n");
                return context;
            }
            catch (const std::exception& ex) {
                rr64_log("\n[RR64-CRASH] C++ exception during renderer creation: %s\n", ex.what());
                rr64_log("[RR64-RT64] Requested API: %s\n", forced_graphics_api_name());
                rr64_log("[RR64-RT64] Retry with the alternate API runner (D3D12 or Vulkan).\n");
#ifdef _WIN32
                std::string message = std::string("RT64 renderer initialization failed.\n\n") +
                    ex.what() +
                    "\n\nTry selecting the other graphics API in Settings.\n\nLog:\n" +
                    g_runtime_log_path.string();
                MessageBoxA(nullptr, message.c_str(), "Road Rash 64 Recompiled - RT64 Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
#endif
                std::quick_exit(EXIT_FAILURE);
            }
            catch (...) {
                rr64_log("\n[RR64-CRASH] Unknown C++ exception during renderer creation.\n");
                rr64_log("[RR64-RT64] Requested API: %s\n", forced_graphics_api_name());
                rr64_log("[RR64-RT64] Retry with the alternate API runner (D3D12 or Vulkan).\n");
                std::quick_exit(EXIT_FAILURE);
            }
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = get_input_with_trace,
        .set_rumble = set_gameplay_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = update_gameplay_rumble,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_callbacks{
        .message_box = [](const char* message) {
            // Preserve runtime errors before a modal blocks its calling thread.
            rr64_log("[RR64-UI-ERROR] runtime-message=%s\n", message ? message : "<null>");
            recompui::message_box(message);
        },
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    // Texture packs are normal runtime mods. An RT64 .rtz is a self-contained
    // replacement pack whose rt64.json database identifies its texture data.
    // Registering the content and container types lets the standard Mods UI
    // install, order, enable, disable, and hot-reload packs without putting any
    // third-party artwork or original game assets in the executable.
    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_packs,
    };
    const auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);
    if (!recomp::mods::register_mod_container_type("rtz", { texture_pack_content_type_id }, false)) {
        rr64_log("[RR64-MODS] Warning: failed to register the RT64 texture-pack container type.\n");
    }

    rr64_log("[RR64] Starting N64ModernRuntime/RecompFrontend.\n");
    rr64_log("[RR64-STAGE] Entering recomp::start.\n");
    // Achievement unlocks at race results must not do disk I/O on the UI or
    // network thread. The worker coalesces changes and orderly exit flushes
    // the last snapshot, including unlocks not yet shown as a toast.
    std::jthread progress_service_thread([](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            rr64::achievements::flush_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    std::jthread netplay_service_thread([](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            rr64::netplay::update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    // Audio-device opening and codec work must not stall movement servicing.
    // Voice still has one owner; join it before destroying its device/codecs.
    std::jthread voice_service_thread([](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            rr64::voice_chat::update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    try {
        recomp::start(
            project_version,
            {},
            rsp_callbacks,
            renderer_callbacks,
            audio_callbacks,
            input_callbacks,
            gfx_callbacks,
            events_callbacks,
            error_callbacks,
            threads_callbacks
        );
    }
    catch (const std::exception& ex) {
        rr64_log("\n[RR64-CRASH] C++ exception escaped recomp::start: %s\n", ex.what());
        rr64::mk64_import::shutdown();
        rr64::netplay::shutdown();
        std::quick_exit(EXIT_FAILURE);
    }
    catch (...) {
        rr64_log("\n[RR64-CRASH] Unknown C++ exception escaped recomp::start.\n");
        rr64::mk64_import::shutdown();
        rr64::netplay::shutdown();
        std::quick_exit(EXIT_FAILURE);
    }
    rr64::mk64_import::shutdown();
    voice_service_thread.request_stop();
    netplay_service_thread.request_stop();
    progress_service_thread.request_stop();
    voice_service_thread.join();
    netplay_service_thread.join();
    progress_service_thread.join();
    rr64::achievements::flush_progress();
    rr64::voice_chat::shutdown();
    rr64::netplay::shutdown();
    rr64_log("[RR64-STAGE] recomp::start returned. window=%d renderer=%d launcher=%d update=%d\n",
        g_window_created ? 1 : 0,
        g_renderer_created ? 1 : 0,
        g_launcher_init_seen ? 1 : 0,
        g_update_gfx_seen ? 1 : 0
    );
#ifdef _WIN32
    if (!g_window_created) {
        std::wstring message =
            L"Road Rash 64 Recompiled returned before creating a window.\n\n"
            L"See the diagnostic log:\n";
        message += g_runtime_log_path.wstring();
        MessageBoxW(nullptr, message.c_str(), L"Road Rash 64 Recompiled - Early Runtime Exit", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
#endif

    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
    }
    if (g_audio_trace_file != nullptr) {
        std::fclose(g_audio_trace_file);
        g_audio_trace_file = nullptr;
    }
    NFD_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    { DiagnosticReport report; report_preparation_trace(); }
    rr64_log("[RR64] Native probe exiting normally.\n");
    rr64_prediction_flush_cases();
    if (g_runtime_log != nullptr) {
        std::fclose(g_runtime_log);
        g_runtime_log = nullptr;
    }
    // recomp::start() only joins its bootstrap thread; the emulated N64 OS
    // threads it spawns (game/audio/etc.) are left running, permanently
    // blocked in ultramodern::wait_for_external_message() on a static
    // condition_variable (mesgqueue.cpp). A normal return here would run
    // that condition_variable's destructor via libc's exit() while those
    // threads are still waiting on it; glibc's pthread_cond_destroy blocks
    // in that situation, deadlocking the process. quick_exit() skips static
    // destructors entirely and terminates immediately, matching how a plain
    // process exit behaves on Windows (where condition_variable's destructor
    // over CONDITION_VARIABLE is a no-op, so the same abandoned threads are
    // harmless there).
#ifdef _WIN32
    return EXIT_SUCCESS;
#else
    std::quick_exit(EXIT_SUCCESS);
#endif
}












extern "C" bool rr64_draw_distance_enabled() {
    return rr64::presentation_options::draw_distance.load(std::memory_order_relaxed)>0;
}
extern "C" double rr64_draw_distance_percent() {
    return rr64::presentation_options::draw_distance.load(std::memory_order_relaxed);
}
