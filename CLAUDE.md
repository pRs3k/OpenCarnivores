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
| Texture shimmer / alpha flicker on foliage at distance | No mipmaps — `GL_TEXTURE_MIN_FILTER` was `GL_LINEAR` for all textures, causing GPU to sample a random full-res texel from a tiny triangle → shimmer on opaque surfaces, binary 0/1 flicker on alpha-keyed foliage. Terrain excluded: its manual DataA/DataB LOD swap + diagonal triangulation produce a pinwheel artifact with hardware mipmaps. | `glGenerateMipmap` + `GL_LINEAR_MIPMAP_LINEAR` added for transparent textures in `gl_UploadTexture16` (keyed on `hasTransparency`) and in `RendererGL::UploadTexture16`. Alpha discard threshold lowered `0.5 → 0.1` so mip-averaged leaf coverage (e.g. 30%) still passes instead of being discarded. "Upload twice" trick for foliage: upload with transparent pixels filled to average opaque colour → `glGenerateMipmap` → re-upload original at level 0. Mip levels 1+ average correctly without darkening; level 0 retains original black transparent pixels for close-up. |
| Terrain texture pop at ~2560 units (DataA→DataB switch) | Original `ProcessMap`/`ProcessMap2` selected 128×128 `DataA` vs 64×64 `DataB` based on `zs` distance threshold; hard pop visible without fog. | Added `#ifdef _opengl` guard to always use `DataA`; hardware trilinear mipmapping handles LOD smoothly. |
| Sun too large / white box / white ring / solid white disc | (1) No resolution scaling: sun model sized for `CameraW=400` (640×480); at 2560×1440 `CameraW=1200` makes sun 3× bigger. (2) `fproc1` forced `color.a=1.0` → additive at 100% intensity → rays washed out. (3) Filled transparent pixels in mip levels 1+ bled white in additive mode → white ring at disc edge. | (1) `d *= (CameraW/400.f) * 1.15f` in `RenderSun` normalises angular size across resolutions. (2) Changed `d3dFlushBuffer(fproc1,0)` → `d3dFlushBuffer(0,fproc1)` so vertex alpha (~0.78) governs additive brightness. (3) Disable mip sampling during sun draw: bind texture, set `GL_TEXTURE_MIN_FILTER=GL_LINEAR`, draw, restore `GL_LINEAR_MIPMAP_LINEAR`. |
| Tree / bush appearance changes completely at certain distances | `_RenderObject` switched from the 3D model to a flat pre-rendered BMP billboard sprite at `ctViewRM*256 = 6144` units — the billboard looks completely different from the 3D geometry. Original masked with fog; without fog the hard switch is jarring. | Added `#ifdef _opengl zs = 0; #endif` in `_RenderObject` to force the 3D-model path for all distances in GL. Modern GPU handles it trivially. |
| Software mipmap (`lpTexture2`) wiped out most leaf pixels | `CreateMipMapMT`/`CreateMipMapMT2` early-exited with output=0 whenever the top-left pixel of a 2×2 block was transparent — even if the other three pixels were solid leaf. Result: any leaf at the corner of a 2×2 block was zeroed in `lpTexture2`, making the distance-LOD texture look completely bare. | Changed both functions: only output 0 when ALL four source pixels are transparent; otherwise borrow a non-zero neighbour before averaging. |
| Ground terrain pops height as camera moves (LOD boundary) | `RenderGround` switches between `ProcessMap` (full-detail 1×1 tiles, inner ring) and `ProcessMap2` (coarse 2×2 tiles, outer ring) at `ctViewR1=28`. As the camera moves, tiles cross this boundary and the coarser triangulation interpolates different heights than the fine mesh, causing visible height jumps. T-junction snapping helped edges but couldn't fix the interior difference. | In GL path: `ProcessMap2` outer loop skipped entirely; `ProcessMap` loop extended to start from `ctViewR` so all tiles at all distances use the full-detail mesh. Odd-vertex skip and T-junction snapping also disabled for GL (no longer needed, would corrupt heights). |
| Player can multi-jump by holding jump button | `kfJump` was set every frame the key was held, so `YSpeed == 0 && !SWIM` could pass again mid-air on the frame of landing while key still held | Gate `kfJump` on rising edge only: `if (!(\_KeyFlags & kfJump))` — same pattern already used for `kfCall` |
| Binoculars expose raw scene on widescreen sides | `BINOCUL.3DF` model was designed for 4:3; Hor+ widescreen leaves extra horizontal area uncovered | After rendering the binocular model, fill side bars with `FillRect` black: width = `(WinW − WinH×4/3) / 2` each side. TODO: replace black bars with a native widescreen binocular graphic. |

