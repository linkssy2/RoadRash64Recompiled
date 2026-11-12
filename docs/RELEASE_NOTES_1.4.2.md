# Road Rash 64 Recompiled 1.4.2

Changes since 1.4.1.

- **Experimental MK64 items:** imported question boxes can award shells, bananas,
  fake boxes, mushrooms, Lightning, Star, Boo and multi-use items alongside Road
  Rash weapons and attack multipliers. The saved **MK64 Items** race option turns
  them on or off. The roulette shares the normal weapon square; normal weapon
  cycling returns to a retained MK64 item. **Use MK64 Item** is separately remappable
  (D-pad Down / X by default). Items are still being tested: balance, collisions,
  visuals and online interactions may need further fixes.
- **Nearby rival engines:** positional stereo and distance falloff for nearby AI
  and nonlocal human bikes, with **Rival Engine Volume** under Audio. Playback is
  limited to three nearby rivals to preserve sound channels. Local split-screen
  players retain their original engines.
- **Big Game bonus races:** preserves the original Level 5 ending and adds an
  optional, replayable Insanity chapter afterward. Fixes the invalid purchase/Join
  transition and supports bonus progress in saves. Back up existing saves before
  upgrading; a complete live bonus-campaign round trip still needs wider testing.
- **Online fixes:** revised connected-player input correction and resampling,
  sound ownership, highlight rider poses and post-race menu synchronization.
  Online remains experimental; these changes do not guarantee stutter-free play.
- **Item fixes and cleanup:** resolves the captured material-call freeze, revises
  shell/trap contacts, adds Lightning strike flashes and aligns shrunken bikes,
  riders and weapons. Shells are 25% smaller than the last private test. Removes
  an obsolete HUD sizing wrapper and documents the new module boundaries.

## Downloads and setup

Windows x64, experimental native Linux x86-64 AppImage, and matching source are
provided. Everyone in an online session must use **1.4.2 (protocol 65)** and the
same enabled track pack. Older online builds are incompatible.

The optional **Road Rash 64 Remastered Edition** texture mod by **Crisaty
(Cristian A.T.)** is a **separate download**, unchanged from the previous release.
Install its RTZ through Settings > Mods; avoid duplicate copies.

Main platform/source downloads contain no ROMs or extracted gameplay textures,
maps, course previews or audio. Existing achievement images, custom launcher
artwork, app icon and normal UI resources are retained. This is not a claim that
the package contains no game-derived material: native translated code and the
authorized artwork remain. Supply your own supported Road Rash 64 USA v1.0 ROM.
MK64 tracks/items require your own supported Mario Kart 64 USA ROM; import it
through Mods > MK64 Race Tracks > Import. Converter **1.0.1-c40** supplies item
art/audio. Existing c40 imports work; older packs need reimporting for items.

## Testing and limits

Both platforms pass the offline item, audio, campaign and network suites plus
settings-restart checks. The newest item-cycle/Lightning/shrink changes have not
had a final live visual acceptance run. Linux hardware coverage, bonus-campaign
completion, imported terrain and Internet sessions need further community tests.
Rival-engine listening balance and Steam Deck hitching remain areas for testing;
this release does not claim a Deck performance fix.

Draw Distance can load the entire original map, including all its races. Start
around **50–60%**; distant geometry is more demanding, especially in split screen.
Normal diagnostic capture is opt-in. Fatal errors can still write a runtime log.

Credits and third-party notices accompany the downloads. Existing N64Recomp,
N64ModernRuntime, RT64, RecompFrontend, MK64 decompilation, Linux contributor and
library credits remain; no new third-party engine/library was added for items.
