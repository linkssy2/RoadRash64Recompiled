# Source layout and editing conventions

The native project code uses the formatting rules in `native/src/.clang-format`.
The readability pass expands compressed functions, branches and data structures
without changing names, expressions, constants, includes or execution order.
Use the formatter on the files you edit, rather than reformatting dependencies
or generated guest output. Existing comments explain original-engine contracts.

## Where to start

Start with [the native build map](BUILD_MODULES.md) for target ownership and
offline checks, and [player package layout](PLAYER_PACKAGE.md) for distribution
files. The table below identifies the main runtime boundaries; related feature
guides describe the engine contracts in more detail.

For the recent traffic, per-player eject, menu borders and geometry reuse changes,
see [Traffic and presentation editing](TRAFFIC_AND_PRESENTATION_EDITING.md).

| Area | Project-owned source | Preserve when editing |
| --- | --- | --- |
| Startup and settings | `main.cpp` | Initialization order, saved settings keys and device lifetime. This line-sensitive entry point was excluded from the formatting pass. |
| Runtime bridges | `rr64_runtime_shims.cpp`, `rr64_engine_layout.hpp` | Original argument/register contracts and checked guest-memory ranges. |
| Actor presentation | `rr64_actor_render_snapshot.cpp`, `rr64_actor_render_runtime.cpp`, `rr64_actor_held_pose.cpp` | Authored pose ownership and independently moving detached riders. |
| Weapons | `rr64_weapon_render.cpp` | Per-view rider roots, attachment ownership and restoration after drawing. |
| Terrain | `rr64_world_terrain.cpp`, `rr64_world_terrain_assets.cpp` | Cached original geometry, draw-distance admission and safe frame-buffer ownership. |
| Scenery | `rr64_world_objects.cpp`, `rr64_world_object_assets.cpp` | Stable placement identities, animation state and consistent course bounds. |
| Video | `rr64_video_mode.cpp`, view/frustum headers | Separate camera projection, HUD policy and original framebuffer restrictions. |
| Audio and music | `rr64_audio_output.cpp`, `rr64_music.cpp` | Queue lifetime, original fades and the shared music-volume setting. |
| Imported audio mixers | `rr64_course_audio.cpp`, `rr64_mk64_item_audio.cpp` | Prepare fixed-size voice lists once per callback while holding the bank lock; keep sample accumulation order and persistent voice state unchanged. |
| Diagnostic switches | `rr64_diagnostic_options.hpp` | General diagnostics requires exactly `1`; legacy runtime/autotest flags retain their existing nonzero convention. Errors remain available without verbose capture. |
| Achievements | `rr64_achievements.cpp`, achievement helpers | Local save identity and bounded UI work. |
| Player profile names | `rr64_profile_names.cpp` | New-profile/solo boundaries; never overwrite loaded saves. |
| Campaign completion | `rr64_campaign_completion.cpp` | Native ending rewards, profile checksum and explicit save confirmation. |
| Offline cheats | `rr64_offline_modifiers.cpp` | Exclude humans, online, prediction, attract and highlight playback. |
| Imported-course progress | `rr64_experimental_course_route.cpp` | Signed movement and eligible human/AI authority; stock roads remain native. |
| Local race additions | `docs/CUSTOM_COP_EDITING_GUIDE.md` | Player counts, local-only gates and existing control IDs. |
| Online play | `rr64_netplay.cpp`, `rr64_online_menu.cpp` | Follow `docs/multiplayer-plan.md`; transport remains experimental. |

Guest hook locations are declared in `config/roadrash64.us.toml`; regenerate
`build/RecompiledFuncs` when those change. Do not hand-edit generated functions.
Dependency changes belong in the locked patch/override export, not just the
private dependency worktree. Keep compiler-generated files, ROMs, captures and
old test packages out of the source distribution.

## Reviewing a readability-only change

Keep include order and preprocessor boundaries. Do not rename persisted keys or
change numerical constants, pointer layouts, timing, branches or memory access
as incidental cleanup. Avoid moving code with line-sensitive macros. Small
contract comments are more useful than narrating each statement.

Compare source tokens and directives, then rebuild and run relevant offline
checks. Formatting can change debug line information and binary hashes; treat
the rebuilt candidate as a new artifact. Runtime acceptance and publication
remain separate steps. Optional diagnostics and validation guards are not dead
code merely because ordinary play leaves them disabled.

## 1.4.2 additions

See [MK64 item modules](mk64-items.md), [rival-engine ownership](rival-engine-audio.md),
and [Insanity campaign mapping](insanity-campaign.md). Keep gameplay simulation,
local input/HUD focus, recorded presentation and sound ownership separate.
The release removes the unused scalar-size HUD wrapper; all draws use the actual
native weapon rectangle. Tests target that same production entry point.

## Cleanup and performance review

The September 30 audit followed maintained CMake, hook, source and fixture
references across every project-owned native source/header. No additional
whole-file deletion was justified: the small guest-entry wrappers and optional
test-only recorder still have callers. Preserve them unless their entire
calling path is deliberately replaced.

Routine reports are opt-in. Avoid assembling a verbose message, scanning a
recorded clip, claiming a diagnostic queue slot or sampling a diagnostic clock
when its recorder is disabled. Keep failure handling, input validation and
game-state changes outside those logging gates. A once-only report must not
perform a locked atomic update on every later frame.

Audio callbacks use bounded stack storage and existing nonblocking locking.
Only hoist values that cannot change during that callback; never cache pointers
past the owning lock. Mixer refactors are checked against the original
production implementation for exact PCM and voice-state equality, not just
similar-looking equations. Offline callback timings measure those callbacks,
not total game FPS or Steam Deck performance.

Do not turn a cleanup into a blanket rewrite of generated code, renderer
dependencies or gameplay hooks. Preserve known fixes and add short comments at
ownership and lifetime boundaries rather than narrating every statement.

## 1.4.4 release-candidate review

The October 5 inventory covers all maintained native source, hooks, build/test
scripts, importer/mod tools and dependency exports. Every production translation
unit and header has a build/include reference. Generated replay budget calls are
intentional, even though a simple source-only caller search misses them.

Recent AI selection and course-guidance code follows the existing formatting
rules; tokens and comments were checked before rebuilding both platforms.
Use `rr64_ai_bike_selection` for opponent donor selection, `rr64_course_ai` for
imported-route steering/recovery, and `rr64_rival_engine` for shared native
engine-voice ownership. These stay separate from rendering and network transport.
Routine diagnostics remain opt-in; error reporting and regression fixtures stay.
Player packages exclude test tools, logs, captures, saves and extracted ROM data.
