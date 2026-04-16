# Carnivores 2 Source Port — Project Guide

## Constraints for Claude Code
Just give me outputs, no explanations.
Output only changed code blocks.
Answer in one sentence.

## Project Overview

This is a source port of Carnivores 2 (1999, Action Forms) from its original Win32/DirectX 6 codebase to a modern, cross-platform engine using SDL2 and OpenGL 3.3. The goal is to produce a clean, maintainable engine that runs on modern systems while remaining fully compatible with the original game assets.

## Source Code Origin

The original codebase is a flat directory of ~20 C++ files targeting Visual Studio 6 (MSVC 6.0) on Windows 98/NT. It uses:
- **Win32 API** for windowing, input, and system calls
- **DirectDraw/Direct3D 6** for hardware-accelerated rendering (`renderd3d.cpp`)
- **3Dfx Glide API** for Voodoo card rendering (`Render3DFX.cpp`)
- **Software renderer** as fallback (`RenderSoft.cpp`, `renderasm.cpp` with x86 ASM)
- **DirectSound** for audio (`Audio.cpp`, `Audio_DLL.cpp`)
- **Custom binary formats** for models (.CAR), maps, and textures

Key source files and their roles:
- `Hunt.h` — Main header with all structs, globals, and constants
- `Hunt2.cpp` — Main game loop, Win32 window setup, WndProc
- `Game.cpp` — Game logic, AI, dinosaur behavior, physics
- `Characters.cpp` — Character/model loading and animation
- `Interface.cpp` — Menu system, HUD, UI rendering
- `Loading.cpp` — Asset loading (maps, textures, models, resources)
- `Resources.cpp` — _RES.txt parsing (game data definitions)
- `mathematics.cpp` — Vector math, matrix ops, collision detection
- `renderd3d.cpp` — Direct3D 6 rendering backend
- `Render3DFX.cpp` — 3Dfx Glide rendering backend
- `RenderSoft.cpp` — Software rendering backend
- `renderasm.cpp` — x86 ASM optimized software rendering routines
- `Audio.cpp` / `Audio_DLL.cpp` — DirectSound audio system
- `eax.h` — Creative EAX audio extensions header

## Development Environment

- **OS**: Windows 10
- **GPU**: NVIDIA RTX 2070 Super
- **CPU**: AMD Ryzen 7 7800X3D
- **Compiler**: MSVC (Visual Studio 2022) or MinGW-w64
- **Build system**: CMake (original uses VS6 .dsp/.dsw)
- **Target**: Windows 10+ initially, cross-platform later, VR later

## Phase 1 — Get It Building (COMPLETE)

## Phase 2 — Get It Running (COMPLETE)

## Phase 3 — Abstract the Renderer (COMPLETE)

### Architecture
```
renderer/
  Renderer.h      — Abstract interface with RenderVertex struct
  RendererGL.cpp  — SDL2 + OpenGL 3.3 Core backend
  RendererGL.h    — RendererGL class declaration
```

### Key implementation notes
- `#ifdef _opengl` guards throughout `renderd3d.cpp` — all D3D6 code is conditionally compiled
- `g_glRenderer` (RendererGL*) in renderd3d.cpp is the global GL renderer instance
- Vertex format: `RenderVertex` mirrors `D3DTLVERTEX` — screen-space pre-transformed
- Texture cache keyed by CPU pointer; LRU eviction at 128 entries
- `GL_BGRA` vertex attrib for color/specular channels (D3D ARGB in-memory = BGRA for GL)
- Depth: `GL_GEQUAL` with `glClearDepth(0.0)` — matches D3D6 GREATEREQUAL convention
- `d3dStartBuffer()` / `d3dStartBufferG()` lock into renderer's own staging buffers
- Execute buffer instruction building all guarded with `#ifdef _d3d`

## Phase 4 — Resolution and Display (COMPLETE)

### Implemented and verified
- Arbitrary resolution via `-width=N -height=N` command-line args
- Widescreen Hor+ FOV: `CameraW = CameraH = VideoCX*1.25*(WinH*4/3/WinW)` — keeps 4:3 VFOV (61°) constant, expands HFOV with aspect ratio (77° at 4:3 → 94° at 16:9). Water frustum cull uses `FOVK` to match expanded FOV.
- Fullscreen (`-fullscreen`), borderless (`-borderless`), windowed (default)
- Adaptive VSync (`SDL_GL_SetSwapInterval(-1)` with fallback to 1), toggle with `-vsync`/`-novsync`
- HiDPI: `SDL_WINDOW_ALLOW_HIGHDPI`, drawable size synced back to game globals
- In-menu resolution/display-mode selector: cycles preset list (640×480 → 3840×2160) for windowed and fullscreen; borderless always snaps to desktop res
- `display.cfg` persists display settings across launches; version-stamped so stale configs are rejected; post-load validation clamps mode and rejects sub-640 resolutions

