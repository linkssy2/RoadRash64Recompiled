# Road Rash 64 Recompiled — 1.4.4

An unofficial PC recompilation for Windows and experimental native Linux of **Road Rash 64**, built with N64Recomp, N64ModernRuntime, RT64, and RecompFrontend.

**You must supply your own legally obtained Road Rash 64 USA v1.0 ROM. No ROM or soundtrack is provided.** This is release **1.4.4**. Online multiplayer remains experimental.

## Why this exists, credits, and AI disclosure

I wanted a childhood game to be playable on PC in all its glory: clearer presentation, smoother motion, detailed riders and bikes, and a practical way to keep playing it on modern hardware.

I do not want to take credit for creating Road Rash 64, the recompilation technology, the renderer, the libraries, or other people's artwork. That credit belongs to the original game developers and the authors and contributors of the tools listed in [CREDITS.md](../CREDITS.md). My role has been bringing this project together, testing it, and describing the experience I hoped to achieve.

**AI was used extensively during development**, including code changes, debugging assistance, documentation, and parts of the launcher artwork. I am sorry to people who are disappointed by that choice or concerned about its effects on this work. I want to be upfront about how the project was made rather than imply it was entirely hand-written. AI assistance does not establish correctness or ownership, and it does not shift responsibility away from this project. Please judge the implementation by reviewable code and reproducible results. Upstream projects have their own contribution policies; this project's changes are not endorsed by them.

## Getting started

1. Use **RoadRash64Recompiled-v1.4.4-Win64.zip** from the 1.4.4 release downloads. Players do not need the source-code download or a compiler.
2. Extract the entire ZIP to a writable folder. Do not run it inside the archive.
3. Open **RoadRash64Recompiled.exe**. Choose **Select Game ROM** and select your supported `.z64`, `.n64`, or `.v64` dump.
4. After validation, choose **Start Game**. Use **Settings** to configure your controller, graphics, and sound.

**RoadRash64DirectStart.exe** skips the launcher once a valid ROM has been configured. It falls back to ROM selection if necessary. Neither executable should open a command window.

Your ROM is validated and stored locally by the runtime. It is not uploaded by ROM selection. Do not upload ROMs to Issues, Discussions, pull requests, or release attachments.

Supported normalized ROM SHA-1: `87727a298f583ec8325f5655088ff21e37b335b2` (USA v1.0, 32 MiB). This identifies the supported dump; it is not a download link.

## What the project does

N64Recomp translates the game's machine-code routines into native code during the **developer build**. The distributed executable already contains that translated code. At play time, the runtime reads game content from the player's ROM. Selecting a ROM does **not** compile the whole game on the player's computer.

