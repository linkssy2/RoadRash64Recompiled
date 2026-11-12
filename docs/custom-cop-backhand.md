# Custom Cop backhand input repair

The previous cop adapter consumed C-Down as a trick alias. This also intercepted
right-stick down, whose native action is the backhand attack. Dedicated weapon
trick input now carries the reserved N64 button bit 0x40 through the existing
16-bit input snapshot. The shared actor-control adapter consumes this bit and
converts it to the native trick action; directional attack bits remain unchanged.
Non-cop riders in Custom Cop also retain their ordinary down attack. LB tap/hold
handling remains separate. Protocol 38 rejects peers with the old interpretation.

Offline checks cover down attack held/pressed, dedicated trick and marker removal,
wheel-jam chord, and existing siren/shout cases. Production Release and existing
roaming/HUD checks pass. Live backhand animation verification is pending.

Live acceptance, September 19: user confirmed 'yep its fixed' after testing the protocol38 private build (SHA256 274BE56D22BB648AA12018E40952A07C918AE40407F9F1F49FB7DA9B48AEC6AF). This confirms the reported cop attack symptom in that test; paired online input verification remains separate.
