# 1.4.7 compatibility

Use matching protocol 68 builds. Imported racer guidance is owned by the host.

# Multiplayer and course ownership in 1.4.7

Online remains experimental. Preserve original local multiplayer behavior unless
a change explicitly requires altering it. The active implementation is the
host-authoritative runtime in `native/src/rr64_netplay.cpp` and its authoritative
step/prediction helpers. Earlier relay/lobby prototypes are not foundations for
new work. 1.4.7 uses protocol **68**; all participants must use the same release.
Public 1.4.2 protocol 65 sessions are incompatible.

- The host owns shared settings, race state and pause. Each participant selects
  their own rider/bike and uses their own full-screen presentation.
- Local race presets and single-player Thrash have separate persisted state;
  share option semantics without enabling split-screen ownership for solo play.
- MK64 Music is remembered across imported races within those presets. Online
  uses the host's session choice. Earlier per-course enabled bits migrate to On.
- Imported courses synchronize by stable course/pack identity. Missing, disabled
  or mismatched packs must be rejected before racing. Every participant converts
  their own ROM locally; the host does not supply assets.
- MK64 item awards and hits remain host-authoritative. Local weapon-HUD focus
  must not mutate inventory. Private prediction omits live rendering and audio
  callbacks; retain effect snapshots and per-view rendering ownership.
- Protocol 66 carries the current stock wall/building contact rules. Preserve
  native collision ownership and recorded replay presentation separately.
- Tag and Deathmatch retain native point-based completion rules, including on
  imported courses. Circuit progress must not turn them into lap-finish races.
- Offline cheats remain suppressed during every active online session, including
  transition and failure paths. Overlay state alone is not an authority boundary.
- Highlights replay recorded presentation without rerunning physics, damage,
  inventory grants or race progression. The host owns online highlight controls.
- Proximity voice follows the rider body, including ejection; bike position and
  HUD viewports are not the speaking body's position.
- Optional native rider skins are offline/local appearances only. Their host
  catalog and local preferences do not alter native save IDs or synchronize
  custom appearances online.

See `RELEASE_CANDIDATE_EDITING_GUIDE.md`, `mk64-items.md`, and `rider-skins.md` for
implementation ownership. Offline tests and a solo Tag test do not establish WAN
smoothness, live online parity or the full larger-player-count matrix. Keep
packet/session evidence private and separate from source exports.

## Local additions in 1.4.7

Local Big Game Co-op uses two players and one campaign owner. See
`big-game-coop.md`. Split-screen traffic, pedestrians and AI cops are separate
options; eliminated players spectate survivors (`local-world-options.md`).
CPU skin randomization is a local cosmetic choice, including online, and mixes
stock riders with enabled More Characters skins. Cheats remain offline only.
