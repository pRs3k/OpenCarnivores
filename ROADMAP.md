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

## Graphics and shaders
- **Phase 1: Post-Processing Pipeline** — build framebuffer infrastructure for full-screen effects (G-Buffer, effect chaining, compositing)
- **Phase 2: Shader Pack** — implement optional visual enhancement effects:
  - Dynamic shadow mapping (cascaded, PCF filtering)
  - Bloom + tone mapping + color grading
  - Screen-space reflections (SSR)
  - Normal mapping quality improvements & parallax mapping
  - All effects disabled by default, user-configurable via menu/config
- **Phase 3: Advanced Graphics Menu** — add Options → Video → Advanced Graphics with toggles/sliders for effect intensity
- See [RENDERING.md](RENDERING.md) for full shader enhancement plan and effort estimates

## Specialized domains
- [RENDERING.md](RENDERING.md) — renderer abstraction, texture overrides, shader enhancement plan.
- [AUDIO.md](AUDIO.md) — EFX reverb zones, HRTF, terrain occlusion.
- [VR.md](VR.md) — full VR pipeline, OpenXR, comfort features.

## Development infrastructure
- GitHub Actions CI for Windows + Linux + macOS builds per commit.
- Unit tests for `mathematics.cpp` (pure functions, easy win).
- SDL3 migration when stable (better HiDPI, gamepad, dialog APIs).
- clang-tidy / ASan pass over the 1999 code for undefined-behavior issues.
