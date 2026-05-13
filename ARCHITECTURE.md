# Architecture and Core Files

## Core game files
- `Hunt.h` — Main header with all structs, globals, and constants.
- `Hunt2.cpp` — Main game loop, Win32 window setup, WndProc.
- `Game.cpp` — Game logic, AI, dinosaur behavior, physics.
- `Characters.cpp` — Character/model loading and animation.
- `Interface.cpp` — Menu system, HUD, UI rendering.
- `Loading.cpp` — Asset loading (maps, textures, models, resources).
- `Resources.cpp` — _RES.txt parsing (game data definitions).
- `mathematics.cpp` — Vector math, matrix ops, collision detection.

## Domain-specific guides
- [RENDERING.md](RENDERING.md) — All rendering backends and texture override system.
- [AUDIO.md](AUDIO.md) — Audio system, backends, and 3D positional audio.
- [VR.md](VR.md) — VR plumbing, OpenXR, input, and current progress.
