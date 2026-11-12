#include "rr64_actor_presentation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "rr64_msvc_crt_compat.hpp"

#include "rr64_actor_pose.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_native.hpp"

namespace rr64::engine {
namespace {

bool valid_tier_zero_segments(unsigned char *rdram, std::uint32_t node,
                              std::uint16_t segment_count) noexcept {
    if (segment_count == 0u || segment_count > actor_scene::maximum_segment_records_per_tier) {
        return false;
    }

    const std::uint32_t table = node + actor_scene::primary_segment_records;
    for (std::uint32_t index = 0; index < segment_count; ++index) {
        std::uint32_t record = 0;
        if (!read_u32(rdram, table + index * sizeof(std::uint32_t), record) ||
            !valid_guest_range(record + actor_scene::segment_record_payload,
                               actor_scene::render_transform_size)) {
            return false;
        }
    }
    return true;
}

bool selected_transform_buffer_valid(unsigned char *rdram, std::uint32_t node,
                                     std::uint16_t transform_count,
                                     bool require_sibling_capacity = false) noexcept {
    std::uint32_t viewport = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t transform_buffer = 0;
    if (transform_count == 0u || transform_count > actor_scene::maximum_render_transforms ||
        !read_u32(rdram, globals::active_viewport, viewport) ||
        !read_u32(rdram, globals::actor_render_buffer_slot, buffer_slot) ||
        viewport >= actor_scene::maximum_viewports ||
        buffer_slot >= actor_scene::render_buffer_slot_count ||
        !read_u32(rdram,
                  node + actor_scene::render_transform_buffers +
                      viewport * actor_scene::render_buffer_viewport_stride +
                      buffer_slot * actor_scene::render_buffer_slot_stride,
                  transform_buffer)) {
        return false;
    }

    const std::uint32_t required_bytes =
        static_cast<std::uint32_t>(transform_count) * actor_scene::render_transform_size;
    if (!valid_guest_range(transform_buffer, required_bytes)) {
        return false;
    }
    if (!require_sibling_capacity) {
        return true;
    }

    std::uint32_t minimum_gap = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t candidate_viewport = 0; candidate_viewport < actor_scene::maximum_viewports;
         ++candidate_viewport) {
        for (std::uint32_t candidate_slot = 0;
             candidate_slot < actor_scene::render_buffer_slot_count; ++candidate_slot) {
            std::uint32_t candidate_buffer = 0;
            if (!read_u32(rdram,
                          node + actor_scene::render_transform_buffers +
                              candidate_viewport * actor_scene::render_buffer_viewport_stride +
                              candidate_slot * actor_scene::render_buffer_slot_stride,
                          candidate_buffer) ||
                candidate_buffer == 0u || candidate_buffer == transform_buffer) {
                continue;
            }
            const std::uint32_t gap = candidate_buffer > transform_buffer
                                          ? candidate_buffer - transform_buffer
                                          : transform_buffer - candidate_buffer;
            minimum_gap = gap < minimum_gap ? gap : minimum_gap;
        }
    }
    return minimum_gap != std::numeric_limits<std::uint32_t>::max() &&
           minimum_gap >= required_bytes;
}

bool active_actor_node(unsigned char *rdram, std::uint32_t node, std::uint32_t node_type) noexcept {
    std::uint32_t active_racers = 0;
    std::uint32_t bike_pool = 0;
    if (!read_u32(rdram, globals::active_racer_count, active_racers) ||
        !read_u32(rdram, globals::bike_pool_pointer, bike_pool) || active_racers == 0u ||
        active_racers > kMaximumRacers ||
        !valid_guest_range(bike_pool, active_racers * bike::stride)) {
        return false;
    }

    if (node_type == 1u) {
        std::uint32_t entity = 0;
        if (!read_u32(rdram, node + actor_scene::entity, entity) || entity < bike_pool) {
            return false;
        }
        const std::uint32_t offset = entity - bike_pool;
        return (offset % bike::stride) == 0u && (offset / bike::stride) < active_racers;
    }

    for (std::uint32_t racer = 0; racer < active_racers; ++racer) {
        const std::uint32_t bike_address = bike_pool + racer * bike::stride;
        std::uint32_t model_state = 0;
        std::uint32_t pose_owner = 0;
        std::uint32_t rider_node = 0;
        if (read_u32(rdram, bike_address + bike::model_state_pointer, model_state) &&
            valid_guest_range(model_state + actor_scene::model_state_pose_owner,
                              sizeof(std::uint32_t)) &&
            read_u32(rdram, model_state + actor_scene::model_state_pose_owner, pose_owner) &&
            valid_guest_range(pose_owner + actor_scene::pose_owner_rider_node,
                              sizeof(std::uint32_t)) &&
            read_u32(rdram, pose_owner + actor_scene::pose_owner_rider_node, rider_node) &&
            rider_node == node) {
            return true;
        }
    }
    return false;
}

} // namespace

bool valid_actor_presentation_pair(unsigned char *rdram, std::uint32_t bike_node,
                                   std::uint32_t rider_node) noexcept {
    std::uint32_t bike_type = 0;
    std::uint32_t rider_type = 0;
    std::uint32_t bike_entity = 0;
    std::uint32_t model_state = 0;
    std::uint32_t pose_owner = 0;
    std::uint32_t linked_rider_node = 0;
    return valid_guest_range(bike_node, actor_scene::node_minimum_size) &&
           valid_guest_range(rider_node, actor_scene::node_minimum_size) &&
           read_u32(rdram, bike_node + actor_scene::type, bike_type) &&
           read_u32(rdram, rider_node + actor_scene::type, rider_type) && bike_type == 1u &&
           rider_type == 2u && read_u32(rdram, bike_node + actor_scene::entity, bike_entity) &&
           valid_guest_range(bike_entity, bike::stride) &&
           read_u32(rdram, bike_entity + bike::model_state_pointer, model_state) &&
           valid_guest_range(model_state + actor_scene::model_state_pose_owner,
                             sizeof(std::uint32_t)) &&
           read_u32(rdram, model_state + actor_scene::model_state_pose_owner, pose_owner) &&
           valid_guest_range(pose_owner + actor_scene::pose_owner_rider_node,
                             sizeof(std::uint32_t)) &&
           read_u32(rdram, pose_owner + actor_scene::pose_owner_rider_node, linked_rider_node) &&
           linked_rider_node == rider_node;
}

