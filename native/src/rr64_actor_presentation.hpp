#pragma once

#include <cstdint>

namespace rr64::engine {

enum class ActorPresentationDecisionReason : std::uint32_t {
    Disabled = 0,
    StockTierZero = 1,
    InvalidInput = 2,
    IncoherentStockModel = 3,
    MissingTierZeroResources = 4,
    InvalidTransformBuffer = 5,
    IncompatibleTransformAllocation = 6,
    CompatibleTransformRemap = 7,
    UnsupportedNodeType = 8,
    TierZeroGraphSubstitution = 9,
    InactiveActorNode = 10,
};

struct ActorPresentationDecision {
    std::uint16_t stock_lod = 0;
    std::uint16_t presentation_lod = 0;
    std::uint16_t stock_transform_count = 0;
    std::uint16_t presentation_transform_count = 0;
    std::uint64_t stock_allocation_hash = 0;
    std::uint64_t presentation_allocation_hash = 0;
    std::uint32_t current_model = 0;
    std::uint32_t stock_model = 0;
    std::uint32_t presentation_model = 0;
    std::uint32_t presentation_display_list = 0;
    std::uint16_t presentation_segment_count = 0;
    std::uint32_t node_type = 0;
    ActorPresentationDecisionReason reason = ActorPresentationDecisionReason::Disabled;
};

// Validates the tier-zero model, segment, display-list, and transform-buffer
// resources required by the render-only presentation transaction. The caller
// may temporarily expose those resources to the game's complete actor
// preparation/render path, but must restore the stock node state afterwards.
bool choose_actor_presentation_lod(unsigned char *rdram, std::uint32_t node,
                                   std::uint16_t stock_lod, bool high_detail_enabled,
                                   ActorPresentationDecision &decision,
                                   bool validated_actor_pair = false) noexcept;

// The stock actor-preparation loop supplies a bike node and the rider node
// reached through that bike's model-state/pose-owner chain. This relationship
// covers both the local player and AI racers; active_racer_count only describes
// locally controlled racers in single-player and must not be used to exclude AI.
bool valid_actor_presentation_pair(unsigned char *rdram, std::uint32_t bike_node,
                                   std::uint32_t rider_node) noexcept;

// Retained as a pure selector for ROM-free validation and diagnostics. Runtime
// rendering now uses the coherent preparation transaction instead of changing
// only this late local model pointer.
std::uint32_t choose_actor_transform_model(unsigned char *rdram, std::uint32_t node,
                                           std::uint16_t stock_lod, std::uint32_t stock_model,
                                           bool high_detail_enabled,
                                           ActorPresentationDecision &decision) noexcept;

} // namespace rr64::engine
