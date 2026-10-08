#pragma once
#include "rr64_ai_bike_selection.hpp"
#include "rr64_rider_skin_menu.hpp"
#include "rr64_rider_skin_render.hpp"
#include "rr64_campaign_completion.hpp"
#include "rr64_terrain_graphics_pool.hpp"
#include "rr64_campaign_bonus_save.hpp"
#include "rr64_roaming_route.hpp"
#include "rr64_race_end_trace.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_thrash_options.hpp"
#include "rr64_race_pack_menu.hpp"
#include "rr64_online_terrain.hpp"
#include "rr64_highlights.hpp"
#include "rr64_rival_engine.hpp"
#include "rr64_highlight_camera.hpp"
#include "rr64_highlight_render_boundary.hpp"
#include "rr64_shadow_tags.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_offline_modifiers_bikes.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#include "rr64_course_material.hpp"
#include "rr64_course_items.hpp"
#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_hud.hpp"
#include "rr64_mk64_item_material.hpp"
#include "rr64_course_pickups.hpp"
#include "rr64_course_walls.hpp"
#include "rr64_course_hazards.hpp"
#include "rr64_course_ai.hpp"
#include "rr64_course_boost.hpp"
#include "rr64_course_diagnostics.hpp"
#include "rr64_course_sky.hpp"
#endif

