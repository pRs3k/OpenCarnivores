# OpenCarnivores

A modern source port of **Carnivores 2** (Action Forms, 1999), reborn on SDL2 + OpenGL 3.3 + OpenAL Soft. The original DirectX 6 / DirectSound / x86 Glide binaries are gone; the engine now builds and runs natively on 64-bit Windows 10/11 and is being prepared for Linux, macOS, and VR.

---

## Quick start (for players)

**You need a legal copy of Carnivores 2.** The CD, a GOG copy, or an old install folder all work — anything that contains the `HUNTDAT/` folder and the `*.RSC`, `*.CAR`, `*.MAP`, `*.WAV` game files.

### Step 1 — Get the game files
If you own the CD, copy everything off it into a folder on your PC (for example, `C:\Games\Carnivores2`). Make sure the folder contains a `HUNTDAT` subfolder. If your copy already has a `Carn2.exe`, you can leave it — OpenCarnivores will live next to it.

### Step 2 — Download OpenCarnivores
Go to the **[Releases page](https://github.com/pRs3k/OpenCarnivores/releases)** on GitHub and download the latest `OpenCarnivores-Windows.zip`. Unzip it.

### Step 3 — Put the files together
Copy `OpenCarnivores.exe` (and the `.dll` files and `shaders/` folder next to it) into your Carnivores 2 folder — the one with `HUNTDAT` inside it.

### Step 4 — Double-click `OpenCarnivores.exe`
That's it. The game should launch at your monitor's native resolution.

### Troubleshooting
- **"Missing file" or black screen on launch** — you probably don't have the `HUNTDAT` folder next to `OpenCarnivores.exe`. Re-check Step 3.
- **Game is too dark or too bright** — open the in-game Options menu and adjust the Brightness slider; it updates live.
- **Resolution looks wrong** — open Options → Display and pick a resolution from the list. The choice is saved to `display.cfg`.

---

## What's new vs. the original engine

- **Runs on modern Windows** without compatibility shims, wrappers, or dgVoodoo2
- **Any resolution, any aspect ratio** — widescreen Hor+ FOV, 4K, HiDPI-aware
- **Adaptive VSync** and uncapped framerate
- **OpenGL 3.3 renderer** with hardware trilinear mipmapping (no more terrain texture pop, no more foliage shimmer)
- **OpenAL Soft audio** — 3D positional audio, terrain occlusion, ready for HRTF and EFX reverb
- **PNG / TGA / BMP / JPEG / DDS texture overrides** at any resolution with true 8-bit alpha (see below)
- **glTF / OBJ model overrides** alongside the retail `.CAR` files
- **PBR materials** (normal / metallic-roughness / AO maps) per-asset, opt-in
- **Custom GLSL shaders per asset** via `.material` files — no C++ required
- **Hot reload** for textures, shaders, `.material` files, and `_RES.txt` — edit & save, no restart
- **Lua 5.4 scripting** with `OnSpawn` / `OnDamage` / `OnFire` hooks for gameplay mods
- **Modern input** — raw mouse, Xbox / DualShock / Switch Pro gamepad support (hot-plug, analog look), and fully rebindable keyboard + gamepad bindings persisted to `controls.cfg`
- Many subtle rendering and AI fixes

All original game assets (`.CAR`, `.3DF`, `.RSC`, `.MAP`, `.TGA`, `.WAV`) load **unchanged**.

---

## For modders: high-resolution texture overrides

Previously, every in-world texture in Carnivores 2 was stored inside the binary `.CAR` / `.3DF` / `.RSC` files as **RGB555 (15-bit color + 1-bit transparency key)** — only 32,768 possible colors, fixed resolutions (256×256 for models, 128×128 for terrain), and no partial alpha. Editing anything meant ripping pixels out of binary files with third-party tools.

**You can now drop a PNG (or TGA/BMP/JPEG/DDS) next to any game asset and OpenCarnivores will use it instead** — at any resolution, with true 24-bit color and 8-bit alpha, automatically mipmapped by the GPU.

The original files are never touched. If you delete the override, the game reverts to the original textures with zero fuss.

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
- Extensions are probed in order: `.dds`, `.png`, `.tga`, `.bmp`, `.jpg` — first one found wins. DDS (BC1/3/5/7) is ~4–6× smaller in VRAM than raw RGBA, ideal for 4K packs.

### How to port an existing texture mod

1. **Rip** the textures you want to replace out of the `.CAR` / `.RSC` using your favorite tool (e.g. [C3Dit](https://github.com/carnivores-cpe/c3dit)). Or, for the `.RSC` textures, just reference the load order in the RSC file.
2. **Upscale / repaint** at any resolution — 512×512, 1024×1024, even 4K. Save as 32-bit PNG with alpha if you want smooth transparency.
3. **Name and drop** the file per the table above.
4. **Launch the game.** That's it.

You don't need to recompile anything, repack the `.RSC`, or modify game files. Remove the PNG to revert. Mix and match with other mods freely — overrides take priority over the binary data, and two texture packs that override different files coexist without conflict. **Hot reload** means you can edit a PNG in your image editor, hit save, and the game picks up the new version within a second — no restart.

### Tips

- Transparent pixels in the original had to be solid color-key; PNG alpha gives you smooth edges on leaves, glass, smoke, and water trims.
- Model UVs are normalized 0..1, so any resolution works — a 1024×1024 PNG just looks 4× sharper than the original 256×256.
- The game's alpha-test shader discards pixels under ~10% opacity, so feathered edges near full transparency work as you'd expect.
- High-res textures increase VRAM use; a modern GPU handles it easily, but a full 4K texture pack will use more memory than the original.

---

## PBR materials (normal / metallic-roughness / AO)

Drop these sibling files next to your base texture and they'll be picked up automatically:

| Map | Filename suffix | Channel convention |
|---|---|---|
| Normal | `<stem>_normal.png` | Tangent-space RGB |
| Metallic-roughness | `<stem>_mr.png` | Metallic in R, roughness in G (glTF) |
| Ambient occlusion | `<stem>_ao.png` | Greyscale in R |

PBR is strictly opt-in per asset. Assets without these sibling files continue to use the original Lambert shading, pixel-for-pixel. Tangent frames are reconstructed in the fragment shader from screen-space derivatives — you don't need to regenerate UV seams or tangent data.

---

## Custom shaders per asset (`.material` files)

Drop `<stem>.material` next to any asset to attach a custom GLSL program — e.g. `HUNTDAT/TREX.material` next to `HUNTDAT/TREX.CAR`. Line-based directive syntax:

```
shader  custom_tint            # loads shaders/custom_tint.vert + .frag
tex     uDetail  noise.png     # binds to next free sampler unit (unit 0 = albedo)
float   uMix     0.25
vec3    uTint    1.05 1.0 0.95
```

Editing the `.material` file, the `.vert`, or the `.frag` hot-reloads live. Precedence at draw time is **custom material → PBR material → default Lambert**. A reference pair and annotated template ship in `shaders/custom_tint.{vert,frag}` and `shaders/example.material`.

---

## glTF / OBJ model overrides

Drop `<stem>.gltf`, `<stem>.glb`, or `<stem>.obj` next to the retail `.CAR` and it replaces the model geometry (textures still come from the PNG sibling or the original `.CAR` data). Retail `.CAR` loading is untouched and remains the fallback.

---

## For modders: data-driven dino & weapon stats (JSON overlays)

Every dinosaur and weapon stat (health, damage, precision, smell/hearing/sight, trophy score, reload time, etc.) was previously baked into the retail `HUNTDAT/_res.txt` script. OpenCarnivores still reads `_res.txt` as the authoritative baseline, but you can now drop a JSON overlay next to it to tweak or add entries without touching the original file.

### Quick start

Create `HUNTDAT/dinos.json`:

```jsonc
{
  "dinosaurs": [
    {
      "name": "T-Rex",      // matches the retail entry by name
      "health": 1500,       // buffed from 1024
      "basescore": 25,
      "smell": 0.95
    },
    {
      "name": "Moschops",
      "health": 4           // nerfed so the shotgun doesn't one-shot
    }
  ]
}
```

Create `HUNTDAT/weapons.json`:

```jsonc
{
  "weapons": [
    { "name": "Shotgun", "power": 2.0, "shots": 8, "reload": 900 },
    { "name": "Sniper",  "prec": 1.4, "loud": 0.2 }
  ]
}
```

That's it — launch the game and the stats apply. Only fields present in the JSON are overwritten; everything you don't mention keeps its `_res.txt` value. **Hot reload** is wired: save the file and the running game re-layers the overlay within a second.

### Matching rules

Each entry is looked up against the retail table in this order:

1. `"id"` — retail index (0..N-1 after `_res.txt` loads). Most exact, brittle if another mod reorders the table.
2. `"ai"` — AI type constant (dinos only; `AI_TREX=18`, etc. — see `Hunt.h`).
3. `"name"` — exact case-sensitive match against the `name` in `_res.txt`.

If none of the above hit, the entry is appended as a brand-new creature or weapon. Capacity is 32 dinos and 8 weapons total across `_res.txt` + JSON.

### Supported fields

**Dinos**: `name`, `file`, `pic`, `ai`, `health`, `basescore`, `mass`, `length`, `radius`, `smell`, `hear`, `look`, `scale0`, `scaleA`, `shipdelta`, `danger` (bool)
**Weapons**: `name`, `file`, `pic`, `power`, `prec`, `loud`, `rate`, `shots`, `reload`, `trace`, `optic` (bool), `fall` (bool)

The parser accepts `//` line comments and `/* block */` comments — real JSON doesn't, but a modder config file is a better experience with them. Annotated templates live in `docs/dinos.example.json` and `docs/weapons.example.json`.

---

## For modders: Lua scripting

OpenCarnivores embeds **Lua 5.4** with three event hooks so you can add gameplay logic without recompiling the engine. Drop any number of `.lua` files into a `scripts/` folder next to the exe; each is loaded once at startup, and errors in one script log-and-continue so a typo in one mod can't break the others.

### Hooks

| Hook | Signature | When it fires |
|---|---|---|
| `OnInit` | `()` | Once, after all scripts have loaded |
| `OnSpawn` | `(ch)` | When a dinosaur (or the player) is (re)spawned |
| `OnDamage` | `(ch, amount)` | When the player's shot reduces a dino's HP |
| `OnFire` | `(weaponIndex)` | When the player successfully discharges a weapon |

`ch` is a read-only snapshot table with the fields: `id` (index into the `Characters` array), `ctype`, `name` (e.g. `"Trex"`, `"Parasaur"`), `ai`, `health` (post-damage for `OnDamage`), `state`, `x`, `y`, `z`, `alpha`, `beta`.

### Engine helpers

All exposed under a single `oc` global table:

```lua
oc.log(msg)                -- write to the log file
oc.message(msg)            -- show an in-game HUD message
oc.setHealth(id, hp)       -- write hp back (0 = instant kill)
oc.getHealth(id)           -- read hp; -1 if id invalid
oc.playerPos()             -- returns x, y, z of the player
oc.dinoCount()             -- number of live entries in Characters[]
oc.getCharacter(id)        -- same snapshot table as the hooks receive
```

### Example

```lua
-- scripts/announcer.lua
function OnSpawn(ch)
    if ch.name == "Trex" or ch.name == "Ceratosaurus" then
        oc.message("A " .. ch.name .. " is nearby...")
    end
end

function OnDamage(ch, amount)
    if amount >= 40 and ch.health > 0 then
        oc.message(string.format("Solid hit on %s (-%d HP)", ch.name, amount))
    end
end
```

A full annotated template ships as `scripts/example.lua`.

---

## Input and bindings

- **Mouse** — raw relative input via SDL's unscaled path; OS mouse-accel and Windows sens sliders are bypassed so aim is 1:1 with hand motion. The in-game **Mouse Sensitivity** slider scales both mouse and gamepad look.
- **Gamepad** — any SDL-recognised controller works (Xbox, DualShock 4/5, Switch Pro, generic). Hot-plug supported. Defaults: left stick = move, right stick = look (analog), A = jump, B = crouch, X = call, Y = change call, LB/RB = prev/next weapon, RT = fire, LT = binoculars, Back = map, Start = pause/menu, L3 = run, D-pad = turn up/down/left/right.
- **Rebinding** — **Options → Controls** shows three columns per action (name / keyboard / gamepad). Click either slot to capture a new binding; press **ESC** to clear it (unbind); the **Reset** button restores all defaults. Triggers (LT/RT) and stick directions can be assigned to any action.
- **Persistence** — rebinds are written to `controls.cfg` on exit and reloaded on launch; unknown or missing actions safely fall back to the hardcoded defaults.

---

## For modders: the `mods/` folder

OpenCarnivores ships a virtual filesystem that mounts `mods/<PackName>/` folders on top of the retail game folder. **Retail files are never modified.** Any file the game opens is checked against the enabled mod list first (top = highest priority); the first hit wins, otherwise the retail file is used.

### Installing a mod

1. Extract the pack into `mods/<PackName>/` so its layout mirrors the game root — e.g. `mods/Carnivores 2+/HUNTDAT/MENU/MENUM.TGA`, `mods/MyPack/HUNTDAT/AREAS/AREA1.RSC`.
2. Launch OpenCarnivores and open the main menu.
3. Click **MODS** (bottom-right of the main menu) and toggle the packs you want on.
4. Restart the game — the mount stack is rebuilt from `mods.cfg` on launch.

Old retail-era mod packs that shipped a zipped Carnivores 2 install (including a `Carn2.exe`) work without modification: unzip into `mods/<PackName>/`, enable it in the MODS menu, and launch `OpenCarnivores.exe`. The bundled `Carn2.exe` is ignored — OpenCarnivores has the engine fixes. The `.CAR` / `.RSC` / `.TGA` / `.WAV` files the pack ships still load through the retail parsers, so compatibility is the same as the original engine.

### What gets redirected

Everything that reads game assets: character `.CAR`, standalone `.3DF`, area `.RSC`/`.MAP`, sound `.WAV`, menu `.TGA`/`.RAW`, area text `.TXT`, `_res.txt`, `dinos.json`/`weapons.json`, PBR / custom material sibling files, glTF / OBJ model overrides, GLSL shaders. Runtime files — trophy saves, screenshots, `display.cfg`, `mods.cfg`, `render.log` — stay in the game root and are not routed.

### Priority and conflict resolution

`mods.cfg` stores one enabled pack name per line, in priority order (top-to-bottom). If two packs both replace the same file (e.g. both ship a `HUNTDAT/TREX.CAR`), the one listed higher wins. Reorder by toggling off the lower-priority pack, then the higher one, then back — or edit `mods.cfg` directly. To uninstall a pack, untoggle it or delete the folder.

---

## Coming next

Planned additions that will further expand what modders can do (see `CLAUDE.md` for the full roadmap):

- Zip-archive mounting alongside the existing `mods/` folder support
- Expanded Lua API (more hooks, richer Character mutation, AI behaviour tree overrides)
- Behavior-tree AI and NavMesh pathfinding
- Physics integration (ragdoll, foliage interaction)
- EFX reverb zones and HRTF toggle in OpenAL
- VR support (OpenXR)

---

## Building from source

```
cmake -B build
cmake --build build --config Release
```

Requires Visual Studio 2022 (or MSVC toolchain) and CMake 3.20+. All dependencies (SDL2, OpenAL Soft, glad, stb_image, tinygltf, tinyobjloader, Lua 5.4) are vendored under `deps/`.

Run `build/Release/OpenCarnivores.exe` from the Carnivores 2 game folder, or pass `-width=N -height=N -fullscreen` on the command line.

---

## Contributing

Pull requests welcome! The repo is configured to require a PR + 1 review before merging to `main`, and squash-merges only for a clean history. Fork, branch, and open a PR — bug reports and small fixes are just as appreciated as new features.

---

## Credits

- Original game: **Action Forms**, 1999
- Source code preservation: [videogamepreservation/carnivores2](https://github.com/videogamepreservation/carnivores2)
- OpenCarnivores port: engine modernization, renderer abstraction, audio/asset pipelines
- Dependencies: SDL2 (zlib), OpenAL Soft (LGPL), glad (MIT), stb_image (public domain), tinygltf (MIT), tinyobjloader (MIT), Lua 5.4 (MIT)

Not affiliated with Action Forms. You must own a legal copy of Carnivores 2 to use this engine.
