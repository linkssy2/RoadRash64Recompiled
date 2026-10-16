#pragma once
#ifdef __cplusplus
#include <atomic>
namespace rr64::campaign_coop {
inline std::atomic_bool enabled{false};
inline std::atomic_int input_request{-1};
}
extern "C" {
#endif
void rr64_coop_main_menu(unsigned char *);
unsigned rr64_coop_mode(unsigned char *, unsigned mode);
void rr64_coop_selection_begin(unsigned char *);
void rr64_coop_selection_end(unsigned char *);
unsigned rr64_coop_bike_count(unsigned slot, unsigned original);
unsigned rr64_coop_bike_entry(unsigned char *, unsigned slot, unsigned original);
void rr64_coop_roster(unsigned char *);
void rr64_coop_riders(unsigned char *);
unsigned rr64_coop_result_actor(unsigned char *, unsigned original);
unsigned rr64_coop_result_state(unsigned char *, unsigned original);
int rr64_coop_spectating(void);
#ifdef __cplusplus
}
#endif