bool choose_actor_presentation_lod(unsigned char *rdram, std::uint32_t node,
                                   std::uint16_t stock_lod, bool high_detail_enabled,
                                   ActorPresentationDecision &decision,
                                   bool validated_actor_pair) noexcept {
    decision = {};
    decision.stock_lod = stock_lod;
    decision.presentation_lod = stock_lod;
    if (!high_detail_enabled) {
        decision.reason = ActorPresentationDecisionReason::Disabled;
        return true;
    }
    if (rdram == nullptr || stock_lod >= actor_scene::lod_record_count ||
        !valid_guest_range(node, actor_scene::node_minimum_size)) {
        decision.reason = ActorPresentationDecisionReason::InvalidInput;
        return false;
    }

    std::uint16_t selected_lod = 0;
    if (!read_u32(rdram, node + actor_scene::type, decision.node_type)) {
        decision.reason = ActorPresentationDecisionReason::InvalidInput;
        return false;
    }
    if (decision.node_type != 1u && decision.node_type != 2u) {
        decision.reason = ActorPresentationDecisionReason::UnsupportedNodeType;
        return true;
    }
    if (stock_lod == 0u) {
        decision.presentation_lod = 0u;
        decision.reason = ActorPresentationDecisionReason::StockTierZero;
        return true;
    }
    if (!validated_actor_pair && !active_actor_node(rdram, node, decision.node_type)) {
        decision.reason = ActorPresentationDecisionReason::InactiveActorNode;
        return true;
    }
    if (!read_u16(rdram, node + actor_scene::selected_lod, selected_lod) ||
        selected_lod != stock_lod ||
        !read_u32(rdram, node + actor_scene::current_model, decision.current_model) ||
        !read_u32(rdram,
                  node + actor_scene::lod_models + stock_lod * actor_scene::lod_pointer_stride,
                  decision.stock_model) ||
        decision.current_model == 0u || decision.current_model != decision.stock_model) {
        decision.reason = ActorPresentationDecisionReason::IncoherentStockModel;
        return false;
    }

    if (!read_u32(rdram, node + actor_scene::lod_models, decision.presentation_model) ||
        !read_u32(rdram, node + actor_scene::display_lists, decision.presentation_display_list) ||
        !read_u16(rdram, node + actor_scene::segment_counts, decision.presentation_segment_count) ||
        decision.presentation_model == 0u || decision.presentation_display_list == 0u ||
        !valid_tier_zero_segments(rdram, node, decision.presentation_segment_count)) {
        decision.reason = ActorPresentationDecisionReason::MissingTierZeroResources;
        return false;
    }

    ModelGraphTopologySnapshot stock_topology{};
    ModelGraphTopologySnapshot presentation_topology{};
    if (!capture_model_graph_topology(rdram, decision.stock_model, stock_topology) ||
        !capture_model_graph_topology(rdram, decision.presentation_model, presentation_topology)) {
        decision.reason = ActorPresentationDecisionReason::InvalidInput;
        return false;
    }
    decision.stock_transform_count = stock_topology.transform_count;
    decision.presentation_transform_count = presentation_topology.transform_count;
    decision.stock_allocation_hash = stock_topology.allocation_hash;
    decision.presentation_allocation_hash = presentation_topology.allocation_hash;

    if (!selected_transform_buffer_valid(rdram, node, decision.stock_transform_count)) {
        decision.reason = ActorPresentationDecisionReason::InvalidTransformBuffer;
        return false;
    }
    const bool allocation_compatible =
        compatible_transform_allocation(stock_topology, presentation_topology);

    // A matching allocation signature only proves that both graphs write the
    // same number and shape of matrices. It does not prove that matrix N means
    // the same rider bone, bike body, or wheel in both LODs. Reusing a lower
    // tier's transforms with tier-zero geometry was the source of intermittent
    // detached and sunken actors. Evaluate tier zero's complete graph locally
    // for every upgraded actor instead.
    // Equal-size graphs already fit the stock allocation. The far-bike graph
    // expands from four to six matrices, so it additionally needs the measured
    // sibling-buffer capacity check before the original transform generator is
    // allowed to write the two extra matrices.
    const bool transform_capacity_valid =
        allocation_compatible
            ? selected_transform_buffer_valid(rdram, node, presentation_topology.transform_count)
            : selected_transform_buffer_valid(rdram, node, presentation_topology.transform_count,
                                              true);
    if (transform_capacity_valid) {
        decision.presentation_lod = 0u;
        decision.reason = ActorPresentationDecisionReason::TierZeroGraphSubstitution;
        return true;
    }

    decision.reason = allocation_compatible
                          ? ActorPresentationDecisionReason::InvalidTransformBuffer
                          : ActorPresentationDecisionReason::IncompatibleTransformAllocation;
    return true;
}

std::uint32_t choose_actor_transform_model(unsigned char *rdram, std::uint32_t node,
                                           std::uint16_t stock_lod, std::uint32_t stock_model,
                                           bool high_detail_enabled,
                                           ActorPresentationDecision &decision) noexcept {
    choose_actor_presentation_lod(rdram, node, stock_lod, high_detail_enabled, decision);
    if (decision.reason == ActorPresentationDecisionReason::TierZeroGraphSubstitution &&
        decision.presentation_model != 0u && decision.stock_model == stock_model) {
        return decision.presentation_model;
    }
    return stock_model;
}

} // namespace rr64::engine

namespace {

std::atomic_bool g_high_detail_actors_enabled{false};
std::atomic_uint64_t g_actor_presentation_sequence{0};
std::atomic_uint64_t g_actor_graph_matrix_sequence{0};
std::atomic_uint64_t g_actor_render_sequence{0};
std::atomic_uint64_t g_actor_rendered_matrix_sequence{0};
std::atomic_uint64_t g_actor_pose_binding_sequence{0};
std::mutex g_actor_presentation_trace_mutex;

// The stock selector runs before the game prepares model-dependent rider and
// bike data. A late model-pointer substitution skips that preparation and can
// combine a tier-zero display list with stale far-tier pose/segment state. Keep
// the override inside one presentation transaction instead: snapshot the exact
// stock node state, expose tier zero to the original preparation and renderer,
// then restore every field before simulation can observe it on the next frame.
struct ActorPresentationTransaction {
    std::uint32_t node = 0;
    std::uint32_t stock_model = 0;
    std::uint16_t stock_lod = 0;
    std::uint16_t stock_previous_lod = 0;
    rr64::engine::ActorPresentationDecision decision{};
    std::uint16_t presentation_record_count = 0;
    std::array<std::uint32_t, rr64::engine::actor_scene::maximum_model_records>
        presentation_records{};
};

// func_8005D9A4 supplies the authoritative bike/rider ownership pair for both
// local and AI racers. Keep that relationship in native presentation state so
// a later renderer invocation can validate an AI node after the temporary
// guest LOD transaction has already been restored. The entity guard prevents
// a recycled scene-node address from inheriting an earlier actor's decision.
struct ValidatedActorPresentationNode {
    std::uint32_t node = 0;
    std::uint32_t node_type = 0;
    std::uint32_t entity = 0;
    std::uint32_t peer_node = 0;
};

constexpr std::size_t kMaximumActorPresentationTransactions = rr64::engine::kMaximumRacers * 2u;
std::array<ActorPresentationTransaction, kMaximumActorPresentationTransactions>
    g_actor_presentation_transactions{};
std::size_t g_actor_presentation_transaction_count = 0;
std::array<ValidatedActorPresentationNode, kMaximumActorPresentationTransactions>
    g_validated_actor_nodes{};
std::size_t g_validated_actor_node_count = 0;
std::size_t g_validated_actor_node_replacement = 0;
unsigned char *g_actor_presentation_rdram = nullptr;
std::mutex g_actor_presentation_transaction_mutex;
thread_local bool g_actor_presentation_scope_active = false;
thread_local unsigned char *g_actor_presentation_scope_rdram = nullptr;

bool pose_source_is_prepared(unsigned char *rdram, std::uint32_t pose_source) noexcept {
    constexpr std::uint32_t pose_word_count = 7u;
    if (!rr64::engine::valid_guest_range(pose_source, pose_word_count * sizeof(std::uint32_t))) {
        return false;
    }

    std::array<std::uint32_t, pose_word_count> words{};
    bool all_zero = true;
    for (std::uint32_t index = 0; index < words.size(); ++index) {
        if (!rr64::engine::read_u32(rdram, pose_source + index * sizeof(std::uint32_t),
                                    words[index])) {
            return false;
        }
        const std::uint32_t magnitude = words[index] & 0x7FFFFFFFu;
        if ((magnitude & 0x7F800000u) == 0x7F800000u) {
            // A NaN or infinity must never be allowed into the renderer's
            // matrix builder, even if the rest of the graph looks prepared.
            return false;
        }
        all_zero = all_zero && magnitude == 0u;
    }
    if (all_zero) {
        return false;
    }

    // The game's cleared pose is an origin translation and identity
    // quaternion. It is valid storage but not a prepared distant actor pose.
    const bool zero_translation_and_vector =
        (words[0] & 0x7FFFFFFFu) == 0u && (words[1] & 0x7FFFFFFFu) == 0u &&
        (words[2] & 0x7FFFFFFFu) == 0u && (words[3] & 0x7FFFFFFFu) == 0u &&
        (words[4] & 0x7FFFFFFFu) == 0u && (words[5] & 0x7FFFFFFFu) == 0u;
    const bool unit_scalar = (words[6] & 0x7FFFFFFFu) == 0x3F800000u;
    return !zero_translation_and_vector || !unit_scalar;
}

bool tier_zero_pose_graph_is_prepared(unsigned char *rdram, std::uint32_t node) noexcept {
    std::uint32_t tier_zero_model = 0;
    if (!rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::lod_models,
                                tier_zero_model) ||
        tier_zero_model == 0u) {
        return false;
    }

