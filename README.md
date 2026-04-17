# OpenCarnivores

A modern source port of **Carnivores 2** (Action Forms, 1999), reborn on SDL2 + OpenGL 3.3 + OpenAL Soft. The original DirectX 6 / DirectSound / x86 Glide binaries are gone; the engine now builds and runs natively on 64-bit Windows 10/11 and is being prepared for Linux, macOS, and VR.

> **You still need a legal copy of Carnivores 2.** OpenCarnivores ships only the engine. Drop the `.exe` + DLLs into your existing game folder next to `HUNTDAT/`, `AREA1.rsc`, etc.

---

## What's new vs. the original engine

- **Runs on modern Windows** without compatibility shims, wrappers, or dgVoodoo2
- **Any resolution, any aspect ratio** — widescreen Hor+ FOV, 4K, HiDPI-aware
- **Adaptive VSync** and uncapped framerate
- **OpenGL 3.3 renderer** with hardware trilinear mipmapping (no more terrain texture pop, no more foliage shimmer)
- **OpenAL Soft audio** — ready for HRTF, EFX reverb, and true 3D positional audio
- **PNG / TGA / BMP / JPEG texture overrides** at any resolution with true 8-bit alpha (see below)
- Many subtle rendering fixes the original had: correct fog, correct alpha on transparent foliage, proper z-buffer on HUD overlays, no more black boxes around distant sprites, etc.

All original game assets (`.CAR`, `.3DF`, `.RSC`, `.MAP`, `.TGA`, `.WAV`) load **unchanged** — existing mods like Carnivores 2+ drop in and work.

---

## For modders: high-resolution texture overrides

Previously, every in-world texture in Carnivores 2 was stored inside the binary `.CAR` / `.3DF` / `.RSC` files as **RGB555 (15-bit color + 1-bit transparency key)** — only 32,768 possible colors, fixed resolutions (256×256 for models, 128×128 for terrain), and no partial alpha. Editing anything meant ripping pixels out of binary files with third-party tools.

**You can now drop a PNG (or TGA/BMP/JPEG) next to any game asset and OpenCarnivores will use it instead** — at any resolution, with true 24-bit color and 8-bit alpha, automatically mipmapped by the GPU.

The original files are never touched. If you delete the PNG, the game reverts to the original textures with zero fuss.

### Naming convention

Place override files in the game folder following this pattern:

| Asset | Override file | Example |
|---|---|---|
| Standalone `.3DF` (sun, moon, compass, binoculars) | `<same-stem>.png` next to the original | `HUNTDAT/BINOCUL.png` next to `HUNTDAT/BINOCUL.3DF` |
| Character `.CAR` (dinos, weapons, ship, wind) | `<same-stem>.png` next to the original | `HUNTDAT/TREX.png` next to `HUNTDAT/TREX.CAR` |
| Terrain textures (baked into the `.RSC`) | `<ProjectName>_tex_NN.png` in the game root | `AREA1_tex_00.png`, `AREA1_tex_01.png`, … |
| Object 3D models (baked into the `.RSC`) | `<ProjectName>_obj_NN.png` | `AREA1_obj_00.png` |
| Object billboards (baked into the `.RSC`) | `<ProjectName>_obj_NN_bmp.png` | `AREA1_obj_00_bmp.png` |
| Sky dome | `<ProjectName>_sky.png` | `AREA1_sky.png` |

- `<ProjectName>` is the base name of the map's `.rsc` file, lowercase/uppercase as used by the game.
- `NN` is a zero-padded 2-digit index matching the load order in the `.RSC` (texture 0 → `_tex_00.png`, texture 1 → `_tex_01.png`, etc.).
- Extensions `.png`, `.tga`, `.bmp`, `.jpg` are all accepted — first one found wins.

### How to port an existing texture mod

1. **Rip** the textures you want to replace out of the `.CAR` / `.RSC` using your favorite tool (e.g. [C3Dit](https://github.com/carnivores-cpe/c3dit)). Or, for the `.RSC` textures, just reference the load order in the RSC file.
2. **Upscale / repaint** at any resolution — 512×512, 1024×1024, even 4K. Save as 32-bit PNG with alpha if you want smooth transparency.
3. **Name and drop** the file per the table above.
4. **Launch the game.** That's it.

You don't need to recompile anything, repack the `.RSC`, or modify game files. Remove the PNG to revert. Mix and match with other mods freely — PNG overrides take priority over the binary data, and two texture packs that override different files coexist without conflict.

### Tips

- Transparent pixels in the original had to be solid color-key; PNG alpha gives you smooth edges on leaves, glass, smoke, and water trims.
- Model UVs are normalized 0..1, so any resolution works — a 1024×1024 PNG just looks 4× sharper than the original 256×256.
- The game's alpha-test shader discards pixels under ~10% opacity, so feathered edges near full transparency work as you'd expect.
- High-res textures increase VRAM use; a modern GPU handles it easily, but a full 4K texture pack will use more memory than the original.

---

## Coming next

Planned additions that will further expand what modders can do (see `CLAUDE.md` for the full roadmap):

- DDS/BCn compressed texture loading (smaller VRAM footprint for 4K packs)
- glTF / OBJ model loading alongside `.CAR`
- JSON/TOML dino and weapon stats (no more recompiling to tweak balance)
- Virtual filesystem with `mods/` folder priority — clean mod packs, no file overwrites
- Hot-reload for textures, shaders, and resource definitions
- Normal maps, PBR materials, custom shaders
- Lua scripting for AI and gameplay events
- VR support (OpenXR)

---

## Building from source

```
cmake -B build
cmake --build build --config Release
```

Requires Visual Studio 2022 (or MSVC toolchain) and CMake 3.20+. All dependencies (SDL2, OpenAL Soft, glad, stb_image) are vendored under `deps/`.

Run `build/Release/OpenCarnivores.exe` from the Carnivores 2 game folder, or pass `-width=N -height=N -fullscreen` on the command line.

---

## Credits

- Original game: **Action Forms**, 1999
- Source code preservation: [videogamepreservation/carnivores2](https://github.com/videogamepreservation/carnivores2)
- OpenCarnivores port: engine modernization, renderer abstraction, audio/asset pipelines
- Dependencies: SDL2 (zlib), OpenAL Soft (LGPL), glad (MIT), stb_image (public domain)

Not affiliated with Action Forms. You must own a legal copy of Carnivores 2 to use this engine.
