# Local multiplayer populations

Regular Road Rash tracks expose traffic, pedestrians and AI police independently
in multiplayer race options, including one human against CPU racers.
Police choices are No Cops / A Few Cops / Some Cops / Too Many Cops.
They reserve 0–3 native officers and set the native road-encounter density to
0 / 0.2 / 0.25 / 1. The earlier On/Off implementation reserved actors but left
that density at zero, preventing encounters. These settings do not change aggression.
Traffic choices are No Traffic / Light / Some / Heavy, using native density
0 / 0.25 / 0.75 / 1. Heavy retains the native vehicle capacity rather than
allocating an unbounded population. Pedestrians remain independent.

`local-race-options.cfg` version 7 stores packed preferences, traffic and police
choices. Versions 1–6 remain readable. AI police default off; previous On maps
to Too Many Cops. At most eleven entrants plus three police fit the fourteen
native actors. Online setup packets and rules remain unchanged.

Solo Thrash and local multiplayer also offer separate Racer Wpn and Cop Wpn
selectors: Default or any of the fourteen native weapons. These replace only
CPU starting equipment; pickups, weapon loss and human equipment stay native.
They share `cpu-weapon-options.cfg` and are excluded online. Big Game retains
its original equipment progression.

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

October 15 targeted Windows/Linux evidence: `analysis/cpu-customization-20261015/`.
Live population and menu verification remains pending.