    rr64::engine::ModelGraphTopologySnapshot topology{};
    if (!rr64::engine::capture_model_graph_topology(rdram, tier_zero_model, topology)) {
        return false;
    }

    bool root_prepared = false;
    bool child_prepared = false;
    std::uint32_t transform_record_count = 0;
    for (std::uint32_t index = 0; index < topology.record_count; ++index) {
        const auto &record = topology.records[index];
        if (record.transform_count == 0u) {
            continue;
        }

        std::uint32_t pose_source = 0;
        if (!rr64::engine::read_u32(
                rdram, record.address + rr64::engine::actor_scene::model_record_pose_source,
                pose_source)) {
            return false;
        }
        const bool prepared = pose_source_is_prepared(rdram, pose_source);
        if (transform_record_count == 0u) {
            root_prepared = prepared;
        } else {
            child_prepared = child_prepared || prepared;
        }
        ++transform_record_count;
    }

    // A one-record model has no child pose to validate. Multi-record detailed
    // riders and bikes require both a live root and at least one independently
    // animated child, which filters the dormant identity graphs seen in r19.
    return root_prepared && transform_record_count != 0u &&
           (transform_record_count == 1u || child_prepared);
}

void remember_validated_actor_node_locked(unsigned char *rdram, std::uint32_t node,
                                          std::uint32_t peer_node) {
    std::uint32_t node_type = 0;
    std::uint32_t entity = 0;
    if (!rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::type, node_type) ||
        !rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::entity, entity)) {
        return;
    }

    ValidatedActorPresentationNode entry{node, node_type, entity, peer_node};
    for (std::size_t index = 0; index < g_validated_actor_node_count; ++index) {
        if (g_validated_actor_nodes[index].node == node) {
            g_validated_actor_nodes[index] = entry;
            return;
        }
    }

    if (g_validated_actor_node_count < g_validated_actor_nodes.size()) {
        g_validated_actor_nodes[g_validated_actor_node_count++] = entry;
        return;
    }
    g_validated_actor_nodes[g_validated_actor_node_replacement] = entry;
    g_validated_actor_node_replacement =
        (g_validated_actor_node_replacement + 1u) % g_validated_actor_nodes.size();
}

bool find_validated_actor_pair_locked(unsigned char *rdram, std::uint32_t node,
                                      std::uint32_t &peer_node) noexcept {
    if (rdram == nullptr || rdram != g_actor_presentation_rdram) {
        return false;
    }
    for (std::size_t index = 0; index < g_validated_actor_node_count; ++index) {
        const auto &entry = g_validated_actor_nodes[index];
        if (entry.node != node) {
            continue;
        }

        std::uint32_t current_type = 0;
        std::uint32_t current_entity = 0;
        if (!rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::type, current_type) ||
            !rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::entity,
                                    current_entity) ||
            current_type != entry.node_type || current_entity != entry.entity) {
            return false;
        }

        const bool pair_valid =
            entry.node_type == 1u
                ? rr64::engine::valid_actor_presentation_pair(rdram, node, entry.peer_node)
                : entry.node_type == 2u &&
                      rr64::engine::valid_actor_presentation_pair(rdram, entry.peer_node, node);
        if (!pair_valid) {
            return false;
        }
        peer_node = entry.peer_node;
        return true;
    }
    return false;
}

bool populate_presentation_record_map(unsigned char *rdram,
                                      ActorPresentationTransaction &transaction) {
    rr64::engine::ModelGraphTopologySnapshot presentation{};
    if (!rr64::engine::capture_model_graph_topology(rdram, transaction.decision.presentation_model,
                                                    presentation)) {
        return false;
    }

    transaction.presentation_record_count = presentation.record_count;
    for (std::uint32_t index = 0; index < transaction.presentation_record_count; ++index) {
        transaction.presentation_records[index] = presentation.records[index].address;
    }
    return transaction.presentation_record_count != 0u;
}

bool experimental_environment_enabled() {
    static const bool enabled = [] {
        char *value = nullptr;
        std::size_t length = 0;
        const errno_t result = _dupenv_s(&value, &length, "RR64_EXPERIMENTAL_HIGH_DETAIL_ACTORS");
        const bool selected =
            result == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
        std::free(value);
        return selected;
    }();
    return enabled;
}

