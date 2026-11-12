// Link-only application callbacks for the real RT64 matching fixture.
// Matching never enters renderer/presentation threads; abort if it does.
#include <cstdlib>
extern "C" {
void rr64_record_pipeline_stage(unsigned, unsigned long long) { std::abort(); }
int rr64_is_race_mode_active() { std::abort(); }
int rr64_is_race_presentation_active() { std::abort(); }
void rr64_record_geometry_compatibility(unsigned long long, unsigned, unsigned) { std::abort(); }
void rr64_record_owned_batch(unsigned long long, unsigned, unsigned, unsigned) { std::abort(); }
void rr64_record_interpolation_workload(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) { std::abort(); }
void rr64_record_pipeline_resources(unsigned long long, unsigned long long, unsigned long long,
    unsigned long long, unsigned long long, unsigned long long,
    unsigned long long, unsigned long long, unsigned long long, unsigned long long) { std::abort(); }
void rr64_record_present_batch(int, int) { std::abort(); }
void rr64_record_present_batch_mismatch(unsigned) { std::abort(); }
void rr64_record_owned_present(unsigned long long, unsigned long long, unsigned, unsigned) { std::abort(); }
void rr64_record_present_sequence(unsigned long long, unsigned long long, unsigned long long,
    unsigned long long, unsigned long long, unsigned long long, unsigned, unsigned, unsigned, unsigned) { std::abort(); }
void rr64_record_present_target_sample(unsigned long long, unsigned long long, unsigned long long,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned) { std::abort(); }
void rr64_record_present_interval(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) { std::abort(); }
}
