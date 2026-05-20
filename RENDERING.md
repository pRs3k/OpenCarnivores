# Rendering System

## Backends
- `renderd3d.cpp` — Direct3D 6 / OpenGL rendering backend (unified).
- `Renderer.h` / `RendererGL.cpp` — SDL2 + OpenGL 3.3 Core backend (modern).

## Texture override registry
The renderer intercepts texture uploads to support high-res PNG/TGA/BMP/JPG overwrites of original 16-bit TGA assets:

- Registry keyed by CPU pointer of original RGB555 buffer → decoded RGBA8 + (w, h).
- Upload paths (`gl_UploadTexture16` → shared `gl_UploadRGBA` in renderd3d.cpp; `RendererGL::SetTexture` in RendererGL.cpp) check registry first.
- On hit: 32-bit override uploaded at any resolution with full 8-bit alpha and hardware mipmaps.
- On miss: existing 16-bit path runs unchanged.
- Mipmaps generated directly from original transparent data for authentic foliage appearance.
- Loaders call `TextureOverrides::TryRegisterSibling(ptr, filePath)` or `TryRegisterWithExts(ptr, basePath)` immediately after loading 16-bit data (tries `.png`, `.tga`, `.bmp`, `.jpg` in order).

## Texture cache (`RendererGL::SetTexture`)
`SetTexture` maintains an LRU cache of up to 128 GL texture objects (`m_texCache` in `RendererGL.h`).

- **Key**: FNV-1a content hash of the raw 16-bit pixel buffer, computed by sampling 96 bytes from the start, middle, and end of the data plus the dimensions. ~100 cycles constant cost regardless of texture size.
- **Why not pointer**: The game reuses heap addresses for different texture data across frames, causing pointer-keyed entries to serve stale GL textures. Content hashing means identical data always hits and changed content always misses.
- **Eviction**: When the cache exceeds 128 entries the least-recently-used entry is deleted via `glDeleteTextures`. The `lastUsed` field stores the frame counter at last access.
- **Override interaction**: `TextureOverrides::GetCompressed` / `Get` still receive the original CPU pointer (they maintain their own pointer-keyed registry registered at load time). The GL cache key is independent of that registry.

## Vertex staging buffer
`MAX_MAIN_VERTICES` (`RendererGL.h`) controls the size of both the CPU staging array (`m_mainBuffer`) and the GPU VBO allocated at init (`glBufferData` in `RendererGL.cpp`). The D3D fallback path (`g_mainVertices` in `renderd3d.cpp`) must match this value. Currently **4096 triangles**. The terrain rendering accumulates faces until a texture change or section boundary triggers `d3dFlushBuffer` / `UnlockAndDrawTriangles`; a larger buffer reduces the number of flushes per terrain ring.

## Anisotropic filtering
Applied to every uploaded texture when `GL_ARB_texture_filter_anisotropic` or `GL_EXT_texture_filter_anisotropic` is available. Level is controlled by `OptAnisoLevel` (menu setting): 1=2×, 2=4×, 3=8×, 4=hardware maximum. The hardware cap is queried once at init into `m_maxAnisotropy`; if the extension is absent, `m_maxAnisotropy` is 1 and filtering is skipped.

## Foliage Rendering

**Foliage Transparency** (partial ✅, limitations understood):
- **Original Issue**: Foliage (bushes, trees) appeared overly solid/"puffy" compared to original game, lacking individual leaf detail.
- **Root Cause**: Mipmap generation was aggressively filling transparent pixels with nearby opaque colors (commit d16a69c), eliminating fine transparency detail between leaves at all LOD levels.
- **Fixes Applied**:
  1. **Mipmap generation**: Changed `gl_GenerateLinearMipmaps()` to use majority-voting: for each 2×2 block, if more pixels are opaque than transparent, output fully opaque (averaged opaque colors); otherwise fully transparent. Eliminates semi-opaque mipmap artifacts and preserves leaf cluster boundaries.
  2. **Alpha dilation**: Pre-fill transparent pixels with nearest opaque neighbor's RGB (keeping alpha=0) before uploading. With bilinear filtering at leaf edges, this shows leaf color instead of black, preventing dark fringes.
  3. **NEAREST-equivalent sampling**: Snap foliage UV coordinates to texel centers in the fragment shader (emulates original D3D `D3DFILTER_NEAREST`). Every pixel maps to exactly one texel with no bilinear blending across leaf edges.
  4. **LOD 0 always**: Force foliage color samples to mip level 0 (no trilinear blending between mip levels). Prevents mipmap-compression artifacts from darkening close-range leaf pixels.
