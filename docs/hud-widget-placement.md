# Native HUD widget placement

September 19 private candidate, not visually accepted yet.

## Visual regression and origin correction

The 9282059C candidate failed the user's visual test: nameplates and countdown
were placed per view, but the world became pillarboxed/retained stale side strips
and speed readouts disappeared. Pause Options also remained misaligned.

Root cause traced in RT64 `RDP::movedFromOrigin`: explicit-origin coordinates
are relative, and RDP adds `origin * colorImage.width * 4 / 1024`. Sending absolute
coordinates doubled the right-edge offset. A 320-wide HUD scissor became 640 wide,
altering framebuffer-pair bounds used for world widescreen placement. Right-side
labels likewise moved outside the screen. Push/pop scissor alone did not prevent
this because submitted HUD bounds affect framebuffer-wide calculations.

The follow-up subtracts the origin from rectangle offsets and both scissor edges.
Tests now decode the emitted commands through those coordinate semantics and
check effective bounds, instead of only checking command IDs and restoration.
Pause labels use one fullscreen center scope, preventing the Options label from
being classified as the lower player's health HUD. Countdown copies are retained.
205 assertions and production compilation pass; live visual confirmation pending.
The follow-up stage is `pending-hud-origin-correction`; build.json records its hash.

The prior per-draw anchoring candidate separated the two-player nameplate text
from its backing. The user also reported 3/2/1 crossing the viewport divider.
Do not describe the prior mathematical checks as proof of visual correctness.

`native/src/rr64_hud_widgets.cpp` adds placement at native producer boundaries:

- `80030220` brackets HUD text records. Capture layout **after** the online
  presentation override starts, and finish before it restores simulation inputs.
- The 64-record text queue starts at `800D9650`, with 88-byte records. The full
  label's authored origin at +40/+44 chooses its corner once. Each record's
  consumer (`80079780`, loop at `80079818`) receives one standard RT64 extended
  rectangle anchor covering both its backing and all glyphs.
- Push a per-player scissor before drawing the label; pop it and restore neutral
  rectangle alignment immediately after `8000DEF4` returns. Clear metadata at
  queue drain. Menu text and later queue users must not inherit these settings.
- The exact countdown sprite enqueue is `8006AF20` in `8006A638`. It uses the
  shared phase at `800A6600`, sprite table at `800A6560`, and the full framebuffer
  center. Duplicate only the record appended by this call, giving each local
  view a centered half-scale copy. Keep its asset, tint and phase unchanged.
  Other messages/sprites and race timing are not replaced.

Both sprite buffers retain the native 96-record limit. Invalid/reused spans and
insufficient space leave native records intact. Extended commands use the native
display-list pool bounds and preserve headroom for the label and restore command.
No texture files, fonts or original-game assets were added.

This is a scoped overlay-placement correction, not a complete HUD rewrite.
Health/weapon/speed graphics still use the existing per-view renderer placement.
Online's single viewport and Big Game retain their existing layout; local
two-player halves and three/four-player quadrants use the captured HUD layout.
Cloud removal remains enabled for all game types.

Verification: production build, 173 native-widget/queue assertions, 28 existing
renderer-coordinate cases, cloud-filter fixture and RT64 dependency reverse
check passed. Generated hooks were inspected at loop/exit and countdown call
boundaries. No live test was launched. Visual approval of corner placement,
long names, countdown size and 3/4-player layout is still pending.

Private test executable: `analysis/release-1.2-online/sky-review-20260919/`
under the workspace root; preserved stage `pending-hud-widgets`.
SHA256: `9282059CEE3972A9D954A2B4000D7F8943182CDDF4404FD14E39C6201249C678`.
Wait for the user's next ready before launching.


## October 16: issue 30 follow-up

The single-view analog dial uses native 12668 orthographic model rendering.
Scoped viewport/scissor anchoring now moves it as one right-edge widget.
Whole-label placement also covers single-player messages and speed text,
with pause labels kept centered. 229 HUD checks, quadrant checks and Windows
production build pass; visual and Linux checks remain pending. See local
`analysis/hud-issue30-20261016/NOTES.md` for trace and test identity.
