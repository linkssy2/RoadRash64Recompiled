#pragma once
#ifdef __cplusplus
#include <array>
#include <cstdint>
namespace rr64::shadow_tags {
// Called with the recorder's existing ownership/recovery generations locked.
void begin(unsigned char *memory, unsigned round, unsigned views,
           const std::array<std::uint32_t, 14> &generations);
}
extern "C" {
#endif
void rr64_shadow_tags_reset(void);
void rr64_shadow_tags_before(unsigned char *memory, unsigned node, unsigned graph, unsigned part);
void rr64_shadow_tags_after(unsigned char *memory);
void rr64_shadow_tags_finalize(unsigned char *memory, unsigned submitted_words);
unsigned long long rr64_shadow_tags_count(void);
#ifdef __cplusplus
}
#endif
