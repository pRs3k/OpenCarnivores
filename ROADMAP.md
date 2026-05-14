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
  - FBO management, effect registry, shader loading system
  - Global toggles for Bloom, Tone Mapping, SSR, Shadow Mapping (all disabled by default)
  - Integration in flatscreen and VR render paths
  - See [RENDERING.md](RENDERING.md) for details

- **Phase 2 — Visual Enhancement Effects** (In Progress)
  - Dynamic Shadow Mapping — cascaded PCF shadows from sun light
  - Bloom + Tone Mapping — bright-pixel bloom, HDR→SDR Reinhard curve
  - Screen-Space Reflections — ray-marched reflections on shiny surfaces
  - Normal Mapping Quality — parallax mapping, PBR parameters
  - See [RENDERING.md](RENDERING.md) Phase 2 roadmap

## VR Enhancements
- **Supersampling Runtime Adjustment ✅ Complete** — Supersampling (eye FBO resolution) now adjustable at runtime via menu slider (100–200%), with immediate swapchain recreation
- **World Scale Tuning ✅ Complete** — Adjusted from 133 GU/m to 143 GU/m (~7% reduction) to improve perceived detail and reduce "too large" sensation
- **Next: Independent weapon aiming** — controller-relative pointing for true VR gun aiming (vs. camera-centered)

## Specialized domains
- [RENDERING.md](RENDERING.md) — renderer abstraction, multi-backend support, post-processing.
- [AUDIO.md](AUDIO.md) — EFX reverb zones, HRTF, terrain occlusion.
- [VR.md](VR.md) — full VR pipeline, OpenXR, comfort features, graphics settings.

## Development infrastructure
- GitHub Actions CI for Windows + Linux + macOS builds per commit.
- Unit tests for `mathematics.cpp` (pure functions, easy win).
- SDL3 migration when stable (better HiDPI, gamepad, dialog APIs).
- clang-tidy / ASan pass over the 1999 code for undefined-behavior issues.
