# Optional native rider skins

`More Characters` supplies diffuse textures for native donor riders 0 (male)
and 10 (female). It never supplies replacement rider meshes. Native selection IDs,
physics, animation, and Controller Pak records retain their original format.
Custom appearance IDs live in the host catalog and a separate local preference
file. Selection is supported offline and in local multiplayer, not online.

The implementation is split by responsibility:

The catalog has a fixed capacity of 32; More Characters 2.1.0 supplies 25 skins.
The released Turtle skins use painted shells on the native body. No shell
attachment is enabled in the pack.

- `rr64_rider_skins`: immutable catalog and bounded per-player selections.
- `rr64_rider_skin_mod_ui`: archive validation and normal Mods integration.
- `rr64_rider_skin_menu`: native selection, preview, spawn and name hooks.
- `rr64_rider_skin_preferences`: stable-ID persistence outside native saves.
- `rr64_rider_skin_render`: actor-scoped texture bindings and bounded caches.

Startup prepares the catalog before runtime mod loading, then seals it at the
game entrypoint. Published texture storage stays alive for the process lifetime.
Changing the installed catalog requires restarting the app. This avoids dangling
render pointers during rescans or launcher/game transitions.

The archive contains `mod.json`, `rr64-rider-skins.json`, a README and four CI8
payloads per character: three 64x32 textures and one 32x16 texture. Each payload
ends with 256 big-endian RGBA5551 palette entries. Palettes must be opaque. Native
model headers and geometry are read from the user's ROM, never from the mod.

`scripts/build_more_character_skins.py` converts the authored atlases in
`mods/more-characters-skins/art/` into this format. Texture packing accommodates
the original UV islands, including their differing vertical orientations;
native geometry stays unchanged. Dedicated head textures split the native
horizontal UV span by face side while preserving its vertical mapping. The offline authoring step requires
Pillow. The game does not require Python or Pillow to load the mod.

Appearances with dedicated faces use dedicated opaque head strips from
`art/heads/<id>.png`. `head-layout.json` must declare every such strip; a missing
entry or file stops packaging. Ghost Rider and Master Chief retain
their original authored atlas heads. The manifest's optional `source_x` and
`target_x` arrays fit facial landmarks in logical 38x32 coordinates. Both arrays
must increase from 0 to 38 with matching lengths. Fitting only resamples the
authored pixels, followed by the native vertical flip; it never changes the
mesh or UVs. The same fitted face is packed into all three distance layouts.
The authoring report records the head-image hashes, manifest hash and fitting
coordinates alongside the original atlas hashes.

Offline previews can call `native_tiles` with `head_strip` and `head_layout`
directly. Texture conversion does not load files implicitly, so previews and
packaging can use exactly the same prepared tiles.

Seven male skins also use `body-layout.json` for reviewed clothing-panel and
limb-material corrections. Its geometry-free `[destination, source]` pairs are
row-major texel offsets within one tile, applied after CI8 quantization. All
reads use the unmodified tile; copies never cascade. Keeping the original
palette and rejecting writes into the face/filter borders preserves corrected
head colors exactly. Sparse `paletteIndices` pairs select an existing palette
color when it has no source texel; they also cannot write palette bytes.
Artwork and input/output payload hashes pin this calibration:
changed artwork or palette conversion requires another visual review before
packaging. This metadata stays in the authoring source; it is not a runtime
dependency or part of the mod archive.

The native arms and legs already share texture regions between sides. Mirroring
an entire limb strip swaps its wrap, so these corrections target clothing panels
and specific misplaced materials. Original pose and silhouette differences remain.
Offline previews of the finished pack should decode its CI8 payloads; calling
`native_tiles` alone shows the pre-calibration artwork.

Doom Guy and Marcus Fenix replacement faces are packed after the calibrated body. The packer
merges duplicate RGBA5551 palette aliases outside the head without changing their
decoded colors, then assigns the new face only to unused palette entries. This
keeps every body color exact even though face and limbs share a texture. The same
fitted head is resized into all three native distance layouts. Final validation
compares decoded non-head colors; index or palette-byte equality alone would be
the wrong invariant for this replacement.

Renderer copies are private to each selected actor/frame. They substitute
authenticated image and palette pointers, preserve the original draw commands,
and compose with existing MK64 item material effects. Native characters and
disabled/online paths continue through the original material implementation.

The showroom dispatcher (`80024EB8`) uses different graph renderers for the
bike (`8000F9E8`) and rider (`8000FF64`). Preview capture must wrap the rider's
actual graph traversal, including its shared early-return epilogue. Hooking
only the bike traversal leaves custom riders invisible in the selector even
when the race renderer works. Authentication still requires the selector's
active rider pool, local slot, donor and graph; bikes and weapons keep their
own textures. The native-call preview regression covers this wiring separately
from the lower-level texture substitution tests.

Preview ownership follows native modes 33, 35, 45 and 46 and validates their
shared callbacks; mode 35 also checks local player count. The renderer classifies
head material and native UV ranges rather than fixed pose-space bounds. This
keeps translated/high-detail showroom heads mapped correctly. Asymmetric faces
choose the texture half from the loaded batch's lateral center; native vertical
UVs are preserved. Private vertex copies are restored after each draw.

For a focused selector investigation, set both `RR64_DIAGNOSTICS=1` and
`RR64_RIDER_SKIN_PREVIEW_TRACE=1` before launch. Up to 128 fixed records capture
the actual preview gate, original actor links, donor headers and emitted image
addresses. Each graph/selection is sampled at most three times, spaced by native
frames. Rendering never performs log formatting or file I/O: the event thread
drains copied samples using a non-blocking lock. Records are diagnostic context,
not rendering authority; texture pixels are not recorded. `reason=0` means the
replacement list was emitted, not that it was visually correct. Reasons 1â€“8
identify ownership/graphics, command-buffer bounds, changed context, invalid
range, oversized list, texture staging, unmatched/unsupported commands, and
frame-copy failure respectively. Queue omissions are counted. Both switches
default off; the trace is optional evidence, not part of the appearance correction.

Headless suites cover archive failures, startup order, all local player slots,
save separation, texture identity, preview isolation, LODs and material effects.
These checks do not replace a visual test of the authored skins in the game.
