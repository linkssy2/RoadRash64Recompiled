# Android boundary patches for the probe (and the full game)

Provenance: adapted from AeroGauge's `patches/0019`, `0020`, `0023`
(`alondero/aerogauge-recomp`, PR #60). AeroGauge's RT64/plume pins differ
from RR64's, so the patches were re-based and re-verified here — see the
status lines below. The full-game patch inventory (`patches/series.json`)
uses the same files.

## 0019-rt64-android-cross-build.patch — RT64 (Android only)

- Host DXC on Android (`${RR64_HOST_DXC}` — renamed from AeroGauge's
  `${AERO_HOST_DXC}`); skip building `file_to_c` and
  `nativefiledialog-extended` on device targets; `PLUME_SDL_VULKAN_ENABLED`
  for Linux **or** Android; link SDL2 on Android too; drop the
  `static_assert(false && "Android unimplemented")` in
  `rt64_application_window.cpp` (bounds computation site only — a second
  identical `static_assert` remains in the same file at the `SDL_GetWindowWMInfo`
  site, but it is dead code once `RT64_SDL_WINDOW_VULKAN` is defined: the
  `#elif defined(RT64_SDL_WINDOW_VULKAN)` branch wins over the `__ANDROID__`
  branch).
- File dialogs stubbed (no-ops on Android; ROM import belongs to the Java
  launcher).
- **Vulkan capability gate**: on Android, `setup()` returns
  `GraphicsDeviceNotFound` when `descriptorIndexing` or `scalarBlockLayout`
  is missing.
- **RGBA swapchain** (`R8G8B8A8_UNORM` on Android vs BGRA desktop) in
  `rt64_application.cpp` and the fullscreen pipeline in
  `rt64_shader_library.cpp` — keep in lockstep with RecompFrontend's
  `SwapChainFormat` (patch 0022).

**Adaptations vs AeroGauge's 0019** (verified 2026-11-08 against RT64
`6f1c2d99` + RR64's `native_lib_rt64.patch` applied on top):
- DXC hunk: upstream's `add_compile_definitions(NOMINMAX)` became a
  multi-line block (`NOMINMAX`, `__PRFCHWINTRIN_H`); context fixed.
- Capability-gate hunk: RR64's patch changed the device-info print below
  the insertion point (now gated on `RR64PipelineDiagnostics::enabled()`);
  context fixed. The gate itself is unchanged.
- All other hunks applied cleanly, including on top of RR64's patch.

## 0020-plume-android-sdl-window.patch — plume (Android only)

Verbatim copy of AeroGauge's 0020. Verified to apply cleanly on top of
RR64's `native_lib_rt64_src_contrib_plume.patch` at plume `d890ac89`.
- `PLUME_SDL_VULKAN_ENABLED` allowed for `IS_LINUX OR ANDROID`.
- `RenderWindow` typedef: the SDL-Vulkan `SDL_Window*` branch now wins
  over the `ANativeWindow*` branch on Android.
- On Android + SDL-Vulkan, `vkGetInstanceProcAddr` comes from
  `SDL_Vulkan_GetVkGetInstanceProcAddr()` (`volkInitializeCustom`) instead
  of `volkInitialize()`.

## 0023-rt64-adreno-endian-swap.patch — RT64 (ALL platforms)

Verbatim copy of AeroGauge's 0023. Verified to apply cleanly on top of
RR64's `native_lib_rt64.patch` at RT64 `6f1c2d99`.
- `EndianSwapUINT16` in `src/shaders/FbCommon.hlsli`: `|` -> `^` (the two
  masked fields occupy disjoint bit ranges, so the result is identical).
  The Adreno 750 system driver rejects compute pipelines whose shader
  contains the OR form (`vkCreateComputePipelines` returns
  `VK_ERROR_UNKNOWN`, then a null pipeline handle crashes the driver on
  bind). Ships on desktop too — it is a driver workaround, not an Android
  workaround.
