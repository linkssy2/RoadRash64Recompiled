#pragma once

#ifdef __cplusplus
namespace rr64::rival_engine {
void set_enabled(bool value) noexcept;
bool get_enabled() noexcept;
void set_volume_percent(double value) noexcept;
double get_volume_percent() noexcept;
}

extern "C" {
#endif
void rr64_rival_engine_frame(unsigned char *, void *);
void rr64_rival_engine_gate(unsigned char *, void *);
int rr64_rival_engine_gain(unsigned char *, void *);
void rr64_rival_engine_threshold(unsigned char *, void *);
void rr64_rival_engine_pitch(unsigned char *, void *);
void rr64_rival_engine_transition(unsigned char *, void *);
void rr64_rival_engine_mode(unsigned char *, void *, unsigned);
void rr64_rival_engine_recovery(unsigned char *, void *, unsigned);
void rr64_rival_engine_audio_reset();
void rr64_rival_engine_heap(unsigned char *, void *);
void rr64_rival_engine_audio_init(unsigned char *, void *);
unsigned rr64_rival_engine_effect_limit(unsigned);
void rr64_rival_engine_allocation_range(unsigned char *, void *, unsigned);
int rr64_rival_engine_allocate(unsigned char *, void *);
void rr64_rival_engine_no_steal(unsigned char *, void *);
void rr64_rival_engine_allocated(unsigned char *, void *);
void rr64_rival_engine_child(unsigned char *, void *, unsigned);
void rr64_rival_engine_child_adopt(unsigned char *, void *);
#ifdef __cplusplus
}
#endif
