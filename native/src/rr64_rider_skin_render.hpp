#pragma once
#ifdef __cplusplus
#include "rr64_mk64_item_material.hpp"
#include <span>

namespace rr64::rider_skins {
// Only CI8 image/TLUT references change. The original vertex, matrix, triangle,
// tile and material commands remain the native model's own instructions.
struct ImageBinding {
    unsigned image{}, palette{}, replacement_image{}, replacement_palette{};
};
unsigned replace_images(std::span<const mk64_items::MaterialCommand> source,
                        std::span<mk64_items::MaterialCommand> output,
                        std::span<const ImageBinding> bindings) noexcept;
void reset_render_session() noexcept;
}

extern "C" {
#endif
unsigned rr64_rider_skin_actor_list(unsigned char *, unsigned node, unsigned original,
                                    unsigned lod, unsigned vertex_base
#ifdef __cplusplus
                                    = 0
#endif
                                    );
unsigned rr64_rider_skin_actor_call(unsigned char *, unsigned node, unsigned original,
                                    unsigned call, unsigned lod);
void rr64_rider_skin_preview_begin(unsigned char *, unsigned root);
void rr64_rider_skin_preview_end(unsigned char *);
#ifdef __cplusplus
}
#endif
