# Positional rival engines

## October 7: public 1.4.4 dropout report

The supplied pack-riding video was confirmed as public 1.4.4, before the private
thirteen-source reservation and separate effects pool described below. Those
existing private fixes remain the candidate for source competition/dropouts.
No additional production audio change was justified by this review.

The fixture now executes original note tuning, exponential pitch conversion
and final worker gain, rather than relying only on live handles. All 31 valid
model entries pass idle/acceleration/coast and approaching/receding cases;
thirteen mixed-family engines retain nonzero worker gain beside a fallen rider.
The device sample mixer, actual output and peak event/command occupancy still
require runtime verification.

A proposed pitch clamp was rejected: the native worker can silence an
over-range note without retiring its handle, but the ROM's actual engine note
tuning keeps the tested RPM/Doppler combinations below that limit. A deliberate
over-range control verifies the test detects that silence. Do not infer audible
output merely from a valid handle, or infer a pitch defect using neutral note
tuning instead of the authored sample metadata. Evidence is in project-root
`analysis/audio-replay-followup10-20261007/`.

This feature covers AI opponents in solo and local multiplayer, and
nonlocal human and AI bikes online. It builds on the original ROM-loaded bike
sounds. No new sound assets or SDL_mixer dependency are distributed.

## Listening behavior

October 5 follow-up: every nearby riding rival can now have an engine voice
(up to thirteen other racers). Fourteen dedicated rows include one temporary
RPM-loop replacement; the original music/effects pool remains separate.
This removes the three-bike cap and prevents ordinary effects or music from
taking a rival's channel. Native initialization grows the heap by 80 KiB and
sizes physical voices, DMA caches and command storage together. The fixed
storage is reserved at startup; turning Rival Engines off still skips their
sound production and updates.

The October 4 distance-fade fix is retained: a quiet rival previously lost its entire
gain in one update when another bike took its slot. Gain changes now use a
60 ms time constant (about 180 ms to complete 95% of a change), including
newly selected bikes, instead of subtracting a fixed full-volume step. The
volume curve applies independently to every nearby rival.
Native RPM changes also retain their existing loop if the replacement's gain
would be rejected by the native quiet-sound gate. The retained loop still
receives normal volume and pitch updates. Final integer gain can reach zero
at the quiet end of the range; no extra distant voices are kept indefinitely.
Offline checks and owner listening acceptance are recorded separately in
root `analysis/rival-distance-fade-20261004/`.
The owner tested Followup03 and reported that the audio seems fixed. That is
listening acceptance for this run, not additional Linux or Steam Deck coverage.

Audio > Game Audio > **Rival Engines** enables the managed engine sounds and
defaults to On. Off requests a stop for active rival loops at the next game
audio update, rejects further owned script-child voices, and skips subsequent
listener, bike-profile and rival sound processing. The choice is saved in
`sound.json` independently of **Rival Engine Volume**, which retains its 35%
default. Existing settings without the switch remain enabled.

The volume slider still fades sounds to silence at 0%; use the switch to skip
the rival update work as well. Main Volume still applies. The native local
player engine path remains intact, including each local split-screen player's
own engine. Online nonlocal native engines remain suppressed when Off so they
cannot return as unpositioned full-volume sounds. Crash, weapon, music and other
effects remain available.

Rival engines reuse the original shared engine sound bank. Off avoids rival
voice creation and updates; it does not unload samples needed by the player's
engine. There is no separate rival asset package to load or unload, and these
offline checks do not establish a measurable FPS or device-performance gain.

The manager includes every nearby riding rival. Sound stays on each bike;
the listener follows the local rider's body when detached. The view direction
determines stereo left/right. Volume reaches the slider's full level within
12 world units, then fades smoothly to zero at 320 units. Changes in selection,
volume and pan are smoothed. A passing bike joins the mix without replacing
another audible rival. Original bike profiles and engine state determine sample and RPM pitch.
The native traffic Doppler calculation adds a bounded, smoothed passing pitch
offset to the continuous engine loop; short race-start cues keep their authored
pitch.

Online peers listen from their own rider. The original nonlocal-human engine
dispatcher is suppressed so it cannot produce a second centered engine. Local
split-screen shares one speaker mix: each AI bike uses its strongest local
listener, rather than creating a voice for every viewport.

## September 29 audibility follow-up

The 1.4.2 implementation incorrectly accepted only bike model indices below
17. The ROM sound table has 32 pointer entries, including later choppers,
Scooter, both Insanity models and cop variants. The same restriction also ran
while finding the local listener: selecting an excluded chopper could silence
all managed engines, even rivals whose own models were accepted.

Listener authentication now validates actor/bike/body ownership and position
independently of sound metadata. Sources accept all valid entries in the native
table, checking their four effect IDs against the loaded effect bank. Reserved
model 22 points to unrelated packed data and is rejected as a sound source.
The original engine producer still chooses the ROM-authored sound family and
throttle/RPM pitch. Some models intentionally share sound samples; the change
does not invent unique recordings for each model.

The old fixed gain of 98.56 also limited maximum rival volume to 70% of the
native local engine baseline of 140.8, before applying the short squared
distance fade. The follow-up reads that native baseline from the ROM, reaches
the slider's full gain within 12 world units and fades smoothly to zero over
320 world units. These are game coordinates, not a claim of physical metres.
The saved slider value and its 35% default remain unchanged. At maximum, a
nearby rival of the same model and throttle state receives the same input gain
as a local player's engine; actual perceived balance still needs listening.

