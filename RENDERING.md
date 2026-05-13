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

## Post-Processing Pipeline (Phase 1 ✅ Complete)

**Infrastructure** — Ready for Phase 2 effect implementation:
- `PostProcessing.h/cpp` — FBO management, effect registry, shader infrastructure
  - `FramebufferObject` — Color/depth textures, blitting, composition
  - `PostProcessingPipeline` — Effect lifecycle, enable/disable toggles, composition modes
  - Composition modes: REPLACE, ADDITIVE, ALPHA_BLEND, SCREEN, MULTIPLY, OVERLAY
- `Hunt.h` — Global toggles (disabled by default, menu-controllable):
  - `g_enableBloom`, `g_bloomIntensity`, `g_bloomThreshold`, `g_bloomKnee`
  - `g_enableToneMapping`, `g_tonemapExposure`
  - `g_enableSSR`, `g_ssrIntensity`
  - `g_enableShadows`, `g_shadowQuality`
- Integration:
  - `RendererGL::Init()` — pipeline initialization
  - `Hunt2.cpp` — `ApplyEffects()` calls in flatscreen and VR render paths
  - `IRenderer` interface — virtual accessor for pipeline

**Shaders** — Placeholder files ready for Phase 2:
- `shaders/postprocess/quad.vert` — Fullscreen quad vertex shader
- `shaders/postprocess/bloom_threshold.frag` — Bright pixel extraction
- `shaders/postprocess/bloom_blur_h.frag`, `bloom_blur_v.frag` — Separable Gaussian blur
- `shaders/postprocess/tonemap.frag` — Reinhard tone mapping (HDR→SDR)
- `shaders/postprocess/shadows.frag` — PCF shadow lookup (placeholder)
- `shaders/postprocess/ssr.frag` — Ray-marched screen-space reflections (placeholder)
- `shaders/postprocess/desaturate.frag` — Test shader proving pipeline works

## Phase 2 Roadmap (In Progress)
- **Phase 2.1**: Dynamic Shadow Mapping — Cascaded PCF shadows from sun light (5-6 hours)
- **Phase 2.2**: Bloom + Tone Mapping — Bright-pixel bloom, Reinhard tone curve (4-5 hours)
- **Phase 2.3**: Screen-Space Reflections — Ray-marched reflections on shiny surfaces (4-5 hours)
- **Phase 2.4**: Normal Mapping Quality — Parallax mapping, PBR parameters (2-3 hours)

## Long-term Roadmap
- Formalize the renderer abstraction: move all `d3d*` functions behind the `Renderer` interface entirely, kill `renderd3d.cpp` glue so Vulkan/Metal/WebGPU backends become drop-in.