FILE *actor_presentation_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_ACTOR_PRESENTATION_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(
                opened,
                "schema,sequence,node,node_type,stock_lod,presentation_lod,reason,stock_transforms,"
                "presentation_transforms,stock_allocation_hash,presentation_allocation_hash,"
                "current_model,stock_model,presentation_model,presentation_display_list,"
                "presentation_segment_count\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *actor_graph_matrix_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_ACTOR_GRAPH_MATRIX_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(opened, "schema,sequence,node,node_type,stock_lod,transform_index,"
                                 "m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23,"
                                 "m30,m31,m32,m33,transform_buffer,viewport,buffer_slot,"
                                 "minimum_local_buffer_gap,required_transform_bytes\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *actor_render_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_ACTOR_RENDER_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(opened,
                         "schema,sequence,event,renderer,list_head,node,node_type,actor_count,"
                         "actor_hash,entity,depth_bits,current_model,model_flags,selected_lod,"
                         "previous_lod,model0,model1,model2,display0,display1,display2,"
                         "segment0,segment1,segment2,transform_buffer,transaction_active\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *actor_rendered_matrix_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_ACTOR_RENDERED_MATRIX_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(opened, "schema,sequence,node,node_type,entity,stock_lod,selected_lod,"
                                 "model_record,source_matrix,rendered_matrix,transform_buffer,"
                                 "transform_index,render_flag,depth_bits,"
                                 "source_m00,source_m01,source_m02,source_m03,"
                                 "source_m10,source_m11,source_m12,source_m13,"
                                 "source_m20,source_m21,source_m22,source_m23,"
                                 "source_m30,source_m31,source_m32,source_m33,"
                                 "fixed_m00,fixed_m01,fixed_m02,fixed_m03,"
                                 "fixed_m10,fixed_m11,fixed_m12,fixed_m13,"
                                 "fixed_m20,fixed_m21,fixed_m22,fixed_m23,"
                                 "fixed_m30,fixed_m31,fixed_m32,fixed_m33,"
                                 "front_x,front_y,front_z,body_x,body_y,body_z,"
                                 "rear_x,rear_y,rear_z\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *actor_pose_binding_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_ACTOR_POSE_BINDING_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(opened,
                         "schema,sequence,stage,category,node,node_type,entity,stock_lod,"
                         "model_lod,model,model_identity,record_index,record_address,"
                         "record_type,next_delta,pose_source,source_record,"
                         "pose_w0,pose_w1,pose_w2,pose_w3,pose_w4,pose_w5,pose_w6,pose_w7\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

struct ActorRenderTraceRow {
    std::uint32_t node = 0;
    std::uint32_t node_type = 0;
    std::uint32_t entity = 0;
    std::uint32_t depth_bits = 0;
    std::uint32_t current_model = 0;
    std::uint16_t model_flags = 0;
    std::uint16_t selected_lod = 0;
    std::uint16_t previous_lod = 0;
    std::array<std::uint32_t, rr64::engine::actor_scene::lod_record_count> models{};
    std::array<std::uint32_t, rr64::engine::actor_scene::lod_record_count> displays{};
    std::array<std::uint16_t, rr64::engine::actor_scene::lod_record_count> segments{};
    std::uint32_t transform_buffer = 0;
    bool transaction_active = false;
};

struct ActorRenderListTraceState {
    std::uint32_t renderer = 0;
    std::uint32_t list_head = 0;
    std::uint64_t actor_hash = 0;
    bool occupied = false;
};

constexpr std::size_t kMaximumActorRenderTraceRows = rr64::engine::kMaximumRacers * 2u;
std::array<ActorRenderListTraceState, 16> g_actor_render_list_trace_states{};

struct ActorRenderedMatrixTraceState {
    std::uint32_t node = 0;
    std::uint64_t render_count = 0;
    bool capture_current_render = false;
};

std::array<ActorRenderedMatrixTraceState, kMaximumActorPresentationTransactions>
    g_actor_rendered_matrix_trace_states{};

// A stage is reached once per presentation producer, not once per actor. Keep
// the diagnostic bounded while still sampling long enough for a rider to move
// from the normal tier-zero range into both distant stock tiers.
std::array<std::atomic_uint64_t, 8> g_actor_pose_binding_stage_calls{};
constexpr std::uint64_t kActorPoseBindingSamplePeriod = 30u;
constexpr std::uint64_t kActorPoseBindingMaximumSamples = 240u;

void mix_actor_render_hash(std::uint64_t &hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ull;
}

void trace_actor_presentation(std::uint32_t node,
                              const rr64::engine::ActorPresentationDecision &decision) {
    FILE *file = actor_presentation_trace_file();
    if (file == nullptr) {
        return;
    }
    std::lock_guard lock{g_actor_presentation_trace_mutex};
    const std::uint64_t sequence = ++g_actor_presentation_sequence;
    std::fprintf(file,
                 "1,%llu,0x%08X,%u,%u,%u,%u,%u,%u,0x%016llX,0x%016llX,"
                 "0x%08X,0x%08X,0x%08X,0x%08X,%u\n",
                 static_cast<unsigned long long>(sequence), node, decision.node_type,
                 static_cast<unsigned>(decision.stock_lod),
                 static_cast<unsigned>(decision.presentation_lod),
                 static_cast<unsigned>(decision.reason),
                 static_cast<unsigned>(decision.stock_transform_count),
                 static_cast<unsigned>(decision.presentation_transform_count),
                 static_cast<unsigned long long>(decision.stock_allocation_hash),
                 static_cast<unsigned long long>(decision.presentation_allocation_hash),
                 decision.current_model, decision.stock_model, decision.presentation_model,
                 decision.presentation_display_list,
                 static_cast<unsigned>(decision.presentation_segment_count));
    if (decision.reason ==
        rr64::engine::ActorPresentationDecisionReason::TierZeroGraphSubstitution) {
        // The matrix trace flushes each rebuilt set. Keep the matching decision
        // durable as well so bounded test termination cannot create false
        // count mismatches between the two diagnostics.
        std::fflush(file);
    }
}

void trace_actor_graph_matrices(unsigned char *rdram, std::uint32_t node,
                                const rr64::engine::ActorPresentationDecision &decision) {
    FILE *file = actor_graph_matrix_trace_file();
    if (file == nullptr ||
        decision.reason !=
            rr64::engine::ActorPresentationDecisionReason::TierZeroGraphSubstitution) {
        return;
    }

    std::uint32_t viewport = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t transform_buffer = 0;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::active_viewport, viewport) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::actor_render_buffer_slot,
                                buffer_slot) ||
        viewport >= rr64::engine::actor_scene::maximum_viewports ||
        buffer_slot >= rr64::engine::actor_scene::render_buffer_slot_count ||
        !rr64::engine::read_u32(
            rdram,
            node + rr64::engine::actor_scene::render_transform_buffers +
                viewport * rr64::engine::actor_scene::render_buffer_viewport_stride +
                buffer_slot * rr64::engine::actor_scene::render_buffer_slot_stride,
            transform_buffer)) {
        return;
    }

    rr64::engine::RenderTransformSetSnapshot transforms{};
    if (!rr64::engine::capture_render_transform_set(
            rdram, transform_buffer, decision.presentation_transform_count, transforms)) {
        return;
    }

    std::uint32_t minimum_local_buffer_gap = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t candidate_viewport = 0;
         candidate_viewport < rr64::engine::actor_scene::maximum_viewports; ++candidate_viewport) {
        for (std::uint32_t candidate_slot = 0;
             candidate_slot < rr64::engine::actor_scene::render_buffer_slot_count;
             ++candidate_slot) {
            std::uint32_t candidate_buffer = 0;
            if (!rr64::engine::read_u32(
                    rdram,
                    node + rr64::engine::actor_scene::render_transform_buffers +
                        candidate_viewport *
                            rr64::engine::actor_scene::render_buffer_viewport_stride +
                        candidate_slot * rr64::engine::actor_scene::render_buffer_slot_stride,
                    candidate_buffer) ||
                candidate_buffer == 0u || candidate_buffer == transform_buffer) {
                continue;
            }
            const std::uint32_t gap = candidate_buffer > transform_buffer
                                          ? candidate_buffer - transform_buffer
                                          : transform_buffer - candidate_buffer;
            if (gap < minimum_local_buffer_gap) {
                minimum_local_buffer_gap = gap;
            }
        }
    }
    if (minimum_local_buffer_gap == std::numeric_limits<std::uint32_t>::max()) {
        minimum_local_buffer_gap = 0u;
    }
    const std::uint32_t required_transform_bytes =
        static_cast<std::uint32_t>(decision.presentation_transform_count) *
        rr64::engine::actor_scene::render_transform_size;

    std::lock_guard lock{g_actor_presentation_trace_mutex};
    const std::uint64_t sequence = ++g_actor_graph_matrix_sequence;
    for (std::uint32_t index = 0; index < transforms.transform_count; ++index) {
        const auto &matrix = transforms.transforms[index].values;
        std::fprintf(file,
                     "1,%llu,0x%08X,%u,%u,%u,"
                     "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                     "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                     "0x%08X,%u,%u,%u,%u\n",
                     static_cast<unsigned long long>(sequence), node, decision.node_type,
                     static_cast<unsigned>(decision.stock_lod), index, matrix[0], matrix[1],
                     matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8],
                     matrix[9], matrix[10], matrix[11], matrix[12], matrix[13], matrix[14],
                     matrix[15], transform_buffer, viewport, buffer_slot, minimum_local_buffer_gap,
                     required_transform_bytes);
    }
    std::fflush(file);
}

} // namespace

extern "C" void rr64_set_high_detail_actors_enabled(int enabled) {
    g_high_detail_actors_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" int rr64_is_high_detail_actors_enabled() {
    return (g_high_detail_actors_enabled.load(std::memory_order_relaxed) ||
            experimental_environment_enabled())
               ? 1
               : 0;
}

