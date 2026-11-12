#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rr64_actor_pose.hpp"

// This module has no guest-function or host-renderer dependencies. Allocation
// records come from the original allocator; pose snapshots come from a separate
// RDRAM mapping after the original visual passes have completed there.
namespace rr64::lod {

constexpr std::size_t maximum_pairs = engine::kMaximumRacers;
constexpr std::size_t maximum_actors = maximum_pairs * 2u;
constexpr std::size_t maximum_poses = engine::actor_scene::maximum_model_records;
constexpr std::size_t pose_words = 8u;

// Recording retains local child animation, not a drawable camera-space root.
// Live consumption always repeats the normal camera-range certificate.
enum class PreparationPurpose { LiveDraw, HighlightRecording };

struct Pose {
    std::uint32_t record = 0;
    std::uint32_t source_record = 0;
    std::uint32_t address = 0;
    std::uint16_t type = 0;
    std::array<std::uint32_t, pose_words> words{};
};

// A detailed root keeps its own graph/vertices and uses the common long-range
// camera depth convention. The certificate records the exact source-1
// and stock-root inputs which authorize a 100 -> 10 unit conversion.
struct RootRenderPlan {
    std::uint32_t record = 0;
    std::uint32_t source_record = 0;
    std::uint32_t stock_model = 0;
    std::uint32_t stock_source_record = 0;
    std::uint32_t distance_address = 0;
    std::uint32_t distance_squared_bits = 0;
    std::uint32_t private_distance_squared_bits = 0;
    std::uint32_t source_scale_bits = 0;
    std::uint32_t render_scale_bits = 0;
    std::uint16_t original_source = 0;
    std::uint16_t stock_source_index = 0;
    std::uint16_t render_source = 0;
    bool normalized = false;

    bool operator==(const RootRenderPlan &) const = default;
};

bool capture_root_render_plan(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
                              RootRenderPlan &plan,
                              PreparationPurpose purpose = PreparationPurpose::LiveDraw) noexcept;
bool root_render_bank_ready(unsigned char *rdram, const RootRenderPlan &plan,
                            std::uint32_t viewport, std::uint32_t slot) noexcept;
bool rider_in_stock_view(unsigned char *rdram, std::uint32_t entity) noexcept;

struct ActorSnapshot {
    std::uint32_t node = 0;
    std::uint32_t entity = 0;
    std::uint32_t model_state = 0;
    std::uint32_t type = 0;
    std::uint16_t stock_lod = 0;
    std::uint16_t previous_lod = 0;
    std::uint32_t stock_model = 0;
    std::uint32_t model = 0;
    std::uint32_t display_list = 0;
    std::uint32_t display_list_bytes = 0;
    std::uint16_t segment_count = 0;
    std::array<std::uint32_t, 8> segment_records{};
    std::uint32_t transform_buffer = 0;
    std::uint32_t allocation_bytes = 0;
    // Pose preparation precedes the renderer's buffer-slot selection on the
    // cached path. Certify both destinations without binding the pose to one.
    std::array<std::uint32_t, 2> certified_transform_buffers{};
    std::array<std::uint32_t, 2> certified_allocation_bytes{};
    std::uint64_t resource_signature = 0;
    RootRenderPlan root_plan{};
    std::uint16_t pose_count = 0;
    std::array<Pose, maximum_poses> poses{};
};

struct PairSnapshot {
    std::array<ActorSnapshot, 2> actors{};
    std::uint64_t generation = 0;
    std::uint32_t viewport = 0;
    // Preparation provenance only; find validates the active slot against the
    // two exact certificates saved for each actor.
    std::uint32_t buffer_slot = 0;
    bool valid = false;
    bool drawable = true;
    PreparationPurpose purpose = PreparationPurpose::LiveDraw;
};

// The runtime checks these before executing any original helper on its private
// mapping. It repeats the checks when publishing and consuming a snapshot.
bool mounted_pair(unsigned char *rdram, std::uint32_t bike_node, std::uint32_t rider_node) noexcept;
// Visual ownership survives a crash/ejection. Each actor retains its own
// current root and animation; attachment is not needed to validate the links.
bool visual_pair(unsigned char *rdram, std::uint32_t bike_node, std::uint32_t rider_node) noexcept;
bool supported_scene(unsigned char *rdram) noexcept;

enum class FindFailure : std::size_t {
    None = 0,
    InvalidContext,
    MissingPair,
    Generation,
    View,
    Slot,
    Ownership,
    Capture,
    StockState,
    Resource,
    Allocation,
    Isolation,
    RootPlan,
    SourceBank,
    Count
};

class SnapshotStore {
  public:
    // Call on a new guest mapping and before each guest simulation update or
    // actor-preparation pass. Allocations survive frame invalidation, snapshots
    // do not. Starting allocation viewport zero replaces a node's old record.
    void reset(unsigned char *rdram) noexcept;
    void invalidate() noexcept;
    // A previous pose may seed an original partial animation only in the next
    // actor-preparation epoch. It is never an independently drawable snapshot.
    void begin_pose_epoch(unsigned char *rdram) noexcept;
    bool observe_allocation(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
                            std::uint32_t bytes) noexcept;
    bool forget_allocation(unsigned char *rdram, std::uint32_t node) noexcept;

