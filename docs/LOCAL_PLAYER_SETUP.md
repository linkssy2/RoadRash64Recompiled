# Local player setup candidate

September 26 follow-up: a keyboard-only user without assigned player cards now
retains the working shared port-one input when entering Local Multiplayer.
This permits solo play against AI and preserves navigation/Back. Assigned
controllers continue using independent ports. See
`local-multiplayer-keyboard-entry.md` for diagnosis and verification limits.
The older keyboard-toggle description below is historical; current builds
select Mouse and Keyboard directly in each Controls card.

Configure local players before entering Local Multiplayer through **Settings →
Controls** in the launcher or in-game settings overlay. Each of the four cards
has an **In-game name**, control-profile selector and profile editor. Names are
saved when the field loses focus. The original game fields support eleven
uppercase letters, digits or spaces; empty/unsupported names fall back to PLAYER
1–4. Names belong to player slots, not to a controller's hardware identity.

Connected gamepads are assigned automatically to available slots. Existing
connected players retain their slots when another controller disconnects. The
disconnected slot becomes empty; a new device can fill it. At most four ports are
assigned, even if more devices are connected. **Assign players** remains available
to change the order manually. Bindings and in-game names are separate.

Keyboard enrollment can be enabled in **Settings → Gameplay → Keyboard Player in
Local Multiplayer**; it uses a free slot. Manual assignment also allows choosing
a keyboard player explicitly. Single-player and online retain their shared local
input path. Local Multiplayer switches to assigned physical ports and resumes
the original race menus directly. There is no additional local-setup page.

The controller query/read callbacks already propagate disconnected-port errors
to the guest game. This candidate feeds them actual assigned slots in local
mode. Local names are written at the existing stock multiplayer-name hook, and
online names retain their separate existing path.

Implementation starts in `native/src/rr64_local_players.hpp`, `main.cpp`,
`rr64_online_menu.cpp` and `rr64_online_race_sync.cpp`. Frontend integration is in
`players.cpp`, `profiles.cpp`, `ui_config_page_controls.cpp` and `ui_player_card.cpp`.
The pure `rr64_player_assignment.h` policy preserves surviving slots and limits
the roster to four. Input mode/profile indices are atomic; published roster
reads use snapshots under the assignment lock. Do not hold a controller-state
lock while mutating assignment state.

Offline tests cover empty slots, more than four devices, unplug/replug without
shifting surviving players, manual ordering, keyboard opt-in/removal and bounded
name conversion. They do not prove live multi-controller input or visual layout.
Live verification remains pending a new ready: connect two controllers, edit
both names, verify independent controls in a local race, then check disconnect,
reconnect and return to single-player. Nothing has been published.
