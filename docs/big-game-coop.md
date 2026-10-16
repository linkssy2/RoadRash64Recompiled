# Big Game Co-op — experimental

Choose **Big Game Co-op** on the original main menu. Assign controllers to
Players 1 and 2 in the Controls overlay first. Player 1 runs the normal New/Load,
shop and campaign menus. Choosing a race opens the native two-player rider/bike
selector; both players confirm before loading.

- Player 1 keeps the purchased campaign bike and can choose a rider.
- Player 2 chooses a rider and a bike from the campaign's current tier, including
  Insanity. The partner's bike-list choice is remembered per tier during this
  session. Existing character preferences handle rider persistence.
- The best finished human determines qualification and one native campaign
  payout. A busted or wrecked human does not end the surviving partner's race;
  their viewport spectates the partner. Both eliminated follows campaign failure.
- Only the owner's campaign profile is saved. The guest does not spend money or
  replace the owner's purchased bike. This is local two-player co-op.

## Implementation boundaries

`rr64_campaign_coop.cpp` owns the temporary selection/race state. Hooks in the
recompilation config route the campaign's pre-load step through the existing
multiplayer selector, reserve two humans before actor allocation, and redirect
native award readers to the best finished human. Original campaign arithmetic,
save serialization, chapter advancement and endings remain in use.

The selector temporarily borrows the campaign course state and native shop's
scratch bike entry. The course descriptor, level and track are restored on
confirmation or cancellation. The two-player roster stays within 14 actors.
Input mode changes are queued to the UI thread. Returning to the main menu or
entering an online session retires co-op ownership. Solo and regular local
multiplayer do not activate this adapter.

## Verification and playtest

`RR64CampaignCoopSmoke` exercises routing, cancellation, roster limits, separate
bikes, elimination and online exclusion. `RR64CampaignBonusSmoke` runs the
generated native constructor and award routines for all six campaign chapters,
including a busted owner with a qualifying partner. It compares the resulting
profile against exactly one original payout. Existing campaign bike/save, local
options and audio checks are also run. Build identities and results live in root
`analysis/biggame-coop-20261013/`.

The owner tested the feature candidate successfully. Before relying on a campaign save, verify both controllers and
previews, a normal finish, each player's bust/wreck while the other continues,
Save/Load after a partner qualification, cancellation, and return to solo play.
Keep a backup of campaign saves while testing this experimental mode.
