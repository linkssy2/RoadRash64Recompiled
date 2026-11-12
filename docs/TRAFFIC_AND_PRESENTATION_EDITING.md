# Traffic, local input and presentation: editing guide

This guide covers the fixes accepted during the September 2026 performance tests.
Change project-owned sources and regenerate guest hooks; do not edit the generated
functions in `build/RecompiledFuncs`. Their numeric names identify original routines.

| Feature | Entry points | Important constraints |
| --- | --- | --- |
| Traffic placement | `rr64_traffic_distance.hpp`, `rr64_traffic_spawn_distance` in `rr64_runtime_shims.cpp`; hook for `func_8006BFF4` in the TOML config | The slider advances the proposed route location before original road validation. Keep the 20-car allocator, speed, collision, damage and update routines intact. |
| Traffic draw range | `rr64_traffic_within_draw_distance`, `rr64_traffic_render_visibility`; `rr64_engine_layout.hpp` | Prepared roots use ten times world coordinates and squared distance. Traffic position is at entity offset `0xA8`. Use the current view's prepared root, never another view's pose. |
| Per-player eject | `main.cpp`, `rr64_local_eject.hpp`, `rr64_runtime_shims.cpp` | Each controller has its own held edge and request bit. Resolve the current player-to-bike mapping before applying or restoring durability; stale actor addresses must not survive reassignment. |
| Floating weapon correction | `rr64_weapon_render.cpp`, `rr64_actor_render_runtime.cpp`, `rr64_actor_render_snapshot.*` | A weapon must use the certified rider root for this view and source bank. Restore the original root after the draw; retain child attack animation and shadow exclusions. |
| Black menu side borders | RT64 `common/rt64_rr64_menu_borders.h`, `render/rt64_vi_renderer.*`, `hle/rt64_present_queue.cpp` | Clip the centered menu against the already cleared black output. Do not change the race viewport, sky geometry or HUD projection. |
| Frame matching reuse | RT64 `hle/rt64_game_frame.cpp` | The secondary index is an optimization of the original candidate order. Ineligible entries require the full fallback. A hash or topology certificate alone does not establish animation identity. |
| Controller hotplug | RecompFrontend input/device code and `rr64_player_assignment.h` | Retain stable player assignment and saved binding names. Rebuilding device lists and UI resources on each input poll can cause hitches. |

## Traffic placement versus visibility

Increasing a draw cutoff cannot show a car that has not been created. The diagnostic
capture that motivated this fix showed newly created cars already inside the stock
draw cutoff and admitted to drawing immediately. The accepted fix advances their
route location, up to 650 additional route units at full Draw Distance. Near the
route end, it uses only half the remaining valid headroom. Route distance differs
from straight-line camera distance, especially around curves.

The original allocator and driving rules remain in control, but the timing and
arrangement of encounters may change. The user explicitly accepted that tradeoff.
Do not raise the pool cap or change simulation rules as incidental rendering cleanup.

## Diagnostics and verification

The temporary traffic probes have been removed from the production hooks and target.
`rr64_traffic_trace.cpp` remains available to `RR64TrafficTraceSmoke` as developer
support; it is not linked into the release game. Its kind-0 position is sampled
before placement and is not the final birth position. A draw-admission event is not
proof of visible pixels. Record capture loss and the view IDs actually observed.

Normal diagnostics remain opt-in. Keep the developer launchers, captures, PDBs and
historical experiments out of the player package. Fatal error reporting is retained.
Useful offline checks include `RR64MenuEjectSmoke`, `RR64WeaponRenderSmoke`,
`RR64TrafficTraceSmoke`, world tests and frame-matching tests. They do not replace
in-game testing across tracks and player counts.

Dependency edits must be exported as a patch plus any new override headers and
their exact hashes. Reconstruct against the locked revision and check Git checkout
bytes; a successful local build alone does not validate a contributor's download.
