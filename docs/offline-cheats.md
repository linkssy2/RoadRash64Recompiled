# Offline Cheats tab

Requested September 23, 2026. The launcher/settings overlay owns a separate
**Cheats** tab, with individual toggles rather than custom-race-only settings.
All default off and persist in `cheats.json`. Big Game, single-player Thrash
and local multiplayer share the implementation, including installed race packs.
These options never become online lobby settings or authoritative game rules.

- **Infinite Rider Health** suppresses native attack/impact stamina damage for
  human racers. Physical detachment, stun, busts and recovery still run. Native
  crashes temporarily clear the rider bar as part of recovery; this is not a
  no-crash switch and does not resurrect a depleted rider.
- **Indestructible Bikes** suppresses only the human bike's native durability
  charge on a crash. It leaves impact momentum, ejection and recovery intact.
- **All Bikes Available** exposes the original selectable bikes through their
  native previews and selection/purchase code. Big Game purchases still use
  normal prices and save the purchased bike normally. This option does not
  write permanent unlock flags. Custom Cop keeps its cop-rider restriction.
- **All Tracks Available** opens every native race level and its reversed copy
  (the `5F420` availability table at `800A5334`), matching the original
  main-menu code's track unlock without entering it each launch. It is
  independent of All Bikes. Only levels that were locked are opened and
  remembered; switching off or going online relocks exactly those, never earned
  levels. A level earned while this is on may stay hidden until the main menu
  restores saved unlocks. Saves and Big Game progress are not written.
- **Start with All Weapons** grants the native maximum inventory quantity once
  per race and once when enabled during a live race. Ordinary use/theft remains
  unchanged; disabling the toggle does not confiscate a granted inventory.
  Human cops retain police equipment. Question boxes remain the only loose
  weapon pickup source on imported courses.

Every active online session suppresses cheats, including the lobby and connection
failure/disconnect transitions. The overlay disables the controls while online,
retaining the user's offline preferences. Guest-side guards enforce the same
rule independently of the UI. Prediction copies, highlight playback, attract
demo and AI riders are excluded. No shared network protocol change is required.
Local achievements do not unlock while a cheat is enabled in offline play.

UI callbacks publish atomic preferences. Equipment changes run on the native
game thread after actor links are initialized or at a live frame boundary.
Health protection intercepts verified damage stores rather than repeatedly
filling bars or short-circuiting physics. Menu lists borrow original records and
preserve purchase checks; no arbitrary bike IDs or native list sizes are invented.

Offline evidence and candidate identity are recorded under
`analysis/offline-modifiers-20260923`. Read root `ACTIVE_BUILD.md` for the actual
build/test status; implementation and offline checks do not establish visual
acceptance. No launch is authorized until the owner says **ready**.
