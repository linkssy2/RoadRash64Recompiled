#include "rr64_racer_view.hpp"
#include "rr64_view_width.hpp"
#include "rr64_actor_render_snapshot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rr64::lod {
namespace {
using namespace engine;
constexpr std::uint32_t node_bytes = 0x148u;
constexpr std::uint32_t compiled_renderer = 0x800A65C4u;
constexpr std::uint32_t viewport_count = 0x8009DB88u;
// The live race count drives camera layout in func_8005C320. EF5C is a
// persistent multiplayer-menu selection and can remain 2 in a one-player race.
constexpr std::uint32_t race_player_count = 0x800A6578u;
constexpr auto root_distance_limit = std::bit_cast<float>(0x4d7fffe4u);
constexpr std::uint32_t source_scales = 0x8009DBACu;
constexpr std::uint32_t root_distance_view = 0x8009DBA8u;

bool retained_actor_scene(std::uint32_t mode, std::uint32_t pending) noexcept {
    if (is_live_race_transition(mode, pending)) {
        return true;
    }
    // Each results mode retains its family's race actor lists, draw callback
    // 6A638 and preparation callback 6AFFC. Limit the additional admission to
    // that family; a menu/scene exit must revoke prepared poses immediately.
    for (const auto result : {0x0bu, 0x14u, 0x19u, 0x1eu}) {
        if ((mode == result || pending == result) && mode >= result - 2u && mode <= result &&
            pending >= result - 2u && pending <= result) {
            return true;
        }
    }
    return false;
}

bool root_plan(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
               const ModelGraphTopologySnapshot &topology, RootRenderPlan &plan,
               PreparationPurpose purpose) noexcept {
    plan = {};
    if (viewport >= 4u || topology.record_count == 0u ||
        topology.records[0].type != actor_scene::model_record_triple_transform) {
        return false;
    }
    plan.record = topology.records[0].address;
    if (!read_u32(rdram, plan.record + 0x14u, plan.source_record) ||
        !valid_guest_range(plan.source_record, 0x14u) ||
        !read_u16(rdram, plan.source_record + 0x12u, plan.original_source) ||
        plan.original_source > 2u ||
        !read_u32(rdram, source_scales + plan.original_source * 4u, plan.source_scale_bits)) {
        return false;
    }
    std::uint16_t stock_lod = 0, stock_record_type = 0;
    std::uint32_t selected_model = 0;
    if (!read_u16(rdram, node + actor_scene::selected_lod, stock_lod) || stock_lod >= 3u ||
        !read_u32(rdram, node + actor_scene::current_model, plan.stock_model) ||
        !read_u32(rdram, node + actor_scene::lod_models + stock_lod * 4u, selected_model) ||
        selected_model != plan.stock_model || !valid_guest_range(plan.stock_model, 0x18u) ||
        !read_u16(rdram, plan.stock_model + 4u, stock_record_type) ||
        stock_record_type != actor_scene::model_record_triple_transform ||
        !read_u32(rdram, plan.stock_model + 0x14u, plan.stock_source_record) ||
        !valid_guest_range(plan.stock_source_record, 0x14u) ||
        !read_u16(rdram, plan.stock_source_record + 0x12u, plan.stock_source_index) ||
        plan.stock_source_index > 2u) {
        return false;
    }
    plan.render_source = plan.original_source;
    plan.render_scale_bits = plan.source_scale_bits;
    // Only source 1 -> 2 has a verified common camera basis and unit ratio.
    // Other detailed sources may keep their own bank, but cannot silently
    // replace the stock camera/depth convention during a model promotion.
    if (plan.original_source != 1u) {
        return plan.stock_source_index == plan.original_source;
    }
    if (plan.stock_source_index == 0u) {
        return false;
    }
    plan.distance_address = node + 8u + viewport * 4u;
    if (plan.source_scale_bits != 0x42c80000u ||
        !read_u32(rdram, plan.distance_address, plan.distance_squared_bits)) {
        return false;
    }
    const float distance_squared = std::bit_cast<float>(plan.distance_squared_bits);
    if (!std::isfinite(distance_squared) || distance_squared < 0.0f) {
        return false;
    }
    plan.private_distance_squared_bits = plan.distance_squared_bits;
    // Source 1 clamps the camera's world far plane to about 327.67, whereas
    // source 2 retains the race camera's full 900-unit range. This changes Z
    // even for an in-range stock-source-1 rider: terrain can hide its detailed
    // model until the old root limit switches it to source 2 farther away.
    // Use the verified source-2 basis at every distance for detailed source-1
    // actors, keeping one depth convention through that transition. Root XYZ
    // is converted at draw time; detailed vertices and children stay intact.
    std::uint32_t distance_view = 0;
    if (!read_u32(rdram, root_distance_view, distance_view) || distance_view != viewport ||
        !read_u32(rdram, source_scales + 8u, plan.render_scale_bits) ||
        plan.render_scale_bits != 0x41200000u) {
        return false;
    }
    const float private_distance_squared = distance_squared * 0.01f;
    const bool in_range = 100.0f * distance_squared < root_distance_limit &&
                          10000.0f * private_distance_squared < root_distance_limit;
    if (!in_range && purpose != PreparationPurpose::HighlightRecording) {
        return false;
    }
    // A second source-setting root would change the renderer's final source
    // and perspective normalization. Only the verified single-root actor
    // graphs can share one converted parent matrix with unchanged children.
    for (std::uint32_t i = 1; i < topology.record_count; ++i) {
        if (topology.records[i].type == actor_scene::model_record_triple_transform) {
            return false;
        }
    }
    // The cloned root writers still need to run for off-camera recordings.
    // Their roots are discarded; playback rebuilds them from recorded world
    // anchors and its own camera. Never use this exemption for live drawing.
    plan.private_distance_squared_bits =
        std::bit_cast<std::uint32_t>(in_range ? private_distance_squared : 0.0f);
    plan.render_source = 2u;
    plan.normalized = true;
    return true;
}

bool normalized_pose_in_range(const Pose &pose, bool require_draw_range = true) noexcept {
    // Root writers retain their real source-1 pose. Before selecting bank 2,
    // also certify the actual captured translation (not just a node's distance
    // cache) and a bounded finite quaternion for the original matrix builder.
    double translation_squared = 0.0;
    for (std::size_t i = 0; i < 3u; ++i) {
        const double value = std::bit_cast<float>(pose.words[i]);
        if (!std::isfinite(value)) {
            return false;
        }
        translation_squared += value * value;
    }
    if (require_draw_range && !(translation_squared * 0.01 < root_distance_limit)) {
        return false;
    }
    double quaternion_squared = 0.0;
    for (std::size_t i = 3; i < 7u; ++i) {
        const double value = std::bit_cast<float>(pose.words[i]);
        if (!std::isfinite(value)) {
            return false;
        }
        quaternion_squared += value * value;
    }
    return quaternion_squared > 0.0 && quaternion_squared <= 16.0;
}

bool overlaps(std::uint32_t a, std::uint32_t an, std::uint32_t b, std::uint32_t bn) noexcept {
    return std::uint64_t(a) < std::uint64_t(b) + bn && std::uint64_t(b) < std::uint64_t(a) + an;
}

void mix(std::uint64_t &hash, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 32; i += 8) {
        hash ^= (value >> i) & 0xffu;
        hash *= 1099511628211ull;
    }
}