extern "C" void rr64_actor_trace_pose_bindings(unsigned char *rdram, unsigned int stage) {
    FILE *file = actor_pose_binding_trace_file();
    if (file == nullptr || rdram == nullptr || stage >= 8u) {
        return;
    }

    // Capture the first visit, then roughly twice per second at the stock
    // update rate. Sampling instead of logging every visual pass avoids
    // perturbing the timing that this trace is intended to diagnose.
    const std::uint64_t call = ++g_actor_pose_binding_stage_calls[stage];
    const std::uint64_t sample = 1u + call / kActorPoseBindingSamplePeriod;
    if ((call != 1u && (call % kActorPoseBindingSamplePeriod) != 0u) ||
        sample > kActorPoseBindingMaximumSamples) {
        return;
    }

    struct PoseTraceActor {
        std::uint32_t node = 0;
        std::uint32_t node_type = 0;
        std::uint32_t entity = 0;
        std::uint16_t stock_lod = 0;
        std::uint16_t model_lod = 0;
        std::uint32_t model = 0;
        const char *category = nullptr;
    };

    std::array<PoseTraceActor, 16> actors{};
    std::size_t actor_count = 0;
    {
        std::lock_guard transaction_lock{g_actor_presentation_transaction_mutex};
        if (rdram != g_actor_presentation_rdram) {
            return;
        }

        // Record both sides of each promoted actor. The tier-zero graph is the
        // one being drawn; the stock graph is the live-distance reference.
        for (std::size_t index = 0;
             index < g_actor_presentation_transaction_count && actor_count + 3u < actors.size();
             ++index) {
            const auto &transaction = g_actor_presentation_transactions[index];
            if (transaction.stock_lod == 0u) {
                continue;
            }

            std::uint32_t node_type = 0;
            std::uint32_t entity = 0;
            rr64::engine::read_u32(rdram, transaction.node + rr64::engine::actor_scene::type,
                                   node_type);
            rr64::engine::read_u32(rdram, transaction.node + rr64::engine::actor_scene::entity,
                                   entity);

            actors[actor_count++] = {
                transaction.node,      node_type, entity,
                transaction.stock_lod, 0u,        transaction.decision.presentation_model,
                "promoted-tier0",
            };
            actors[actor_count++] = {
                transaction.node,
                node_type,
                entity,
                transaction.stock_lod,
                transaction.stock_lod,
                transaction.stock_model,
                "stock-distance",
            };
        }
    }

    // Retain one naturally selected tier-zero bike and rider as controls. They
    // show what valid detailed bindings look like without an override.
    std::array<bool, 3> have_control{};
    const std::array<std::uint32_t, 2> list_globals{
        rr64::engine::globals::actor_scene_head,
        rr64::engine::globals::actor_scene_head + sizeof(std::uint32_t),
    };
    for (const std::uint32_t list_global : list_globals) {
        std::uint32_t node = 0;
        if (!rr64::engine::read_u32(rdram, list_global, node)) {
            continue;
        }
        for (std::uint32_t visited = 0; node != 0u && visited < 64u && actor_count < actors.size();
             ++visited) {
            if (!rr64::engine::valid_guest_range(node,
                                                 rr64::engine::actor_scene::node_minimum_size)) {
                break;
            }
            std::uint32_t node_type = 0;
            std::uint32_t entity = 0;
            std::uint32_t model = 0;
            std::uint32_t next = 0;
            std::uint16_t selected_lod = 0;
            if (!rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::type, node_type) ||
                !rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::entity, entity) ||
                !rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::next, next) ||
                !rr64::engine::read_u16(rdram, node + rr64::engine::actor_scene::selected_lod,
                                        selected_lod)) {
                break;
            }
            if ((node_type == 1u || node_type == 2u) && selected_lod == 0u &&
                !have_control[node_type] &&
                rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::lod_models,
                                       model) &&
                model != 0u) {
                actors[actor_count++] = {
                    node, node_type, entity, 0u, 0u, model, "natural-tier0",
                };
                have_control[node_type] = true;
            }
            if (next == node) {
                break;
            }
            node = next;
        }
    }

    std::lock_guard trace_lock{g_actor_presentation_trace_mutex};
    for (const auto &actor : actors) {
        if (actor.node == 0u || actor.model == 0u || actor.category == nullptr) {
            continue;
        }
        rr64::engine::ModelGraphTopologySnapshot topology{};
        if (!rr64::engine::capture_model_graph_topology(rdram, actor.model, topology)) {
            continue;
        }

        std::uint32_t model_identity = 0;
        rr64::engine::read_u32(rdram, actor.model + 0x14u, model_identity);
        for (std::uint32_t record_index = 0; record_index < topology.record_count; ++record_index) {
            const auto &record = topology.records[record_index];
            if (record.transform_count == 0u) {
                continue;
            }

            std::uint32_t pose_source = 0;
            std::uint32_t source_record = 0;
            std::array<std::uint32_t, 8> pose_words{};
            rr64::engine::read_u32(rdram, record.address + 0x0Cu, pose_source);
            rr64::engine::read_u32(rdram, record.address + 0x14u, source_record);
            if (rr64::engine::valid_guest_range(
                    pose_source, static_cast<std::uint32_t>(pose_words.size() * 4u))) {
                for (std::uint32_t word = 0; word < pose_words.size(); ++word) {
                    rr64::engine::read_u32(rdram, pose_source + word * 4u, pose_words[word]);
                }
            }

            const std::uint64_t sequence = ++g_actor_pose_binding_sequence;
            std::fprintf(file,
                         "1,%llu,%u,%s,0x%08X,%u,0x%08X,%u,%u,0x%08X,0x%08X,"
                         "%u,0x%08X,0x%04X,%d,0x%08X,0x%08X,"
                         "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                         static_cast<unsigned long long>(sequence), stage, actor.category,
                         actor.node, actor.node_type, actor.entity,
                         static_cast<unsigned>(actor.stock_lod),
                         static_cast<unsigned>(actor.model_lod), actor.model, model_identity,
                         record_index, record.address, static_cast<unsigned>(record.type),
                         static_cast<int>(record.next_delta), pose_source, source_record,
                         pose_words[0], pose_words[1], pose_words[2], pose_words[3], pose_words[4],
                         pose_words[5], pose_words[6], pose_words[7]);
        }
    }
    std::fflush(file);
}