## Phase 5 — Audio (COMPLETE)

### Architecture
```
Audio_SDL.cpp     — Legacy SDL2 software mixer (kept for reference / fallback)
Audio_OpenAL.cpp  — OpenAL Soft backend (current default): native 3D positional audio via alSource3f
                    + AL_POSITION + AL_LINEAR_DISTANCE_CLAMPED; per-voice EFX AL_LOWPASS filter for
                    terrain occlusion; 16 one-shot channels + 2 ambient slots with crossfade.
Audio_DLL.cpp     — Thin wrapper exposing the original engine's audio API over whichever backend.
```

### OpenAL Soft specifics
- `SDL_Audio_AddVoice3dv` picks a free source, sets `AL_POSITION`, applies an AL_LOWPASS filter (`AL_DIRECT_FILTER`) derived from `ComputeTerrainOcclusion` for spatial voices. Listener-relative voices (UI/weapon) play dry.
- `ComputeTerrainOcclusion` raycasts 12 samples of `GetLandH` between listener and source; returns `[0..1]` weighted by fraction-blocked × penetration-depth, with a long-distance grazing boost. Maps to `AL_LOWPASS_GAIN` (up to −85%) and `AL_LOWPASS_GAINHF` (up to −95%) so blocked roars sound low and distant.
- Distance attenuation: `MAX_RADIUS=14000`, `MIN_RADIUS=768`, `AL_ROLLOFF_FACTOR=1.0`. World scale is ~256 units/meter; the older 6000-unit radius made sounds drop off within a few seconds of walking.
- Buffer cache keyed by `(data ptr, length)` — game keeps sfx alive for the session, so pointer identity is stable.
- Ambient crossfade tick lives in `SDL_Audio_SetCameraPos`; menu `SetAmbient` snaps-to-target to avoid silence on menu transitions where the camera-pos update isn't called.

### Key implementation notes
- `SDL_INIT_AUDIO` added to `SDL_Init` in Hunt2.cpp
- `InitAudioSystem` ignores the legacy `driver` index (was DLL selector); only `g_noSnd` (set by `-nosnd`) disables audio — old save files with `OptSound=-1` no longer silence the game
- `ReleaseResources()` calls `AudioStop()` before freeing `Ambient[].sfx.lpData` and `RandSound[].lpData` to prevent use-after-free in the SDL audio callback thread
- Menu ambients: `fxMenuAmb/Go/Mov` loaded at startup; `MenuStartAmb()` in Menu.cpp starts MENUAMB on MENUM/MENU2/OPT screens; `gMenuAmbActive` guard prevents audible loop restarts on sub-screen transitions; `gMenuAmbActive` reset at `RunMenus()` entry so ambient restarts correctly after returning from a hunt
- `CompositeMenu()` plays MENUMOV on hover-enter and MENUGO on map-button click
- Ship hum (`ShipModel.SoundFX[0]`) set as ambient in `RunMenus()` for the MENUR (player select) screen; MENUAMB crossfades in when the player reaches MENUM

### Bugs fixed
| Bug | Cause | Fix |
|---|---|---|
| No audio on launch | `OptSound=-1` saved in trophy file from previous `-nosnd` run; `InitAudioSystem` used it as disable flag | `InitAudioSystem` now ignores `driver` param; only explicit `-nosnd` flag (via `Audio_SetNoSnd()`) disables audio |
| Crash clicking menus | `ReleaseResources()` freed `Ambient[].sfx.lpData` while SDL audio callback still held pointer | Call `AudioStop()` at start of `ReleaseResources()` |
| MENUAMB never audible | `AudioCB` ambient loop skipped fade logic when `volume==0` (initial state), so volume never incremented | Move fade step before the mix; early-out only after fade when volume is still 0 |
| Sounds dropped off too quickly while walking away | `MAX_RADIUS=6000` (~23 m at 256 units/m) with linear-clamped attenuation — passed zero inside a few seconds of walking | Bumped `MAX_RADIUS=14000` / `MIN_RADIUS=768` so the gain curve stays near 1.0 for a natural range before rolling off |
| Flying dino impact thud played twice | `AnimateDimorDead` called `ActivateCharacterFx` on both the `DIM_FALL` transition and the `DIM_DIE` transition; flyer CARs often map both phases to the same thud sample, so they fired back-to-back at ground impact | Compare `Anifx[DIM_FALL]` vs `Anifx[DIM_DIE]` at the impact transition and skip the DIE fx when they match |
| Periodic stutter every few seconds at steady 144 Hz | `HotReload::Tick` stat()'d every registered file (hundreds of textures) on each 333 ms poll, spiking a frame past the VSync budget while the average FPS counter didn't reflect it | Round-robin 8 files per 100 ms tick (`HotReload.cpp`); full sweep takes a few seconds — still fine for edit-and-save |
| Ground models displayed wrong textures after an inter-level reload | `LoadPictureTGA` registered `pic.lpImage` as a `TextureOverrides` key; after `ReleaseResources` freed the menu buffer, the heap recycled that address for a terrain/model buffer and the stale menu override bled onto ground geometry | Reverted the `LoadPictureTGA` override hook; menu/UI overrides need a path-keyed registry instead of pointer-keyed |
| Ground/object impact particles rendered pale yellow instead of sandy brown | Two stacked issues: (1) `RenderElements` cleared `hTexture` but left `hMaterial`/`hCustomMaterial` pointing at whatever `d3dSetTexture` last set during `RenderWater`, so particles drove `uPBR=1` with garbage normal/MR/AO bindings; (2) `basic.frag`'s hard `clamp(rgb*uBrightness, 0, 1)` clipped R and G before B on any saturated warm color, turning `#B4824B × 1.7+` into pale yellow `(1,1,0.6)` | `RenderElements` entry now also clears `hMaterial = hCustomMaterial = NULL`; `basic.frag` replaces the hard clamp with a hue-preserving scale-down: if `max(r,g,b) > 1` divide the whole vector by it, so chroma survives elevated brightness |