## Current Rendering Status - FULLY WORKING
- Run with: `OpenCarnivores.exe`
  - Note: use forward slashes in bash; the game accepts either

### Key GL compatibility fixes applied
| Issue | Root cause | Fix |
|---|---|---|
| Black screen | Buffer mismatch: `lpVertex`→local array, GL reads from renderer buffer | Route through `g_glRenderer->LockVertexBuffer()` |
| Black geometry | Stale texture rebind in UnlockAndDraw* | Removed the redundant `glBindTexture` calls |
| Green/wrong colors | GL read D3D ARGB as RGBA (R↔B swap) | `GL_BGRA` attrib type for color/specular |
| Grey terrain | Terrain UVs tile > 1.0 | Changed to `GL_REPEAT` |
| Sky noise (green) | Textures are RGB555; GL path was decoding as RGB565 | Always decode game textures as RGB555 in `gl_UploadTexture16` |
| Model ghost polygon | `gScrp[v].x = 0xFFFFFF` sentinel for behind-camera verts submitted as geometry | Skip faces where any vertex has sentinel x in `RenderModel` |
| Model alpha corruption | `ml + VLight > 255` overflows alpha byte of packed color DWORD | Clamp `_ml` to 255 before `* 0x00010101` |
| Sky jitter on look | World position (CameraX/Z ~ 100000) rotated through camera-space for sky UV | Divide `vbase.x/z` by 128 to damp rotation effect |
| Terrain banding | Sky plane (sz=0.0001) written to depth buffer, fights distant terrain | Disable depth test/write for sky geometry |
| Wrong model UVs (solid single color) | `fp_conv` in Resources.cpp divided UV coords by 256 only for `_d3d`; `_opengl` used raw integer UVs → GL_REPEAT collapsed all samples to texel (0,0) | `#if defined(_d3d) \|\| defined(_opengl)` so both paths divide by 256 |
| All models fog-colored (grey) | `RenderModelClip` computed `ev.Fog = (float)(vFogT[v] >> 24)` but `vFogT` is 0–255 (not pre-shifted), so fog factor was always 0 → full fog | Remove `>> 24`: `ev.Fog = (float)vFogT[v]`; then `specular = ev.Fog << 24` gives `0xFF000000` (no fog) correctly |
| Terrain triangles pop in/out on slopes | Software backface-cull dot-product in `DrawTPlane`/`DrawTPlaneClip` is near zero on sloped terrain; floating-point sign flips as camera moves. In D3D6 masked by fog; visible without fog. | Guard the test with `#ifndef _opengl` — GL uses `glDisable(GL_CULL_FACE)` + depth testing and needs no software backface culling |
| Compass inner texture missing | Near-model faces (outer ring fproc1, inner disc fproc2/same-z) at similar camera depth; outer ring writes depth, inner disc at slightly larger z fails `GL_GEQUAL` on overlapping pixels. | `Hardware_ZBuffer(FALSE)` now calls `SetZBufferEnabled(false)` — disables depth test for HUD pass; `Hardware_ZBuffer(TRUE)` restores it. HUD models use software backface culling (sfNeedVC) so painter order is correct. |
| fproc1 black pixels transparent (grip visible through glove) | GL blend always enabled; RGB555 c=0 → texel.a=0 → transparent for fproc1 faces, but D3D6 had blending OFF for fproc1. | Fragment shader: `if (!uAlphaTest) color.a = 1.0` — fproc1 always fully opaque. |
| Shiny gun visible through player's arm | `RenderModelClipEnvMap`/`RenderModelClipPhongMap` called `SetZBufferEnabled(false)` before their additive pass, so the shine rendered through all geometry. | Added `SetDepthMask(bool)` to RendererGL; overlay passes use depth read-only (`glDepthMask(GL_FALSE)` with test still enabled) so they're occluded by closer geometry but don't corrupt the depth buffer. |
 Scene too dark in all modes (day/night/HUD) | `BrightenTexture` baked `OptBrightness` into texture data at load time; old D3D6 configs saved `OptBrightness=0` → 50% brightness baked in; HUD elements also darkened. | Moved brightness to a live `uBrightness` shader uniform (`1.0 + OptBrightness/128`); `BrightenTexture` now only applies night-mode green desaturation; default `OptBrightness=0` = neutral (1.0×); slider changes take effect instantly with no texture reload. |
