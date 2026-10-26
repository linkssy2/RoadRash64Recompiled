#include "rr64_traffic_roster.hpp"
#pragma once

#include "rr64_course_hazard_state.hpp"
#include "rr64_mk64_item_state.hpp"
#include "rr64_world_sync.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace rr64::highlights {
constexpr unsigned maximum_racers = 14, maximum_bones = 128;
constexpr unsigned rolling_frames = 180, maximum_clip_frames = 180, maximum_clips = 3;
constexpr std::uint64_t before_us = 2000000, after_us = 3000000, distinct_event_us = 1000000;
constexpr std::size_t memory_limit = 128u * 1024u * 1024u;
using Vec3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // Native XYZW, in world space for roots.

struct Bone {
    // Original ordered graph record index, not a guest address. Root record 0
    // is omitted: playback rebuilds it using the separate world render anchor.
    std::uint16_t index = 0, type = 0;
    std::array<float, 7> values{}; // Local translation XYZ, quaternion XYZW.
    bool operator==(const Bone &) const = default;
};
struct Pose {
    bool valid = false;
    std::uint16_t count = 0, record_count = 0, lod = 0, source_bank = 0;
    // Ordered native types, hierarchy flags and source-template offsets;
    // excludes process-local renderer record/pose allocation spacing.
    std::uint64_t topology = 0;
    std::array<Bone, maximum_bones> bones{};
    bool operator==(const Pose &) const = default;
};
constexpr unsigned maximum_weapon_records = 32;
struct WeaponPose {
    // Actual animated held model, distinct from the selected inventory item.
    // Record order is the local resource tree's preorder, never guest pointers.
    bool valid = false;
    std::uint16_t model = 0, count = 0;
    std::uint64_t topology = 0;
    std::array<std::array<float, 7>, maximum_weapon_records> transforms{};
    bool operator==(const WeaponPose &) const = default;
};
enum CrashFlag : std::uint32_t {
    BikeAttached = 1, RiderAttached = 2, Ejected = 4, DriveLockout = 8
};
struct Racer {
    bool active = false;
    std::uint32_t model = 0, character = 0, weapon = 0, crash_flags = 0;
    // Increment for recovery/teleport/reassignment. A changed generation is a
    // hard presentation cut even when its two endpoints are spatially close.
    std::uint32_t generation = 0;
    Vec3 bike_origin{}, rider_origin{}, bike_anchor{}, rider_anchor{};
    Quaternion bike_rotation{0, 0, 0, 1}, rider_rotation{0, 0, 0, 1};
    Pose bike_pose{}, rider_pose{};
    WeaponPose held_weapon{};
    bool operator==(const Racer &) const = default;
};
struct Frame {
    std::uint64_t time_us = 0, tick = 0;
    // Array position is the canonical racer slot. Never store native pointers.
    std::array<Racer, maximum_racers> racers{};
    std::array<world_sync::Traffic, rr64::traffic::capacity> traffic{};
    netplay::CourseHazardState hazards{};
    mk64_items::Snapshot items{};
    bool operator==(const Frame &) const = default;
};
struct Clip {
    unsigned slot = 0;
    float score = 0;
    std::uint64_t event_tick = 0, event_time_us = 0;
    bool truncated_before = false, truncated_after = false, capacity_limited = false;
    std::span<const Frame> frames{};
};
struct Statistics {
    std::uint64_t accepted_frames = 0, rejected_frames = 0, rejected_poses = 0;
    std::uint64_t crash_marks = 0, rejected_marks = 0, deduplicated_events = 0;
    std::uint64_t discarded_events = 0, truncated_clips = 0;
};

bool valid_pose(const Pose &) noexcept;
bool valid_weapon_pose(const WeaponPose &) noexcept;
bool valid_frame(const Frame &) noexcept;
bool compatible_pose(const Pose &, const Pose &) noexcept;
// Pure presentation calculation. Discontinuities retain the earlier sample
// until the later endpoint; no physics, results, rewards or guest writes occur.
bool interpolate(const Frame &a, const Frame &b, std::uint64_t time_us, Frame &out) noexcept;
bool sample(const Clip &, std::uint64_t time_us, Frame &out) noexcept;

class Recorder {
  public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    // One producer/game thread owns all methods. Allocate bounded storage once
    // in the constructor, outside the simulation hot path. No I/O or callbacks.
    void begin(std::uint64_t session = 1) noexcept;
    void reset() noexcept;
    bool push(const Frame &) noexcept;
    // Merge a later view's fresh pose into the latest accepted authored tick,
    // including any in-progress clip copies. Cannot modify sealed clips.
    // A coarser later view cannot downgrade a complete detailed pose.
    bool update_pose(std::uint64_t current_tick, unsigned slot, bool rider, const Pose &) noexcept;
    // Same authored-tick boundary as update_pose; an empty value records that
    // the attack ended. Never retain the preceding weapon after disappearance.
    bool update_weapon(std::uint64_t current_tick, unsigned slot, const WeaponPose &) noexcept;
    // Native crash hooks enqueue here; next accepted push assigns event time.
    bool mark_crash(unsigned slot, float score) noexcept;
    // Freeze at the terminal transition. Best distinct events first; references
    // remain stable until begin/reset/destruction. Early race end is explicit.
    std::span<const Clip> seal() noexcept;
    std::span<const Clip> clips() const noexcept;
    const Statistics &statistics() const noexcept;
    bool sealed() const noexcept;
    std::size_t memory_bytes() const noexcept;
  private:
    struct Storage;
    std::unique_ptr<Storage> data_;
};
} // namespace rr64::highlights
