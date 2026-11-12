#include "rr64_actor_pose.hpp"

#include <cstring>

namespace rr64::engine {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_word(std::uint64_t &hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= kFnvPrime;
    }
}

std::uint16_t transforms_for_record(std::uint16_t type) noexcept {
    if (type == actor_scene::model_record_single_transform) {
        return 1u;
    }
    if (type == actor_scene::model_record_triple_transform) {
        return 3u;
    }
    return 0u;
}

} // namespace

float fixed_16_16_to_float(std::int16_t integer, std::uint16_t fraction) noexcept {
    const std::uint32_t bits =
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(integer)) << 16u) |
        static_cast<std::uint32_t>(fraction);
    std::int32_t signed_fixed = 0;
    static_assert(sizeof(signed_fixed) == sizeof(bits));
    std::memcpy(&signed_fixed, &bits, sizeof(signed_fixed));
    return static_cast<float>(signed_fixed) / 65536.0f;
}

bool decode_n64_matrix(unsigned char *rdram, std::uint32_t matrix_address,
                       Matrix4x4Snapshot &snapshot) noexcept {
    snapshot = {};
    if (!valid_guest_range(matrix_address, actor_scene::render_transform_size)) {
        return false;
    }

    constexpr std::uint32_t component_count = 16u;
    constexpr std::uint32_t fractional_half = component_count * sizeof(std::uint16_t);
    for (std::uint32_t component = 0; component < component_count; ++component) {
        std::uint16_t integer_bits = 0;
        std::uint16_t fraction = 0;
        if (!read_u16(rdram, matrix_address + component * sizeof(std::uint16_t), integer_bits) ||
            !read_u16(rdram, matrix_address + fractional_half + component * sizeof(std::uint16_t),
                      fraction)) {
            return false;
        }
        snapshot.values[component] =
            fixed_16_16_to_float(static_cast<std::int16_t>(integer_bits), fraction);
    }

    snapshot.valid = true;
    return true;
}

bool capture_model_graph_topology(unsigned char *rdram, std::uint32_t first_record,
                                  ModelGraphTopologySnapshot &snapshot) noexcept {
    snapshot = {};
    std::uint32_t record = first_record;
    std::uint32_t transform_count = 0;
    std::uint64_t hash = kFnvOffset;
    std::uint64_t allocation_hash = kFnvOffset;

    while (record != 0u && snapshot.record_count < snapshot.records.size()) {
        if (!valid_guest_range(record, actor_scene::model_record_minimum_size)) {
            return false;
        }
        for (std::uint32_t i = 0; i < snapshot.record_count; ++i) {
            if (snapshot.records[i].address == record) {
                return false;
            }
        }

        std::uint16_t type = 0;
        std::uint16_t next_delta_bits = 0;
        if (!read_u16(rdram, record + actor_scene::model_record_type, type) ||
            !read_u16(rdram, record + actor_scene::model_record_next_delta, next_delta_bits)) {
            return false;
        }

        const std::uint16_t record_transform_count = transforms_for_record(type);
        if (transform_count + record_transform_count > actor_scene::maximum_render_transforms) {
            return false;
        }

        ModelRecordTopologySnapshot &output = snapshot.records[snapshot.record_count++];
        output.address = record;
        output.type = type;
        output.next_delta = static_cast<std::int16_t>(next_delta_bits);
        output.first_transform = static_cast<std::uint16_t>(transform_count);
        output.transform_count = record_transform_count;
        transform_count += record_transform_count;

        hash_word(hash, type);
        hash_word(hash, next_delta_bits);
        hash_word(allocation_hash, type);
        hash_word(allocation_hash, output.first_transform);
        hash_word(allocation_hash, output.transform_count);
        if (output.next_delta == 0) {
            record = 0u;
            continue;
        }

        const std::int64_t next =
            static_cast<std::int64_t>(record) +
            static_cast<std::int64_t>(output.next_delta) * actor_scene::model_record_stride;
        if (next < static_cast<std::int64_t>(kRdramBegin) ||
            next > static_cast<std::int64_t>(kRdramEnd - actor_scene::model_record_minimum_size)) {
            return false;
        }
        record = static_cast<std::uint32_t>(next);
    }

    if (record != 0u || snapshot.record_count == 0u || transform_count == 0u) {
        return false;
    }

    snapshot.transform_count = static_cast<std::uint16_t>(transform_count);
    snapshot.topology_hash = hash;
    snapshot.allocation_hash = allocation_hash;
    snapshot.valid = true;
    return true;
}

