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
- Mipmaps generated directly from original transparent data for authentic foliage appearance.
- Loaders call `TextureOverrides::TryRegisterSibling(ptr, filePath)` or `TryRegisterWithExts(ptr, basePath)` immediately after loading 16-bit data (tries `.png`, `.tga`, `.bmp`, `.jpg` in order).

## Foliage Rendering

**Foliage Transparency** (in progress):
- **Issue**: Foliage (bushes, trees) appeared overly solid/"puffy" compared to original game, lacking individual leaf detail.
- **Root Cause**: Mipmap generation was aggressively filling transparent pixels with nearby opaque colors (commit d16a69c), eliminating fine transparency detail between leaves at all LOD levels.
- **Partial Fix**: Commented out pixel-fill logic in `CreateMipMapMT()` and `CreateMipMapMT2()` (Resources.cpp lines 1034-1036, 1062-1064) for both 256→128 and 128→64 mipmap levels. This restored individual leaf outlines and detail.
- **Remaining Issue**: Black gaps now appear between leaves where transparency should show through. Likely causes:
  - GL mipmap generation (`rgl_GenerateLinearMipmaps`) still blending black color-key pixels into leaf colors
  - Shader alpha test threshold (0.5) filtering out legitimate semi-opaque edge pixels
  - Color-key (black = transparent) not properly distinguished from actual geometry during downsampling

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
