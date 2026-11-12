#pragma once

#include <array>
#include <cstdint>

#include "rr64_engine_layout.hpp"

namespace rr64::engine {

// Host-readable form of one N64 16.16 fixed-point matrix. Values remain in
// the original row/column order used by the guest; presentation code can
// choose its own convention only after this boundary has been validated.
struct Matrix4x4Snapshot {
    bool valid = false;
    std::array<float, 16> values{};

    float at(std::uint32_t row, std::uint32_t column) const noexcept {
        return values[row * 4u + column];
    }
};

struct ModelRecordTopologySnapshot {
    std::uint32_t address = 0;
    std::uint16_t type = 0;
    std::int16_t next_delta = 0;
    std::uint16_t first_transform = 0;
    std::uint16_t transform_count = 0;
};

struct ModelGraphTopologySnapshot {
    bool valid = false;
    std::uint16_t record_count = 0;
    std::uint16_t transform_count = 0;
    std::uint64_t topology_hash = 0;
    std::uint64_t allocation_hash = 0;
    std::array<ModelRecordTopologySnapshot, actor_scene::maximum_model_records> records{};
};

struct RenderTransformSetSnapshot {
    bool valid = false;
    std::uint32_t buffer = 0;
    std::uint16_t transform_count = 0;
    std::array<Matrix4x4Snapshot, actor_scene::maximum_render_transforms> transforms{};
};

struct SegmentTableSnapshot {
    bool valid = false;
    std::uint32_t table = 0;
    std::uint16_t segment_count = 0;
    std::uint64_t aggregate_hash = 0;
    std::array<std::uint32_t, actor_scene::maximum_segment_records_per_tier> records{};
    std::array<std::uint64_t, actor_scene::maximum_segment_records_per_tier> payload_hashes{};
};

float fixed_16_16_to_float(std::int16_t integer, std::uint16_t fraction) noexcept;

bool decode_n64_matrix(unsigned char *rdram, std::uint32_t matrix_address,
                       Matrix4x4Snapshot &snapshot) noexcept;

bool capture_model_graph_topology(unsigned char *rdram, std::uint32_t first_record,
                                  ModelGraphTopologySnapshot &snapshot) noexcept;

bool capture_render_transform_set(unsigned char *rdram, std::uint32_t transform_buffer,
                                  std::uint16_t transform_count,
                                  RenderTransformSetSnapshot &snapshot) noexcept;

bool capture_segment_table(unsigned char *rdram, std::uint32_t node, std::uint32_t lod,
                           std::uint16_t segment_count, SegmentTableSnapshot &snapshot) noexcept;

// Strict compatibility deliberately ignores absolute guest addresses but
// requires the same record traversal and transform allocation. Equal matrix
// counts alone are not enough to establish a safe cross-tier mapping.
bool compatible_transform_topology(const ModelGraphTopologySnapshot &left,
                                   const ModelGraphTopologySnapshot &right) noexcept;

// Weaker than exact topology compatibility: permits different byte strides
// between records but requires the same ordered transform-producing records.
bool compatible_transform_allocation(const ModelGraphTopologySnapshot &left,
                                     const ModelGraphTopologySnapshot &right) noexcept;

} // namespace rr64::engine
