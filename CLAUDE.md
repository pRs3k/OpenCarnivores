# Carnivores 2 Source Port — Project Guide

## Constraints
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
- **Build system**: CMake (to be created — original uses VS6 .dsp/.dsw)
- **Target**: Windows 10+ initially, cross-platform later, VR later

## Phase 1 — Get It Building (COMPLETE)

## Phase 2 — Get It Running

1. Validate that the game plays identically to the original retail release
2. Document any behavioral differences

### Asset Structure
The game expects assets in the working directory:
- `HUNTDAT/` — Map files, terrain data
- `ANIMALS/` — .CAR model files for dinosaurs
- `WEAPONS/` — Weapon models
- `MENU/` — Menu graphics and RAW image files
- `_RES.TXT` — Master resource definition file
- Various .TGA, .BMP texture files

### Definition of Done
- Gameplay loop works: select hunt → load map → hunt → return to menu
- No crashes during normal gameplay

## Phase 3 — Abstract the Renderer ✅ COMPLETE

### Architecture (implemented)
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

## Phase 4 — Resolution and Display

### Implemented
- Arbitrary resolution via `-width=N -height=N` command-line args
- Widescreen Hor+ FOV: `CameraH = CameraW * (WinH/WinW) * (4/3)`
- Fullscreen (`-fullscreen`), borderless (`-borderless`), windowed (default)
- Adaptive VSync (`SDL_GL_SetSwapInterval(-1)` with fallback to 1), toggle with `-vsync`/`-novsync`
- HiDPI: `SDL_WINDOW_ALLOW_HIGHDPI`, drawable size synced back to game globals

## Current Rendering Status ✅ FULLY WORKING

### Working
- Game launches, loads AREA1, enters hunt scene
- Terrain tiles with correct RGB555 textures, GL_REPEAT wrapping
- Sky plane (RenderSkyPlane) with correct sunset colors, slow world-position scroll
- Trees/vegetation (RenderElements) — billboard circles visible
- Character models (RenderModel / RenderModelClip) with correct textures, alpha, and fog
- Water circles (RenderWCircles via RenderModel)
- Fog system (CalcFogLevel → specular alpha channel → shader mix)
- Depth test (GL_GEQUAL, clear=0) — correct front-to-back order
- All HUD/UI elements: compass, wind indicator, sun, bullet indicator, call indicator, exit prompt
- Run with: `OpenCarnivores.exe "prj=HUNTDAT/AREAS/AREA1" -nosnd`
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
