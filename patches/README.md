# Dependency patches — Android series

Applied by `scripts/dependency_patches.py` (copied from AeroGauge) via
`scripts/build_android.py` (`apply_platform_patches(ROOT, "android")`).
Order matters: RR64's own `dependency-patches/*.patch` are applied FIRST by
`scripts/setup_dependencies.py`; these Android patches apply on top of
them. All four were verified to apply cleanly on top of RR64's patches at
the pinned commits (2026-11-08) — see `android/probe/patches/README.md`
for the per-patch verification notes.

| Patch | Target | Platforms |
|---|---|---|
| `0019-rt64-android-cross-build.patch` | `native/lib/rt64` | android |
| `0020-plume-android-sdl-window.patch` | `native/lib/rt64/src/contrib/plume` | android |
| `0021-sdl-android-usb-receiver.patch` | `build-android/deps/SDL` | android |
| `0022-recompfrontend-android.patch` | `native/lib/RecompFrontend` | android |
| `0023-rt64-adreno-endian-swap.patch` | `native/lib/rt64` | windows, linux, android |

`0019` and `0022` are **adapted** from AeroGauge's versions (two hunks
each needed re-basing onto RR64's pins + RR64's own patches); `0020`,
`0021` and `0023` are verbatim copies. Provenance and hunk notes:
`android/probe/patches/README.md`. `0021` (SDL USB receiver registration
for API 33+, gamepad hotplug on Android 13+) was verified to apply cleanly
at the SDL pin used here (`5d24957...` = SDL 2.32.10); it is not needed
for the probe, so it lives only in this inventory, not in
`android/probe/patches/`.

## Not in this inventory (yet)

- RR64's own `dependency-patches/*.patch` are NOT duplicated here; they
  live in `dependency-patches/` and are applied by `setup_dependencies.py`.
