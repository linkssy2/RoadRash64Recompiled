#pragma once

#ifdef __cplusplus
namespace rr64::quadrant_hud {
// Graphics preference; published by the settings UI, read on the game thread.
void set_enabled(bool enabled);
bool enabled();
}

extern "C" {
#endif
// Runs from 30220's shared exit (342C4). For 3/4-player layouts, draws the
// two-player health bars and weapon box into each active view with the same
// display-list commands, sprites and label producer as the stock block.
void rr64_quadrant_hud_draw(unsigned char *rdram, void *context);
// Sprite flush (8001EF8C) per-record scope: gives this HUD's queued sprites
// their view's explicit alignment so RT64's automatic HUD anchoring leaves the
// panel intact. Other records are untouched.
void rr64_quadrant_hud_sprite_begin(unsigned char *rdram, unsigned record);
void rr64_quadrant_hud_sprite_end(unsigned char *rdram);
#ifdef __cplusplus
}
#endif
