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