extern "C" void rr64_actor_trace_render_list(unsigned char *rdram, unsigned int list_head,
                                             unsigned int renderer) {
    FILE *file = actor_render_trace_file();
    if (file == nullptr || rdram == nullptr) {
        return;
    }

    std::uint32_t viewport = 0;
    std::uint32_t buffer_slot = 0;
    rr64::engine::read_u32(rdram, rr64::engine::globals::active_viewport, viewport);
    rr64::engine::read_u32(rdram, rr64::engine::globals::actor_render_buffer_slot, buffer_slot);

    std::array<ActorRenderTraceRow, kMaximumActorRenderTraceRows> rows{};
    std::size_t row_count = 0;
    std::uint32_t actor_count = 0;
    std::uint64_t actor_hash = 1469598103934665603ull;
    mix_actor_render_hash(actor_hash, renderer);
    mix_actor_render_hash(actor_hash, list_head);

    std::lock_guard transaction_lock{g_actor_presentation_transaction_mutex};
    std::uint32_t node = list_head;
    for (std::uint32_t visited = 0; node != 0u && visited < 512u; ++visited) {
        if (!rr64::engine::valid_guest_range(node, rr64::engine::actor_scene::node_minimum_size)) {
            mix_actor_render_hash(actor_hash, 0xBAD00000u | visited);
            break;
        }

        std::uint32_t node_type = 0;
        std::uint32_t next = 0;
        if (!rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::type, node_type) ||
            !rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::next, next)) {
            mix_actor_render_hash(actor_hash, 0xBAD10000u | visited);
            break;
        }

        if (node_type == 1u || node_type == 2u) {
            ActorRenderTraceRow row{};
            row.node = node;
            row.node_type = node_type;
            rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::entity, row.entity);
            if (viewport < rr64::engine::actor_scene::maximum_viewports) {
                rr64::engine::read_u32(rdram,
                                       node + rr64::engine::actor_scene::viewport_depth +
                                           viewport * sizeof(std::uint32_t),
                                       row.depth_bits);
            }
            rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::current_model,
                                   row.current_model);
            rr64::engine::read_u16(rdram, node + rr64::engine::actor_scene::selected_lod,
                                   row.selected_lod);
            rr64::engine::read_u16(rdram, node + rr64::engine::actor_scene::previous_lod,
                                   row.previous_lod);
            if (row.current_model != 0u) {
                rr64::engine::read_u16(rdram, row.current_model + 0x0Au, row.model_flags);
            }
            for (std::uint32_t lod = 0; lod < rr64::engine::actor_scene::lod_record_count; ++lod) {
                rr64::engine::read_u32(rdram,
                                       node + rr64::engine::actor_scene::lod_models +
                                           lod * rr64::engine::actor_scene::lod_pointer_stride,
                                       row.models[lod]);
                rr64::engine::read_u32(rdram,
                                       node + rr64::engine::actor_scene::display_lists +
                                           lod * rr64::engine::actor_scene::lod_pointer_stride,
                                       row.displays[lod]);
                rr64::engine::read_u16(rdram,
                                       node + rr64::engine::actor_scene::segment_counts +
                                           lod * sizeof(std::uint16_t),
                                       row.segments[lod]);
            }
            if (viewport < rr64::engine::actor_scene::maximum_viewports &&
                buffer_slot < rr64::engine::actor_scene::render_buffer_slot_count) {
                rr64::engine::read_u32(
                    rdram,
                    node + rr64::engine::actor_scene::render_transform_buffers +
                        viewport * rr64::engine::actor_scene::render_buffer_viewport_stride +
                        buffer_slot * rr64::engine::actor_scene::render_buffer_slot_stride,
                    row.transform_buffer);
            }
            for (std::size_t transaction = 0; transaction < g_actor_presentation_transaction_count;
                 ++transaction) {
                if (g_actor_presentation_transactions[transaction].node == node) {
                    row.transaction_active = true;
                    break;
                }
            }

            ++actor_count;
            mix_actor_render_hash(actor_hash, row.node);
            mix_actor_render_hash(actor_hash, row.node_type);
            mix_actor_render_hash(actor_hash, row.entity);
            mix_actor_render_hash(actor_hash, row.current_model);
            mix_actor_render_hash(actor_hash, row.model_flags);
            mix_actor_render_hash(actor_hash, row.selected_lod);
            mix_actor_render_hash(actor_hash, row.previous_lod);
            // The game alternates transform buffers every presentation. Keep
            // that pointer in each emitted row for diagnosis, but do not treat
            // the normal double-buffer flip as an actor-state transition.
            mix_actor_render_hash(actor_hash, row.transaction_active ? 1u : 0u);
            for (std::uint32_t lod = 0; lod < rr64::engine::actor_scene::lod_record_count; ++lod) {
                mix_actor_render_hash(actor_hash, row.models[lod]);
                mix_actor_render_hash(actor_hash, row.displays[lod]);
                mix_actor_render_hash(actor_hash, row.segments[lod]);
            }
            if (row_count < rows.size()) {
                rows[row_count++] = row;
            }
        }

        if (next == node) {
            mix_actor_render_hash(actor_hash, 0xBAD20000u | visited);
            break;
        }
        node = next;
    }
    mix_actor_render_hash(actor_hash, actor_count);

    std::lock_guard trace_lock{g_actor_presentation_trace_mutex};
    ActorRenderListTraceState *state = nullptr;
    for (auto &candidate : g_actor_render_list_trace_states) {
        if (candidate.occupied && candidate.renderer == renderer &&
            candidate.list_head == list_head) {
            state = &candidate;
            break;
        }
        if (state == nullptr && !candidate.occupied) {
            state = &candidate;
        }
    }
    if (state != nullptr && state->occupied && state->actor_hash == actor_hash) {
        return;
    }
    if (state != nullptr) {
        *state = ActorRenderListTraceState{renderer, list_head, actor_hash, true};
    }

    const std::uint64_t sequence = ++g_actor_render_sequence;
    std::fprintf(file,
                 "1,%llu,list,%u,0x%08X,0x00000000,0,%u,0x%016llX,"
                 "0x00000000,0x00000000,0x00000000,0x0000,0,0,"
                 "0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,"
                 "0x00000000,0,0,0,0x00000000,0\n",
                 static_cast<unsigned long long>(sequence), renderer, list_head, actor_count,
                 static_cast<unsigned long long>(actor_hash));
    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
        const auto &row = rows[row_index];
        std::fprintf(
            file,
            "1,%llu,actor,%u,0x%08X,0x%08X,%u,%u,0x%016llX,"
            "0x%08X,0x%08X,0x%08X,0x%04X,%u,%u,"
            "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,"
            "%u,%u,%u,0x%08X,%u\n",
            static_cast<unsigned long long>(sequence), renderer, list_head, row.node, row.node_type,
            actor_count, static_cast<unsigned long long>(actor_hash), row.entity, row.depth_bits,
            row.current_model, static_cast<unsigned>(row.model_flags),
            static_cast<unsigned>(row.selected_lod), static_cast<unsigned>(row.previous_lod),
            row.models[0], row.models[1], row.models[2], row.displays[0], row.displays[1],
            row.displays[2], static_cast<unsigned>(row.segments[0]),
            static_cast<unsigned>(row.segments[1]), static_cast<unsigned>(row.segments[2]),
            row.transform_buffer, row.transaction_active ? 1u : 0u);
    }
    std::fflush(file);
}

extern "C" unsigned int rr64_actor_select_render_lod(unsigned char *rdram, unsigned int node,
                                                     unsigned int selected_lod) {
    if (rdram == nullptr || node == 0u ||
        selected_lod >= rr64::engine::actor_scene::lod_record_count ||
        rr64_is_high_detail_actors_enabled() == 0) {
        return selected_lod;
    }

    bool transaction_active = false;
    bool pair_validated = false;
    std::uint16_t stock_lod = static_cast<std::uint16_t>(selected_lod);
    std::uint32_t peer_node = 0;
    rr64::engine::ActorPresentationDecision transaction_decision{};
    {
        std::lock_guard lock{g_actor_presentation_transaction_mutex};
        if (rdram == g_actor_presentation_rdram) {
            for (std::size_t index = 0; index < g_actor_presentation_transaction_count; ++index) {
                const auto &transaction = g_actor_presentation_transactions[index];
                if (transaction.node == node) {
                    transaction_active = true;
                    stock_lod = transaction.stock_lod;
                    transaction_decision = transaction.decision;
                    break;
                }
            }
            pair_validated = find_validated_actor_pair_locked(rdram, node, peer_node);
        }
    }

    // A naturally selected tier-zero actor already has fully coherent guest
    // state. No native ownership record is needed for that stock path.
    if (!transaction_active && selected_lod == 0u) {
        return 0u;
    }

    const std::uint32_t safe_fallback_lod =
        transaction_active ? static_cast<std::uint32_t>(stock_lod) : selected_lod;
    if (!pair_validated || !tier_zero_pose_graph_is_prepared(rdram, node) ||
        !tier_zero_pose_graph_is_prepared(rdram, peer_node)) {
        return safe_fallback_lod;
    }

    if (transaction_active) {
        return transaction_decision.reason ==
                       rr64::engine::ActorPresentationDecisionReason::TierZeroGraphSubstitution
                   ? 0u
                   : safe_fallback_lod;
    }

    // The transaction has already restored the guest node by this point on
    // the deferred renderer path. Re-evaluate its current stock tier, but mark
    // the pair as authoritative so AI racers are not mistaken for inactive
    // local-controller slots. This is a pure selector and performs no guest
    // writes: one local register drives the model graph, display list, segment
    // table, transform count, and pose sources together inside func_80011CC0.
    rr64::engine::ActorPresentationDecision decision{};
    if (!rr64::engine::choose_actor_presentation_lod(rdram, node, stock_lod, true, decision,
                                                     true) ||
        decision.reason !=
            rr64::engine::ActorPresentationDecisionReason::TierZeroGraphSubstitution) {
        return safe_fallback_lod;
    }
    return 0u;
}

extern "C" int rr64_actor_select_render_pose_source(unsigned char *rdram, unsigned int node,
                                                    unsigned int selected_lod,
                                                    unsigned int model_record,
                                                    unsigned int original_pose_source) {
    (void)rdram;
    (void)node;
    (void)selected_lod;
    (void)model_record;

    // The preparation transaction already exposes tier zero before the game's
    // original rider/bike pose-update passes run. Those passes therefore
    // refresh the complete detailed skeleton and suspension data in their own
    // pose blocks. Far tiers are not alternate representations of that data:
    // distant riders carry identity child poses and the far bike collapses
    // three independently positioned detailed parts into one transform.
    // Preserve every tier-zero pose source rather than cross-wiring those
    // incompatible records. Keep the signed return form required by generated
    // MIPS memory access for guest addresses in the 0x80xxxxxx range.
    return static_cast<std::int32_t>(original_pose_source);
}