bool finite_pose(const Pose &pose, bool &nonidentity) noexcept {
    nonidentity = false;
    bool quaternion_nonzero = false;
    for (std::size_t i = 0; i < 7; ++i) {
        const auto bits = pose.words[i] & 0x7fffffffu;
        if ((bits & 0x7f800000u) == 0x7f800000u) {
            return false;
        }
        if (i >= 3) {
            quaternion_nonzero |= bits != 0u;
        }
        nonidentity |= i == 6 ? bits != 0x3f800000u : bits != 0u;
    }
    return quaternion_nonzero;
}

bool actor_identity(unsigned char *rdram, std::uint32_t node, std::uint32_t &type,
                    std::uint32_t &entity) noexcept {
    return rdram && valid_guest_range(node, node_bytes) &&
           read_u32(rdram, node + actor_scene::type, type) &&
           read_u32(rdram, node + actor_scene::entity, entity) && (type == 1u || type == 2u) &&
           valid_guest_range(entity, type == 1u ? bike::stride : rider::stride);
}
} // namespace

bool capture_root_render_plan(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
                              RootRenderPlan &plan, PreparationPurpose purpose) noexcept {
    std::uint32_t graph = 0;
    ModelGraphTopologySnapshot topology{};
    return read_u32(rdram, node + actor_scene::lod_models, graph) &&
           capture_model_graph_topology(rdram, graph, topology) &&
           root_plan(rdram, node, viewport, topology, plan, purpose);
}

bool root_render_bank_ready(unsigned char *rdram, const RootRenderPlan &plan,
                            std::uint32_t viewport, std::uint32_t slot) noexcept {
    if (!plan.normalized) {
        return true;
    }
    if (viewport >= 4u || slot >= 2u || plan.original_source != 1u || plan.render_source != 2u) {
        return false;
    }
    std::uint16_t valid = 0, normalization = 0;
    const auto source = std::uint32_t(plan.render_source);
    const auto bank_offset = viewport * 0x180u + source * 0x80u + slot * 0x40u;
    // 167BC reads this halfword before building the chosen slot. The flag is
    // per viewport/source; both matrices and the normalization are per slot.
    if (!read_u16(rdram, 0x800B6B68u + viewport * 0x9cu + source * 0x34u + 0x30u, valid) ||
        valid == 0u ||
        !read_u16(rdram, 0x800B73E8u + viewport * 12u + source * 4u + slot * 2u, normalization) ||
        normalization == 0u) {
        return false;
    }
    Matrix4x4Snapshot projection{}, view{};
    if (!decode_n64_matrix(rdram, 0x800B6568u + bank_offset, projection) ||
        !decode_n64_matrix(rdram, 0x800B6DE8u + bank_offset, view)) {
        return false;
    }
    // Unwritten or damaged slots must not be made usable by the other slot's
    // valid flag. These are the nonzero perspective and affine-view invariants
    // of the actual guPerspective/guLookAt producers.
    return projection.values[0] != 0.0f && projection.values[5] != 0.0f &&
           projection.values[11] != 0.0f && view.values[15] == 1.0f && view.values[3] == 0.0f &&
           view.values[7] == 0.0f && view.values[11] == 0.0f;
}

bool supported_scene(unsigned char *rdram) noexcept {
    std::uint32_t mode = 0, pending = 0, views = 0, local = 0;
    std::uint16_t compiled = 0;
    return read_u32(rdram, globals::main_mode, mode) &&
           read_u32(rdram, globals::pending_mode, pending) && retained_actor_scene(mode, pending) &&
           read_u32(rdram, viewport_count, views) && views >= 1u && views <= 4u &&
           // Attract setup (72068) has zero human riders but one camera.
           // Admit that actor scene without changing its AI/player count.
           read_u32(rdram, race_player_count, local) &&
           (local == views || (local == 0u && views == 1u)) &&
           read_u16(rdram, compiled_renderer, compiled) && compiled != 0u;
}

