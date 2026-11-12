# Road Rash 64 Recompiled 1.4.0

Changes since **1.3.1 Hotfix Revision 2**.

- **Optional MK64 race pack:** import all 16 courses from your own supported
  Mario Kart 64 USA ROM in **Settings > Mods > MK64 Race Pack**. Imported courses
  have selection previews, scenery, hazards, item boxes and sound effects.
  The mod can be enabled or disabled through Mods.
- **Course music and item roulette:** the remembered **MK64 Music** setting plays
  each imported course's soundtrack through the race and highlights. Question
  boxes award weapons or native 25-second ×2/×4 attack multipliers; multipliers
  keep your current weapon equipped.
- **Crash highlights:** major player and AI crashes play after a race with
  changing camera angles and slow motion. Press **A** to continue; online, the
  host controls this. Playback includes rider, bike and weapon presentation,
  with improved distant NPC detail and cleanup of lingering race sounds.
- **More single-player Thrash options:** custom opponent counts, bike levels,
  Scooter, Insanity and Custom Cop choices, with a separate saved solo preset.
- **Offline Cheats tab:** optional rider-health protection, indestructible
  bikes, all bikes available and starting weapons. Cheats are disabled online;
  local achievements do not unlock while cheats are enabled.
- **Race and menu fixes:** corrected special-bike previews and purchased-bike
  selection, remembered character choices, improved lap/recovery bookkeeping,
  Big Game Bike Shop framing, and targeted imported-course terrain and collision
  repairs. Insanity and finish-transition crash fixes are included.
- **Controls and frame timing:** improved assigned-profile routing and addressed
  avoidable timing and Vulkan presentation waits. Performance still depends on
  hardware, settings and the amount of geometry visible.
- **Contributor cleanup:** clearer item/HUD helpers and comments, updated source
  navigation and credits, and removal of routine item debug output. Private test
  logs, personal state and obsolete candidate artifacts are excluded.

## Installing and updating

Both platform ZIPs include the optional **Road Rash 64 Remastered Edition** texture
mod by **Crisaty (Cristian A.T.)** in `optional-mods`. Install its RTZ through
**Settings > Mods**; replace an older copy instead of installing duplicates.
The existing achievement badges are unchanged.

Your own supported **Road Rash 64 USA v1.0 ROM** is required. Importing courses
also requires your own supported **Mario Kart 64 USA ROM**. Neither ROM nor
extracted course meshes, textures, previews, soundtrack or sound effects is
included. The importer creates those files locally. The optional texture mod,
existing badge artwork and custom launcher artwork have their own credits;
see `CREDITS.md`, `LEGAL.md` and `THIRD_PARTY_NOTICES.md`.

Use a fresh folder for the new download. Existing corrected RC4/RC5/RC6 MK64
packs remain compatible. Packs from RC3 or earlier need **Reimport MK64 ROM**
before Start Game for the updated Koopa Beach terrain. Converter: `1.0.1-c39`.

## Experimental features and performance

**Online multiplayer and imported courses remain experimental.** Everyone must
use a compatible protocol-57 build and the same enabled, locally imported pack.
Missing, disabled or different packs are rejected before racing; the host does
not distribute ROM assets. Offline checks do not establish Internet-play
smoothness or complete greater-than-four-player custom-option parity.

**Draw Distance can load the entire Road Rash map, including all of its race
areas. Start around 50–60%.** Dense distant geometry can reduce performance,
especially in split screen. These timing fixes do not guarantee hitch-free play.

This release provides Windows x64, an experimental Linux x86-64 AppImage and
corresponding source. Both include a native MK64 importer; players do not need
Python or a compiler. Linux graphics, controller hardware and gameplay still
need broader community testing.