## Phase 6 — Modern Asset Pipeline

### Implemented and verified

**32-bit texture overrides (PNG/TGA/BMP/JPEG)** — `TextureOverrides.cpp`/`.h`, decoded via stb_image (`deps/stb/stb_image.h`).

Architecture:
- Registry keyed by the CPU pointer of the original RGB555 buffer → decoded RGBA8 + (w,h).
- Renderer upload paths (`gl_UploadTexture16` → shared `gl_UploadRGBA` in renderd3d.cpp; `RendererGL::SetTexture` in RendererGL.cpp) check the registry before decoding the 16-bit data. On hit, the 32-bit override is uploaded at any resolution with full 8-bit alpha and hardware-generated mipmaps; on miss, existing 16-bit path runs unchanged.
- Shared `gl_UploadRGBA` preserves the foliage-mip-fix (fill transparent pixels with avg opaque color for levels 1..N, restore original at level 0).
- Loaders call `TextureOverrides::TryRegisterSibling(ptr, filePath)` or `TryRegisterWithExts(ptr, basePath)` immediately after loading the 16-bit data. Tries `.png`, `.tga`, `.bmp`, `.jpg` in order.

Override file naming convention:

| Asset type | Override path |
|---|---|
| Standalone `.3DF` (sun, moon, compass, binoculars) | `<path>.png` sibling (e.g. `HUNTDAT/BINOCUL.png`) |
| Character `.CAR` (dinos, weapons, wind, ship) | `<path>.png` sibling (e.g. `HUNTDAT/TREX.png`) |
| Terrain textures (in `.RSC`) | `<ProjectName>_tex_NN.png` (e.g. `AREA1_tex_00.png`) |
| Object 3D models (in `.RSC`) | `<ProjectName>_obj_NN.png` |
| Object BMP billboards (in `.RSC`) | `<ProjectName>_obj_NN_bmp.png` |
| Sky dome | `<ProjectName>_sky.png` |

NN is zero-padded 2-digit index matching the RSC load order. Zero-impact when no overrides are present — rendering is byte-identical to the 16-bit-only path. Game .CAR/.3DF/.RSC files are never modified.

Key implementation notes:
- stb_image with `req_comp=4` returns `R0 G0 B0 A0 …` — as little-endian uint32 that's `0xAABBGGRR`, matching `gl_UploadTexture16`'s output exactly, so a straight `memcpy` into the registry buffer works.
- GL path only uses terrain `DataA` (all-`DataA` fix from Phase 4), so one override per terrain texture is sufficient — `DataB/C/D` never bind in GL.
- Registry takes ownership of RGBA buffers; `TextureOverrides::Shutdown()` frees all. Re-registration for the same key deletes the previous buffer.

**DDS / BCn compressed texture overrides** — `TextureOverrides.cpp` `RegisterDDS`. Parses the DXT1/DXT3/DXT5/BC5/BC7 subset of DirectDraw Surface files and uploads directly via `glCompressedTexImage2D`, preserving all mip levels from the file. `TryRegisterWithExts` probes `.dds` first, then `.png/.tga/.bmp/.jpg`. BCn is ~4–6× smaller in VRAM than uncompressed RGBA so 4K overrides stay practical.

