# Architecture and Core Files

## Core game files
- `Hunt.h` — Main header with all structs, globals, and constants.
- `Hunt2.cpp` — Main game loop, Win32 window setup, WndProc.
- `Game.cpp` — Game logic, AI, dinosaur behavior, physics.
- `Characters.cpp` — Character/model loading and animation.
- `Interface.cpp` — Menu system, HUD, UI rendering, asset loading.
- `Resources.cpp` — _RES.txt parsing (game data definitions).
- `mathematics.cpp` — Vector math, matrix ops, collision detection.

## Domain-specific guides
- [RENDERING.md](RENDERING.md) — All rendering backends and texture override system.
- [AUDIO.md](AUDIO.md) — Audio system, backends, and 3D positional audio.
- [VR.md](VR.md) — VR plumbing, OpenXR, input, and current progress.

## Global variable initialization pattern

Hunt.h uses a macro-based pattern for global variables:
```cpp
#ifdef _MAIN_
 #define EXTORNOT          // Empty — variable is defined
#else
 #define EXTORNOT extern   // Extern — variable is declared
#endif

EXTORNOT int VideoCX, VideoCY;  // Defined in Hunt2.cpp (which defines _MAIN_)
EXTORNOT float CameraW, CameraH;
```

**Key insight**: Hunt2.cpp must define `#define _MAIN_` before including Hunt.h to instantiate all globals. Other translation units see them as extern declarations. This pattern predates C++17 inline variables and centralizes all globals in Hunt.h with their declarations and definitions in one place.

## Renderer multiplexing

Both renderd3d.cpp (Direct3D 6) and renderer/RendererGL.cpp (SDL2+OpenGL 4.1 Core) are compiled into every build. The active renderer is selected via:
- CMake: `RENDERER=d3d` or `RENDERER=opengl` (-DRENDERER flag)
- Compile-time: `-D_opengl` or `-D_d3d` define gates renderer-specific code in Hunt2.cpp

Hunt2.cpp dispatches to the appropriate backend via function pointers and conditional compilation. This allows both renderers to coexist without linker conflicts.

**OpenGL version**: The codebase requests **OpenGL 4.1 Core** at context creation time (RendererGL.cpp:437-439) because OpenXR runtimes require GL 4.0+, and 4.1 is the highest Core version supported on macOS. Shader code is written for 3.3 compatibility and is fully forward-compatible with 4.1.

## Logging convention

Global logging via `PrintLog(const char* msg)` (Resources.cpp):
- Accepts `const char*` to support C++ string literals and ternary expressions
- Appends to logt[] and writes to hlog file handle
- Used throughout for debug/diagnostic output

Forward declarations in multiple files (VFS.cpp, renderd3d.cpp, PostProcessing.cpp) prevent header dependencies; the single definition in Resources.cpp handles all callers.

## Shader loading and hot-reload