| Black boxes around distant object sprites (BMP models) | Mixed-distance `_RenderObject` loop: nearby objects call `RenderModelClip`→`d3dFlushBuffer(fproc1,0)`→`UnlockAndDrawTriangles` which sets `uAlphaTest=false` (no alpha-test faces). Subsequent BMP sprite with same texture skips the texture-change block (`hGTexture==hTexture`), so `uAlphaTest` stays false → background (c=0) pixels render as opaque black instead of being discarded. | In `d3dEndBufferG` GL path, call `g_glRenderer->SetAlphaTest(true)` immediately before `UnlockAndDrawGeometry` to re-assert correct state regardless of what `d3dFlushBuffer` set in between. |
| Underwater overlay covers only ~80% of screen / fullscreen corner chunk missing | `RendererGL::Init()` sets `glViewport` and `m_width/m_height` from the pre-sync window size. Hunt2.cpp then calls `SetVideoMode(drawable_w, drawable_h)` which updates `WinW/WinH` (and thus the projection) but NOT the viewport or `m_width`. `DrawFullscreenRect` built its quad using `m_width/m_height`, so the teal overlay only reached `m_width/WinW` of the screen. Geometry beyond the stale viewport was also clipped. | `BeginFrame()` now detects `m_width != WinW` and calls `glViewport(0,0,WinW,WinH)` + updates `m_width/m_height`. `DrawFullscreenRect` quad uses `WinW/WinH` directly. |
| Fullscreen resolution list only showed 3 entries (640×480, 800×600, 1600×900) | `presetsBuilt` cached from `SDL_GetDesktopDisplayMode` which returns the *current* exclusive mode (1600×900), so only 3 presets fit the `≤ dm.w/dm.h` filter. | Use `SDL_GetNumDisplayModes`/`SDL_GetDisplayMode` to find the monitor's true maximum hardware resolution; filter presets against that. |
| Borderless stuck at 1707×960 on 2560×1440 / 150% DPI display | `SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2")` was placed inside `RendererGL::Init()`, but `SDL_Init(SDL_INIT_VIDEO)` is called earlier in `Hunt2.cpp` `main()` — hint was ignored because SDL was already initialized. | Moved hint to `Hunt2.cpp` `main()` immediately before `SDL_Init`. Removed the dead copy from `RendererGL::Init`. |
| Fullscreen 2560×1440 rendered in a sub-region (black bars on sides) | After `SDL_SetWindowFullscreen`, the OS display-mode change is async. `PollMenuEvents` processed a stale `SIZE_CHANGED` event with the pre-change drawable size and called `SetVideoMode` with the wrong dimensions before the mode settled. | Added `SDL_PumpEvents()` + `SDL_GL_GetDrawableSize` in `ApplyDisplay` immediately after entering exclusive fullscreen so `SetVideoMode` always receives the settled size. |
| Game starts in broken borderless resolution on first launch (cursor misaligned, scene crushed) | `display.cfg` saved with `kDisplayVer=1` stored the pre-DPI-fix logical resolution (e.g. 1707×960); on re-launch with DPI awareness active the window was the wrong physical size. | Bumped `kDisplayVer` to 2 to invalidate old configs; added post-load validation that clamps `OptDisplayMode` to 0–2 and resets sub-640 saved resolutions to 0 (triggers safe `SetupRes` preset). |
| Dinosaurs run in place (ground AI only; flyers unaffected) | `MoveCharacter()` had a stray `return;` as its first statement — a debug stub that was never removed, preventing all position updates. Flying dinosaurs (`AI_DIMOR`/`AI_PTERA`) use a separate movement path that bypasses `MoveCharacter`. | Removed the `return;` stub from `Characters.cpp`. |
| Texture shimmer / alpha flicker on foliage at distance | No mipmaps — `GL_TEXTURE_MIN_FILTER` was `GL_LINEAR` for all textures, causing GPU to sample a random full-res texel from a tiny triangle → shimmer on opaque surfaces, binary 0/1 flicker on alpha-keyed foliage. Terrain excluded: its manual DataA/DataB LOD swap + diagonal triangulation produce a pinwheel artifact with hardware mipmaps. | `glGenerateMipmap` + `GL_LINEAR_MIPMAP_LINEAR` added for transparent textures in `gl_UploadTexture16` (keyed on `hasTransparency`) and in `RendererGL::UploadTexture16`. Alpha discard threshold lowered `0.5 → 0.1` so mip-averaged leaf coverage (e.g. 30%) still passes instead of being discarded. |
| Tree / bush appearance changes completely at certain distances | `_RenderObject` switched from the 3D model to a flat pre-rendered BMP billboard sprite at `ctViewRM*256 = 6144` units — the billboard looks completely different from the 3D geometry. Original masked with fog; without fog the hard switch is jarring. | Added `#ifdef _opengl zs = 0; #endif` in `_RenderObject` to force the 3D-model path for all distances in GL. Modern GPU handles it trivially. |
| Software mipmap (`lpTexture2`) wiped out most leaf pixels | `CreateMipMapMT`/`CreateMipMapMT2` early-exited with output=0 whenever the top-left pixel of a 2×2 block was transparent — even if the other three pixels were solid leaf. Result: any leaf at the corner of a 2×2 block was zeroed in `lpTexture2`, making the distance-LOD texture look completely bare. | Changed both functions: only output 0 when ALL four source pixels are transparent; otherwise borrow a non-zero neighbour before averaging. |
| Ground terrain pops height as camera moves (LOD boundary) | `RenderGround` switches between `ProcessMap` (full-detail 1×1 tiles, inner ring) and `ProcessMap2` (coarse 2×2 tiles, outer ring) at `ctViewR1=28`. As the camera moves, tiles cross this boundary and the coarser triangulation interpolates different heights than the fine mesh, causing visible height jumps. T-junction snapping helped edges but couldn't fix the interior difference. | In GL path: `ProcessMap2` outer loop skipped entirely; `ProcessMap` loop extended to start from `ctViewR` so all tiles at all distances use the full-detail mesh. Odd-vertex skip and T-junction snapping also disabled for GL (no longer needed, would corrupt heights). |

