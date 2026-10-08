# Frame-pacing investigation and candidate 01 — 2026-09-20

Status: implemented, Windows/Linux production builds and eight offline test invocations per platform passed. First user run completed on D3D12; Vulkan changes still await live verification. Public 1.3.1 Revision 2 unchanged. Private candidate in analysis/frame-pacing-20260920/RoadRash64-Frame-Pacing-01-Win64. Executable f00e0c898da27f981d78ca46d92823427690e3a3a3d10d73ea4531dcfce43b6a.

## Findings and limits

Users report occasional race hitches resembling 30 FPS, more obvious without VRR. The saved heavy-race Performance 16 log (`analysis/performance-20260909/session-20260910-063810-830/runtime.log`) shows authored frames averaging 22.624 ms (maximum 48.914 ms) and presentation intervals averaging 19.60 ms (maximum 64.57 ms). Matching averaged 14.654 ms versus 0.633 ms for GPU command preparation, with interpolation mostly rejected. This predates 1.2 but belongs to the retained runtime lineage.

The recent September 19 sky diagnostic mostly shows 60 authored/present submissions per second with isolated 32.9 ms gaps. It covers attract mode, not a fixed-refresh player race. No supplied log measures actual display scanout or establishes G-Sync as the cause. These findings do not establish that all geometry hitching is fixed or support a percentage improvement.

## Changes

- Plume Vulkan treated `VkSurfaceCapabilitiesKHR.maxImageCount == 0` as the minimum, wrongly clamping requested triple buffering. Zero now means unlimited; actual finite surface limits remain respected. Present IDs reset after successful swapchain recreation/release, so waits cannot target the retired chain. A new helper and 17,400-case offline check cover both. References: https://docs.vulkan.org/refpages/latest/refpages/source/VkSurfaceCapabilitiesKHR.html and https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentIdKHR.html
- Stable Vulkan full-refresh VSync uses FIFO cadence without a redundant software timer. Lower software caps and unknown display refresh keep their timer. Queue-depth wait occurs before that timer, consuming its budget rather than adding another wait afterward. D3D12 and disabled-stable comparison behavior are preserved. Triple buffering is already requested in the frontend; no default quality/config change. Reference: https://docs.vulkan.org/samples/latest/samples/performance/swapchain_images/README.html
- A queued newer writer in the same scene and VI interval can suppress optional matching/intermediate images only when an exact native image can be retained. One owned native endpoint keeps writer/source metadata, GPU wait and presentation watermark. Different scene/VI, ray tracing, pause/debug, composition copies, unknown target and current-rate frames keep existing behavior. Source cadence is preserved, every game update still executes, and geometry checks remain intact. The next workload independently resumes interpolation. This narrow pressure guard may not activate in every slowdown.
- RT64 elapsed presentation timing uses steady_clock across Timestamp/current/sleep, preventing host wall-clock adjustments from affecting Linux presentation deadlines. No guest/network clock changes; not claimed as observed hitch cause.
- Diagnostics log at most 32 pacing policy transitions and 16 pressure occurrences; the aggregate present-throttle-wait stage is opt-in. Continuous packet tracing is disabled. Physical display timing remains unmeasured.

## Verification

Windows and Linux full game builds pass. FramePacingSmoke enabled/disabled; VulkanSwapChainSmoke; InterpolationPressureSmoke; AuthoredCadenceSmoke; FrameMetadataSmoke; PipelineDiagnosticsSmoke enabled/disabled all pass. Tests include lower/unknown caps, legacy and D3D predicate equivalence, delayed queue budget, monotonic clock and expired deadline, native retention/writer certificates and 10,000 scene transitions. These are offline tests, not visual acceptance. One initial orchestration target name was incorrect and diagnostics required an argument; corrected invocations all pass.

RT64 and Plume dependency patches rebuilt and clean reconstructed against pinned commits; new helper overrides hashed in dependencies.lock.json. Export evidence and Linux/Windows hashes reside under analysis/frame-pacing-20260920. Existing published mirrors and previous test archives untouched.

## First session result

Session `20260920-171756-f0fc06fb` ran for 401.55 seconds on D3D12, target 60 FPS,
detected display 120 Hz, with VSync off/on/off and draw distance 50%. Mode 23
race/pause reports contain 17,717 present-call intervals averaging 16.669 ms,
maximum 23.41 ms, and no 30 Hz authored cadence. Active-gameplay report samples
cover approximately 175 seconds at 59.94–60.06 updates per second. Earlier
launcher/attract/menu windows include gaps up to 94.72 ms; the session was not
entirely free of hitches. All logged DXGI presentation attempts succeeded.

The user confirmed forced closure, accounting for exit code 1 and absence of a
normal shutdown marker. No crash error was recorded. The full review and metrics
are saved beside this session's logs. Vulkan and interpolation-pressure activation
were not exercised; no improvement percentage or scanout smoothness is established.
The test launcher now explicitly selects Vulkan on startup. No new launch occurred.

## Remaining test

Run a five-minute race at the usual resolution/draw distance, with fixed refresh, VRR off, and 60 FPS/VSync on. Select Vulkan to exercise the backend fixes. Include dense geometry, pause/resume, and a finish if practical. Start only after **ready**. Repeated crash exercises are unnecessary. Evaluate perceived pacing and diagnostic policy/pressure activity. Windows D3D retains its existing display pacing; manual sub-refresh D3D caps and long geometry preparation remain further concerns.


## 2026-10-07 — Followup11 prepared; second-race terrain pacing

Followup10 launch02 ended normally (exit 0) after 281.023 seconds. Owner reports
smooth first ejection; second race slowed at high terrain distance, then stayed
smooth for the same scenery after lowering and restoring distance. Pause already
restored steady cadence before the first slider change, so a slider reset is not
established. Slow preparation precedes buffer growth; matching adds about 10ms
per matched workload during the slowdown. The exact initial stall is unresolved.

Followup11 skips optional matching for eligible retained race frames at a <=60 FPS
cap. Measured source cadence stays intact; genuine 30Hz content remains native at
that cap. Higher caps and unsupported retention paths keep existing behavior.
The 512-slot preparation recorder now reuses drained slots instead of exhausting
its lifetime capacity during the second race. Audio/physics/collision unchanged.

Windows and Linux builds plus 9 targets / 13 invocations each pass, including the
expected negative. Independent policy and recorder reviews found no blockers.
Private test: analysis/terrain-followup11-20261007/RoadRash64-1.4.4-Followup11-Win64.
Evidence: analysis/terrain-followup11-20261007/verification.json.
Prepared launch-01 has no authorization. Game closed; fresh ready required.
Live improvement, residual Dumoto cutoffs, and results-collision acceptance remain
unverified. Private folder contains owner ROM/imports; do not redistribute.
No publication authorized.

