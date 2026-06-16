# Roadmap

## Gameplay and engine
- Improved AI (behavior trees, NavMesh pathfinding). For example, dinos should be less inclined to  swim straight out into the water when fleeing from the player unless they're swimming to another land mass. Dinos should not be able to detect players using line of sign if the player is behind an object.
- Physics integration (ragdoll, foliage interaction).
- Embed Lua or AngelScript with bindings for `Character` struct + event hooks (OnDamage, OnSpawn, OnFire) to open modding to non-C++ devs.
- Virtual filesystem: wrap scattered path lookups in a VFS that can mount zips with priority (folders already implemented).
- Dinosaurs are getting stuck on seemingly nothing, occasionally.
- Dinosaurs killed swimming in water should sink to the ground
- The sky renders correctly when looking straight up, but ripples/waves distort it when it gets very close to the horizon/furthest away from the player
- **In VR, sky textures shift when turning head.** Root cause: flat-plane UV math couples sky appearance to camera yaw. Attempted fixes (cylindrical UV, fixed world position, pitch-only vbase) all failed. Solution: replace with 3D dome model rendered at fixed world position through normal geometry pipeline.
- Remove any dead code

## Graphics Fixes
- **Foliage Transparency (In Progress)** — Foliage appearing overly solid/"puffy" instead of showing individual leaves. 
  - **Partial Fix ✅**: Mipmap generation was aggressively filling transparent pixels with opaque neighbors (commit d16a69c), eliminating detail. Fixed by commenting out pixel-fill logic in `CreateMipMapMT()` and `CreateMipMapMT2()` (Resources.cpp). This restored individual leaf outlines.
  - **Remaining Issue**: Black gaps now appear between leaves where transparency should show through. Root cause under investigation (likely color-key transparency handling in GL mipmap generation or shader alpha test threshold).

## Graphics and Rendering
- **Phase 1 — Post-processing Infrastructure ✅ Complete**

- **Phase 2 — Visual Enhancement Effects** (In Progress)
  - Cascaded Shadow Maps ✅ — 3-cascade PCF (2048² per cascade); texel-proportional per-cascade bias; 30% cross-fade blend zones at cascade boundaries; semi-transparent dinosaur flat shadows (vertex alpha restored); cascade splits {10%, 35%, 100%} of shadow range
  - Bloom + Tone Mapping ✅ — bright-pass threshold, 2-iteration separable Gaussian blur (half-res ping-pong), additive composite; ACES filmic + Reinhard tone mapping; color grading (contrast/saturation/lift/gain); unsharp-mask sharpening; toggle Shift+P
  - Screen-Space Reflections — ray-marched reflections on shiny surfaces
  - Normal Mapping / Parallax / PBR ✅ — world-space Cook-Torrance GGX; parallax from normal-map alpha; auto-detected at load time
  - See [RENDERING.md](RENDERING.md) Phase 2 roadmap

## VR Enhancements
- **Next: Independent weapon aiming** — controller-relative pointing for true VR gun aiming (vs. camera-centered)

## Specialized domains
- [RENDERING.md](RENDERING.md) — renderer abstraction, multi-backend support, post-processing.
- [AUDIO.md](AUDIO.md) — EFX reverb zones, HRTF, terrain occlusion.
- [VR.md](VR.md) — full VR pipeline, OpenXR, comfort features, graphics settings.

## Development infrastructure
- GitHub Actions CI for Windows + Linux + macOS builds per commit.
- Unit tests for `mathematics.cpp` (pure functions, easy win).
- SDL3 migration when stable (better HiDPI, gamepad, dialog APIs).
- clang-tidy pass ✅ — float promotion, dead stores, NULL→nullptr, uncached glGetUniformLocation (55+/frame eliminated), shadow shader macro→function refactor.
- ASan pass over the 1999 code for remaining undefined-behavior issues.

## Terrain LOD — Attempted, Rolled Back (2026-06)

**Goal:** Reduce far-zone terrain draw calls by merging adjacent tiles into single quads (`ProcessMapMerged`), targeting framerate improvement at large view distances (ctViewR ≥ 128).

**What was implemented:**
- `ProcessMapMerged(x, y, step)` — rendered a step×step block as 2 triangles using each tile's DataB (64×64 mip). step=2 for mid zone (64–128 tiles, ~123–246 m), step=4 for far zone (>128 tiles).
- `RenderGround()` gained two pre-passes emitting merged quads before the per-tile near ring loop.
- `shadowFloor` — sampled minimum LMap across the merged tile footprint to propagate baked tree shadow darkness to merged tile corners.
- World-space shadow path inside `ProcessMapMerged` shadow pass — called `SubmitWorldSpaceShadowTriangle` with raw HMap heights to bypass camera frustum culling during the CSM depth pass.
- CSM changes made alongside: edge fade in `sampleCascade`, `CASCADE_RANGE_FRACS` widened to {0.40, 0.70, 1.0}, `far_z` in `BeginWorldShadowPass` extended to `dist + range`.

**Limitations found:**
1. **Moving shadow cutoff line** — The big hill cast a hard-edged shadow that moved with the player. Root cause: merged tiles outside the camera frustum were absent from the CSM depth map; the depth map boundary moved as the player turned. Adding a world-space HMap shadow path improved but did not eliminate the artifact.
2. **Banding at LOD zone boundaries** — Seam visible at the step=2/step=1 (64-tile) and step=4/step=2 (128-tile) boundaries due to mismatch in texel sampling (DataB 64×64 vs. per-tile full-res) and lighting discontinuity at the transition ring.
3. **Shadow coverage gap** — The merged tile grid only covers tiles within `ctViewR` of the player; the CSM ortho frustum extends beyond that. Tiles in the shadow depth map's coverage area but outside `ctViewR` had no geometry submitted, leaving a shadow-free band.
4. **No measurable framerate gain** — VMap is still populated O(ctViewR²) per frame; the bottleneck was not `ProcessMap` call count. FPS remained 22–25 before and after.
5. **`(std::max)`/`(std::min)` required** — `minwindef.h` defines `max`/`min` as C macros; parenthesized form needed throughout.

**What was reverted:**
- `ProcessMapMerged` function removed entirely.
- `RenderGround()` GL section restored to full-resolution per-tile ring loop.
- CSM changes reverted: `CASCADE_RANGE_FRACS` back to {0.10, 0.35, 1.0}, `far_z` back to `dist + range * 0.75f`, `sampleCascade` edge fade removed.

**Recommended future approach:**
- Pre-bake terrain into VBOs by region; GPU renders whole VBO in one draw call. No per-tile CPU dispatch.
- True GPU-side LOD (geometry shader or tessellation) so shadow geometry is always complete regardless of camera frustum.
- Move VMap population off the CPU critical path (compute shader or pre-transformed buffer).