extern "C" void
rr64_actor_trace_rendered_matrix(unsigned char *rdram, unsigned int node, unsigned int selected_lod,
                                 unsigned int model_record, unsigned int source_matrix,
                                 unsigned int rendered_matrix, unsigned int transform_buffer,
                                 unsigned int render_flag) {
    FILE *file = actor_rendered_matrix_trace_file();
    if (file == nullptr || rdram == nullptr) {
        return;
    }

    std::uint16_t stock_lod = 0;
    std::uint16_t presentation_transform_count = 0;
    std::uint16_t presentation_record_index = std::numeric_limits<std::uint16_t>::max();
    {
        // Use the transaction snapshot rather than the temporarily rewritten
        // node field. It records which distance tier the stock selector chose
        // before tier zero was exposed to the pose-update and render passes.
        std::lock_guard transaction_lock{g_actor_presentation_transaction_mutex};
        bool active_transaction = false;
        for (std::size_t index = 0; index < g_actor_presentation_transaction_count; ++index) {
            const auto &transaction = g_actor_presentation_transactions[index];
            if (transaction.node == node) {
                stock_lod = transaction.stock_lod;
                presentation_transform_count = transaction.decision.presentation_transform_count;
                for (std::uint16_t record_index = 0;
                     record_index < transaction.presentation_record_count; ++record_index) {
                    if (transaction.presentation_records[record_index] == model_record) {
                        presentation_record_index = record_index;
                        break;
                    }
                }
                active_transaction = true;
                break;
            }
        }
        if (!active_transaction || stock_lod == 0u) {
            return;
        }
    }

    // Resolve the buffer from the node at the same viewport/slot used by the
    // stock renderer. A type-0x13 root copies its first two matrices directly,
    // so the first matrix-builder callback is index two rather than zero.
    // Sampling by record zero below captures that real traversal boundary.
    std::uint32_t viewport = 0;
    std::uint32_t buffer_slot = 0;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::active_viewport, viewport) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::actor_render_buffer_slot,
                                buffer_slot) ||
        viewport >= rr64::engine::actor_scene::maximum_viewports ||
        buffer_slot >= rr64::engine::actor_scene::render_buffer_slot_count ||
        !rr64::engine::read_u32(
            rdram,
            node + rr64::engine::actor_scene::render_transform_buffers +
                viewport * rr64::engine::actor_scene::render_buffer_viewport_stride +
                buffer_slot * rr64::engine::actor_scene::render_buffer_slot_stride,
            transform_buffer) ||
        rendered_matrix < transform_buffer) {
        return;
    }
    const std::uint32_t transform_delta = rendered_matrix - transform_buffer;
    if ((transform_delta % rr64::engine::actor_scene::render_transform_size) != 0u) {
        return;
    }
    const std::uint32_t transform_index =
        transform_delta / rr64::engine::actor_scene::render_transform_size;
    if (transform_index >= rr64::engine::actor_scene::maximum_render_transforms) {
        return;
    }

    std::array<float, 16> source{};
    for (std::uint32_t component = 0; component < source.size(); ++component) {
        if (!rr64::engine::read_float(rdram, source_matrix + component * sizeof(float),
                                      source[component])) {
            return;
        }
    }
    rr64::engine::Matrix4x4Snapshot fixed{};
    if (!rr64::engine::decode_n64_matrix(rdram, rendered_matrix, fixed)) {
        return;
    }

    std::uint32_t node_type = 0;
    std::uint32_t entity = 0;
    std::uint32_t depth_bits = 0;
    rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::type, node_type);
    rr64::engine::read_u32(rdram, node + rr64::engine::actor_scene::entity, entity);
    if (rr64::engine::read_u32(rdram, rr64::engine::globals::active_viewport, viewport) &&
        viewport < rr64::engine::actor_scene::maximum_viewports) {
        rr64::engine::read_u32(rdram,
                               node + rr64::engine::actor_scene::viewport_depth +
                                   viewport * sizeof(std::uint32_t),
                               depth_bits);
    }

    std::array<float, 9> bike_positions{};
    if (node_type == 1u) {
        const std::array<std::uint32_t, 3> offsets{
            rr64::engine::bike::front_wheel_position,
            rr64::engine::bike::body_position,
            rr64::engine::bike::rear_wheel_position,
        };
        for (std::uint32_t position = 0; position < offsets.size(); ++position) {
            for (std::uint32_t component = 0; component < 3u; ++component) {
                rr64::engine::read_float(rdram,
                                         entity + offsets[position] + component * sizeof(float),
                                         bike_positions[position * 3u + component]);
            }
        }
    }

    std::lock_guard trace_lock{g_actor_presentation_trace_mutex};
    ActorRenderedMatrixTraceState *state = nullptr;
    for (auto &candidate : g_actor_rendered_matrix_trace_states) {
        if (candidate.node == node) {
            state = &candidate;
            break;
        }
        if (state == nullptr && candidate.node == 0u) {
            state = &candidate;
        }
    }
    if (state == nullptr) {
        return;
    }
    if (state->node == 0u) {
        state->node = node;
    }
    if (presentation_record_index == 0u) {
        ++state->render_count;
        // Two to four complete samples per second are sufficient to catch the
        // persistent far-distance drop without producing multi-gigabyte logs.
        state->capture_current_render =
            state->render_count == 1u || (state->render_count % 15u) == 0u;
    }
    if (!state->capture_current_render) {
        return;
    }

    const std::uint64_t sequence = ++g_actor_rendered_matrix_sequence;
    std::fprintf(file,
                 "1,%llu,0x%08X,%u,0x%08X,%u,%u,0x%08X,0x%08X,0x%08X,"
                 "0x%08X,%u,%u,0x%08X",
                 static_cast<unsigned long long>(sequence), node, node_type, entity,
                 static_cast<unsigned>(stock_lod), selected_lod, model_record, source_matrix,
                 rendered_matrix, transform_buffer, transform_index, render_flag, depth_bits);
    for (const float value : source) {
        std::fprintf(file, ",%.9g", value);
    }
    for (const float value : fixed.values) {
        std::fprintf(file, ",%.9g", value);
    }
    for (const float value : bike_positions) {
        std::fprintf(file, ",%.9g", value);
    }
    std::fputc('\n', file);
    if (transform_index + 1u >= presentation_transform_count) {
        std::fflush(file);
    }
}

