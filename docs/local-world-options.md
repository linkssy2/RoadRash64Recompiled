## October 16 - population parity follow-up

Traffic and pedestrian choices match solo Thrash hardware profiles. Police
already matches 0/1/2/3 officers and native encounter density. Racers share
the 11-entrant ceiling; each additional human replaces an available AI slot.
Windows: 601 regression checks passed. Live and Linux follow-up pending.
Campaign authored populations and online native authority remain unchanged.

# Local multiplayer populations

## October 16 - Heavy traffic experiments withdrawn

Owner requested restoring pre-experiment Heavy traffic and deferring a complete
traffic AI rewrite. Native density, twenty-car capacity, stop/avoidance rules and
spawn cadence are restored. Extra starting cars and relocated roster allocation
are removed. Existing far-view presentation/spawn-distance support remains.
Accepted police placement/parked-cop and global hill-contact fixes remain.
Private candidates and logs are retained under analysis/issues-29-30-20261016.

Regular Road Rash tracks expose traffic, pedestrians and AI police independently

in multiplayer race options, including one human against CPU racers.

Police choices are No Cops / A Few Cops / Some Cops / Too Many Cops.

They reserve 0–3 native officers and set the native road-encounter density to

0 / 0.2 / 0.25 / 1. The earlier On/Off implementation reserved actors but left

that density at zero, preventing encounters. These settings do not change aggression.

Traffic choices are No Traffic / Light / Some / Heavy, using native density

0 / 1.5 / 5 / 9 (or 0 / 1.5 / 2.5 / 4 for the native reduced hardware profile), matching solo Thrash with the original scheduler. The native twenty-vehicle
capacity and road-validity checks remain in place. Pedestrians remain independent.

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



October 16 police/traffic follow-up (discussion #28): police placement now calls
the native road validator `8006B9E8` before `8006B740` resets the rider. Both use
the same road-type/bridge checks. Previously a rejected placement could clear
the rider's attachment and change route/ranking state without relocating the bike.
The preflight uses scratch state; valid placements retain the native reset.

Offline evidence: `analysis/issues-29-30-20261016/police-regression.py` compares
45 road-type/flag combinations against the original rejection path. Rejected
placements preserve rider and route state; accepted cases still reach the reset.
Local race-option and campaign-bike smoke checks pass. The exact reported
encounters, heavier traffic performance, split screen and online remain untested
in live play. This corrects a demonstrated reset-order defect; the videos alone
do not establish that it accounts for every officer found on the ground.


October 16 traffic follow-up, corrected after launch-06: solo Thrash Heavy is
4 or 9 (native hardware profiles), not multiplayer's 1. The earlier 1-to-4
conversion missed the tested solo setting. Configurable races now normalize
those Heavy values to 36 at scheduler initialization, giving four times the
native maximum encounter frequency; lighter and disabled values stay intact.
The twenty-car allocation cap and native placement validation remain unchanged.
Big Game and campaign co-op retain authored populations, per owner clarification.
Shared hill-contact timing and safe police placement remain enabled in all modes.

Police-Traffic03 adds opt-in RR64_POPULATION_TRACE=1 using the existing bounded,
UI-drained queue: placement before/after snapshots, police attachment/active/state
changes, and periodic car counts/density/spacing. No guest-thread I/O or allocation;
1200 samples maximum per process, prediction excluded, disabled by default.
The previous test still showed a fallen officer: the preflight fixes a proven
rejected-placement mutation, but does not yet explain every detached officer.
Next live test: solo Thrash Level 1 Race 1, Heavy Traffic / Too Many Cops.

Windows build, 551 local-option checks, population/disabled/existing trace checks
and ground-contact checks pass. Live density, fallen-cop cause, split screen,
Linux and online remain unverified. No launch or publication authorized.

October 16 Police-Traffic04 follows the failed launch-07 traffic test. The 36
rate filled all 20 car slots (40 captured samples at capacity), with 26.99 route
units between events. This setting is withdrawn: configurable Heavy uses the
native maximum 9, not a fourfold multiplier. Big Game/co-op remain unchanged.

Native 79334 catch-up invokes 79230 up to three times with the same proposed
position. A direct compiled-native fixture reproduced this. Configurable race
spawns now skip a proposed location within 30 route units (one second at native
initial speed) of any active car, before allocation. Existing cars, lane choice,
steering, braking and native road checks remain intact. The loop is bounded by
the native twenty-car table and skips retired/invalid records. Client ownership
still exits before this check. This is a pile-up prevention candidate, not proof
that every stopped car has been fixed.

The opt-in population snapshot also records each car's route position, direction,
current/target speed, active state and position, using the same bounded queue.
Next live run must verify flow and distinguish braking from stalled updates.
# October 16: Heavy traffic flow candidate

The owner verified the parked-police watchdog fix in launch-10. Heavy traffic
still queued at a road-type boundary: the trace recorded a positive target speed
being replaced with zero by the native road-event gate, followed by other cars
braking behind it.

For configurable Heavy traffic, restore only that gate's zero target from the
earlier avoidance result, capped at the native 30-unit cruising speed. The hook
runs before the native route-end checks. Earlier following stops, route-end
stops, ground checks and collisions remain active. Light/normal traffic and
Big Game/co-op retain the original gate. Online clients retain host-owned cars.

The extracted native gate passes 96 forward/reverse, endpoint and prior-braking
cases; the local-options suite passes 681 checks. Live flow and collision
acceptance remain pending. Evidence: `analysis/issues-29-30-20261016` in the
parent workspace; this candidate is private and has not been published.

