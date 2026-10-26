#pragma once

// Reserved N64 button bit carried with input snapshots, consumed before native controls.
#ifdef __cplusplus
inline constexpr unsigned rr64_cop_weapon_trick_button = 0x0040;

extern "C" {
#endif
int rr64_custom_cop_roaming(unsigned char *, unsigned);
int rr64_custom_cop_can_recover(unsigned char *, unsigned);
// Canonical human cop identity, independent of mounted/crash timing.
int rr64_custom_cop_is_player(unsigned char *, unsigned);
int rr64_custom_cop_trick(unsigned char *, unsigned);
float rr64_custom_cop_win_age(unsigned char *);
void rr64_custom_cop_post(unsigned char *, void *, unsigned);
int rr64_police_spawn_allowed(unsigned char *, void *);
int rr64_police_waiting(unsigned char *, unsigned);
int rr64_custom_cop_pursuit(unsigned char *, unsigned);
void rr64_custom_cop_control(unsigned char *, void *);
int rr64_custom_cop_siren(unsigned char *, unsigned);
float rr64_custom_cop_cue(unsigned char *, unsigned);
void rr64_custom_cop_hud(unsigned char *, void *);
int rr64_custom_cop_enabled();
int rr64_custom_cop_active();
void rr64_custom_cop_notification(unsigned char *memory, unsigned stats, unsigned event,
                                  unsigned name);
void rr64_custom_cop_equipment(unsigned char *memory, unsigned actor);
void rr64_custom_cop_spawn(unsigned char *memory, unsigned slot, unsigned offsets);
void rr64_custom_cop_bust_message(unsigned char *memory, unsigned attacker, unsigned victim);

void rr64_custom_cop_begin(unsigned char *memory, int enabled);
void rr64_custom_cop_reset();
unsigned rr64_custom_cop_bike_count(unsigned stock);
unsigned rr64_custom_cop_bike_entry(unsigned char *memory, unsigned player, unsigned stock);
unsigned rr64_custom_cop_rider(unsigned char *memory, unsigned player, unsigned selected);
void rr64_custom_cop_roles(unsigned char *memory);
int rr64_custom_cop_arrest(unsigned char *memory, unsigned attacker, unsigned victim);
int rr64_custom_cop_finished(unsigned char *memory);
int rr64_custom_cop_can_start(unsigned char *memory);
int rr64_custom_cop_confirm(unsigned char *memory);
void rr64_custom_cop_selection_hint(unsigned char *memory, void *context);
#ifdef __cplusplus
}
#endif