This is the same broad static-recompilation/user-ROM architecture described by [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp#plug-and-play). It is not an endorsement from that project, a claim of identical implementation, or a legal conclusion. Gameplay content and soundtrack data remain ROM-supplied. The existing achievement images and custom launcher artwork are retained unchanged.

## Features

- **Improved presentation targeting smooth 60 FPS**, with fixes for pacing regressions, interpolation, and rendering flicker. Actual performance depends on hardware, settings, and scene complexity.
- **MAX LOD** for riders and bikes, including work on distant, crashed, airborne, paused, and finish-area models. It preserves the highest detail the game supplies; it does not create new high-resolution models.
- **Draw Distance slider** for terrain and objects across game modes, with terrain/object rendering optimizations. These remain more demanding than original draw distances and are not a promise that every object is visible from every point.
- **16:9 and 21:9 aspect options**, a **30–240 FPS presentation slider**, and D3D12/Vulkan rendering through RT64.
- **Keyboard and controller remapping**, including **Eject from Bike** (left-stick click by default) and **Spoke Jam Attack** (right-stick click by default). With fists selected, the latter retains the existing punch/weapon-steal behavior and original proximity/timing rules.
- **Persistent saves** through the virtual Controller Pak implementation and saved frontend settings.
- **Optional custom music rotation** from the `music` folder. WAV, OGG, FLAC, MP3, MP4/M4A, AAC, and WMA are all supported on both Windows (via Windows Media Foundation) and Linux (via a bundled shared FFmpeg subset). No additional music is included and there is no song-title popout.
- **52 local achievements**, saved on this computer, with paged browsing. There is no RetroAchievements account login, account scoring, or unlock synchronization.
- **Mod/texture-pack support** through Settings. The release provides a separate optional download of Crisaty’s Remastered Edition pack; see its credits and installation instructions below.
- A compact launcher, direct-start executable, and input guards intended to stop overlay dismissal from activating a menu behind it.

Draw Distance adjusts terrain and scenery range together. MAX LOD controls rider/bike detail separately. The Music Volume slider covers original and custom music. Local player controls support controller assignment and per-player names. Existing mods targeting the internal 1.0.6 interface remain compatible.

This 1.4.4 release supplies Windows x64, an experimental Linux x86-64 AppImage, and corresponding source. Both platform downloads include a native MK64 importer. The optional texture mod is a separate release download. Linux gameplay and hardware coverage remain limited; offline build and import checks do not replace community testing.

## Requirements

Both platforms need a CPU with AVX (Intel Sandy Bridge / AMD Bulldozer, 2011, or newer); AVX2 code paths are used automatically where the CPU has them. On an older CPU the game shows a message and exits. A broad hardware compatibility matrix has not been established for either platform. See [SIMD.md](SIMD.md).

**Windows:** 64-bit Windows 10/11, and a GPU/driver supporting the selected RT64 D3D12 or Vulkan backend. Custom media decoding uses Windows Media Foundation.

**Linux:** a 64-bit distro with a working Vulkan driver (Mesa RADV for AMD, NVIDIA's proprietary driver, or AMDVLK) — RT64 only targets D3D12 and Vulkan, and only Vulkan is available outside Windows, so there is no software-rendering fallback if the driver is missing or broken. A running audio server (PipeWire, PulseAudio, or ALSA) is also expected, as on any modern desktop Linux. See [Linux notes](LINUX_EXPERIMENTAL.md) for the experimental AppImage's requirements and limitations. Custom media decoding uses bundled shared FFmpeg libraries. The MK64 importer includes its own Python/NumPy runtime; players need no compiler, Python installation or system codec package.

## Multiplayer — early and very untested

**Treat online multiplayer as experimental.** Connection reliability, synchronization, player control assignment, race completion, and larger sessions need considerably more testing. Earlier tests reported connection failures, incorrect peer state/maps, and a host crash at the finish. Later fixes and offline protocol tests do not establish that every issue is resolved.

The game offers local split-screen multiplayer and an early direct-connect online path. Online sessions have an experimental limit of 14 participants, but large-session gameplay has not been established as reliable. There is no promise of matchmaking, relay fallback, or automatic NAT traversal. Direct connections may require appropriate UDP forwarding/firewall settings; CGNAT can prevent hosting. Do not disable your firewall wholesale.

Please include host/client roles, player count, exact map/game type, and both machines' observations when reporting an online bug. Report problems to this repository, not to the upstream tools' maintainers.

## Known limitations and testing status

- MK64 items, imported tracks and online play remain experimental. Broader hardware, map and online coverage remains limited.
- High detail/distance settings can expose performance or visibility problems on particular maps, cameras, and hardware.
- Mods may affect stability. Reproduce issues with mods disabled where practical.
- Routine performance logs are disabled. Fatal failures can write `RoadRash64Recompiled-runtime.log` beside the EXE; writing can fail in a protected folder. A normal run need not create a log.
- Existing local achievements are not verified RetroAchievements account progress. Their current [standalone policy](https://docs.retroachievements.org/general/standalone-support.html) excludes recompilations.

See [BUILDING.md](../BUILDING.md) to compile it yourself and [LEGAL.md](../LEGAL.md) for copyright, licensing, and distribution limitations.

## Bugs, discussion, and suggested commits

Use **Issues → Bug report** for reproducible problems and **Issues → Feature or patch proposal** for suggested changes. Include the release version, map/mode, steps, hardware, and expected versus observed behavior. Short screenshots or video are useful; remove private information from logs.

Use **Discussions**, when enabled, for setup questions, map-testing results, ideas, and general conversation. Until it is enabled, the discussion issue template provides a place for those conversations.

Code contributions are welcome as focused pull requests. Explain the problem, implementation, tests, and remaining uncertainty. Disclose AI assistance and preserve upstream notices. Do not submit ROMs, extracted game data, generated game code, credentials, or unrelated large files. Read [CONTRIBUTING.md](../CONTRIBUTING.md) before proposing commits.

## Optional texture conversion

Road Rash 64 Remastered Edition was created by **Crisaty (Cristian A.T.)**. The separate optional download provides the same **RT64 conversion of that original Jabo texture pack** previously supplied with 1.3.1. All credit for the original mod and artwork belongs to its creator; the recompilation project does not claim authorship of those textures or endorsement by the creator.

[Original creator and project page](https://crisaty.tumblr.com/post/113116999840/road-rash-64-remastered-edition)

Download **RoadRash64Recompiled-Optional-Texture-Mod.zip** separately and extract its RTZ. Install it through Settings > Mods. Replace an older copy rather than installing duplicates. The source ZIP does not include the texture pack. Attribution is not a redistribution license; no explicit redistribution license was found in the supplied pack or linked creator page.

See [current local race options and controls](LOCAL_RACE_OPTIONS_RELEASE.md). Release 1.4 retains these options and adds them to single-player Thrash. See [release notes](../RELEASE_NOTES.md).

Contributor navigation: [source layout](SOURCE_READABILITY.md) and [traffic/presentation contracts](TRAFFIC_AND_PRESENTATION_EDITING.md).


## New in 1.4

- **Optional MK64 race pack:** import all 16 courses from your own supported
  Mario Kart 64 USA ROM. Includes course previews, scenery, hazards, question-box
  weapons and optional course music. Select MK64 as a race level in Thrash,
  local multiplayer or experimental online play.
- **Crash highlights:** end-of-race clips include major crashes from human and
  AI riders, with replay cameras and slow motion. Press A to continue; online control
  remains with the host.
- **Offline Cheats tab:** rider health, bike durability, freezing opponents, bike availability and
  starting weapon options. These are disabled online and suppress local
  achievement unlocks while active.
- **Race options and fixes:** single-player Thrash options, remembered character
  selection, corrected special/purchased bikes, and lap/recovery/crash fixes.
- **Pacing and controls:** further frame-pacing work and Linux N64-controller
  routing fixes. Hardware-dependent hitches and driver limitations can remain.

### Import the optional tracks

1. Configure your Road Rash 64 ROM in the launcher.
2. Open **Settings → Mods → MK64 Race Pack → Import MK64 ROM**.
3. Select your supported **Mario Kart 64 USA ROM**. Allow the import to finish;
   progress and cancellation are shown in the same Mods entry. The first import
   took about six minutes on the development PC and may take longer elsewhere.
4. The completed pack is enabled automatically. Later launches use the saved
   files; use the normal mod toggle to disable it before starting the game.

Neither ROM nor converted MK64 meshes, textures or audio is shipped. Conversion
uses the bundled tools locally; no Python installation or network download is
needed. Failed/cancelled conversion preserves an existing pack. All online peers
need the same game build and matching locally imported pack; the host does not
send track assets to guests. Online and imported-course gameplay remain subject
to testing, and some authored console effects use documented adaptations.

Maximum Draw Distance can load the entire map, including the areas used by all
its races. Dense distant geometry can slow the game, especially in split-screen
multiplayer. **50–60% is the recommended starting point** for balanced performance.

See [release notes](../RELEASE_NOTES.md) and the
[contributor feature map](RELEASE_CANDIDATE_EDITING_GUIDE.md).

## 1.4.4

See [release notes](../RELEASE_NOTES.md) for bonus-save fixes, rival-engine improvements, controller routing, highlights, imported Tag and the optional More Characters skins. Online peers must all use 1.4.4 (protocol 68).

## Optional More Characters

Install the separate **MoreCharacters2.0.5.nrm** download through **Settings → Mods** before starting the game. Its eleven authored skins use native male/female models and existing animations. They are selectable offline and in local multiplayer; online custom appearances are disabled. See [rider skins](rider-skins.md) and [credits](../CREDITS.md).