The per-frame scan remains bounded by fourteen racers. Sources beyond the
audible range fade out and release their voices. No performance claim for Steam
Deck follows from the offline producer benchmark.

Remote pitch uses the guest's native engine simulation after authority restores
the bike's movement. The native update still computes wheel/gear speed and the
smoothed engine state for active remote riders. Exact host throttle, gear and
RPM are not sent as audio fields, so their pitch can differ between peers. This
feature does not change packets or claim sample-identical engine audio online.

The passing effect uses the original traffic routine's horizontal relative
velocity calculation (`80057D48..80057DB4`) and ROM pitch constants. It reads
the source bike's velocity and the chosen listener's bike or detached body
velocity, rather than estimating motion from successive position corrections.
The same listener drives gain, pan and pitch. Listener changes, attachment
transitions and large teleports reset stale pitch offsets; invalid motion or
coincident positions retain the original RPM pitch. The scoped `8005753C` hook
adds the offset exactly once per native engine update, without changing local
player engines or unrelated effects.

## Native mixer protection

`rr64_rival_engine.cpp` calls original producer `800571DC` in a scoped context.
Its overrides do not apply to the local engine, attacks, weapon changes or other
effects. The stock `58600` gain threshold of 18 rejects quiet engine starts;
managed rivals permit gains above 1 so fade-in and a low volume setting work.

Admission examines the live native effects rows, not merely the racer cache.
It permits thirteen sustained engine rows, appended after the original eight
or sixteen effects rows. One additional row is allowed only while replacing an owned
RPM loop whose old row is awaiting release. Other loop transitions keep playing
their current sound until there is room. This prevents a full pack from stopping
all its loops and then denying every replacement. The temporary allowance cannot
start another bike or script child. Added voices use priority zero and cannot
steal another row.
Deferred releases remain occupied until the native audio worker frees them.
Native SFX allocation, unique-effect reuse and music-track allocation all stay
inside the original pool. Handle updates/stops and audio-worker processing still
cover the complete pool. This keeps the additional engines isolated without
reducing original crash, weapon, local-engine or music capacity.

Ownership uses both row address and native handle. A reused row cannot be stopped
as if it were an old rival sound. Script-child allocations share the same budget
and ownership safeguards; current engine scripts do not create children. The
manager uses fixed storage and no per-frame file output or heap allocation.

Recovery, invalid owners, dismounts, large teleports, pause, menu changes,
disconnect and highlight playback release the managed voices. Private prediction
does not emit, move or stop these sounds. Highlight audio remains governed by
its existing lifecycle; this feature does not replay recorded engine audio.

## Code and verification

- `native/src/rr64_rival_engine.cpp/.hpp`: source/listener selection, scoped
  native hooks, voice admission, ownership and lifecycle.
- `native/src/rr64_rival_engine_config.cpp/.hpp`: saved Audio option.
- `config/roadrash64.us.toml`: native dispatcher, producer, allocator and cleanup
  hook sites; `generate_prediction_frame.py` excludes live audio hooks.
- `native/tests/rr64_rival_engine_smoke.cpp`: production manager plus extracted
  original producer, dispatcher, allocator, volume, pitch and pan operations.
- `native/tests/rr64_rival_engine_init_cases.hpp`: real native heap, physical
  voice, filter, command-buffer and DMA initialization for both quality presets.
- `native/tests/rr64_rival_doppler_cases.hpp`: native traffic arithmetic oracle,
  approaching/receding motion, smoothing, fallen listeners, teleports and local
  and online listener mappings.
- `native/tests/rr64_rival_engine_config_smoke.cpp`: real Config save/load,
  compatibility with older settings, invalid values, callbacks and file backup.

Initial evidence is retained under root `analysis/rival-engine-20260927/`;
the audibility follow-up uses `analysis/rival-engine-followup-20260929/`. Offline
verification checks the real producer and native mixer contracts without opening
an audio device. It does not establish audible balance, physical stereo output,
Internet acceptance or Steam Deck frame times. Those require listening/runtime
confirmation. Windows and Linux build results are recorded there when complete.

The October 4 dropout regression executes clustered native idle, acceleration
and coast changes, including both Insanity models, delayed worker releases,
reserved effect capacity, and passbys beside a fallen rider. The old manager
fails the clustered-transition check. Windows passes 31,811 checks with the
fix; the 20,000-frame bounded fixture allocates no host heap memory. Evidence:
root `analysis/rival-audio-dropout-20261004/`. Audible confirmation remains pending.

The September 29 follow-up compiled the complete game on Windows and Linux.
Both passed eleven relevant suites, including 30,180 rival-engine checks,
45,525 online-audio checks and the retained campaign save/label tests. Separate
writer/reader processes also passed the save fixture. Isolated negative controls
restoring the old model guard, old gain ceiling or removing Doppler each failed
the corresponding new regression. The long bounded producer loop allocated no
host heap memory; this remains offline evidence, not a hardware performance or
listening result. No game or audio device was opened for these checks.

In 1.4.2, online peers must share protocol65. Listening and hardware coverage remain limited.