extern "C" void rr64_actor_begin_presentation_pair(unsigned char *rdram, unsigned int bike_node,
                                                   unsigned int rider_node) {
    // func_8005D9A4 is called from both the main update thread and the race
    // render worker. Only the two explicitly bounded presentation producers may
    // expose a different model graph. This thread-local guard prevents render
    // state from entering physics, AI, collision, or the next frame's terrain-
    // contact update.
    if (!g_actor_presentation_scope_active || g_actor_presentation_scope_rdram != rdram) {
        return;
    }

    constexpr std::size_t pair_size = 2u;
    const std::array<std::uint32_t, pair_size> nodes{bike_node, rider_node};
    std::array<ActorPresentationTransaction, pair_size> pending{};
    std::size_t pending_count = 0;
    const bool pair_valid =
        rr64::engine::valid_actor_presentation_pair(rdram, bike_node, rider_node);
    const bool high_detail_enabled = rr64_is_high_detail_actors_enabled() != 0;

    for (const std::uint32_t node : nodes) {
        std::uint16_t stock_lod = 0;
        rr64::engine::ActorPresentationDecision decision{};
        if (!rr64::engine::read_u16(rdram, node + rr64::engine::actor_scene::selected_lod,
                                    stock_lod)) {
            return;
        }
        rr64::engine::choose_actor_presentation_lod(rdram, node, stock_lod, high_detail_enabled,
                                                    decision, pair_valid);
        trace_actor_presentation(node, decision);

        // Keep the paired actor coherent: if either half cannot present tier
        // zero, neither half is changed. A stock tier-zero half needs no
        // transaction, while a substituted half is snapshotted below.
        if (!pair_valid ||
            (decision.reason != rr64::engine::ActorPresentationDecisionReason::StockTierZero &&
             decision.reason !=
                 rr64::engine::ActorPresentationDecisionReason::TierZeroGraphSubstitution)) {
            return;
        }
        if (decision.reason == rr64::engine::ActorPresentationDecisionReason::StockTierZero) {
            continue;
        }

        std::uint16_t stock_previous_lod = 0;
        if (decision.presentation_model == 0u ||
            !rr64::engine::read_u16(rdram, node + rr64::engine::actor_scene::previous_lod,
                                    stock_previous_lod)) {
            return;
        }
        pending[pending_count].node = node;
        pending[pending_count].stock_model = decision.current_model;
        pending[pending_count].stock_lod = stock_lod;
        pending[pending_count].stock_previous_lod = stock_previous_lod;
        pending[pending_count].decision = decision;
        if (!populate_presentation_record_map(rdram, pending[pending_count])) {
            return;
        }
        ++pending_count;
    }

    if (pending_count == 0u) {
        return;
    }

    std::lock_guard lock{g_actor_presentation_transaction_mutex};
    if (g_actor_presentation_rdram != nullptr && g_actor_presentation_rdram != rdram) {
        // A replacement RDRAM mapping cannot safely receive writes intended
        // for the old mapping. Discard stale native bookkeeping; the normal
        // frame-start restore hook prevents this in a live game.
        g_actor_presentation_transaction_count = 0;
        g_validated_actor_node_count = 0;
        g_validated_actor_node_replacement = 0;
    }
    g_actor_presentation_rdram = rdram;

    if (pending_count >
        g_actor_presentation_transactions.size() - g_actor_presentation_transaction_count) {
        return;
    }
    for (std::size_t pending_index = 0; pending_index < pending_count; ++pending_index) {
        for (std::size_t active_index = 0; active_index < g_actor_presentation_transaction_count;
             ++active_index) {
            if (g_actor_presentation_transactions[active_index].node ==
                pending[pending_index].node) {
                return;
            }
        }
    }

    std::size_t written_count = 0;
    for (; written_count < pending_count; ++written_count) {
        const auto &transaction = pending[written_count];
        const bool model_written = rr64::engine::write_u32(
            rdram, transaction.node + rr64::engine::actor_scene::current_model,
            transaction.decision.presentation_model);
        const bool lod_written =
            model_written &&
            rr64::engine::write_u16(rdram,
                                    transaction.node + rr64::engine::actor_scene::selected_lod, 0u);
        // Keep the stock selector's previous tier visible to the original
        // presentation updater.  Writing 0 here made a forced tier-zero actor
        // look as though it had already been detailed for the preceding
        // frame.  The rider animation pass then took its far-distance skip
        // path and left the tier-zero child poses as identities, collapsing
        // all detailed limbs onto the actor root.  Preserving the previous
        // tier makes this a real stock-style LOD transition while the bounded
        // presentation scope is active; the exact guest history is still
        // restored when the scope closes.
        const bool previous_lod_written =
            lod_written && rr64::engine::write_u16(
                               rdram, transaction.node + rr64::engine::actor_scene::previous_lod,
                               transaction.stock_previous_lod);
        if (!previous_lod_written) {
            rr64::engine::write_u32(rdram,
                                    transaction.node + rr64::engine::actor_scene::current_model,
                                    transaction.stock_model);
            rr64::engine::write_u16(rdram,
                                    transaction.node + rr64::engine::actor_scene::selected_lod,
                                    transaction.stock_lod);
            rr64::engine::write_u16(rdram,
                                    transaction.node + rr64::engine::actor_scene::previous_lod,
                                    transaction.stock_previous_lod);
            break;
        }
    }

    if (written_count != pending_count) {
        while (written_count != 0u) {
            const auto &transaction = pending[--written_count];
            rr64::engine::write_u32(rdram,
                                    transaction.node + rr64::engine::actor_scene::current_model,
                                    transaction.stock_model);
            rr64::engine::write_u16(rdram,
                                    transaction.node + rr64::engine::actor_scene::selected_lod,
                                    transaction.stock_lod);
            rr64::engine::write_u16(rdram,
                                    transaction.node + rr64::engine::actor_scene::previous_lod,
                                    transaction.stock_previous_lod);
        }
        return;
    }

    for (std::size_t index = 0; index < pending_count; ++index) {
        g_actor_presentation_transactions[g_actor_presentation_transaction_count++] =
            pending[index];
    }
    remember_validated_actor_node_locked(rdram, bike_node, rider_node);
    remember_validated_actor_node_locked(rdram, rider_node, bike_node);
}

extern "C" int rr64_actor_presentation_transaction_active(unsigned char *rdram, unsigned int node) {
    if (!g_actor_presentation_scope_active || g_actor_presentation_scope_rdram != rdram ||
        node == 0u) {
        return 0;
    }

    std::lock_guard lock{g_actor_presentation_transaction_mutex};
    if (rdram != g_actor_presentation_rdram) {
        return 0;
    }
    for (std::size_t index = 0; index < g_actor_presentation_transaction_count; ++index) {
        if (g_actor_presentation_transactions[index].node == node) {
            return 1;
        }
    }
    return 0;
}

extern "C" void rr64_actor_begin_presentation_scope(unsigned char *rdram) {
    // Presentation preparation and race draw are not recursive. If a future
    // path enters twice, close the earlier scope conservatively so no guest
    // model pointer can leak across an ownership boundary.
    if (g_actor_presentation_scope_active && g_actor_presentation_scope_rdram != nullptr) {
        rr64_actor_restore_presentation_transactions(g_actor_presentation_scope_rdram);
    } else if (rdram != nullptr) {
        // Clear any interrupted transaction left by an aborted prior draw.
        rr64_actor_restore_presentation_transactions(rdram);
    }

    g_actor_presentation_scope_rdram = rdram;
    g_actor_presentation_scope_active = rdram != nullptr;
}

extern "C" void rr64_actor_end_presentation_scope(unsigned char *rdram) {
    if (!g_actor_presentation_scope_active) {
        return;
    }

    unsigned char *const scope_rdram = g_actor_presentation_scope_rdram;
    g_actor_presentation_scope_active = false;
    g_actor_presentation_scope_rdram = nullptr;

    // The callers end this either after the post-physics preparation tail or
    // before func_8006A638 returns to the render worker. Both boundaries leave
    // generated matrices intact while restoring guest model/LOD ownership
    // before any subsequent simulation can observe the temporary graph.
    if (scope_rdram != nullptr) {
        (void)rdram;
        rr64_actor_restore_presentation_transactions(scope_rdram);
    }
}

extern "C" void rr64_actor_restore_presentation_transactions(unsigned char *rdram) {
    std::lock_guard lock{g_actor_presentation_transaction_mutex};
    if (rdram == nullptr || rdram != g_actor_presentation_rdram) {
        g_actor_presentation_transaction_count = 0;
        g_validated_actor_node_count = 0;
        g_validated_actor_node_replacement = 0;
        g_actor_presentation_rdram = rdram;
        return;
    }

    // Reverse order mirrors acquisition and also keeps paired rider/bike nodes
    // coherent if this hook is ever interrupted during diagnostic shutdown.
    while (g_actor_presentation_transaction_count != 0u) {
        const auto &transaction =
            g_actor_presentation_transactions[--g_actor_presentation_transaction_count];
        trace_actor_graph_matrices(rdram, transaction.node, transaction.decision);
        rr64::engine::write_u32(rdram, transaction.node + rr64::engine::actor_scene::current_model,
                                transaction.stock_model);
        rr64::engine::write_u16(rdram, transaction.node + rr64::engine::actor_scene::selected_lod,
                                transaction.stock_lod);
        rr64::engine::write_u16(rdram, transaction.node + rr64::engine::actor_scene::previous_lod,
                                transaction.stock_previous_lod);
    }
}