**glTF / OBJ model overrides** — `ModelOverrides.cpp`/`.h` (via tinygltf + tinyobjloader). `TryLoadSibling(carPath)` probes `<stem>.gltf/.glb/.obj` next to the original `.CAR`; registry keyed by the game's model pointer so `RenderModel` can substitute on a hit. Retail .CAR load path is untouched and remains the fallback.

**PBR material system** — `Materials.cpp`/`.h`. Strictly opt-in per asset: drop `<stem>_normal.png`, `<stem>_mr.png` (metallic in R, roughness in G — glTF convention), `<stem>_ao.png` next to the base texture and `Materials::RegisterFromStem` binds them to the same CPU-pointer key as the albedo override. `basic.frag` gets a Cook-Torrance GGX path (DistributionGGX + GeometrySmith + FresnelSchlick) gated on `uPBR`; tangent frames are reconstructed in the fragment shader from screen-space derivatives (Christian Schüler trick) so no per-vertex tangent attribute is needed. `RendererGL::BindMaterial(key)` flips the uniform on before a draw and back off after; null key = retail Lambert path, byte-identical to pre-PBR.

**Hot reload for textures, shaders, and `_RES.txt`** — `HotReload.cpp`/`.h`. `HotReload::Watch(path, callback)` registers a file; `HotReload::Tick()` is called once per frame from `Hunt2.cpp`'s main loop. Polling is throttled to 100 ms cadence with a round-robin budget of 8 files per tick (see `HotReload::Tick`) so hundreds of registered textures don't hitch the frame — full sweep takes a few seconds, plenty fast for edit-and-save. Texture edits call `g_glRenderer->InvalidateTextureCache()` + `d3dInvalidateAllTextures()` to nuke GL texture caches and force re-upload; shader edits recompile `basic.vert`/`basic.frag` and keep the old program if recompile fails. `TextureOverrides::TryRegister*` call sites and `LoadResourcesScript` all register watches.

**External shader files** — `shaders/basic.vert`/`shaders/basic.frag` loaded preferentially at `CompileShaders` time via `ReadTextFile`, with the embedded string literal kept as a fallback for release builds where shaders aren't shipped as loose files.

**Shader-based custom material system** — `CustomMaterials.cpp`/`.h`. Modders attach a custom GLSL shader to any asset by dropping a `<stem>.material` file next to the asset (e.g. `HUNTDAT/TREX.material` next to `HUNTDAT/TREX.CAR`). The file format is line-based, whitespace-separated, with `#` comments — full JSON/TOML would drag in a dependency for what's essentially a flat key/value list.

Directives:

| Directive | Effect |
|---|---|
| `shader <name>` | Loads `shaders/<name>.vert` + `shaders/<name>.frag` as the program |
| `tex <uniform> <path>` | PNG/TGA/BMP/JPG → next free sampler unit (1..N); unit 0 is the engine's albedo binding |
| `float <uniform> <x>` | Sets a `uniform float` |
| `vec2 <uniform> <x> <y>` | Sets a `uniform vec2` |
| `vec3 <uniform> <x> <y> <z>` | Sets a `uniform vec3` |
| `vec4 <uniform> <x> <y> <z> <w>` | Sets a `uniform vec4` |

Vertex attribute locations are fixed to match `shaders/basic.vert` (`0 aPos`, `1 aDepth`, `2 aColor`, `3 aSpecular`, `4 aTexCoord`) — modder shaders reuse these so the engine's existing vertex buffers just work. The engine always provides `uProjection` (mat4) and binds the albedo to `uTexture` on unit 0; everything else is the modder's call, including fog/alpha-test/brightness (modders who want those can reuse the math from `basic.frag`).

Precedence at draw time: **custom material → PBR material → default Lambert**. `renderd3d.cpp` tracks `hMaterial` and `hCustomMaterial` in lockstep with `hTexture`; `RendererGL::BindCustomMaterial` switches GL to the custom program and `CustomMaterials::Apply` pushes the material's uniforms + textures. Because GL uniforms are per-program and persist across `glUseProgram` swaps, the default engine program's fog/alpha/brightness/PBR state doesn't need re-pushing after the custom draw ends — just `glUseProgram(m_shaderProgram)` restores it. The projection matrix is cached in `m_projMatrix` every `BeginFrame` so the custom program gets the current screen-space transform.

Registry is pointer-keyed the same as `TextureOverrides` and `Materials`, and `Resources.cpp` calls `CustomMaterials::TryRegisterSibling/WithExts` at every texture-load call site. Hot reload is wired to the `.material` file itself — editing the material (or its referenced `.vert`/`.frag`) triggers `CustomMaterials::Reload`, which drops the shader-program cache entry, re-reads the `.vert`/`.frag` from disk, recompiles, and re-applies the material's uniform state. Brand-new `.material` files dropped in live are also picked up without a restart: the reload callback falls through to `TryRegisterWithExts` when the key isn't yet registered.