bool capture_render_transform_set(unsigned char *rdram, std::uint32_t transform_buffer,
                                  std::uint16_t transform_count,
                                  RenderTransformSetSnapshot &snapshot) noexcept {
    snapshot = {};
    if (transform_count == 0u || transform_count > snapshot.transforms.size() ||
        !valid_guest_range(transform_buffer, static_cast<std::uint32_t>(transform_count) *
                                                 actor_scene::render_transform_size)) {
        return false;
    }

    snapshot.buffer = transform_buffer;
    snapshot.transform_count = transform_count;
    for (std::uint32_t index = 0; index < transform_count; ++index) {
        if (!decode_n64_matrix(rdram, transform_buffer + index * actor_scene::render_transform_size,
                               snapshot.transforms[index])) {
            snapshot = {};
            return false;
        }
    }

    snapshot.valid = true;
    return true;
}

bool capture_segment_table(unsigned char *rdram, std::uint32_t node, std::uint32_t lod,
                           std::uint16_t segment_count, SegmentTableSnapshot &snapshot) noexcept {
    snapshot = {};
    if (lod >= actor_scene::lod_record_count || segment_count == 0u ||
        segment_count > snapshot.records.size() ||
        !valid_guest_range(node, actor_scene::node_minimum_size)) {
        return false;
    }

    snapshot.table =
        node + actor_scene::primary_segment_records + lod * actor_scene::segment_table_stride;
    snapshot.segment_count = segment_count;
    std::uint64_t aggregate_hash = kFnvOffset;
    hash_word(aggregate_hash, segment_count);
    for (std::uint32_t index = 0; index < segment_count; ++index) {
        if (!read_u32(rdram, snapshot.table + index * sizeof(std::uint32_t),
                      snapshot.records[index]) ||
            !valid_guest_range(snapshot.records[index] + actor_scene::segment_record_payload,
                               actor_scene::render_transform_size)) {
            snapshot = {};
            return false;
        }

        std::uint64_t payload_hash = kFnvOffset;
        for (std::uint32_t offset = 0; offset < actor_scene::render_transform_size;
             offset += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            if (!read_u32(rdram,
                          snapshot.records[index] + actor_scene::segment_record_payload + offset,
                          word)) {
                snapshot = {};
                return false;
            }
            hash_word(payload_hash, word);
        }
        snapshot.payload_hashes[index] = payload_hash;
        hash_word(aggregate_hash, static_cast<std::uint32_t>(payload_hash));
        hash_word(aggregate_hash, static_cast<std::uint32_t>(payload_hash >> 32u));
    }

    snapshot.aggregate_hash = aggregate_hash;
    snapshot.valid = true;
    return true;
}

bool compatible_transform_topology(const ModelGraphTopologySnapshot &left,
                                   const ModelGraphTopologySnapshot &right) noexcept {
    if (!left.valid || !right.valid || left.record_count != right.record_count ||
        left.transform_count != right.transform_count) {
        return false;
    }

    for (std::uint32_t i = 0; i < left.record_count; ++i) {
        const ModelRecordTopologySnapshot &a = left.records[i];
        const ModelRecordTopologySnapshot &b = right.records[i];
        if (a.type != b.type || a.next_delta != b.next_delta ||
            a.first_transform != b.first_transform || a.transform_count != b.transform_count) {
            return false;
        }
    }
    return true;
}

bool compatible_transform_allocation(const ModelGraphTopologySnapshot &left,
                                     const ModelGraphTopologySnapshot &right) noexcept {
    if (!left.valid || !right.valid || left.record_count != right.record_count ||
        left.transform_count != right.transform_count) {
        return false;
    }

    for (std::uint32_t i = 0; i < left.record_count; ++i) {
        const ModelRecordTopologySnapshot &a = left.records[i];
        const ModelRecordTopologySnapshot &b = right.records[i];
        if (a.type != b.type || a.first_transform != b.first_transform ||
            a.transform_count != b.transform_count) {
            return false;
        }
    }
    return true;
}

} // namespace rr64::engine
