# Completed Big Game saves and Insanity selection

## October 4 — restore earned race levels before selection

Thrash previously saw only the initial unlock flags until the player opened
Big Game's Load Game menu. That menu grants ordinary levels while drawing each
valid save's label; the accepted-record hook restored only completion rewards.

The main-menu transition now reads the existing native Pak cache before race
selection. Accepted records restore their earned ordinary levels as well as
the existing completion and Insanity bonus rewards. This does not select or
copy a campaign into the live profile, write a save, or grant unearned levels.
Automatic scanning suppresses the native scanner's invalid-record deletion.
The native cache avoids additional disk reads on subsequent menu entries.

Cold-start, invalid-save and cache regression coverage uses the native scan
and the production menu hook. A fresh launch is still needed for visual
acceptance of the reported startup sequence.

## October 1 — show the original ending once

The original results screen (`73054`, branch `7327C`) tests whether every track
in the current chapter is qualified. In Level 5 it enters ending mode `38` each
time that condition holds, including when replaying completed races for money.

The result initializer (`72E74`) now snapshots whether the original campaign was
already complete **before** its native prize and qualification writes. A newly
earned final qualification retains the original ending, credits, reward and
Save Game return. Subsequent Level 5 results use the ordinary campaign return,
without another ending or completion award. Payouts, repeat-prize divisors,
inventory, bike selection and cash remain native.

Completion comes from all eight original Level 5 qualification nibbles in the
saved profile (`+50`), so existing completed saves retain this behavior after
restarting. The transient results-screen decision is rebuilt at each result
initialization; it is not the persistent source of completion. No save format,
sidecar, achievement requirement or automatic save is added. Optional Level 6
progress and its existing backward-compatible save extension are unchanged.

The focused fixtures exercise first completion on each of the eight tracks,
repeat wins and losses, saturated counters, unchanged native cash/bike data,
legacy native Pak loading, and separate-process reloads. Visual confirmation
still requires a user-authorized game test.

Private September 26 follow-up. No game was launched for these changes.

## Completion save

The original `73054` terminal branch sends Level 5 completion to mode `38`.
That mode runs `22C90`: the completion summary, original-gang reward and credits.
Its final exit at `23810` previously requested main-menu mode `1`, leaving no
opportunity to save the newly completed campaign.

The exit now returns a genuinely completed profile to campaign-menu mode `2F`
with Save Game highlighted. The original `73658` initializer remains intact:
it selects track zero when all eight Level 5 tracks are qualified and leaves the
campaign at valid level index four. It does not charge a race fee or alter the
profile. The user still chooses a native save slot and confirms any overwrite.
The original ending, credits, gang rewards and achievement hooks remain.

When native Pak scanning accepts a completed saved profile, the new helper
restores the same normal and special tier flags that the original ending grants.
It also verifies the copied profile's version and checksum without changing the
record. Completion follows the native rule: level index four and all eight final
qualification nibbles nonzero. The native reward is Insanity for original-gang
values one/two, Scooter for three/four, and the cop tier for zero. Profile +20 is
assigned when joining the original gang; it is not the difficulty setting. No new save
format, sidecar unlock file, achievement dependency or automatic overwrite is
introduced. The October 4 startup restoration above removes the earlier need
to visit Load Game before using earned tiers in Thrash or local multiplayer.

## Offline verification

`RR64CampaignBikeSmoke` extracts the production generated native ending-exit,
campaign initializer, accepted-save branch, unlock helper and solo selector.
Its private-ROM test passed:

- Ending exit reaches the native Save menu; profile bytes, money, stack and
  return address stay intact. Later ordinary entries retain their original
  behavior, and a completed record chooses a valid replay track.
- Thirty saved rewards (six slots, five original-gang values) match the original
  `5F420` unlock helper. Incomplete levels/tracks, invalid versions, corrupt
  checksums and the native rejected-checksum branch do not grant rewards.
- Ninety solo Insanity selections (fifteen map tiers, Custom Cop off/on, bike
  sequence 25/26/25) preserve both preview/selection fields and the selected
  model in the race-profile helper. The actual character-preference restore is
  included. The two ROM preview mappings select distinct resources 66 and 67.

The reported single Insanity variant was **not reproduced** by this fixture or
the surrounding native-source audit. The selector is unchanged. These checks do
not establish on-screen appearance, disk/Pak round-trip behavior or a full live
campaign completion. Those remain gated on the user's fresh permission to test.
Private output: `build/campaign-bike-smoke.log`.

## Archived input diagnostic

Candidate02 temporarily captured native selection input and model changes.
That run is complete and the selection-only probe has been removed for 1.4.1.
The shared optional race recorder and meaningful native fixture remain.

## Candidate02 owner acceptance

The September 26 11:09 test is complete. The owner confirms that both solo Thrash
and local multiplayer Thrash allow changing Insanity bikes. Solo tracing records
a two-entry list and matching selected/committed/preview models changing
25 -> 26 -> 25 -> 26 -> 25. Its 220 records have contiguous sequence numbers and
no loss/cap notice. Multiplayer acceptance comes from the owner's visual report;
the recorder is intentionally scoped to solo Thrash.

No selector gameplay change was applied, and the original failure was not
reproduced. Keep the working implementation; no further selection-only run is
required. Evidence is in root
`analysis/race-rank-20260926/session-20260926-110922-325/`.
This acceptance does not establish live campaign-save or race-ranking behavior.

## September 27 optional bonus extension

The owner subsequently requested optional Insanity races after the original
ending. [The bonus campaign contract](insanity-campaign.md) extends completed
Level 5's purchase/Join path into eight replayable bonus races. It preserves this
original ending and Save return. Its versioned Pak tail stores the additional
qualification word; the 248-byte original profile remains backward compatible.
The earlier no-save-format-change statement describes the September 26 fix.