Example shader pair and annotated `.material` template ship in `shaders/custom_tint.vert`, `shaders/custom_tint.frag`, and `shaders/example.material`.

### Remaining for this phase

- Virtual filesystem: wrap scattered path lookups in a VFS that can mount zips/folders with priority, enabling mod packs without overwriting game files
- Data-driven dinos/weapons: move hardcoded stats out of `Characters.cpp` / `Game.cpp` into editable TOML/JSON
- Menu/UI picture overrides — `LoadPictureTGA` needs a path-keyed registry (not pointer-keyed — the heap recycles menu-picture addresses for terrain/model buffers after inter-level `ReleaseResources`, so pointer keys cause cross-asset bleed)

## Phase 7 — Gameplay and Engine Improvements (FUTURE)

- Modern input handling (raw input, configurable bindings)
- Improved AI (behavior trees, NavMesh pathfinding)
- Physics integration (ragdoll, foliage interaction)
- ~~Audio modernization (OpenAL or SDL_mixer)~~ DONE — migrated to OpenAL Soft (`Audio_OpenAL.cpp`)
- Support for Virtual Reality
- Formalize the renderer abstraction: move all `d3d*` functions behind the `Renderer` interface entirely, kill `renderd3d.cpp` glue so Vulkan / Metal / WebGPU backends become drop-in
- Decouple frame rate from simulation: split into fixed-step sim + interpolated render for smooth 90/120 Hz VR
- Embed Lua or AngelScript with bindings for `Character` struct + event hooks (OnDamage, OnSpawn, OnFire) to open modding to non-C++ devs

### VR plumbing (requires HMD pipeline in place first)

- Stereo rendering hook: render the scene twice per frame with per-eye view matrices; thread an `Eye` parameter through `RenderHunt` (currently assumes single viewpoint)
- OpenXR session: open an XR session, get headset pose + per-eye projection matrices + eye swapchain textures, submit rendered eye textures back to the runtime for lens-distortion composite
- SDL3 or OpenXR input abstraction: replace `_KeyFlags` bitfield with an action-binding layer so VR controllers, gamepads, and rebindable keyboards all route through it
- Head-tracking as camera source: abstract `CameraAlpha/Beta` behind a `CameraController` interface so OpenXR HMD pose can drive it
- 6DoF locomotion + snap turn options for VR comfort
- World-space UI: `Interface.cpp` draws to 2D screen coords — needs a canvas layer that can render to a quad in 3D for VR

### Audio (building on OpenAL Soft)

- EFX reverb zones: wire per-area reverb presets using `alGenEffects` (ready-made presets in `efx-presets.h` e.g. `EFX_REVERB_PRESET_FOREST`)
- HRTF toggle: `ALC_HRTF_SOFT` enable/disable via options menu; critical for VR immersion
- Occlusion/obstruction via raycast against terrain mesh (currently `Audio_UploadGeometry` is a no-op stub)
- Move ambient fade tick out of `SDL_Audio_SetCameraPos` into a dedicated `SDL_Audio_Update()` called every frame regardless of game state (current snap-to-target in menus is a workaround)

### Dev infrastructure

- GitHub Actions CI for Windows + Linux + macOS builds per commit
- Unit tests for `mathematics.cpp` (pure functions, easy win)
- SDL3 migration when stable (better HiDPI, gamepad, dialog APIs)
- clang-tidy / ASan pass over the 1999 code for undefined-behavior issues

## Coding Conventions

- Use C++17 standard
- Prefer `std::string`, `std::vector`, `std::array` over raw C arrays where practical
- Use `#pragma once` for header guards
- Keep the original file/function names where possible for traceability back to the original source
- Comment any behavioral changes from the original with `// SOURCEPORT: <description>`
- Use shell variables and relative paths, not hardcoded absolute paths
- Validate assumptions before implementing — if unsure about original engine behavior, check the original source first
- **Mod compatibility rule**: every new asset loader is additive; never remove a retail-format parser (.CAR, .3DF, .RSC, .MAP, .TGA, .WAV). Existing mods like Carnivores 2+ must continue to load unchanged. New formats (glTF, PNG, JSON dino defs, etc.) slot in beside the originals, never replace them.

## Important Context
- The .CAR model format is documented by community tools: C3Dit (https://github.com/carnivores-cpe/c3dit) can read/write .CAR files.