    bool can_prepare(unsigned char *rdram, std::uint32_t bike_node, std::uint32_t rider_node,
                     std::uint32_t viewport, std::uint32_t buffer_slot,
                     PreparationPurpose purpose = PreparationPurpose::LiveDraw) const noexcept;

    // live and prepared must be distinct mappings. This operation is read-only
    // for BOTH mappings; it deep-copies every pose into renderer-owned storage.
    bool publish(unsigned char *live, unsigned char *prepared, std::uint32_t bike_node,
                 std::uint32_t rider_node, std::uint32_t viewport,
                 std::uint32_t buffer_slot,
                 PreparationPurpose purpose = PreparationPurpose::LiveDraw) noexcept;

    bool seed_previous_rider_children(unsigned char *live, unsigned char *prepared,
                                      std::uint32_t rider_node, std::uint32_t viewport,
                                      std::uint32_t buffer_slot, bool frozen_pair = false,
                                      bool *missing_history = nullptr) const noexcept;

    // Returns an immutable pair only when both halves still match their exact
    // generation, graph, resource, ownership and allocation certificates.
    const PairSnapshot *find(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
                             std::uint32_t buffer_slot,
                             FindFailure *failure = nullptr) const noexcept;

    std::uint64_t generation() const noexcept {
        return generation_;
    }

    // Copy only the current certified rider root, in its authored source units.
    // Weapons draw after the temporary detailed-model binding is restored.
    bool weapon_root(unsigned char *rdram, std::uint32_t node, std::uint32_t view,
                     std::uint32_t slot, std::array<std::uint32_t, 7> &words,
                     std::uint32_t &source) const noexcept;

  private:
    struct Allocation {
        std::uint32_t node = 0;
        std::uint32_t entity = 0;
        std::uint32_t type = 0;
        std::array<std::uint32_t, 3> models{};
        std::array<std::uint32_t, 8> buffers{};
        std::array<std::uint32_t, 8> bytes{};
    };

    const Allocation *allocation(unsigned char *rdram, std::uint32_t node) const noexcept;
    bool capture_actor(unsigned char *live, unsigned char *poses, std::uint32_t node,
                       std::uint32_t viewport, std::uint32_t slot, bool require_prepared,
                       ActorSnapshot &output,
                       PreparationPurpose purpose = PreparationPurpose::LiveDraw) const noexcept;
    bool isolated_pose_ranges(const PairSnapshot &pair) const noexcept;

    unsigned char *rdram_ = nullptr;
    std::uint64_t generation_ = 1;
    std::array<Allocation, maximum_actors> allocations_{};
    std::array<PairSnapshot, maximum_pairs> pairs_{};
    struct PoseHistory {
        PairSnapshot pair{};
        std::uint64_t epoch = 0;
        std::uint32_t mode = 0, pending = 0;
        std::uint32_t rider_style = 0;
        std::array<std::uint16_t, maximum_poses> animation_links{};
    };
    // Each camera advances separately; another viewport must not age or replace its history.
    std::array<std::uint64_t, 4> pose_epochs_{};
    std::array<PoseHistory, maximum_pairs * 4> pose_history_{};
};

// Installation is deliberately separate from publication: only the original
// renderer's bounded matrix loop may borrow the immutable pose values. Every
// overwritten word is restored, even on repeated closure or failed re-entry.
class PoseBinding {
  public:
    bool begin(unsigned char *rdram, const ActorSnapshot &snapshot) noexcept;
    void end() noexcept;
    bool active() const noexcept {
        return rdram_ != nullptr;
    }
    ~PoseBinding() {
        end();
    }

  private:
    unsigned char *rdram_ = nullptr;
    std::uint16_t count_ = 0;
    std::array<Pose, maximum_poses> saved_{};
};

} // namespace rr64::lod

namespace rr64::lod {
bool racer_in_extended_view(unsigned char *, std::uint32_t, unsigned) noexcept;
}