#ifdef __cplusplus
extern "C" {
#endif
void rr64_course_audio_step(unsigned char *);
void rr64_sky_queue_begin(unsigned char *);
void rr64_sky_queue_end(unsigned char *);
void rr64_hud_widgets_begin(unsigned char *);
void rr64_hud_widgets_end(unsigned char *);
void rr64_hud_label_begin(unsigned char *, unsigned);
void rr64_hud_label_end(unsigned char *);
void rr64_hud_widgets_clear(unsigned char *);
void rr64_hud_countdown_begin(unsigned char *);
void rr64_hud_countdown_end(unsigned char *);
void rr64_online_private_selection_begin(unsigned char *);
void rr64_online_private_selection_end(unsigned char *);
int rr64_online_selection_commit(unsigned char *);
void rr64_online_hud_begin(unsigned char *);
void rr64_online_hud_end(unsigned char *);
int rr64_online_wait_for_race(unsigned char *, unsigned);
int rr64_online_host_pause_active();
void rr64_online_seed_race(unsigned char *);

unsigned int rr64_lod_racer_view(unsigned char *, unsigned int, unsigned int);
void rr64_trace_guest_stage(const char *stage);
void rr64_lod_shadow_finish_hold(unsigned char *rdram, void *context);
unsigned int rr64_music_volume_update(unsigned int original, unsigned int sequence);
unsigned int rr64_music_stock_volume(unsigned int original);

void rr64_lod_release_node(unsigned char *rdram, unsigned int node);
void rr64_lod_reset_actor_pool(unsigned char *rdram);
unsigned int rr64_combined_video_callback(unsigned char *rdram, unsigned int original);
void rr64_refresh_viewport_dimensions(unsigned char *rdram, unsigned int layout);
void rr64_trace_guest_value(const char *stage, unsigned int value);
void rr64_engine_observe_frame(unsigned char *rdram);
void rr64_engine_capture_pre_update(unsigned char *rdram);
void rr64_engine_capture_post_update(unsigned char *rdram);
void rr64_engine_capture_dispatch_boundary(unsigned char *rdram, unsigned int boundary,
                                           unsigned int target);
void rr64_engine_capture_race_update_boundary(unsigned char *rdram, unsigned int boundary);
void rr64_engine_capture_dynamics_boundary(unsigned char *rdram, unsigned int function_id,
                                           unsigned int boundary, unsigned int actor_address);
void rr64_engine_capture_dynamics_auxiliary_boundary(
    unsigned char *rdram, unsigned int function_id, unsigned int boundary,
    unsigned int actor_address, unsigned int auxiliary_0, unsigned int auxiliary_1,
    unsigned int auxiliary_2, unsigned int auxiliary_3, unsigned int auxiliary_4);
void rr64_engine_capture_dynamics_auxiliary8_boundary(
    unsigned char *rdram, unsigned int function_id, unsigned int boundary,
    unsigned int actor_address, unsigned int auxiliary_0, unsigned int auxiliary_1,
    unsigned int auxiliary_2, unsigned int auxiliary_3, unsigned int auxiliary_4,
    unsigned int auxiliary_5, unsigned int auxiliary_6, unsigned int auxiliary_7);
void rr64_engine_capture_actor_presentation(unsigned char *rdram, unsigned int viewport);
void rr64_set_high_detail_actors_enabled(int enabled);
int rr64_is_high_detail_actors_enabled();
// Isolated R1 presentation candidate. These hooks never invoke the legacy
// real-RDRAM actor graph transaction. All preparation executes in a clone.
int rr64_render_only_max_lod_enabled();
void rr64_lod_read_stats(unsigned long long *preparations, unsigned long long *published_pairs,
                         unsigned long long *consumed_actors, unsigned long long *fallback_actors);
void rr64_lod_observe_allocation(unsigned char *rdram, unsigned int node, unsigned int viewport,
                                 unsigned int bytes);
void rr64_lod_invalidate(unsigned char *rdram);
void rr64_lod_begin_preparation(unsigned char *rdram);
void rr64_lod_observe_pair(unsigned char *rdram, void *context);
void rr64_lod_observe_recording_pairs(unsigned char *rdram, void *context);
void rr64_lod_prepare_shadow(unsigned char *rdram, void *context, int direct_order);
int rr64_lod_shadow_rider(unsigned char *rdram, unsigned int node);
void rr64_lod_shadow_stage(unsigned char *rdram, unsigned int node, unsigned int stage);
void rr64_lod_shadow_full_weight(unsigned char *rdram, void *context, int completed);
void rr64_lod_begin_draw(unsigned char *rdram);
void rr64_lod_end_draw(unsigned char *rdram);
unsigned int rr64_lod_select(unsigned char *rdram, unsigned int node, unsigned int stock_lod);
void rr64_lod_observe_rider_range(unsigned char *rdram, unsigned int node, int in_range);
unsigned int rr64_lod_actor_hidden(unsigned char *rdram, unsigned int node,
                                   unsigned int original_hidden);
unsigned int rr64_lod_root_source(unsigned char *rdram, unsigned int node, unsigned int record,
                                  unsigned int original_source);
void rr64_lod_scale_root_matrix(unsigned char *rdram, unsigned int node, unsigned int record,
                                unsigned int matrix_address);
void rr64_lod_end_actor();
int rr64_world_distance_enabled();
void rr64_world_invalidate(unsigned char *rdram);
void rr64_world_observe_allocation(unsigned char *rdram, unsigned node, unsigned view,
                                   unsigned bytes);
void rr64_world_observe_roots(unsigned char *rdram, unsigned type);
void rr64_weapon_begin(unsigned char *, unsigned, unsigned);
int rr64_lod_weapon_root(unsigned char *, unsigned, unsigned *, unsigned *);
void rr64_weapon_alt_begin(unsigned char *, unsigned, unsigned);
unsigned long long rr64_weapon_counter(unsigned);
void rr64_weapon_end();
void rr64_weapon_source(unsigned char *, void *, unsigned);
void rr64_weapon_matrix(unsigned char *, unsigned, unsigned);
void rr64_weapon_packed(unsigned char *, unsigned, unsigned);
void rr64_world_begin_draw(unsigned char *rdram);
void rr64_world_end_draw(unsigned char *rdram);
unsigned rr64_world_actor_hidden(unsigned char *rdram, unsigned node, unsigned hidden,
                                 const void *context);
unsigned rr64_world_select(unsigned char *rdram, unsigned node, unsigned stock_lod);
unsigned rr64_world_root_source(unsigned char *rdram, unsigned node, unsigned record,
                                unsigned source);
void rr64_world_scale_root_matrix(unsigned char *rdram, unsigned node, unsigned record,
                                  unsigned matrix);
void rr64_world_end_actor();
void rr64_world_camera_far(unsigned char *rdram, void *context);
int rr64_world_camera_open_gap(unsigned char *rdram, void *context);
void rr64_world_camera_normalization(unsigned char *rdram, void *context);
void rr64_world_terrain_begin(unsigned char *rdram);
unsigned rr64_world_terrain_stock_state(unsigned char *rdram, unsigned record, unsigned state);
void rr64_world_terrain_observe(unsigned char *rdram, unsigned record);
void rr64_world_terrain_draw(unsigned char *rdram);
void rr64_world_terrain_finalize(unsigned char *rdram, unsigned submitted_words);
void rr64_world_objects_begin(unsigned char *rdram);
void rr64_world_objects_observe(unsigned char *rdram, unsigned placement);
void rr64_world_objects_sample(unsigned char *rdram, unsigned placement, unsigned graph);
void rr64_world_objects_draw(unsigned char *rdram);
void rr64_actor_begin_presentation_scope(unsigned char *rdram);
void rr64_actor_end_presentation_scope(unsigned char *rdram);
void rr64_actor_begin_presentation_pair(unsigned char *rdram, unsigned int bike_node,
                                        unsigned int rider_node);
int rr64_actor_presentation_transaction_active(unsigned char *rdram, unsigned int node);
void rr64_actor_trace_render_list(unsigned char *rdram, unsigned int list_head,
                                  unsigned int renderer);
void rr64_actor_trace_pose_bindings(unsigned char *rdram, unsigned int stage);
unsigned int rr64_actor_select_render_lod(unsigned char *rdram, unsigned int node,
                                          unsigned int selected_lod);
int rr64_actor_select_render_pose_source(unsigned char *rdram, unsigned int node,
                                         unsigned int selected_lod, unsigned int model_record,
                                         unsigned int original_pose_source);
void rr64_actor_trace_rendered_matrix(unsigned char *rdram, unsigned int node,
                                      unsigned int selected_lod, unsigned int model_record,
                                      unsigned int source_matrix, unsigned int rendered_matrix,
                                      unsigned int transform_buffer, unsigned int render_flag);
void rr64_actor_restore_presentation_transactions(unsigned char *rdram);
int rr64_engine_contract_has_warning();
void rr64_trace_race_frame(unsigned char *rdram, void *context, unsigned int mode,
                           unsigned int pending_mode, unsigned int pause_state,
                           unsigned int physics_delta_bits, unsigned int update_ticks_bits,
                           unsigned int wait_ticks_bits, unsigned int total_ticks_bits);
int rr64_is_live_race_mode(unsigned int mode);
int rr64_is_race_mode_active();
// Published race scene includes the results/highlight handoff, unlike live simulation.
int rr64_is_race_presentation_active();
int rr64_is_gameplay_feedback_active();
int rr64_are_gameplay_shortcuts_active();
int rr64_is_road_rumble_allowed();
void rr64_set_rumble_enabled(int enabled);
int rr64_is_rumble_enabled();
void rr64_request_rider_eject(unsigned int slot);
int rr64_traffic_within_draw_distance(unsigned char*, unsigned int, unsigned int, unsigned int, int);
unsigned int rr64_traffic_spawn_distance(unsigned char*, unsigned int);
int rr64_local_rider_has_fists_selected(unsigned slot);
void rr64_custom_cop_ai_pool(unsigned char *memory, void *context);
unsigned int rr64_audio_timeline_epoch();
void rr64_set_maximum_view_distance_enabled(int enabled);
int rr64_is_maximum_view_distance_enabled();
unsigned int rr64_traffic_render_visibility(unsigned char *rdram, unsigned int node,
                                            unsigned int stock_hidden);
unsigned int rr64_terrain_unload_decision(unsigned char *rdram, unsigned int cell,
                                          unsigned int stock_unload);
void rr64_render_resident_terrain(unsigned char *rdram, void *context);
void rr64_trace_terrain_scene(unsigned char *rdram);
void rr64_trace_lod_node(unsigned char *rdram, unsigned int kind, unsigned int node);
unsigned int rr64_online_menu_route_mode(unsigned int requested_mode);
void rr64_online_menu_mode_changed(unsigned int requested_mode);
unsigned rr64_online_postrace_route_mode(unsigned char *rdram,unsigned requested_mode);
int rr64_online_postrace_update(unsigned char *rdram,unsigned mode);
int rr64_online_postrace_wait_for_setup();
unsigned int rr64_online_bike_profile(unsigned char* rdram, unsigned int racer, unsigned int profile);
void rr64_online_bike_profiles_reset();
void rr64_online_main_menu_draw(unsigned char *memory, void *context);
void rr64_online_ready_draw(unsigned char *memory, void *context);
void rr64_online_disconnect_draw(unsigned char *memory, void *context);
void rr64_online_pause_owner(unsigned char *memory);
unsigned rr64_online_player_count_label(unsigned original);
unsigned rr64_online_race_choice(unsigned guest, unsigned original, unsigned bike);
unsigned rr64_online_graphics_layout(unsigned original);
unsigned rr64_online_graphics_viewport(unsigned char *memory, unsigned original);
void rr64_online_graphics_viewport_end(unsigned char *memory);
unsigned rr64_online_render_loop_continue(unsigned original);
void rr64_online_render_begin(unsigned char *memory);
void rr64_online_logical_viewports(unsigned char *memory);
void rr64_online_render_end(unsigned char *memory);
void rr64_online_menu_apply_pending_guest_input(unsigned char *rdram);
void rr64_online_game_setup_before_update(unsigned char *rdram);
void rr64_online_game_setup_after_update(unsigned char *rdram);
unsigned int rr64_online_requested_racer_count(unsigned int original_count);
void rr64_restore_manual_eject_health(unsigned char *, unsigned);
int rr64_local_player_roaming(unsigned char *, unsigned);


int rr64_local_options_input(unsigned char *memory);
void rr64_local_options_menu_begin(unsigned char *memory);
void rr64_local_options_reset_race(void);
int rr64_local_options_navigation(unsigned char *memory);
unsigned int rr64_local_options_table(unsigned int stock);
unsigned int rr64_local_options_visible(unsigned int row, unsigned int stock);
void rr64_local_options_text(unsigned char *memory, unsigned int row, unsigned int buffer);
void rr64_local_options_finish(unsigned char *memory);
void rr64_character_restore(unsigned char *memory, unsigned int slot, unsigned int multiplayer);
void rr64_character_remember(unsigned char *memory, unsigned int slot);
void rr64_character_remember_multiplayer(unsigned char *memory);
void rr64_character_new_campaign(unsigned char *memory);
void rr64_local_options_race(unsigned char *memory);
void rr64_local_options_thrash_roster(unsigned char *memory);
void rr64_local_options_thrash_race(unsigned char *memory);
unsigned int rr64_local_bike_level(unsigned int original);
unsigned int rr64_local_bike_menu_level(unsigned int original);
void rr64_local_bike_ai_pool(unsigned char *memory, void *context);
unsigned int rr64_online_prepare_render_layout(unsigned int stock_layout);
unsigned int rr64_online_render_first_viewport(unsigned int stock_viewport);
unsigned int rr64_online_render_geometry_viewport(unsigned int stock_viewport);
void rr64_online_restore_active_viewport(unsigned char *rdram, unsigned int stock_viewport);
void rr64_online_race_sync_before_update(unsigned char *rdram, unsigned int mode);
void rr64_online_race_sync_after_update(unsigned char *rdram, unsigned int mode);
void rr64_trace_guest_input(unsigned int mode, unsigned int active_mask, unsigned int buttons,
                            unsigned int stick_x_byte, unsigned int stick_y_byte,
                            unsigned int error_0, unsigned int error_1, unsigned int error_2,
                            unsigned int error_3);
void rr64_autotest_input(unsigned char *rdram, unsigned int mode);
void rr64_combat_impact_rumble(unsigned char *rdram, unsigned int first_bike,
                               unsigned int second_bike, unsigned int strength_percent);
enum {
    RR64_PROMPT_BUTTON_A = 0,
    RR64_PROMPT_BUTTON_B = 1,
};
unsigned int rr64_write_button_prompt(unsigned char *rdram, unsigned int logical_button,
                                      unsigned int destination, unsigned int capacity);
void rr64_name_entry_navigation(unsigned char *rdram, unsigned int column_count,
                                unsigned int maximum_row);
void rr64_profile_name_new_campaign(unsigned char *rdram);
void rr64_profile_name_thrash(unsigned char *rdram);
void rr64_online_apply_display_names(unsigned char *rdram);
void rr64_achievement_observe_frame(unsigned char *rdram);
void rr64_achievement_game_event(unsigned char *rdram, unsigned int event_id, unsigned int value,
                                 unsigned int actor);
void rr64_achievement_campaign_level_advanced(unsigned char *rdram, unsigned int new_level);
void rr64_achievement_campaign_completed(unsigned char *rdram);
#ifdef __cplusplus
}
#endif