bool actor_in_view(unsigned char *rdram, std::uint32_t position_address,
                   float side_scale) noexcept {
    // Read-only equivalent of 19FE8's rider+8C -> 19F7C -> 14178 cull.
    // The stock pass skips a range-hidden rider before testing this triangle.
    // Keep its point/boundary policy when extending only the root unit range.
    std::array<float, 2> position{}, origin{}, a{}, b{}, c{};
    float scale = 0.0f, edge_limit = 0.0f;
    if (!valid_guest_range(position_address, 8u) || !read_float(rdram, 0x80000EB8u, scale) ||
        !std::isfinite(scale) || scale <= 0.0f || !read_float(rdram, 0x80000D50u, edge_limit) ||
        edge_limit != 1.0f) {
        return false;
    }
    for (std::uint32_t axis = 0; axis < 2u; ++axis) {
        if (!read_float(rdram, position_address + axis * 4u, position[axis]) ||
            !read_float(rdram, 0x800A4FDCu + axis * 4u, origin[axis]) ||
            !read_float(rdram, 0x800BACA0u + axis * 4u, a[axis]) ||
            !read_float(rdram, 0x800BACB0u + axis * 4u, b[axis]) ||
            !read_float(rdram, 0x800BACC0u + axis * 4u, c[axis]) ||
            !std::isfinite(position[axis]) || !std::isfinite(origin[axis]) ||
            !std::isfinite(a[axis]) || !std::isfinite(b[axis]) || !std::isfinite(c[axis])) {
            return false;
        }
        position[axis] = (position[axis] - origin[axis]) * scale;
        if (!std::isfinite(position[axis])) {
            return false;
        }
    }
    return racer_view_contains(position, a, b, c, side_scale);
}

bool rider_in_stock_view(unsigned char *rdram, std::uint32_t entity) noexcept {
    return actor_in_view(rdram, entity + 0x8cu, 1.0f);
}
bool racer_in_extended_view(unsigned char *rdram, std::uint32_t entity, unsigned type) noexcept {
    if (type != 1u && type != 2u)
        return false;
    return actor_in_view(
        rdram, entity + (type == 1u ? 0x16cu : 0x8cu),
        float(std::max(1.5, rr64::view_width.load(std::memory_order_relaxed) + 1.0 / 6.0)));
}
bool visual_pair(unsigned char *rdram, std::uint32_t bike_node, std::uint32_t rider_node) noexcept {
    std::uint32_t bt = 0, rt = 0, be = 0, re = 0;
    std::uint32_t linked_rider = 0, linked_bike = 0, model_state = 0;
    std::uint32_t owner = 0, owner_node = 0;
    // 37554 detaches the rider without changing these ownership links.
    // 36B78 supplies the detached rider's own +5DC anchor; EB50 copies its
    // own +164 quaternion. AEE0 also has complete crash/recovery samples.
    // Reset/reassignment clears or replaces links and must still reject.
    return bike_node != rider_node && actor_identity(rdram, bike_node, bt, be) && bt == 1u &&
           actor_identity(rdram, rider_node, rt, re) && rt == 2u &&
           read_u32(rdram, be + bike::rider_pointer, linked_rider) && linked_rider == re &&
           read_u32(rdram, re + rider::bike_pointer, linked_bike) && linked_bike == be &&
           read_u32(rdram, be + bike::model_state_pointer, model_state) &&
           valid_guest_range(model_state, actor_scene::model_state_pose_owner + 4u) &&
           read_u32(rdram, model_state + actor_scene::model_state_pose_owner, owner) &&
           owner == re && valid_guest_range(owner, 0x5e8u) &&
           read_u32(rdram, owner + actor_scene::pose_owner_rider_node, owner_node) &&
           owner_node == rider_node;
}

bool mounted_pair(unsigned char *rdram, std::uint32_t bike_node,
                  std::uint32_t rider_node) noexcept {
    std::uint32_t bike_entity = 0, rider_entity = 0;
    std::uint16_t attached = 0, ejected = 0;
    return visual_pair(rdram, bike_node, rider_node) &&
           read_u32(rdram, bike_node + actor_scene::entity, bike_entity) &&
           read_u32(rdram, rider_node + actor_scene::entity, rider_entity) &&
           read_u16(rdram, bike_entity + bike::rider_attached, attached) && attached != 0u &&
           read_u16(rdram, rider_entity + rider::ejected, ejected) && ejected == 0u;
}

void SnapshotStore::reset(unsigned char *rdram) noexcept {
    rdram_ = rdram;
    allocations_.fill({});
    pose_epochs_.fill(0);
    pose_history_.fill({}); // RR64-MAC: in place; "= {}" built a 722 KB temporary on the stack
    invalidate();
}

void SnapshotStore::invalidate() noexcept {
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
    }
    for (auto &pair : pairs_) {
        pair.valid = false;
    }
}

void SnapshotStore::begin_pose_epoch(unsigned char *rdram) noexcept {
    if (rdram != rdram_) {
        reset(rdram);
    }
    std::uint32_t view = 0;
    if (!supported_scene(rdram) || !read_u32(rdram, globals::active_viewport, view) || view >= 4u) {
        pose_history_.fill({}); // RR64-MAC: in place; "= {}" built a 722 KB temporary on the stack
        pose_epochs_.fill(0);
        return;
    }
    if (++pose_epochs_[view] == 0u) {
        pose_history_.fill({}); // RR64-MAC: in place; "= {}" built a 722 KB temporary on the stack
        pose_epochs_.fill(0);
        pose_epochs_[view] = 1u;
    }
}

