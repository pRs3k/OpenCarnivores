# Rendering System

## Backends
- `renderd3d.cpp` — Direct3D 6 rendering backend.
- `Render3DFX.cpp` — 3Dfx Glide rendering backend.
- `RenderSoft.cpp` — Software rendering backend.
- `renderasm.cpp` — x86 ASM optimized software rendering routines.
- `Renderer.h` / `RendererGL.cpp` — SDL2 + OpenGL 3.3 Core backend (modern).

## Texture override registry
The renderer intercepts texture uploads to support high-res PNG/TGA/BMP/JPG overwrites of original 16-bit TGA assets:

- Registry keyed by CPU pointer of original RGB555 buffer → decoded RGBA8 + (w, h).
- Upload paths (`gl_UploadTexture16` → shared `gl_UploadRGBA` in renderd3d.cpp; `RendererGL::SetTexture` in RendererGL.cpp) check registry first.
- On hit: 32-bit override uploaded at any resolution with full 8-bit alpha and hardware mipmaps.
- On miss: existing 16-bit path runs unchanged.
- Foliage mip-fix preserved: transparent pixels filled with avg opaque color for levels 1..N, original at level 0.
- Loaders call `TextureOverrides::TryRegisterSibling(ptr, filePath)` or `TryRegisterWithExts(ptr, basePath)` immediately after loading 16-bit data (tries `.png`, `.tga`, `.bmp`, `.jpg` in order).

## Character rendering

**Dead character shadows** (`renderd3d.cpp` `RenderCharacterPost`):
- When a character dies (`Health==0`), shadows fade out during the death animation. Previously, an early return on the last animation frame prevented shadow rendering entirely, causing dead dinosaurs laying on the ground to cast no shadows.
- **Fix**: Removed the early return so shadows continue fading through the complete death animation cycle, including after the animation ends.

## Shader Enhancement Plan

### Phase 1: Post-Processing Pipeline Infrastructure
Build optional, opt-in post-processing framework for advanced visual effects:

**Architecture:**
- Render scene to G-Buffer (color, position, normal, depth, albedo)
- Chain post-processing shaders as full-screen passes
- Compositing system to blend effects
- Config/menu option to enable/disable (disabled by default)

**Files to create/modify:**
- `renderer/PostProcessing.h` / `PostProcessing.cpp` — pipeline management
- `renderer/Shaders/postprocess/` — post-effect GLSL shaders
- `RendererGL.cpp` — integrate post-processing into main render loop
- Config option: `enable_postprocessing` (default: false)

**Effort:** ~8-10 hours

### Phase 2: Shader Pack — Visual Enhancement Effects
Modular shader pack with four high-impact effects (all disabled by default, config-driven):

**1. Dynamic Shadow Mapping**
- Shadow map rendering from light source
- PCF filtering for soft shadows
- Cascaded shadows (near/far quality tiers)
- Shader: `postprocess/shadows.frag`
- Config: `enable_shadows` (default: false)

**2. Post-Processing Bloom + Tone Mapping**
- Bloom: blur bright pixels, composite back
- Tone mapping: HDR → SDR color space conversion
- Color grading: user-tunable color adjustments
- Shaders: `postprocess/bloom.frag`, `postprocess/tonemap.frag`
- Config: `enable_bloom`, `enable_tonemapping` (default: false)

**3. Screen-Space Reflections (SSR)**
- Trace rays on-screen using depth/normal buffers
- Reflects geometry without full raytracing cost
- Water surfaces, puddles, polished metal
- Shader: `postprocess/reflections.frag`
- Config: `enable_ssr` (default: false)

**4. Normal Mapping Quality Pass**
- Improved normal mapping implementation per-asset
- Parallax mapping for depth illusion
- Enhanced PBR shading model
- Materials: per-asset `.material` files + improved `.normal.png` treatment
- Config: enabled via material system (no global flag needed)

**Effort per effect:** 3-5 hours each = ~16-20 hours total

### Phase 3: User Configuration
Add menu/config options:
- **Options → Video → Advanced Graphics** submenu
- Toggles for: Shadows, Bloom, Tone Mapping, SSR, Color Grade
- Sliders for: shadow quality, bloom intensity, reflection strength
- Persist to `display.cfg`

**Effort:** ~4-6 hours

---

## Rendering Roadmap
- ✅ Texture override registry (PNG/TGA/BMP at any resolution)
- ✅ Anisotropic filtering & trilinear mipmapping
- ✅ Per-texture LOD bias tuning
- ⏳ **Post-processing pipeline** (Phase 1)
- ⏳ **Dynamic shadows** (Phase 2.1)
- ⏳ **Bloom + Tone Mapping** (Phase 2.2)
- ⏳ **Screen-Space Reflections** (Phase 2.3)
- ⏳ **Normal Mapping quality improvements** (Phase 2.4)
- ⏳ **Advanced graphics menu** (Phase 3)
- Future: Sky dome 3D model (replaces flat-plane rendering)
- Future: Formalize renderer abstraction for Vulkan/Metal/WebGPU backends
