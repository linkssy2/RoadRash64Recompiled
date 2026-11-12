#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "rr64_actor_render_snapshot.hpp"

namespace rr64::lod {
struct RootRangeSample {
    std::uint32_t node = 0, record = 0, type = 0, view = 0, epoch = 0, source = 0;
    float x = 0, y = 0, z = 0, maximum = 0;
};
struct RootRangeReport {
    std::array<RootRangeSample, 64> samples{};
    std::uint64_t examined = 0, nearLimit = 0, replaced = 0;
};
RootRangeReport take_root_range_report();
enum class ActivityCounter : std::size_t {
    Draw,
    PairObserved,
    PairQueued,
    Prepare,
    EmptyPrepare,
    Select,
    InactiveSelect,
    SceneRejected,
    Allocation,
    AllocationAccepted,
    PreflightRejected,
    StageRejected,
    PublishRejected,
    MissingBikeRoot,
    MissingRiderRoot,
    MissingRiderAnimation,
    MissingBikeAnimation,
    ConsumedSlot0,
    ConsumedSlot1,
    StockLodMismatch,
    BindingRejected,
    BikeTier1ToMax,
    BikeTier2ToMax,
    RiderTier1ToMax,
    RiderTier2ToMax,
    BikeFarNormalized,
    RiderFarNormalized,
    RiderRangeObserved,
    RiderRangeRestored,
    RiderRangeRejected,
    FarBikeFallback,
    FarRiderFallback,
    HeldBikePrepared,
    FullWeightRiderPrepared,
    HeldResultsPrepared,
    FinishBlendPrepared,
    FinishSeedPrepared,
    Count
};

struct ActorDetailSnapshot {
    std::uint64_t detailed = 0, fallback = 0;
    std::array<std::uint64_t, 3> fallback_by_lod{};
    std::uint32_t rider_style = 0;
    std::uint32_t node = 0, stock_lod = 0, detail_list = 0, fallback_list = 0;
    float detailed_distance = 0.0f, fallback_distance = 0.0f;
    FindFailure last_failure = FindFailure::None;
    // Last fallback only; a zero-fallback interval may retain older values.
    std::uint32_t fallback_state_valid = 0, fallback_attached = 0, fallback_ejected = 0;
    std::uint32_t fallback_contact_phase = 0, fallback_bike_state = 0;
    float fallback_speed = 0.0f, fallback_transition = 0.0f;
};

struct ActivitySnapshot {
    // Cumulative counters per local camera, sampled by the existing diagnostic timer.
    std::array<std::uint64_t, 4> view_published{}, view_detailed{}, view_fallback{};
    std::array<std::uint64_t, static_cast<std::size_t>(ActivityCounter::Count)> counts{};
    std::array<std::uint64_t, static_cast<std::size_t>(FindFailure::Count)> find_failures{};
    // Last observed fields, read independently for diagnostic context only.
    // They never authorize an actor or replace supported_scene's current reads.
    std::uint32_t mode = 0, pending = 0, view_count = 0;
    std::uint32_t race_players = 0, setup_players = 0, compiled_renderer = 0;
    float camera_near = 0.0f, camera_far = 0.0f;
    std::array<ActorDetailSnapshot, maximum_actors> actor_details{};
};

ActivitySnapshot read_activity() noexcept;
}
