#pragma once

#ifdef __cplusplus
#include <cstdint>

namespace rr64::experimental_course {
// Private converted textures retain the native 0x40-byte header. The +0x38
// animation clock is unused when flags&4 is clear. Only validated single-frame,
// nonanimated RGBA16/IA16 records may use it for this explicitly tagged extension.
inline constexpr std::uint32_t material_tag = 0x4d4b0000u;
inline constexpr std::uint32_t material_alpha = 0x80u;
inline constexpr std::uint32_t material_alpha_depth_write = 0x100u;
inline constexpr std::uint32_t material_alpha_interpenetrating = 0x200u;
inline constexpr std::uint32_t material_mask = 0x3ffu;
inline constexpr bool valid_material_word(std::uint32_t word) noexcept {
    // The additional depth modes describe authored translucent passes only.
    return (word & ~material_mask) == material_tag &&
           (!(word & (material_alpha_depth_write | material_alpha_interpenetrating)) ||
            (word & material_alpha));
}
} // namespace rr64::experimental_course

extern "C" {
#define RR64_COURSE_MATERIAL_CAN_THROW noexcept(false)
#else
#define RR64_COURSE_MATERIAL_CAN_THROW
#endif
// Native 10640/EC30 entry and their RGBA16 render-tile word writers. These
// preserve guest registers and touch only the caller's display-list output.
void rr64_course_material_begin(unsigned char *rdram, unsigned texture,
                                unsigned output_pointer) RR64_COURSE_MATERIAL_CAN_THROW;
unsigned rr64_course_material_tile(unsigned char *rdram, unsigned texture,
                                   unsigned tile_word) RR64_COURSE_MATERIAL_CAN_THROW;
unsigned rr64_course_material_format(unsigned char *rdram, unsigned texture,
                                     unsigned command_word) RR64_COURSE_MATERIAL_CAN_THROW;
// Both terrain submesh emitters call this before restoring s1/output_pointer.
// The size pass and actual pass must include the same restoration commands.
void rr64_course_material_end(unsigned char *rdram,
                              unsigned output_pointer) RR64_COURSE_MATERIAL_CAN_THROW;
// Installed packs preflight every stock/imported cell before native sizing.
// Zero preserves the original path when no pack is installed.
unsigned rr64_course_material_scratch(unsigned char *rdram, unsigned cell)
    RR64_COURSE_MATERIAL_CAN_THROW;
void rr64_course_material_scratch_end(unsigned char *rdram, unsigned end,
                                      unsigned bytes) RR64_COURSE_MATERIAL_CAN_THROW;
// Call at guest-session heap reset, never to recycle a live unchanged heap.
// Runtime owns the allocation lifetime; this discards its cached identity.
void rr64_course_material_reset_scratch(void);
#undef RR64_COURSE_MATERIAL_CAN_THROW
#ifdef __cplusplus
}
#endif