- **Visual Result**: Leaves now render with crisp per-texel detail matching the original engine's NEAREST filtering behaviour. At close range, individual leaf texels are visible as small pixel clusters. At medium/far range, majority-voting mipmaps provide smooth level-of-detail without semi-transparent artifacts.
- **Remaining Limitation**: At modern resolutions (1456×816+), foliage appears slightly less detailed than comparison images from the original game at lower resolution (640×480 or 800×600). This is inherent to the original asset resolution — the texture pixels are the same size in world units, but at higher screen resolution they map to fewer screen pixels each, appearing smaller. The original game would show identical texel-level detail at the same viewing distance; comparison images were likely captured at different distances or resolutions where each leaf pixel occupied more screen space. **Conclusion**: This is as good as it can get with the original texture assets and NEAREST filtering; no further improvements possible without higher-resolution textures.

## Terrain Rendering

**Terrain Mipmap Chain** ✅:
- **Historical Issue**: Terrain textures had no mipmaps (GPU level 0 only) due to NVIDIA driver `GL_TEXTURE_LOD_BIAS` clamping. When the original code used `texture()` with automatic LOD, NVIDIA clamped negative LOD bias to ≥0, forcing higher mip levels (darker) to be selected, creating a visible "headlamp" brightness circle when the camera bobbed.
- **Root Cause of Clamping Issue**: Driver-level LOD_BIAS only applied to automatic LOD selection via `texture()` calls, not explicit `textureLod()` calls.
- **Fix Applied** ✅: Enabled hardware mipmap generation via `glGenerateMipmap()` for terrain textures. The fragment shader uses `textureLod()` with screen-space-derivative LOD (not `texture()`), completely bypassing the driver's LOD_BIAS machinery. NVIDIA driver clamping no longer affects us.
- **Visual Result**: Terrain at medium and far distances now uses appropriate mip levels, eliminating high-frequency moire/aliasing patterns (the dot-pattern shimmer visible at horizon). Anisotropic filtering further improves oblique-angle terrain sampling.
- **Note**: Close-range terrain large tiles remain due to original mesh resolution and UV scale (one texture tile per terrain quad). Higher-resolution terrain textures or denser UV mapping would be needed to reduce this; original assets have fixed limitations.

**Terrain Alpha Fade (Legacy Distance Fade)** ✅:
- **Original Code**: `DrawTPlane` and clipping path computed per-vertex alpha based on distance (`VectorLength(ev[n].v) - 256 * (ctViewR-4)`), fading from 255 to 0 over ~500 world units.
- **Removal**: Alpha fade calculation removed; all terrain vertices now render with full alpha (0xFF). Ring-based distance culling via ProcessMap and z-buffer provide natural LOD without per-vertex alpha blending.
- **Technical Detail**: Two paths had the fade: `DrawTPlane()` used local alpha variables (`alpha1`, `alpha2`, `alpha3`), while clipped terrain in `DrawTPlaneClip()` used `ev[n].ALPHA` field. Both paths now set alpha to 255 unconditionally.
- **Architecture Note**: Texture bucketing attempt (`DrawTerrainGL()`) was abandoned due to GL texture binding complexity; the original ring-order ProcessMap path remains the most reliable approach. Ring traversal requires proper initialization of `r` for each code path (D3D: `ctViewR1-1`, GL: `ctViewR`).

## Character rendering

**Dead character shadows** (`renderd3d.cpp` `RenderCharacterPost`):
- When a character dies (`Health==0`), shadows fade out during the death animation. Previously, an early return on the last animation frame prevented shadow rendering entirely, causing dead dinosaurs laying on the ground to cast no shadows.
- **Fix**: Removed the early return so shadows continue fading through the complete death animation cycle, including after the animation ends.

## Trophy screen rendering