bool SnapshotStore::observe_allocation(unsigned char *rdram, std::uint32_t node,
                                       std::uint32_t viewport, std::uint32_t bytes) noexcept {
    if (rdram != rdram_) {
        reset(rdram);
    }
    std::uint32_t type = 0, entity = 0;
    if (viewport >= 4u || bytes == 0u || bytes > 128u * 64u || (bytes % 64u) != 0u ||
        !actor_identity(rdram, node, type, entity)) {
        return false;
    }
    Allocation *destination = nullptr;
    for (auto &entry : allocations_) {
        if (entry.node == node) {
            destination = &entry;
            break;
        }
        if (entry.node == 0u && !destination) {
            destination = &entry;
        }
    }
    if (!destination) {
        return false;
    }
    if (viewport == 0u || destination->node != node || destination->entity != entity ||
        destination->type != type) {
        *destination = {};
    }
    destination->node = node;
    destination->entity = entity;
    destination->type = type;
    for (std::uint32_t lod = 0; lod < 3; ++lod) {
        if (!read_u32(rdram, node + actor_scene::lod_models + lod * 4u, destination->models[lod])) {
            *destination = {};
            return false;
        }
    }
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        const auto index = viewport * 2u + slot;
        if (!read_u32(rdram, node + actor_scene::render_transform_buffers + index * 4u,
                      destination->buffers[index]) ||
            !valid_guest_range(destination->buffers[index], bytes)) {
            *destination = {};
            return false;
        }
        destination->bytes[index] = bytes;
    }
    // Reallocation revokes this actor's history. Other actors may retain their
    // immediately preceding children: seeding recertifies their current
    // resources and isolation against every current allocation before copying.
    // Clearing the entire field here stranded unrelated slow finish blends.
    for (auto &history : pose_history_) {
        if (history.pair.actors[0].node == node || history.pair.actors[1].node == node)
            history = {};
    }
    invalidate();
    return true;
}

bool SnapshotStore::forget_allocation(unsigned char *rdram, std::uint32_t node) noexcept {
    if (rdram != rdram_ || node == 0u)
        return false;
    for (auto &entry : allocations_) {
        if (entry.node != node)
            continue;
        entry = {};
        for (auto &history : pose_history_) {
            if (history.pair.actors[0].node == node || history.pair.actors[1].node == node)
                history = {};
        }
        invalidate();
        return true;
    }
    return false;
}

const SnapshotStore::Allocation *SnapshotStore::allocation(unsigned char *rdram,
                                                           std::uint32_t node) const noexcept {
    if (rdram != rdram_) {
        return nullptr;
    }
    for (const auto &entry : allocations_) {
        if (entry.node != node) {
            continue;
        }
        std::uint32_t type = 0, entity = 0;
        if (!actor_identity(rdram, node, type, entity) || type != entry.type ||
            entity != entry.entity) {
            return nullptr;
        }
        for (std::uint32_t lod = 0; lod < 3; ++lod) {
            std::uint32_t model = 0;
            if (!read_u32(rdram, node + actor_scene::lod_models + lod * 4u, model) ||
                model != entry.models[lod]) {
                return nullptr;
            }
        }
        return &entry;
    }
    return nullptr;
}

