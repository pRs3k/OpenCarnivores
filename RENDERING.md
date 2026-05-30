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

## Sky Rendering ✅

**Sky Bilinear Filtering** (smooth like original D3D mode):
- **Original Issue**: Sky appeared pixelated (large blocky rectangular patches), matching the software-render mode of the original game rather than the smooth D3D mode.
- **Root Cause**: `d3dStartBufferG()` sets `uAlphaTest=true` for all geometry-buffer draws. The fragment shader's alpha-test path snaps UV coordinates to the nearest texel centre (emulating `D3DFILTER_NEAREST` for foliage). Sky geometry is drawn inside the geometry buffer, so it inherited the foliage nearest-equivalent UV snap even though `SkyPic` was uploaded with `GL_LINEAR_MIPMAP_LINEAR`. Inside `d3dEndBufferG`, the alpha-test state is forcibly re-asserted to `true` before the draw call (to handle BMP sprites and water vertex-alpha), which overrode any pre-call attempt to clear it.
- **Fix**: Added `noSnapUV` parameter to `d3dEndBufferG(BOOL ColorKey, BOOL noSnapUV = FALSE)`. When `noSnapUV=TRUE`, `SetAlphaTest(false)` is called immediately after the forced `SetAlphaTest(true)`, so the sky draw executes with `uAlphaTest=false` — bypassing the UV snap and sampling the sky texture with genuine bilinear filtering. All other geometry-buffer callers pass `noSnapUV=FALSE` (default) and are unaffected. The sky call in `RenderSkyPlane` is `d3dEndBufferG(FALSE, TRUE)`.

## Normal Mapping, Parallax, and PBR ✅

Full physically-based rendering pipeline for modder-supplied PBR assets. Purely additive — textures without PBR sidecar maps render unchanged through the original Lambert+vertex-light path.

**Activating PBR for a texture:**
Place sibling PNG files next to the asset (or in `<map>/override/`):
- `<stem>_normal.png` — tangent-space normal map (RGB). **Required** to enable PBR.
- `<stem>_mr.png` — metallic (R) + roughness (G), glTF convention.
- `<stem>_ao.png` — ambient occlusion (R channel).

**Parallax mapping (auto-detected):**
- Encode surface height in the **alpha channel** of `_normal.png` (255=raised, 0=flat).
- `Materials.cpp` scans the alpha channel at load time: if any alpha ≠ 255, parallax is enabled with `parallaxScale=0.05`.
- Standard normal maps (all alpha=255) have parallax disabled automatically — no artefacts.

**Lighting (world-space):**
- All PBR vectors (N, V, L) computed in **world space** — correct at all camera angles.
- Geometric surface normal derived from `dFdx/dFdy` of the interpolated world position (`vWorldPos`), eliminating the need for per-vertex normals in the `.CAR`/`.3DF` format.
- Sun direction (`uSunDirWorld`) updated each frame from `SunShadowK` via `SetSunDirection`, matching the shadow map's light direction exactly.
- Cook-Torrance GGX BRDF: NDF (GGX distribution) + Smith geometry + Fresnel-Schlick.

**Tangent frame:**
- Christian Schüler screen-space derivative technique (`cotangentFrame`) applied to `vWorldPos` + UV — no per-vertex tangents required.
- Geometric normal corrected for back-faces via `dot(geoN, V) < 0` sign flip.

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

## Shader Packs (Modular System)

**Modders can create custom shader packs** to extend visuals without modifying core code:

- **Location**: `shaderpacks/` directory
- **Per-pack structure**: `pack.json` + shader files organized by type
- **Documentation**: See [SHADER_PACKS.md](SHADER_PACKS.md) for modding guide
- **Example pack**: `shaderpacks/example/` — reference implementation

**System components**:
- `ShaderPack.h/cpp` — Pack loading, shader compilation, parameter management
- `ShaderPackManager` — Discovers and loads packs from disk
- Post-process effects: Tone mapping, color grading, bloom (modders can add more)
- Material shaders: Custom terrain/object rendering (extensible foundation)

**Hot-reload**: Shaders are loaded at startup; modders can iterate by restarting game.

**Design philosophy**: Simple, extensible system for modders. Core rendering stays lean.

**Development history**: See [SHADER_DEVELOPMENT_NOTES.md](SHADER_DEVELOPMENT_NOTES.md) for explanation of what we tried, why we chose this direction, and future plans.

## World Shadow Mapping ✅

Real-time sun shadow mapping implemented in `RendererGL.cpp` / `renderd3d.cpp`.

**Architecture (Path B — world-space depth pass)**:
- Shadow pass runs once per frame before the main render: `BeginWorldShadowPass → DrawScene → EndWorldShadowPass → DrawScene`.
- Light placed at `cameraPos + sunDir × 10 000 GU`; orthographic frustum ±`m_shadowRange` GU (dynamically set to `(ctViewR+2)×256` to match player view distance), near=1, far=20 000, 2048² depth texture (`GL_DEPTH_COMPONENT24`, `GL_CLAMP_TO_BORDER` with border=1).
- Sun direction from `SunShadowK` (morning 0.7 / noon 0.5 / evening −0.7): `sunDir = (−K, 1, −K)` normalised.
- PCF 3×3 tap in `basic.frag`; shadow strength is a tunable uniform (`uShadowStrength`).