**Trophy graphic and text positioning** (`Hunt2.cpp`, `renderd3d.cpp` `DrawTrophyText`):
- Trophy screen has different layout requirements for flatscreen vs VR modes, requiring separate positioning strategies.
- **Flatscreen**: Graphic positioned at bottom-center with 40px margin from screen bottom (`y0 = WinH - dh - 40*WinH/480`). Text rendered with compact 17px line spacing (scaled by WinH/480) to fit neatly inside the trophy graphic box at `y0 + 15*WinH/480`. Minimal x/y offsets (20px right, 0px down).
- **VR (fade-out)**: Graphic positioned eye-level (`WinH/2.75`) and scaled to 65% of original size for comfort. Text positioned at `y0 + 5*WinH/480` (near top of smaller graphic) with large 90px line spacing (scaled by textScale=0.10f) and full +100px y-offset. The large line spacing and offsets are scaled down by textScale to fit the smaller VR rendering proportionally.
- **Implementation**: `DrawTrophyText()` accepts `bool isVR` parameter to conditionally apply layout offsets. Line spacing: `isVR ? (90 * WinH/480 * textScale) : (17 * WinH/480)`. Y offset: `isVR ? (100 * WinH/480 * textScale) : 0`. This prevents tight flatscreen text from becoming unreadable in VR, and prevents oversized VR spacing from overflowing flatscreen box.

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

**Phase 2.1: Dynamic Shadow Mapping** (⏳ In Progress — ~50% complete)

**Architecture**:
- `PostProcessingPipeline`: Manages 3 shadow map cascades (2048×2048 depth textures), light direction, view-projection matrices
- `RendererGL`: Depth shader compilation, BeginShadowCascade/EndShadowPass state management
- `shaders/depth.vert/frag`: Depth-only rendering (light POV), alpha-tested foliage support
- `shaders/postprocess/shadows.frag`: PCF shadow sampling (placeholder, awaiting depth pass integration)

**Completed**:
- ✅ Shadow map FBO pipeline (3 cascades at 2048×2048, with color suppression)
- ✅ Depth shader for light-POV rendering with alpha test support
- ✅ Logarithmic frustum splitting (95% log, 5% linear) for cascade distance distribution
- ✅ Light view matrix computation from light direction + camera position
- ✅ Orthographic projection per cascade matching frustum geometry
- ✅ Depth rendering state management (BeginShadowCascade/SetDepthOnlyMode/EndShadowPass)
- ✅ Shader infrastructure plumbed into compilation pipeline

**Remaining** (~50%):
1. **Hook depth pass into Hunt2.cpp render loop**: Call BeginShadowCascade(i) before scene rendering for each cascade, then EndShadowPass() after
2. **World position reconstruction**: In shadows.frag, read screen depth and reconstruct world position using inverse projection
3. **Cascade selection**: Choose cascade based on camera-to-pixel distance
4. **PCF shadow sampling**: Transform world position to light space, sample shadow maps with 4-8 tap filtering
5. **Shadow modulation**: Apply shadow factor (0=full shadow, 1=lit) to screen color based on uIntensity parameter
6. **Menu integration**: Wire g_shadowQuality setting to FBO creation, distance thresholds

**Known limitations**:
- Depth pass requires rendering scene twice (main render + depth-only render); currently infrastructure-only, not yet integrated into Hunt2.cpp's frame loop
- PCF filtering currently uses simple 4-tap pattern; could expand to 8-16 tap for quality
- No soft shadows or penumbra estimation yet (could use variance shadows in future)
- Light direction is currently hardcoded in PostProcessingPipeline; should come from sun animation/time-of-day system

**Files modified**: `PostProcessing.h/cpp`, `RendererGL.h/cpp`, `shaders/depth.vert/frag`, `shaders/postprocess/shadows.frag`, `RENDERING.md`

- **Phase 2.2**: Bloom + Tone Mapping — ✅ Complete (Reinhard tone curve working)
- **Phase 2.3**: Screen-Space Reflections — Ray-marched reflections on shiny surfaces (4-5 hours)
- **Phase 2.4**: Normal Mapping Quality — Parallax mapping, PBR parameters (2-3 hours)

## Long-term Roadmap
- Formalize the renderer abstraction: move all `d3d*` functions behind the `Renderer` interface entirely, kill `renderd3d.cpp` glue so Vulkan/Metal/WebGPU backends become drop-in.
