# Carnivores 2 Source Port — Project Guide

## Project Overview

This is a source port of Carnivores 2 (1999, Action Forms) from its original Win32/DirectX 6 codebase to a modern, cross-platform engine using SDL2 and OpenGL 3.3. The goal is to produce a clean, maintainable engine that runs on modern systems while remaining fully compatible with the original game assets.

The original source code was publicly released by the rights holders (Action Forms / Tatem Games) and is available at:
https://github.com/videogamepreservation/carnivores2

This project is licensed under MIT (matching the existing community forks).

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
- **Target**: Windows 10+ initially, cross-platform later

## Phase 1 — Get It Building (CURRENT PHASE)

### Objectives
1. Fork the videogamepreservation/carnivores2 repo
2. Create a CMakeLists.txt build system to replace the VS6 project files
3. Fix all compiler errors for modern C++ (C++17 standard)
4. Produce a working executable that links and runs

### Common Issues to Expect
- **Missing Windows SDK types**: Old code uses `LPDIRECTDRAW`, `LPDIRECT3D`, etc. — these need the legacy DirectX SDK headers or stubs
- **Deprecated C functions**: `sprintf` → `snprintf`, `strcpy` → `strncpy`, etc.
- **Implicit casts**: MSVC6 was very permissive with pointer/int conversions
- **Inline ASM**: `renderasm.cpp` uses `__asm` blocks — these won't compile on x64. Wrap in `#ifdef _M_IX86` or rewrite in C++
- **Global state**: Nearly everything is in global variables declared in `Hunt.h`
- **Missing headers**: May need `<cstdint>`, `<cstring>`, etc.

### Build System
Create CMakeLists.txt at root level. The project should produce a single executable. Link against:
- Windows SDK (kernel32, user32, gdi32, winmm)
- DirectX SDK headers (for initial compilation — will be removed in Phase 2)
- DirectSound (dsound.lib) for audio initially

### Definition of Done
- `cmake --build .` produces an executable with zero errors
- Executable launches and attempts to load game assets (even if it crashes due to missing assets, it should get past compilation and linking)

## Phase 2 — Get It Running

### Objectives
1. Obtain original Carnivores 2 game assets from a retail install
2. Configure asset paths so the compiled engine finds and loads them
3. Validate that the game plays identically to the original retail release
4. Document any behavioral differences

### Asset Structure
The game expects assets in the working directory:
- `HUNTDAT/` — Map files, terrain data
- `ANIMALS/` — .CAR model files for dinosaurs
- `WEAPONS/` — Weapon models
- `MENU/` — Menu graphics and RAW image files
- `_RES.TXT` — Master resource definition file
- Various .TGA, .BMP texture files

### Definition of Done
- Game launches, shows menu, loads a hunt, renders terrain and dinosaurs
- Gameplay loop works: select hunt → load map → hunt → return to menu
- No crashes during normal gameplay

## Phase 3 — Abstract the Renderer

### Objectives
1. Create a `Renderer` interface/abstract class
2. Move all Direct3D 6 code behind this interface
3. Move all 3Dfx Glide code behind this interface
4. Move software renderer behind this interface
5. Create a new SDL2 + OpenGL 3.3 backend implementing the interface

### Architecture
```
renderer/
  Renderer.h          — Abstract interface
  RendererD3D6.cpp    — Original D3D6 (keep for reference, can be disabled)
  RendererGlide.cpp   — Original Glide (keep for reference, can be disabled)
  RendererSoft.cpp    — Original software (keep for reference, can be disabled)
  RendererGL.cpp      — NEW: SDL2 + OpenGL 3.3 backend
```

### Key Rendering Operations to Abstract
- Initialize display (window creation, context setup)
- Begin/end frame
- Clear buffers
- Draw textured triangle lists (the core rendering primitive)
- Set texture (upload and bind)
- Set fog parameters
- Set blend mode
- Draw the sky/horizon
- Draw water surfaces
- Draw HUD/UI elements (2D overlay)
- Screenshot capture
- Shutdown/cleanup

### Dependencies to Add
- SDL2 (windowing, input, audio — replaces Win32 + DirectSound)
- GLEW or glad (OpenGL extension loading)
- OpenGL 3.3 Core Profile

### Definition of Done
- Game runs with the new OpenGL backend producing visually correct output
- Original renderers still compile behind #ifdef for reference
- SDL2 handles window creation, input events, and audio

## Phase 4 — Resolution and Display (FUTURE)

- Arbitrary resolution support (remove hardcoded 640x480, 800x600, 1024x768)
- Widescreen aspect ratio support (16:9, 21:9)
- Fullscreen, windowed, borderless windowed modes
- VSync toggle
- High-DPI awareness

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

- The original game assets (.CAR models, textures, maps, sounds) are copyrighted by Action Forms / Tatem Games. The engine source code is MIT licensed. This source port distributes only the engine — users must supply their own game assets from a retail copy.
- The Carnivores modding community is active on ModDB and uses the "Modders Edition Engine" (M.E.E.) as their standard base. A long-term goal is for this source port to be compatible with M.E.E. mods and their extended _RES.TXT format.
- The .CAR model format is documented by community tools: C3Dit (https://github.com/carnivores-cpe/c3dit) can read/write .CAR files.
