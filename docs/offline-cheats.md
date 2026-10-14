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
- **Moon Gravity** reduces the native weight term to one-sixth only while bikes and
  riders are airborne. Grounded driving, wheelies and landing retain full weight
  and traction. Attached riders follow bike state 8; detached riders use both
  signed ground-contact gaps. The preference is saved as `moon_gravity`.
  Five force branches in `36948` and `45960` cover normal, special and blended
  states. Drag, steering, collision impulses and shared gravity constants are
  unchanged. Switching off or leaving offline racing takes effect on the next
  physics evaluation. Existing crashes, out-of-bounds recovery and fall timers
  still apply, so long jumps can still end in recovery.

- **Stay Crashed Until A** holds local human recovery until the owner's mapped A
  press, once the native recovery delay has elapsed. Saved as
  `manual_crash_recovery`; default off. The `3FE48` hook at `40280` gates only
  remount/reposition after busted/wrecked handling, leaving sliding and impacts
  active. AI and terminal racers are excluded. Held accelerator input does not
  count as a new press. Disabling the toggle resumes normal recovery.

Moon Gravity uses the same live-memory, prediction, online and highlight guards
as the other physics cheats. `RR64OfflineOpponentsSmoke` executes all five actual
generated force branches and checks one-sixth weight, unchanged accumulated
forces and memory, every offline race mode, disabling, and excluded contexts.

Every active online session suppresses cheats, including the lobby and connection
failure/disconnect transitions. The overlay disables the controls while online,
retaining the user's offline preferences. Guest-side guards enforce the same
rule independently of the UI. Prediction copies, highlight playback, attract
demo are excluded. Health/equipment cheats target local players; combat
multipliers include CPU racers. No shared network protocol change is required.
Local achievements do not unlock while a cheat is enabled in offline play.

**All Racers: 4x Damage and Force** and **All Racers: 100x Damage and Force**
are saved, default-off toggles in the launcher/in-game **Cheats** tab. They set
melee damage and physical knockback for humans and CPUs on stock and imported
courses. If both are enabled, 100x wins; pickup multipliers do not stack with
either setting. Driving speed, gravity and ordinary wall/vehicle impacts are
unchanged. The 100x option can throw riders far from the road.

The shared melee producer `80060370` selects its damage coefficient separately
for each attacker (`60730`, `60898`). Physical impulse `80065E98` caps the base
force by target mass before its independent power-up branch. The cheat replaces
that branch at `65EF8`, retaining its direction and native impulse consumers.
This avoids a damage-only toggle, per-frame velocity multiplication, or altering
power-up timers. `RR64OfflineOpponentsSmoke` executes these generated branches
and tests live-memory ownership, offline modes, pickups and online exclusions.

UI callbacks publish atomic preferences. Equipment changes run on the native
game thread after actor links are initialized or at a live frame boundary.
Health protection intercepts verified damage stores rather than repeatedly
filling bars or short-circuiting physics. Menu lists borrow original records and
preserve purchase checks; no arbitrary bike IDs or native list sizes are invented.

Offline evidence and candidate identity are recorded under
`analysis/offline-modifiers-20260923`. Read root `ACTIVE_BUILD.md` for the actual
build/test status; implementation and offline checks do not establish visual
acceptance. No launch is authorized until the owner says **ready**.
