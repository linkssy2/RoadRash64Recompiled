# Local multiplayer populations

Regular Road Rash tracks expose Traffic, Pedestrians and AI Cops independently
in the multiplayer race options. Traffic is On/Off; pedestrians retain the native
density choices. AI Cops defaults Off and no longer requires Custom Cop Mode.
When enabled, three native police actors join the existing roster of at most
eleven race entrants, staying within the original fourteen-actor allocation.
The same setting continues to govern AI allies in Custom Cop Mode.

`local-race-options.cfg` version 6 keeps the existing packed preferences and adds
one trailing traffic value. Versions 1–5 still load. Single-player Thrash has
its own preset; online setup packets and online rules are unchanged.

Imported courses keep their existing Unavailable options because they do not
provide the original traffic, pedestrian or police road graph.

A busted local player watches the next unfinished human racer. Both native
camera preparation routines temporarily use that rider and the stock chase
camera, then restore the original mapping. Controls, HUD identity and results
remain owned by the busted player. Finished, wrecked and busted riders cannot
be spectator targets. Highlights and online cameras keep their existing owners.

The original arrest routine (`80061224`) already clears eligibility and reduces
the remaining-human counter. The ordinary multiplayer finish evaluator
(`800740A0`) waits for that counter to reach zero. Tag and other score-based
modes retain their native victory rules; Custom Cop retains its existing rules.

Offline checks cover independent option input, police capacity, traffic
persistence, every local spectator slot, exact camera-scope restoration, online
and highlight exclusion, native finish evaluation, campaign bikes and imported
course options. Root evidence: `analysis/splitscreen-world-20261013/`.
Visual camera transitions and real police encounters still require playtesting.
