# Release 1.4.3: contributor guide

Paths below are relative to the source repository root. The release comparison
baseline is published **1.4.2**; the table covers the maintained features.
This release uses protocol 66 and the current native-model skin implementation. Read `docs/multiplayer-plan.md` before changing local multiplayer
or online behavior. Online remains experimental; local gameplay acceptance does
not prove online parity.

Internal workspace note: the active checkout is `work/release-1.2-online`,
originally based on v1.2.0. Its parent's old renderer experiment is not a build
source.

## Where to make changes

| Feature | Source entry point | Constraint |
| --- | --- | --- |
| Launcher, input actions, version | `native/src/main.cpp` | Preserve per-player device ownership and dedicated-action vs directional-attack separation. |
| Assigned input profiles | `native/lib/RecompFrontend/recompinput/src/profiles.cpp`, `input_state.cpp`, `players.cpp` | Native gameplay, dedicated actions and overlay events must resolve the intended player's device/profile without rewriting bindings. See `controller-profile-routing.md`. |
| Local race options and AI cops | `native/src/rr64_local_race_options.cpp` | Packed options are shared with online setup; preserve defaults and roster limits. |
| Single-player Thrash | `native/src/rr64_thrash_options.cpp` | Keep its context and persisted preset separate from multiplayer; never activate split-screen input to reuse race options. |
| Remembered characters | `native/src/rr64_character_preferences.cpp`, `rr64_character_menu.cpp` | Store local confirmed choices; retain loaded campaign selections and online ownership. |
| Cop roles, arrests, victory | `native/src/rr64_custom_cop.cpp` | Use the same eligible racer set for initial counts and victory. |
| Cop controls, recovery and posts | `native/src/rr64_custom_cop_runtime.cpp` | Replay state must stay isolated; LB gestures and RB trick must not consume down attacks. |
| Cop selection reminder / bust display | `native/src/rr64_custom_cop_ui.cpp` | Keep menu footer separate from per-view race HUD. |
| Backtracking / nearest-road recovery | `native/src/rr64_roaming_route.cpp` | Human-only reverse progress; native recovery tail owns pose, terrain height and camera. |
| Split-screen HUD and countdown | `native/src/rr64_hud_widgets.cpp` | Scope each native text/sprite producer, restore state, never anchor world pickups as HUD. |
| Plain sky | `native/src/rr64_sky_sprites.cpp` | Remove only the shared producer's cloud queue entries; preserve all other sprites. |
| Terrain and objects | `native/src/rr64_world_terrain.cpp`, `rr64_world_objects.cpp` | Visual range and collision residency have different ownership. |
| Rider, bike, weapon presentation | `native/src/rr64_actor_render_snapshot.cpp`, `rr64_weapon_render.cpp` | Per-view transforms must be restored after drawing. |
| Online transport | `native/src/rr64_netplay.cpp` | Follow the current protocol in source and `multiplayer-plan.md`; matching rules and installed course-pack identities are required. |
| Online simulation / prediction | `native/src/rr64_authoritative_step.cpp`, `rr64_prediction_reconcile.cpp` | Preserve authoritative ownership and replay isolation. |
| Positional voice | `native/src/rr64_voice_chat.cpp` | Use body location, not bike location; no wall occlusion claim. |
| Imported course lifetime and route | `native/src/rr64_experimental_course.cpp`, `rr64_experimental_course_route.cpp` | Keep stock courses on their native path; branch/recovery reacquisition must not invent or erase completed circuit progress. |
| Course surfaces, hazards and AI | `native/src/rr64_course_material.cpp`, `rr64_course_walls.cpp`, `rr64_course_hazards.cpp`, `rr64_course_ai.cpp` | Preserve finite contacts, actual voids and native rider physics. Resolve authenticated terrain alternatives without deleting genuine branches/decks. |
| Course items and audio | `native/src/rr64_course_items.cpp`, `rr64_course_item_render.cpp`, `rr64_course_audio.cpp`, `rr64_course_music.cpp` | Host/local authority grants items once; roulette is presentation. Audio callbacks perform no file loading or sequence synthesis. |
| Native rider skins | `native/src/rr64_rider_skin_*`, `rr64_rider_skins.cpp` | Authored diffuse textures only; preserve native donor geometry, slot ownership and separate appearance preferences. See `rider-skins.md`. |
| Rival engines | `native/src/rr64_rival_engine.cpp` | All nearby rivals use dedicated native rows; preserve heap sizing, original sound capacity, listener ownership and early disabled path. |
| Bonus campaign saves | `native/src/rr64_campaign_bonus_save.cpp` | Keep saved bonus progress and native bike purchase ownership consistent. |
| Crash highlights | `native/src/rr64_highlights.cpp`, `rr64_highlight_recording.cpp`, `rr64_highlight_pose.cpp`, `rr64_highlight_weapon.cpp`, `rr64_highlight_network.cpp` | Replay recorded presentation without rerunning race physics, inventory grants or damage; restore guest state after drawing. |
| Offline cheats | `native/src/rr64_offline_modifiers.cpp`, `rr64_offline_modifiers_bikes.cpp` | Default off; suppress online, AI, demo and replay effects. Preserve physical crashes and normal save ownership. |
| ROM import and installation | `native/src/rr64_mk64_import.cpp`, `rr64_import_process.cpp`, `rr64_import_install.cpp` | Worker owns conversion; UI polls progress/cancellation. Validate staging before replacing an installed pack; block game start during conversion. |
| Portable converter | `scripts/mk64_importer/`, `scripts/race_pack_*.py`, `tools/mk64-importer-contact/`, `tools/mk64-importer-motion/` | Read donor data from the user's validated ROM. Ship code/address recipes, never extracted geometry, pictures, samples or spawn arrays. |
| Mod action / game-start guard | `native/lib/RecompFrontend/recompui/src/composites/ui_mod_menu.h`, `native/lib/RecompFrontend/recompui/src/base/ui_launcher.h` | Generic callbacks run on the UI thread; both launcher start paths must honor the guard before hiding their contexts. |