**Terrain shadow depth (view-direction-independent)**:
- During the shadow pass `ProcessMap` submits terrain triangles using world positions read directly from `HMap[ty][tx] × ctHScale` — no camera-space VMap involved.
- This is the correct approach: the camera-space pipeline (`ev[i].v`, VMap, RotateVector) is unreliable for shadow geometry because tiles behind the camera have `rv.z > 0` and the reconstruction accumulates floating-point cancellation for large world coords.
- Triangulation matches `ReverseOn` flag: false → (0,1,3)+(0,3,2); true → (0,1,2)+(2,1,3).

**Object/character shadow depth**:
- 3D objects submit via `SubmitWorldSpaceShadowTriangle` → `ws_depth.vert` (takes world XYZ directly, applies `uLightSpace`).
- Accumulated into `m_wsShadowBuffer` (up to `MAX_WS_SHADOW_VERTS`) and flushed in batches.
- **Critical**: the object render loop (`renderd3d.cpp`) calls `glBindTexture(GL_TEXTURE_2D, hTexture)` on unit 0 immediately after each `d3dSetTexture` call. `FlushWorldSpaceShadow` uses unit 0 for foliage alpha-test; without the explicit bind, stale textures from previous renders (or HUD/UI draws) corrupt the alpha pattern and produce wrong tree shadows.

**Sky plane exclusion**:
- `RenderSkyPlane` uses `sz = 0.0001f` (not 0), so `depth.vert` reconstructs a world position ~160 000 GU in the camera-forward direction. Looking up past the sun elevation, that position lands behind the light's near plane and `GL_DEPTH_CLAMP` writes depth=0, blackening the shadow map. Fix: `RenderSkyPlane` returns immediately when `IsShadowPassActive()`.

**Fragment-side sampling**:
- `vWorldPos` reconstructed in `basic.vert` from screen-space `aPos`/`aDepth` via `uCamToWorld` (R^T, column-major mat3 uploaded by `SetCameraWorldUniforms`).
- Shadow block gated on `vRhw < 1.0` (sky/HUD vertices have `aDepth=0` → `vRhw=1.0` → skipped).
- `proj.z >= 0` lower bound prevents behind-light geometry (straddling triangles with a behind-camera vertex produce `vWorldPos ≈ 0`) from spuriously shadowing the view.
- **HUD isolation**: `uWorldShadow=0` must be held through all of `DrawPostObjects` (weapon base + PhongMap + EnvMap overlays). `d3dSetHUDMode(TRUE)` is called before the weapon section and `d3dSetHUDMode(FALSE)` only after PhongMap/EnvMap finish. PhongMap and EnvMap vertices have non-zero `sz` (→ `vRhw<1.0`), so they would incorrectly shadow-sample if `uWorldShadow=1` during those passes.

**Night mode exclusion**:
- Hardware shadow pass is skipped entirely when `OptDayNight == 2` (night hunt). `uWorldShadow` is reset to 0 each `BeginFrame()`; since `EndWorldShadowPass()` is never called at night, the shader's shadow-sampling block (`if (uWorldShadow && ...)`) never fires.
- Flat projected character shadows (`RenderShadowClip` in `RenderCharShadow`) are also skipped when `OptDayNight == 2`.
- `RenderSun()` is skipped at night, suppressing the sun disc, lens flare, and fullscreen glare rect (`SunLight` stays 0).
- Post-processing (bloom, tone mapping, color grading) is skipped at night — night-vision is already a stylised green overlay that would be degraded by ACES contrast compression creating artificial dark halos.

**Debug modes** (F8 cycles):
- Mode 8: raw shadow-map depth at fragment's shadow UV (white=far/unwritten, black=near/geometry).
- Mode 9: shadow UV as RG gradient — uniform colour means world-pos reconstruction is broken.

## GL_TEXTURE0 unit discipline

Any function that binds to `GL_TEXTURE0` must save and restore the previous binding (`glGetIntegerv(GL_TEXTURE_BINDING_2D)` + `glBindTexture` after). The world-space shadow batch (`FlushWorldSpaceShadow`) reads unit 0 for foliage alpha-test. A stale texture from a HUD/UI draw (e.g. `DrawTextWithFont`) will corrupt tree shadow alpha on the frame following the draw — manifesting as shadows changing whenever text, the map, or the weapon appears on screen. Functions that currently save/restore unit 0: `DrawBitmap`, `DrawTextWithFont`.

## Future Enhancements

**Modder-driven feature roadmap**:
- Advanced lighting models (PBR, physically-based materials)
- Screen-space ambient occlusion (SSAO)
- Screen-space reflections (SSR)
- Advanced post-processing (bloom, motion blur, chromatic aberration)
- Custom material types and blend modes
- Cascaded shadow maps for larger shadow coverage at close range

**How to add features**: Create shader packs in `shaderpacks/` with custom effects. See [SHADER_PACKS.md](SHADER_PACKS.md).

## Long-term Roadmap
- Formalize the renderer abstraction: move all `d3d*` functions behind the `Renderer` interface entirely, kill `renderd3d.cpp` glue so Vulkan/Metal/WebGPU backends become drop-in.
