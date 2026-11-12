#pragma once
#include "rr64_actor_render_snapshot.hpp"

namespace rr64::video {
// Menu-preview only: the caller must run both 15C10 AND 15CFC afterward.
// Race startup can intentionally reuse a view without the second call; never
// invoke this helper from the global layout entry or restore its index by hand.
// 15C10 keys its cached region size only by layout. A video-mode change can
// keep the same layout while changing framebuffer dimensions, leaving 15CFC
// to emit an old scissor alongside a newly sized viewport. Ask the original
// routine to rebuild only when its stored dimensions no longer match its own
// division tables. This also invalidates the original viewport-index cache.
inline bool refresh_viewport_dimensions(unsigned char *memory, unsigned layout) noexcept {
    if (!memory || layout > 2u) return false;
    std::uint32_t cached_layout=0, width=0, height=0, columns=0, rows=0;
    std::uint32_t cached_width=0, cached_height=0;
    if (!engine::read_u32(memory,0x8009db80u,cached_layout) || cached_layout!=layout ||
        !engine::read_u32(memory,0x800b0808u,width) || !width || width>1023u ||
        !engine::read_u32(memory,0x800b080cu,height) || !height || height>1023u ||
        !engine::read_u32(memory,0x8009db8cu+layout*4u,columns) || !columns || columns>2u ||
        !engine::read_u32(memory,0x8009db98u+layout*4u,rows) || !rows || rows>2u ||
        !engine::read_u32(memory,0x800b74a8u,cached_width) ||
        !engine::read_u32(memory,0x800b74acu,cached_height)) return false;
    if (cached_width==width/columns && cached_height==height/rows) return false;
    engine::write_u16(memory,0x8009dba4u,1u);
    return true;
}

// Select the original full-height mode writer. Its normal VI/framebuffer path
// still owns sizing and allocation. Never persist a synthetic console mode.
inline unsigned combined_callback(unsigned char *memory, unsigned original, bool enabled) noexcept {
    if (!enabled || !lod::supported_scene(memory))
        return original;
    std::uint32_t width = 0, height = 0, lock = 0, callback = 0;
    if (!engine::read_u32(memory, 0x800bc9c4u, width) || width < 512u ||
        !engine::read_u32(memory, 0x800bc9ccu, height) || height < 240u ||
        !engine::read_u32(memory, 0x800a4f24u, lock) || lock != 0u ||
        !engine::read_u32(memory, 0x8009cc68u, callback) || callback != 0x8000a3a8u)
        return original;
    return callback;
}
}