Frame pacing changes span RT64, Plume and the frontend, rather than the game
simulation clock. Consult `frame-pacing-20260920.md`,
`frame-pacing-02-20260921.md`, `frame-pacing-03-20260921.md` and
`RT64_EDITING_GUIDE.md` before changing queue/timer ownership.

## Hooks and memory

Edit `config/roadrash64.us.toml` and handwritten native helpers. Never hand-edit
`build/RecompiledFuncs`; regenerate it with N64Recomp. `rr64_native.hpp` is the
C-compatible hook surface; Custom Cop declarations live in `rr64_custom_cop.hpp`.
`rr64_engine_layout.hpp` contains validated guest-memory access helpers. Native
addresses refer to the supported USA ROM revision, not host pointers.

Comments should explain ownership, the native call-site contract, and why a guard
exists. Keep cosmetic changes separate from gameplay corrections. Do not remove
fallbacks, prediction fixtures or optional diagnostics because one mode bypasses them.
The optional RTZ texture bridge, course importer and their support tools remain
active features; an optional or experimental name is not evidence of dead code.

Dependency edits require updated `dependency-patches/`, `dependency-overrides/`
and `dependencies.lock.json`. Reconstruct them against each pinned commit and
compare resulting files, including new helper files. Local Git-object availability
does not establish that every commit can currently be fetched from its remote.

## Checks and release handoff

Build Release and run the affected smoke targets: `RR64LocalRaceOptionsSmoke`,
`RR64RoamingRouteSmoke`, `RR64HUDWidgetsSmoke`, `RR64SkySpritesSmoke`,
`RR64OnlineViewportSmoke`, `RR64MenuEjectSmoke`, `RR64CombatCreditSmoke`, and
`RR64VoiceChatSmoke`. Some other targets require arguments or generated fixtures;
read their source before invoking them. Select additional importer, course,
highlight, controller and pacing fixtures according to the touched behavior.
Passing these is not an online playtest. Keep original failing captures and use
negative controls where they establish the regression a fixture should detect.

The course-enabled and explicitly disabled builds must both remain valid.
Converter refactors must reproduce the accepted full pack from a supported ROM,
including route contact masks, placement, material order, animations and audio.
Freeze every package JSON recipe and native helper, and verify the bundled
converter with a minimal environment; a source Python invocation alone is not
proof of a usable player package. See `public-mk64-import.md`.

Package runtime files from an explicit inventory. Exclude ROMs, saves, private
replay captures, generated sky experiments, diagnostics launchers and historical
builds. Generated MK64 course packs are private user-ROM output and must not be
copied into public downloads or source. Keep the approved optional third-party
texture RTZ under `optional-mods`, with its original credits; validate its nested
ZIP data using a decoder that supports Zstandard method 93. The source archive
does not need the texture payload. ROM-free does not mean all artwork/code is
independent of the original games; preserve the existing legal distinctions.

Normal capture remains opt-in. Keep error reporting. Stage a clean ZIP before
adding personal ROMs/settings to a separate local test copy. Test that exact
packaged executable before publication; wait for the user's launch signal and
separate publication approval. Offline evidence, visual acceptance, physical
controller testing and live online acceptance are different gates.

Deeper notes: `roaming-distance-recovery.md`, `custom-cop-backhand.md`,
`custom-cop-victory-jam.md`, `pickup-hud-exclusion.md`, `RT64_EDITING_GUIDE.md`,
`FRONTEND_EDITING_GUIDE.md`, `public-mk64-import.md`, `offline-cheats.md`,
`character-preferences.md`, `RELEASE_CLEANUP_1.4.2.md`, and `multiplayer-plan.md`.