bool SnapshotStore::capture_actor(unsigned char *live, unsigned char *poses, std::uint32_t node,
                                  std::uint32_t viewport, std::uint32_t slot, bool require_prepared,
                                  ActorSnapshot &output, PreparationPurpose purpose) const noexcept {
    output = {};
    const auto *allocation_record = allocation(live, node);
    if (!allocation_record || viewport >= 4u || slot >= 2u || !poses) {
        return false;
    }
    output.node = node;
    output.type = allocation_record->type;
    output.entity = allocation_record->entity;
    if (!read_u16(live, node + actor_scene::selected_lod, output.stock_lod) ||
        output.stock_lod >= 3u ||
        !read_u16(live, node + actor_scene::previous_lod, output.previous_lod) ||
        !read_u32(live, node + actor_scene::current_model, output.stock_model) ||
        output.stock_model != allocation_record->models[output.stock_lod] ||
        !read_u32(live, node + actor_scene::lod_models, output.model) ||
        !read_u32(live, node + actor_scene::display_lists, output.display_list) ||
        !valid_guest_range(output.display_list, 8u)) {
        return false;
    }
    if (require_prepared) {
        std::uint32_t prepared_model = 0;
        std::uint16_t prepared_lod = 3;
        if (!read_u32(poses, node + actor_scene::current_model, prepared_model) ||
            prepared_model != output.model ||
            !read_u16(poses, node + actor_scene::selected_lod, prepared_lod) ||
            prepared_lod != 0u) {
            return false;
        }
    }
    ModelGraphTopologySnapshot topology{}, prepared_topology{};
    if (!capture_model_graph_topology(live, output.model, topology) ||
        !capture_model_graph_topology(poses, output.model, prepared_topology) ||
        !compatible_transform_topology(topology, prepared_topology) ||
        !root_plan(live, node, viewport, topology, output.root_plan, purpose)) {
        return false;
    }
    for (std::uint32_t destination_slot = 0; destination_slot < 2u; ++destination_slot) {
        const auto buffer_index = viewport * 2u + destination_slot;
        auto &buffer = output.certified_transform_buffers[destination_slot];
        auto &bytes = output.certified_allocation_bytes[destination_slot];
        bytes = allocation_record->bytes[buffer_index];
        if (!read_u32(live, node + actor_scene::render_transform_buffers + buffer_index * 4u,
                      buffer) ||
            buffer != allocation_record->buffers[buffer_index] ||
            topology.transform_count * 64u > bytes || !valid_guest_range(buffer, bytes)) {
            return false;
        }
    }
    output.transform_buffer = output.certified_transform_buffers[slot];
    output.allocation_bytes = output.certified_allocation_bytes[slot];
    std::uint64_t signature = 14695981039346656037ull;
    // Detachment/ejection is now an admitted visual state, but a snapshot
    // prepared before that transition must never survive it. Certify the
    // current flags without using them to force mounted geometry.
    std::uint16_t visual_state = 0, prepared_visual_state = 0;
    const auto state_address =
        output.entity + (output.type == 1u ? bike::rider_attached : rider::ejected);
    if (!read_u16(live, state_address, visual_state) ||
        !read_u16(poses, state_address, prepared_visual_state) ||
        visual_state != prepared_visual_state) {
        return false;
    }
    mix(signature, visual_state);
    if (output.type == 1u) {
        std::uint32_t shadow_state = 0, racer_id = 0, shadow_id = 0;
        if (!read_u32(live, output.entity + bike::model_state_pointer, output.model_state) ||
            !read_u32(poses, output.entity + bike::model_state_pointer, shadow_state) ||
            shadow_state != output.model_state ||
            !valid_guest_range(output.model_state, actor_scene::model_state_pose_owner + 4u) ||
            !read_u32(live, output.model_state, racer_id) || racer_id >= kMaximumRacers ||
            !read_u32(poses, output.model_state, shadow_id) || shadow_id != racer_id) {
            return false;
        }
        mix(signature, output.model_state);
        mix(signature, racer_id);
    }
    mix(signature, output.model);
    mix(signature, output.display_list);
    std::uint32_t tail = 0, shadow_tail = 0, shadow_display_list = 0;
    if (!read_u32(live, node + 0x13cu, tail) || tail < output.display_list || (tail & 7u) != 0u ||
        (output.display_list & 7u) != 0u || tail - output.display_list > 0x100000u ||
        !read_u32(poses, node + 0x13cu, shadow_tail) || shadow_tail != tail ||
        !read_u32(poses, node + actor_scene::display_lists, shadow_display_list) ||
        shadow_display_list != output.display_list) {
        return false;
    }
    output.display_list_bytes = tail - output.display_list + 8u;
    if (!valid_guest_range(output.display_list, output.display_list_bytes)) {
        return false;
    }
    for (std::uint32_t offset = 0; offset < output.display_list_bytes; offset += 4u) {
        std::uint32_t word = 0, shadow_word = 0;
        if (!read_u32(live, output.display_list + offset, word) ||
            !read_u32(poses, output.display_list + offset, shadow_word) || word != shadow_word) {
            return false;
        }
        mix(signature, word);
    }
    std::uint16_t segment_count = 0;
    if (!read_u16(live, node + actor_scene::segment_counts, segment_count) || segment_count == 0u ||
        segment_count > 8u) {
        return false;
    }
    mix(signature, segment_count);
    output.segment_count = segment_count;
    for (std::uint32_t i = 0; i < segment_count; ++i) {
        std::uint32_t record = 0, shadow_record = 0;
        if (!read_u32(live, node + actor_scene::primary_segment_records + i * 4u, record) ||
            !read_u32(poses, node + actor_scene::primary_segment_records + i * 4u, shadow_record) ||
            record != shadow_record || !valid_guest_range(record, 128u)) {
            return false;
        }
        mix(signature, record);
        output.segment_records[i] = record;
        for (std::uint32_t offset = 0; offset < 128u; offset += 4u) {
            std::uint32_t word = 0, shadow_word = 0;
            if (!read_u32(live, record + offset, word) ||
                !read_u32(poses, record + offset, shadow_word) || word != shadow_word) {
                return false;
            }
            mix(signature, word);
        }
        // The renderer resolves +0xDC itself from these primary records. It
        // does not consume a producer's cached +0xDC value.
    }
    bool root_ready = false, child_ready = false;
    for (std::uint32_t i = 0; i < topology.record_count; ++i) {
        const auto &record = topology.records[i];
        mix(signature, record.address);
        mix(signature, record.type);
        mix(signature, static_cast<std::uint16_t>(record.next_delta));
        if (record.transform_count == 0u) {
            continue;
        }
        auto &pose = output.poses[output.pose_count];
        pose.record = record.address;
        pose.type = record.type;
        std::uint32_t prepared_address = 0, prepared_source = 0;
        if (!read_u32(live, record.address + 0x0cu, pose.address) ||
            !read_u32(poses, record.address + 0x0cu, prepared_address) ||
            pose.address != prepared_address || !valid_guest_range(pose.address, 32u) ||
            (pose.address & 3u) != 0u ||
            !read_u32(live, record.address + 0x14u, pose.source_record) ||
            !read_u32(poses, record.address + 0x14u, prepared_source) ||
            pose.source_record != prepared_source) {
            return false;
        }
        if (record.type == actor_scene::model_record_triple_transform) {
            std::uint16_t source_index = 0, prepared_index = 0;
            if (!valid_guest_range(pose.source_record, 0x14u) ||
                !read_u16(live, pose.source_record + 0x12u, source_index) ||
                !read_u16(poses, pose.source_record + 0x12u, prepared_index) ||
                source_index != prepared_index || source_index > 2u) {
                return false;
            }
            mix(signature, source_index);
        }
        mix(signature, pose.address);
        mix(signature, pose.source_record);
        for (std::uint32_t word = 0; word < pose_words; ++word) {
            if (!read_u32(poses, pose.address + word * 4u, pose.words[word])) {
                return false;
            }
        }
        if (require_prepared) {
            bool nonidentity = false;
            if (!finite_pose(pose, nonidentity)) {
                return false;
            }
            if (output.pose_count == 0u) {
                root_ready = nonidentity;
            } else {
                child_ready |= nonidentity;
            }
        }
        ++output.pose_count;
    }
    output.resource_signature = signature;
    if (require_prepared && output.root_plan.normalized &&
        (output.pose_count == 0u || output.poses[0].record != output.root_plan.record ||
         !normalized_pose_in_range(output.poses[0], purpose == PreparationPurpose::LiveDraw))) {
        return false;
    }
    return output.pose_count != 0u &&
           (!require_prepared || (root_ready && (output.pose_count == 1u || child_ready)));
}

