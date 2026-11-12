#pragma once

#ifdef __cplusplus
#include "rr64_mk64_item_state.hpp"

namespace rr64::mk64_items {
// Input threads publish edges; only the native game thread consumes them.
bool input_active() noexcept;
void request_use(unsigned controller) noexcept;
unsigned take_action(unsigned controller) noexcept;
void stage_use(unsigned canonical_slot, int y) noexcept;
void reset_runtime() noexcept;
Snapshot capture_state() noexcept;
bool apply_state(const Snapshot &, std::uint32_t round, std::uint64_t tick) noexcept;
bool can_grant(unsigned canonical_slot) noexcept;
bool grant_item(unsigned canonical_slot, Item item) noexcept;
bool render_effect(unsigned char *memory, unsigned node, RiderState &effect,
                   unsigned &clock) noexcept;
// Current visible root, including mounted shrink and recorded playback.
bool render_rider_anchor(unsigned char *memory, unsigned canonical_slot, Vec &anchor) noexcept;
void scale_weapon_matrix(unsigned char *memory, unsigned node, unsigned record, unsigned matrix,
                         unsigned prepared_source) noexcept;
}
extern "C" {
#endif
void rr64_mk64_items_step(unsigned char *memory, void *context);
void rr64_mk64_items_before_physics(unsigned char *memory);
void rr64_mk64_items_mode(unsigned char *memory, unsigned mode);
void rr64_mk64_items_bike_contact(unsigned char *memory, unsigned first, unsigned second);
// These inspect a native actor/entity, never a user-facing controller index.
int rr64_mk64_items_immune(unsigned char *memory, unsigned entity);
int rr64_mk64_items_ghost(unsigned char *memory, unsigned entity);
void rr64_mk64_items_scale_matrix(unsigned char *memory, unsigned node, unsigned record,
                                  unsigned matrix);
void rr64_mk64_items_draw(unsigned char *memory);
#ifdef __cplusplus
}
#endif
