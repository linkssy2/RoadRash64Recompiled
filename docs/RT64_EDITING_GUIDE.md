# Editing the Road Rash 64 renderer

This guide covers the local RT64 changes, not every subsystem in the game. The dependency is pinned in `dependencies.lock.json` in the distribution source tree. Its tracked edits are shipped as `dependency-patches/native_lib_rt64.patch`; project-specific extra headers are shipped under `dependency-overrides/native/lib/rt64`. Edit the working sources in `native/lib/rt64`, then regenerate **both** forms of dependency customization. Do not edit generated shader byte arrays.

## Where to make a change

Paths in this table are relative to `native/lib/rt64/src` unless another root is given.

| Feature | Starting points | What to preserve |
| --- | --- | --- |
| Output timing and smooth presentation | `hle/rt64_present_queue.cpp`, `hle/rt64_rr64_frame_pacing.h`, `common/rt64_timer.cpp`, `hle/rt64_application.cpp` | One pacing owner per backend; game simulation timing is separate from output timing. Keep the native fallback and queue wakeups. |
| Interpolated frame lifetime and reuse | `hle/rt64_workload_queue.cpp`, `hle/rt64_shared_queue_resources.h`, `hle/rt64_rr64_owned_frame_batch.h`, `hle/rt64_rr64_frame_metadata.h` | Scene epoch, writer, color-target identity and resource epoch must agree. A presented image must not be overwritten while in use. |
| Geometry matching | `hle/rt64_game_frame.cpp`, `hle/rt64_game_frame.h` | Topology/correspondence checks reject uncertain matches. Correctness hashes are not optional logging. Retain endpoint and ambiguity checks. |
| Projection/transform interpolation | `render/rt64_projection_processor.cpp`, `render/rt64_transform_processor.cpp` | Preserve exact authored endpoints and projection clipping behavior. |
| Reusing a completed image safely | `render/rt64_framebuffer_renderer.cpp` (`rr64FrameIsSelfContained`), `hle/rt64_workload_queue.cpp` | A complete color/depth overwrite must be proven before retention. Partial frames fall back to native rendering. |
| Widescreen, stretch and high-resolution | `common/rt64_user_configuration.h`, `render/rt64_vi_renderer.cpp`, `render/rt64_framebuffer_renderer.cpp`; frontend `recompui/src/renderer/rt64_render_context.cpp` | Configuration, projection and final VI layout have different jobs. HUD anchoring is restricted to race HUDs; do not stretch result screens indiscriminately. |
| Maximum terrain/object detail | Game-side `native/src/rr64_world_terrain_assets.cpp`, `rr64_world_object_assets.cpp`, `rr64_world_packet_compiler.hpp`; renderer `gbi/rt64_gbi_extended.cpp`, `hle/rt64_rsp.cpp` | Game-side builders emit the same validated packed-triangle command the renderer consumes. Keep original local multiplayer course bounds. |
| Rider/bike LOD | Game-side `native/src/rr64_actor_render_runtime.cpp`, `rr64_actor_render_snapshot.cpp`, `rr64_actor_held_pose.cpp` | LOD policy is primarily outside RT64. Matching/rendering changes can still affect visibility, crashes and finish poses. |
| Fog and horizon haze | `hle/rt64_rsp.cpp`, `hle/rt64_rsp.h`; game-side settings | This is the active vertex-fog path. The removed screen-space VI blur was a separate, disabled experiment. |
| TMEM texture reuse | `hle/rt64_rdp.cpp`, `hle/rt64_rdp_tmem.*`, `common/rt64_tmem_hasher.h` | Byte order, tile/palette identity, invalidation and unaligned fallbacks must remain correct. |
| Optional converted texture packs | `common/rt64_replacement_database.*`, `render/rt64_texture_cache.*`, `tools/texture_packer/texture_packer.cpp` | Jabo hash/path support is used by the converted mod and offline packer. No stock-pack usage does not mean this code is obsolete. |
| Optional performance diagnostics | `hle/rt64_rr64_pipeline_diagnostics.h`, `hle/rt64_state.cpp`, `hle/rt64_workload_queue.cpp`; game-side `native/src/main.cpp` | `RR64_DIAGNOSTICS=1` is read once per process. Observation hashes/resource inventories may be skipped; actual cadence tracking and matching certificates may not. |

## Keep edits reviewable

Change one contract at a time. A brief comment should explain the game-specific reason, a lifetime requirement or a fallback—not narrate a C++ statement. Existing comments near pacing, identity admission, terrain packets, HUD rules and matching checks describe these constraints. Avoid renaming or moving these implementations just to reduce patch size.

Rebuild the renderer after shader, descriptor or shared constant changes. C++ and HLSL layouts must agree; the build generates both DXIL and SPIR-V. Apply the exported patch to a clean tree at the pinned revision, copy the locked extra files, and verify it reconstructs the working sources.

## Verification

Useful existing targets include `RR64FramePacingSmoke`, `RR64FrameMetadataSmoke`, `RR64AuthoredCadenceSmoke`, `RR64FrameOverwriteSmoke`, `RR64OwnedTextureReuseSmoke`, `RR64MatchingPreflightSmoke`, `RR64TranslationRejectionSmoke`, `RR64RSPVertexSmoke`, `RR64RSPTriangleBatchSmoke`, `RR64TMEMLoadSmoke` and `RR64TMEMHashSmoke`. LOD/world/video tests cover the game-side contracts. These are offline checks and do not establish in-game smoothness.

For live verification, use a separate candidate folder and wait for the tester to say **ready** before launching. Compare the same map, direction, graphics settings and texture pack against the preserved baseline. Include Big Game, finish/crash/pause transitions and split-screen lap courses. Report frame-time observations separately from build/test success. Do not publish until explicitly authorized.