bool SnapshotStore::isolated_pose_ranges(const PairSnapshot &pair) const noexcept {
    for (std::size_t actor_index = 0; actor_index < 2; ++actor_index) {
        const auto &actor = pair.actors[actor_index];
        for (std::size_t i = 0; i < actor.pose_count; ++i) {
            const auto &pose = actor.poses[i];
            for (std::size_t peer_index = 0; peer_index < 2; ++peer_index) {
                const auto &peer = pair.actors[peer_index];
                if (overlaps(pose.address, 32u, peer.node, node_bytes) ||
                    overlaps(pose.address, 32u, peer.entity,
                             peer.type == 1u ? bike::stride : rider::stride) ||
                    overlaps(pose.address, 32u, peer.display_list, peer.display_list_bytes)) {
                    return false;
                }
                for (std::size_t segment = 0; segment < peer.segment_count; ++segment) {
                    if (overlaps(pose.address, 32u, peer.segment_records[segment], 128u)) {
                        return false;
                    }
                }
                for (std::size_t j = 0; j < peer.pose_count; ++j) {
                    if ((actor_index != peer_index || i != j) &&
                        overlaps(pose.address, 32u, peer.poses[j].address, 32u)) {
                        return false;
                    }
                    if (overlaps(pose.address, 32u, peer.poses[j].record, 0x18u)) {
                        return false;
                    }
                    if (peer.poses[j].source_record &&
                        overlaps(pose.address, 32u, peer.poses[j].source_record, 0x14u)) {
                        return false;
                    }
                }
            }
            for (const auto &allocation_record : allocations_) {
                if (!allocation_record.node) {
                    continue;
                }
                if (overlaps(pose.address, 32u, allocation_record.node, node_bytes) ||
                    overlaps(pose.address, 32u, allocation_record.entity,
                             allocation_record.type == 1u ? bike::stride : rider::stride)) {
                    return false;
                }
                // The bike's racer/model state is a separate guest block. Its
                // known extent contains the racer id and +E4 pose-owner link;
                // a pose pointer into that block is not render-owned memory.
                std::uint32_t model_state = 0;
                constexpr auto known_state_bytes = actor_scene::model_state_pose_owner + 4u;
                if (allocation_record.type == 1u &&
                    read_u32(rdram_, allocation_record.entity + bike::model_state_pointer,
                             model_state) &&
                    valid_guest_range(model_state, known_state_bytes) &&
                    overlaps(pose.address, 32u, model_state, known_state_bytes)) {
                    return false;
                }
                for (std::size_t slot = 0; slot < 8; ++slot) {
                    if (allocation_record.bytes[slot] &&
                        overlaps(pose.address, 32u, allocation_record.buffers[slot],
                                 allocation_record.bytes[slot])) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool SnapshotStore::can_prepare(unsigned char *rdram, std::uint32_t bike_node,
                                std::uint32_t rider_node, std::uint32_t viewport,
                                std::uint32_t slot, PreparationPurpose purpose) const noexcept {
    std::uint32_t views = 0;
    if (!supported_scene(rdram) || !read_u32(rdram, viewport_count, views) || viewport >= views ||
        !visual_pair(rdram, bike_node, rider_node)) {
        return false;
    }
    PairSnapshot pair{};
    return capture_actor(rdram, rdram, bike_node, viewport, slot, false, pair.actors[0], purpose) &&
           capture_actor(rdram, rdram, rider_node, viewport, slot, false, pair.actors[1], purpose) &&
           isolated_pose_ranges(pair);
}

bool SnapshotStore::publish(unsigned char *live, unsigned char *prepared, std::uint32_t bike_node,
                            std::uint32_t rider_node, std::uint32_t viewport,
                            std::uint32_t slot, PreparationPurpose purpose) noexcept {
    if (live == prepared || !can_prepare(live, bike_node, rider_node, viewport, slot, purpose) ||
        !visual_pair(prepared, bike_node, rider_node)) {
        return false;
    }
    PairSnapshot candidate{};
    candidate.generation = generation_;
    candidate.viewport = viewport;
    candidate.buffer_slot = slot;
    candidate.purpose = purpose;
    if (!capture_actor(live, prepared, bike_node, viewport, slot, true, candidate.actors[0], purpose) ||
        !capture_actor(live, prepared, rider_node, viewport, slot, true, candidate.actors[1], purpose) ||
        !isolated_pose_ranges(candidate)) {
        return false;
    }
    if (purpose == PreparationPurpose::HighlightRecording) {
        // Recording history may retain children outside the live camera's
        // matrix range. Only pairs independently meeting the unchanged draw
        // certificate can also be consumed by the ordinary Max LOD renderer.
        for (const auto &actor : candidate.actors) {
            RootRenderPlan draw_plan{};
            candidate.drawable &= capture_root_render_plan(live, actor.node, viewport, draw_plan) &&
                                  draw_plan == actor.root_plan &&
                                  (!draw_plan.normalized || normalized_pose_in_range(actor.poses[0]));
        }
    }
    PairSnapshot *destination = nullptr;
    bool conflict = false;
    for (auto &previous : pairs_) {
        if (!previous.valid) {
            if (!destination) {
                destination = &previous;
            }
            continue;
        }
        if (previous.actors[0].node == bike_node || previous.actors[1].node == rider_node) {
            destination = &previous;
            continue;
        }
        for (const auto &actor : candidate.actors) {
            for (std::size_t i = 0; i < actor.pose_count; ++i) {
                for (const auto &other : previous.actors) {
                    for (std::size_t j = 0; j < other.pose_count; ++j) {
                        if (overlaps(actor.poses[i].address, 32u, other.poses[j].address, 32u)) {
                            previous.valid = false;
                            conflict = true;
                        }
                    }
                }
            }
        }
    }
    if (conflict || !destination) {
        return false;
    }
    candidate.valid = true;
    *destination = candidate;
    if (pose_epochs_[viewport] != 0u) {
        // Preparation/draw is serial per camera. Retain only that camera's previous
        // partial animation while the immutable draw certificates remain pass-local.
        for (std::size_t index = viewport * maximum_pairs; index < (viewport + 1u) * maximum_pairs;
             ++index) {
            auto &history = pose_history_[index];
            if (history.epoch != pose_epochs_[viewport] ||
                history.pair.actors[1].node == rider_node) {
                history.pair = candidate;
                history.epoch = pose_epochs_[viewport];
                read_u32(live, globals::main_mode, history.mode);
                read_u32(live, globals::pending_mode, history.pending);
                if (!read_u32(live, candidate.actors[1].entity + 0xcu, history.rider_style)) {
                    history.epoch = 0u;
                }
                for (std::size_t i = 0; i < candidate.actors[1].pose_count; ++i) {
                    if (!read_u16(live, candidate.actors[1].poses[i].record + 2u,
                                  history.animation_links[i])) {
                        history.epoch = 0u;
                    }
                }
                break;
            }
        }
    }
    return true;
}

bool SnapshotStore::seed_previous_rider_children(unsigned char *live, unsigned char *prepared,
                                                 std::uint32_t rider_node, std::uint32_t viewport,
                                                 std::uint32_t slot, bool frozen_pair,
                                                 bool *missing_history) const noexcept {
    if (missing_history)
        *missing_history = false;
    if (!live || live == prepared || live != rdram_ || !prepared || !supported_scene(live) ||
        viewport >= 4u) {
        return false;
    }
    std::uint32_t mode = 0, pending = 0;
    std::uint16_t pause = 0, pause_menu = 0;
    if (!read_u32(live, globals::main_mode, mode) ||
        !read_u32(live, globals::pending_mode, pending) ||
        !read_u16(live, globals::gameplay_pause_state, pause) ||
        (frozen_pair ? pause == 0u : pause != 0u) ||
        !read_u16(live, globals::pause_menu_state, pause_menu) ||
        (!frozen_pair && pause_menu != 0u)) {
        return false;
    }
    for (const auto &history : pose_history_) {
        const auto &prior = history.pair;
        bool same_scene = history.mode == mode && history.pending == pending;
        // Live -> waiting -> results retains the same family and actors.
        // Resource, identity and current-root checks below still all apply.
        for (unsigned result : {0x0bu, 0x14u, 0x19u, 0x1eu}) {
            const auto in_family = [result](unsigned value) {
                return value >= result - 2u && value <= result;
            };
            same_scene |= in_family(mode) && in_family(pending) && in_family(history.mode) &&
                          in_family(history.pending);
        }
        if (history.epoch + 1u != pose_epochs_[viewport] || !prior.valid ||
            prior.actors[1].node != rider_node || prior.viewport != viewport || !same_scene) {
            continue;
        }
        if (!mounted_pair(live, prior.actors[0].node, rider_node) ||
            !mounted_pair(prepared, prior.actors[0].node, rider_node)) {
            return false;
        }
        std::uint32_t rider_style = 0, prepared_style = 0;
        if (!read_u32(live, prior.actors[1].entity + 0xcu, rider_style) ||
            !read_u32(prepared, prior.actors[1].entity + 0xcu, prepared_style) ||
            rider_style != history.rider_style || prepared_style != rider_style) {
            return false;
        }
        PairSnapshot current{};
        for (std::size_t actor = 0; actor < 2; ++actor) {
            const auto &saved = prior.actors[actor];
            auto &now = current.actors[actor];
            if (!capture_actor(live, prepared, saved.node, viewport, slot, false, now, prior.purpose) ||
                now.entity != saved.entity || now.model_state != saved.model_state ||
                now.resource_signature != saved.resource_signature ||
                now.pose_count != saved.pose_count ||
                now.certified_transform_buffers != saved.certified_transform_buffers ||
                now.certified_allocation_bytes != saved.certified_allocation_bytes ||
                now.root_plan.source_scale_bits != saved.root_plan.source_scale_bits ||
                now.root_plan.original_source != saved.root_plan.original_source) {
                return false;
            }
        }
        if (!isolated_pose_ranges(current)) {
            return false;
        }
        const auto &rider = prior.actors[1];
        // Resource checks deliberately concern the detailed graph. A genuine
        // stock LOD change or current camera distance must not erase its own
        // immediately preceding animation state. Root words are never reused.
        for (std::size_t i = 0; i < rider.pose_count; ++i) {
            const auto &pose = rider.poses[i];
            std::uint16_t link = 0, prepared_link = 0;
            if (!read_u16(live, pose.record + 2u, link) ||
                !read_u16(prepared, pose.record + 2u, prepared_link) ||
                link != history.animation_links[i] || prepared_link != link) {
                return false;
            }
            if (pose.type != actor_scene::model_record_single_transform) {
                continue;
            }
            bool nonidentity = false;
            if (!finite_pose(pose, nonidentity)) {
                return false;
            }
            float norm = 0.0f;
            for (std::size_t word = 3; word < 7; ++word) {
                const auto value = std::bit_cast<float>(pose.words[word]);
                norm += value * value;
            }
            if (!(norm > 0.0f && norm <= 16.0f)) {
                return false;
            }
        }
        if (frozen_pair) {
            for (std::size_t i = 0; i < prior.actors[0].pose_count; ++i) {
                const auto &pose = prior.actors[0].poses[i];
                if (pose.type != actor_scene::model_record_single_transform)
                    continue;
                bool nonidentity = false;
                if (!finite_pose(pose, nonidentity))
                    return false;
            }
        }
        for (std::size_t actor = frozen_pair ? 0u : 1u; actor < 2u; ++actor) {
            const auto &saved = prior.actors[actor];
            for (std::size_t i = 0; i < saved.pose_count; ++i) {
                const auto &pose = saved.poses[i];
                if (pose.type != actor_scene::model_record_single_transform)
                    continue;
                for (unsigned word = 0; word < 7u; ++word)
                    write_u32(prepared, pose.address + word * 4u, pose.words[word]);
            }
        }
        return true;
    }
    if (missing_history)
        *missing_history = true;
    return false;
}

const PairSnapshot *SnapshotStore::find(unsigned char *rdram, std::uint32_t node,
                                        std::uint32_t viewport, std::uint32_t slot,
                                        FindFailure *failure) const noexcept {
    if (failure) {
        *failure = FindFailure::None;
    }
    const auto reject = [failure](FindFailure reason) -> const PairSnapshot * {
        if (failure) {
            *failure = reason;
        }
        return nullptr;
    };
    if (rdram != rdram_ || !supported_scene(rdram)) {
        return reject(FindFailure::InvalidContext);
    }
    if (viewport >= 4u) {
        return reject(FindFailure::View);
    }
    if (slot >= 2u) {
        return reject(FindFailure::Slot);
    }
    auto absent_reason = FindFailure::MissingPair;
    for (const auto &pair : pairs_) {
        if (pair.actors[0].node != node && pair.actors[1].node != node) {
            continue;
        }
        if (pair.generation != generation_) {
            absent_reason = FindFailure::Generation;
            continue;
        }
        if (!pair.valid) {
            continue;
        }
        if (pair.viewport != viewport) {
            absent_reason = FindFailure::View;
            continue;
        }
        if (!pair.drawable)
            return reject(FindFailure::RootPlan);
        if (!visual_pair(rdram, pair.actors[0].node, pair.actors[1].node)) {
            return reject(FindFailure::Ownership);
        }
        for (const auto &saved : pair.actors) {
            const auto *allocation_record = allocation(rdram, saved.node);
            if (!allocation_record) {
                return reject(FindFailure::Capture);
            }
            for (std::uint32_t destination_slot = 0; destination_slot < 2u; ++destination_slot) {
                const auto buffer_index = viewport * 2u + destination_slot;
                std::uint32_t buffer = 0;
                if (!read_u32(rdram,
                              saved.node + actor_scene::render_transform_buffers +
                                  buffer_index * 4u,
                              buffer) ||
                    buffer != saved.certified_transform_buffers[destination_slot] ||
                    buffer != allocation_record->buffers[buffer_index] ||
                    allocation_record->bytes[buffer_index] !=
                        saved.certified_allocation_bytes[destination_slot]) {
                    return reject(FindFailure::Allocation);
                }
            }
            ActorSnapshot current{};
            if (!capture_actor(rdram, rdram, saved.node, viewport, slot, false, current)) {
                return reject(FindFailure::Capture);
            }
            if (current.entity != saved.entity || current.model_state != saved.model_state) {
                return reject(FindFailure::Ownership);
            }
            if (current.stock_lod != saved.stock_lod ||
                current.previous_lod != saved.previous_lod ||
                current.stock_model != saved.stock_model) {
                return reject(FindFailure::StockState);
            }
            if (current.resource_signature != saved.resource_signature) {
                return reject(FindFailure::Resource);
            }
            if (current.root_plan != saved.root_plan) {
                return reject(FindFailure::RootPlan);
            }
            if (!root_render_bank_ready(rdram, saved.root_plan, viewport, slot)) {
                return reject(FindFailure::SourceBank);
            }
            // The stock renderer chooses the destination after cached pose
            // preparation. Bind to its actual slot, retaining both exact
            // allocation certificates and the single immutable pose generation.
            if (current.transform_buffer != saved.certified_transform_buffers[slot] ||
                current.allocation_bytes != saved.certified_allocation_bytes[slot] ||
                current.certified_transform_buffers != saved.certified_transform_buffers ||
                current.certified_allocation_bytes != saved.certified_allocation_bytes) {
                return reject(FindFailure::Allocation);
            }
        }
        return isolated_pose_ranges(pair) ? &pair : reject(FindFailure::Isolation);
    }
    return reject(absent_reason);
}

bool SnapshotStore::weapon_root(unsigned char *rdram, std::uint32_t node,
    std::uint32_t view, std::uint32_t slot, std::array<std::uint32_t, 7> &words,
    std::uint32_t &source) const noexcept {
    const auto *pair = find(rdram, node, view, slot);
    if (!pair) return false;
    const auto &rider = pair->actors[1];
    if (rider.node != node || rider.type != 2u || !rider.pose_count ||
        rider.poses[0].record != rider.root_plan.record ||
        rider.root_plan.original_source < 1u || rider.root_plan.original_source > 2u)
        return false;
    std::array<std::uint32_t, 7> copy{};
    for (unsigned i = 0; i < copy.size(); ++i) {
        copy[i] = rider.poses[0].words[i];
        if (!std::isfinite(std::bit_cast<float>(copy[i]))) return false;
    }
    words = copy;
    source = rider.root_plan.original_source;
    return true;
}

bool PoseBinding::begin(unsigned char *rdram, const ActorSnapshot &snapshot) noexcept {
    end();
    if (!rdram || snapshot.pose_count == 0u || snapshot.pose_count > saved_.size()) {
        return false;
    }
    for (std::size_t i = 0; i < snapshot.pose_count; ++i) {
        saved_[i].address = snapshot.poses[i].address;
        if (!valid_guest_range(saved_[i].address, 32u)) {
            return false;
        }
        for (std::uint32_t word = 0; word < pose_words; ++word) {
            if (!read_u32(rdram, saved_[i].address + word * 4u, saved_[i].words[word])) {
                return false;
            }
        }
    }
    rdram_ = rdram;
    count_ = snapshot.pose_count;
    for (std::size_t i = 0; i < count_; ++i) {
        for (std::uint32_t word = 0; word < pose_words; ++word) {
            if (!write_u32(rdram, saved_[i].address + word * 4u, snapshot.poses[i].words[word])) {
                end();
                return false;
            }
        }
    }
    return true;
}

void PoseBinding::end() noexcept {
    if (!rdram_) {
        return;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        for (std::uint32_t word = 0; word < pose_words; ++word) {
            write_u32(rdram_, saved_[i].address + word * 4u, saved_[i].words[word]);
        }
    }
    rdram_ = nullptr;
    count_ = 0;
}
} // namespace rr64::lod
