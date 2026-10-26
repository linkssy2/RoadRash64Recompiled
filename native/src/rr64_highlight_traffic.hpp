#include "rr64_traffic_roster.hpp"
#pragma once

#include "rr64_world_object_assets.hpp"
#include "rr64_world_sync.hpp"
#include <array>
#include <span>
#include <string>

namespace rr64::highlights {
struct TrafficAssets {
    // Every byte, including geometry, child matrices and textures, is owned.
    // No native scene, allocator, graph or streamed resource pointer is kept.
    world::ObjectAssets assets;
    std::array<unsigned, 0x127> models{};
};
bool build_traffic_assets(std::span<const unsigned char> rom, TrafficAssets &output,
                          std::string &error) noexcept;

// Prewarm once at race loading/entry, never in a draw or race-finish callback.
// Starts a fresh per-race identity namespace; keep the normal race camera cut.
bool prepare_traffic(unsigned char *memory) noexcept;
// Call once after source-2 camera preparation and before native traffic draw.
// True transfers this view's COMPLETE traffic presentation to the recording;
// only then may the caller hide every native traffic node to prevent doubles.
// False appends nothing and leaves the existing retained-car path available.
bool draw_traffic(unsigned char *memory,
                  const std::array<world_sync::Traffic, rr64::traffic::capacity> &cars,
                  std::uint64_t recording_time_us = 0) noexcept;
// Only with the runtime's guest-heap/session reset, alongside objects_reset_session.
// Replay/race completion must keep allocations alive for submitted graphics.
void reset_traffic_session() noexcept;
struct TrafficStatistics {
    unsigned models = 0, cached_bytes = 0, cars = 0;
    unsigned long long frames = 0, refusals = 0;
};
TrafficStatistics traffic_statistics() noexcept;
}