## Performance Notes

Observed: 30–50fps drops at 2560×1440 fullscreen after Phase 3/4 fixes. Root cause not yet isolated. Attempts so far and outcomes:

| Change | Outcome |
|---|---|
| `d3dmemmapsize` 128 → 512 (texture cache) | No measurable improvement |
| Restore `ProcessMap2` for outer terrain rings (r ≥ ctViewR1) | Terrain height gaps / texture seams returned |
| Frustum cull tiles behind camera in `ProcessMap` | Reversed sign convention — culled visible tiles, broke rendering |
| `GVCnt` flush threshold 380 → 1100 (larger vertex batches) | Caused leaf texture glitches at batch boundaries; no fps gain |
| BMP sprite fade (`if (GlassL==0) zs=0`) | Leaf texture changes appeared at far range; reverted |

Key constraints: re-enabling `ProcessMap2` consistently causes texture seams. The terrain full-detail pass (`ProcessMap` for all rings) and all-3D objects are the two main regressions in triangle/draw-call count versus the original D3D path.

## Phase 5 — Modern Asset Pipeline (FUTURE)

- Load PNG/TGA/DDS textures with mipmapping
- Load standard model formats (glTF, OBJ) alongside .CAR
- Normal map support
- PBR material system

## Phase 6 — Gameplay and Engine Improvements (FUTURE)

- Modern input handling (raw input, configurable bindings)
- Improved AI (behavior trees, NavMesh pathfinding)
- Physics integration (ragdoll, foliage interaction)
- Audio modernization (OpenAL or SDL_mixer)
- Support for Virtual Reality

## Coding Conventions

- Use C++17 standard
- Prefer `std::string`, `std::vector`, `std::array` over raw C arrays where practical
- Use `#pragma once` for header guards
- Keep the original file/function names where possible for traceability back to the original source
- Comment any behavioral changes from the original with `// SOURCEPORT: <description>`
- Use shell variables and relative paths, not hardcoded absolute paths
- Validate assumptions before implementing — if unsure about original engine behavior, check the original source first

## Important Context
- This source port distributes only the engine — users must supply their own game assets from a retail copy.
- The .CAR model format is documented by community tools: C3Dit (https://github.com/carnivores-cpe/c3dit) can read/write .CAR files.
