# Controller remapping and native input

## September 23 report

A player reports an 8BitDo 64 Bluetooth Controller on PikaOS 4 (Debian Sid):
the frontend recognizes/remaps B, but B cannot back out of the original game
menus. A works. Their exact download, controller mode, kernel, active profile
and diagnostic log have not yet been supplied.

## Confirmed source defect

The Controls page exposes assigned device profiles even during single-player
play. However, `profiles::get_n64_input` and the dedicated-action reader select
the global single-player profile whenever the native game is in single-player
mode. Editing the assigned profile can therefore leave gameplay reading the
old default. This mismatch is independent of Linux and does not require a
missing hardware button. An A binding shared by both profiles can work while
a newly remapped B binding appears ineffective.

The overlay also has independent profile choices for Back, menu toggling and
button prompts. Repairing only the native B bit would leave inconsistent
behavior. The fix must use the assigned profile without overwriting saved
custom bindings or merging input from another assigned player's controller.

## Separate mapping layers

The gameplay **B** (heavy brake / go back) and overlay **Back (Menu)** are
separate actions.
The common dual-stick defaults use SDL X for native B; this controller's
physical B can be SDL B. Bind the gameplay action as well as overlay Back.
Recognizing a button in the scanner alone does not prove it reached the native
game as B (`0x4000`).

The earlier Linux controller logs contain working raw and mapped B events.
That is evidence about the earlier reporter, not this new PikaOS installation.
Linux's [Nintendo driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)
maps N64 A and B independently. SDL's N64 HIDAPI path likewise preserves
button labels in its Switch driver (SDL 2.30.3 then, SDL 3.4.16 now:
[SDL_hidapi_switch.c](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapi_switch.c),
`AlwaysUsesLabels`).
Do not apply a global A/B swap or attribute this report to the prior L3 driver
limitation without this reporter's evidence.

## Diagnostic handoff

If B still fails after the routing repair, retain the exact build and selected
player's controls configuration. Existing opt-in flags are sufficient:
`RR64_DIAGNOSTICS=1` records device identity/mapping,
`RR64_CONTROLLER_BUTTON_TRACE=1` records the first 256 raw/mapped button events,
and `RR64_INPUT_TRACE=1` records changes to the final offline port-one input.
Press A and B early, then try a native menu with an available Back action.
Correlate raw input, SDL input, selected binding and final native mask before
changing a driver mapping. No ROM or private save is needed for this report.

The source defect can be reproduced with virtual input; hardware, Bluetooth,
PikaOS and live menu acceptance remain separate verification requirements.

## Repair scope

Native buttons, stick directions and dedicated actions resolve the selected
player's assigned profile. Controller polling uses that player's actual device
when assigned; the legacy all-controller fallback remains only before any
assignment in single player. Overlay events resolve the emitting controller's
selected profile. Invalid/unassigned profiles do not index binding arrays.
Selecting or swapping another device preserves existing slots' selected layouts.

Choosing Keyboard for Player 1 in single-player mode starts with the existing
working Keyboard (SP) layout. An explicitly chosen empty or custom layout stays
as chosen. Existing profile contents are not migrated or overwritten. The
existing UI and physical button defaults are unchanged.

Historical limitation at the September 23 repair: choosing a different saved/custom layout in the
dropdown changes its current player assignment, but that association is not
persisted in the controller registry. Its edited bindings are saved; the layout
may need to be selected again after reconnect/restart. Editing the device's
default assigned layout does persist. This repair does not claim to change that
separate saved-layout association behavior. The September 29
[controller-remap follow-up](controller-remap-followup.md) addresses association
persistence and editor/scanner synchronization, with its own verification scope.

## Offline verification

The actual profile, mapping, binding and SDL polling code passed 113 focused
checks on Windows and Linux using virtual controllers. The old compiled source
reproduces the missing native B bit while the normalized SDL B button is held.
The fixture also executes the extracted production assignment function for
unchanged slots, empty keyboard layouts and device swaps. UI/window state and
file transport are bounded substitutes, not a rendered menu test.

Windows production builds passed with imported courses both enabled and
disabled. A fresh Linux build with courses disabled passed, along with existing
platform controller and assignment checks. Exact hashes and limitations are in
`analysis/controller-profile-routing-20260923/` under the workspace root.
Nothing was launched as a game or published. The new reporter's physical
controller and PikaOS installation remain unverified.
