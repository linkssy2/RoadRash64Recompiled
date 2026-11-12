# Road Rash 64 Recompiled 1.5.0

- **Traffic lifecycle:** keep cars grounded near eligible local players while allowing distant cars to retire and their slots to be reused. Native traffic density and the 20-car limit are preserved.
- **SDL3:** migrate windowing, audio, controller input and menus to SDL 3.4.16, including saved controller profile migration.
- **SIMD improvements:** add vectorized renderer paths with AVX2 runtime dispatch and deterministic floating-point settings. Thanks to ClickClick5 for PRs #41 and #42.
- **MK64 startup recovery:** return to a usable launcher when imported-course initialization fails, with guidance to reimport or disable the pack.
- **Music selection:** constrain long soundtrack lists to a scrolling dropdown with keyboard selection visibility.
- **Big Game Co-op:** initialize both native cameras after the human roster is ready.

**Compatibility:** an x86-64 CPU with AVX support is required; AVX2 is optional.
Windows packages include SDL3.dll. Extract the full package into a new folder
and keep your previous installation and saves while checking the update.
More Characters and the texture mod remain separate optional downloads.

Big Game Co-op, online play and MK64 tracks/items remain experimental.
One completed Windows multiplayer Thrash Heavy Traffic race retained traffic
near the finish and demonstrated slot reuse. Longer races, separated local
split-screen views, full co-op results/highlights, and online host/client
sessions have not been revalidated for this candidate. Its test process returned
exit code 1, so a clean shutdown has not been established. Linux AppImage
preparation is pending; do not treat the older Linux package as this version.

Supply your own supported Road Rash 64 ROM. Importing MK64 tracks also requires your own supported Mario Kart 64 ROM.