void rr64_register_overlays();

#ifdef __cplusplus
extern "C" {
#endif
void rr64_online_sync_before_pose(unsigned char *rdram);
void rr64_online_audio_owner(unsigned char *rdram, void *context);
void rr64_online_audio_listener(unsigned char *rdram, void *context);
void rr64_online_audio_weapon_source(unsigned char *rdram, void *context, unsigned source_body);
void rr64_online_audio_weapon_gain(unsigned char *rdram, void *context);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
void rr64_online_traffic_prepare(unsigned char *rdram);
void rr64_online_traffic_finish(unsigned char *rdram,void *context);
int rr64_online_traffic_owned(void);
void rr64_online_traffic_model(void *context);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C"
#endif
void rr64_online_attack_pose_begin(unsigned char *rdram);
#ifdef __cplusplus
extern "C"
#endif
void rr64_online_attack_pose_end(unsigned char *rdram);

#ifdef __cplusplus
extern "C"
#endif
void rr64_online_presentation_matrix(unsigned char *,unsigned,unsigned,unsigned,unsigned);

#ifdef __cplusplus
extern "C"
#endif
unsigned rr64_online_hud_actor_pointer(unsigned char *,unsigned,unsigned);

#ifdef __cplusplus
extern "C" {
#endif
int rr64_online_hit(unsigned char*,void*,unsigned);
int rr64_valid_combat_statistics(unsigned int address);
void rr64_online_combat_drain(unsigned char*,void*);
int rr64_authority_step_begin(unsigned char*,void*);
int rr64_authority_translate(unsigned char*,void*);
void rr64_authority_step_finish(unsigned char*);
void rr64_authority_actor_route(void*,unsigned);
void rr64_prediction_verify_streaming(unsigned char*,void*);
void rr64_prediction_verify_random(unsigned char*,unsigned);
void rr64_prediction_flush_cases();
int rr64_prediction_run_case(const char*,const char*,int);
#ifdef __cplusplus
}
#endif