The OpenGL renderer loads shaders from disk files instead of using embedded source strings:
- **Location**: `shaders/basic.{vert,frag}` (loaded at runtime from disk via VFS)
- **Fallback**: Embedded source strings in RendererGL.cpp are only used if disk files are missing
- **Hot-reload**: HotReload::Watch("shaders/basic.frag") automatically detects changes and recompiles at runtime
- **Important for debugging**: When adding new debug modes, changes to shaders/basic.frag take effect immediately, but changes to embedded strings in RendererGL.cpp do NOT (they're only used if the disk files are missing)

This architecture enables rapid iteration on shader code during development and allows modders to override shaders via the VFS/mod system.

## Terrain lighting and vertex Light values

Terrain and foliage rendering use **pre-computed per-vertex brightness** stored in the `Light` field of each vertex:

**Data flow:**
1. **Level load time** (Resources.cpp): `RenderLightMap()` generates a static 512×512 lightmap (`LMap[][]`) from level geometry and object shadows
2. **Every frame** (Hunt2.cpp): `PreCashGroundModel()` loads 256×256 terrain tiles around the camera into `VMap[][]` (viewport map)
3. **Light assignment**: For each terrain vertex, Light is read from `LMap[worldY][worldX]`, optionally darkened by `SkyMap` cloud shadows
4. **Rendering** (renderd3d.cpp): Vertex color is set from Light: `color = Light * 0x00010101 | 0xFF000000` (grayscale)

**Key structures:**
- `VMap[VMAP_DIM][VMAP_DIM]` — 512×512 array of environs (cached terrain around camera)
- `LMap[512][512]` — Static lightmap loaded from level at init
- `VMap.Light` — Per-vertex brightness (0-255), derived from LMap + SkyMap
- `SkyMap[128][128]` — Cloud shadow pattern, offset by time and indexed by terrain coordinates

**Debug modes (F8):**
- Mode 21: Light heatmap (grayscale visualization of per-vertex brightness)
- The mysterious "circle of light" artifact appears in mode 21 and persists even when Light is forced to uniform, indicating it originates from outside the per-vertex Light system

## Vector math function signatures

Core functions in mathematics.cpp for 3D geometry:
```cpp
void MulVectorsScal(const Vector3d& v1, const Vector3d& v2, float& r);  // dot product
void MulVectorsVect(const Vector3d& v1, const Vector3d& v2, Vector3d& r); // cross product
Vector3d SubVectors(Vector3d& v1, Vector3d& v2);  // returns temporary
```

**Pattern**: Input vectors are const references (accepting temporaries from SubVectors). Output is passed by non-const reference. This allows chaining like `MulVectorsVect(SubVectors(a, b), SubVectors(c, d), result)` without intermediate variables.

## Fixed-timestep simulation and render interpolation (Hunt2.cpp)

`ProcessGame()` runs the sim in fixed 16 ms ticks (`FIXED_DT_MS`, ~62.5 Hz) peeled off a
wall-clock accumulator, then renders once per outer frame. The camera pose is lerped
between the last two sim snapshots using the accumulator residue as alpha, and the
authoritative (un-interpolated) pose is restored right after drawing so the next tick
integrates from real state.

**Hard-won rules:**
- **Interpolate on EVERY render frame, not just frames that ran a sim tick.** When the
  display outruns the sim, tickless frames must keep lerping with the growing residue
  alpha — an early `steps > 0` gate froze the camera at the raw newest pose between
  ticks, quantizing head-bob to sim rate (stuttery, robotic walk).
- **Anything sampled per-frame for rendering must use the interpolated pose.** The
  post-process camera uniforms (`SetCameraWorldUniforms`, height fog, god rays) are
  uploaded inside the render block while `Camera*` still holds the lerped values.

## Sim clock domains and float trig precision

There are two clock domains and they must not be mixed:
- **Menus** (`ProcessSyncro`, Game.cpp): `RealTime = timeGetTime()` — wall clock.
- **Hunts** (`ProcessGame`, Hunt2.cpp): `RealTime` is **rebased to 0 at hunt start** and
  advances +16 ms per fixed tick. The hunt↔menu base switch is absorbed by
  ProcessSyncro's `TimeDt > 10000 → 10` clamp.

**Why the rebase exists:** all procedural sway evaluates `sin(RealTime/K.f)` in float
(head-bob `stepdy`, weapon sway, water waves, dino tail sway, underwater warp). Once
RealTime carries Windows-uptime milliseconds past 2^24 (~4.6 h), consecutive
milliseconds collapse to the same float and the phase advances in 32–128 ms
stair-steps — the bob turns visibly robotic on any machine that hasn't rebooted for a
few days. A zero base keeps millisecond-exact phase for ~4.6 h of continuous play.

**Rules:** never feed wall-clock-magnitude integers through float trig; and any
deadline compared against `RealTime` must be *set* from `RealTime`, not
`timeGetTime()` (see `MessageList.timeleft` in `AddMessage`).

## Session-cached characters and baked texture tints (Resources.cpp)

`LoadCharacters()` caches dino (`ChInfo[]`) and weapon (`Weapon.chinfo[]`) .CAR data for
the whole session (`if (!mptr)` guard) to avoid reload stutter between hunts. But
`BrightenTexture()` **bakes the night-vision green desaturation into the texture data
at load time** when `OptDayNight == 2`. Each `TCharacterInfo` therefore stamps
`LoadedDayNight` at load, and the cache guard reloads on a night↔day mismatch —
without it, a night hunt leaves green dinos/weapons in later dawn/day hunts.

**Rule:** any new load-time bake keyed on per-hunt settings (time of day, weather,
mods) must either move to a live shader uniform or invalidate every cache that holds
the baked data. Per-map assets (terrain, sky, RSC objects) reload every hunt via
`ReleaseResources()` + `ResetTextureMap()`, so only session-lifetime caches need
stamps.

## Code Quality & Modern C++ Improvements (May 2026)

Recent clang-tidy cleanup pass systematized non-stylistic issues:

### String Conversion Safety
- **Bindings.cpp, ModelOverrides.cpp:** Replaced unchecked `atoi()` with `strtol()` + error checking
- **Pattern:** `std::strtol(val, &end, 10); if (end != val && *end == '\0') /* valid */`
- **Benefit:** Detects malformed config values vs. silently treating them as zero

### Const-Correctness in Generic Functions
- **mathematics.cpp:** Vector math functions now accept const references
- **Impact:** Allows passing temporaries directly: `MulVectorsVect(SubVectors(a,b), SubVectors(c,d), result)` without temporaries
- **Cascading updates:** Hunt.h declarations, all callers (renderd3d.cpp, mathematics.cpp)

### Type Safety
- **Gamepad.h:** Anonymous enum base type changed from implicit int to `uint8_t`
- **Pattern:** `enum : uint8_t { ... }` saves 3 bytes per enum instance
- **Rationale:** Matches modern C++11+ best practice; reduces struct padding

### Logging Function Modernization
- **Resources.cpp, VFS.cpp, PostProcessing.cpp, renderd3d.cpp:** PrintLog signature updated to accept `const char*`
- **Benefit:** Supports C++ string literals and ternary expressions without const_cast workarounds

### SDL2 Compatibility Shims
- **SDL_endian.h, SDL_cpuinfo.h:** Added Clang 22.1.0 builtin conflict workarounds
- **Issue:** SDL2 tries to define `_m_prefetch` which Clang has as builtin
- **Fix:** Guard with `!defined(__clang__)` to skip redefinition on Clang

### Macro Hygiene (Completed Earlier)
- **renderd3d.cpp, XR.cpp, Hunt2.cpp:** Reserved identifier cleanup (_ZSCALE → ZSCALE_, _TMenuSet → TMenuSet_)
- **Macro parentheses:** Fixed `#define ZSCALE_ -16.f` → `#define ZSCALE_ (-16.f)` precedence bugs
