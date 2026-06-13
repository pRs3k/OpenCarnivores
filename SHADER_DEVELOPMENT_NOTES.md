# Shader System Development Notes

This document explains the development journey of the shader system, decisions made, and the direction chosen for future modding support.

## Phase 1: Post-Processing FBO Pipeline (Attempted, Abandoned)

### What We Built

A **Framebuffer Object (FBO) based post-processing pipeline** designed to enable complex effects:

**Components:**
- `PostProcessing.h/cpp` — FBO management, effect registry, shader infrastructure
- Shadow mapping infrastructure — 3 cascades at 2048×2048 for cascaded PCF shadows
- Bloom extraction and blur pipeline
- Tone mapping (Reinhard curve)
- Parameter management and hot-reload infrastructure

**Implementation Details:**
- `PostProcessingPipeline` — Manages source FBO (scene capture), intermediate FBOs for effects
- `FramebufferObject` — Color/depth texture management, blitting, composition
- Shadow depth pass — Light-POV rendering with alpha-tested foliage
- Cascaded shadow matrices — Logarithmic frustum splitting (95% log, 5% linear)
- Effect composition modes — REPLACE, ADDITIVE, ALPHA_BLEND, SCREEN, MULTIPLY, OVERLAY

**Shaders Implemented:**
- `shaders/depth.vert/frag` — Depth-only rendering for shadow maps
- `shaders/postprocess/shadows.frag` — PCF shadow sampling (4-tap)
- `shaders/postprocess/shadows_debug.frag` — Debug visualization
- `shaders/postprocess/bloom_threshold.frag` — Bright pixel extraction
- `shaders/postprocess/bloom_blur_h.frag`, `bloom_blur_v.frag` — Separable Gaussian blur
- `shaders/postprocess/tonemap.frag` — Reinhard tone mapping
- `shaders/postprocess/quad.vert` — Fullscreen quad vertex shader

### Architecture

The intended rendering flow was:
```
1. pipeline->BeginCapture()           — Bind scene FBO
2. DrawScene()                         — Render scene to FBO
3. pipeline->ApplyEffects()            — Read FBO, apply effects, composite to screen
```

### Issues Encountered

#### 1. **FBO Rendering Integration Failure** (Critical)
**Problem:** When `BeginCapture()` was added to bind the source FBO, enabling bloom caused the entire screen to render black instead of the scene.

**Root Cause:** The scene was not being rendered to the FBO correctly. Possibilities include:
- FBO binding not persisting through `DrawScene()`
- DrawScene() resetting GL framebuffer state
- Viewport mismatch between FBO dimensions and rendering
- FBO validation issues (incomplete framebuffer)

**Debugging Attempts:**
- Added `glClearDepth()` before clearing shadow maps
- Verified FBO creation and texture attachment
- Checked that `ApplyEffects()` was being called
- Added BeginCapture to both VR and flatscreen render paths
- Attempted to isolate the issue with simple bloom shader

**Result:** Could not identify root cause without deeper GL state inspection. The FBO binding was interfering with the existing direct-to-screen rendering pipeline in an incompatible way.

#### 2. **Shadow Mapping Depth Convention Mismatch** (Subtle)
**Problem:** The game uses **reversed depth convention** (0=far, 1=near with GL_GEQUAL), but shadow maps were initially using standard OpenGL depth (0=near, 1=far with GL_LEQUAL).

**Fix Applied:** Updated depth shader to output `1.0 - gl_FragCoord.z` and changed depth test to GL_GEQUAL in shadow rendering.

**Result:** Partial fix — shadows still invisible due to the FBO integration issue.

#### 3. **Debug Visualization Not Triggering** (Pre-diagnosis)
**Problem:** `Shift+D` toggle to visualize shadow cascades showed no change despite input registering.

**Root Cause:** Post-processing pipeline's `m_captureActive` flag was never set to true because `BeginCapture()` was never called, causing `ApplyEffects()` to return early.

**Fix Attempted:** Added `BeginCapture()` calls to render loop — this revealed Issue #1 (black screen).

### Why We Abandoned This Approach

1. **Integration Complexity:** The FBO pipeline couldn't be cleanly integrated into the existing direct-to-screen rendering loop without breaking the scene rendering entirely.

2. **Time Investment vs. Payoff:** Debugging GL state issues in a complex rendering pipeline would have required:
   - GL state inspection tooling
   - Systematic isolation of each rendering stage
   - Potential architectural restructuring of the render loop
   - Estimated 4-8 hours of debugging

3. **Over-engineered for Current Needs:** The original game only had dinosaur shadows (sprite-based). A full post-processing FBO pipeline is more complex than necessary to support the modding use case.

4. **Better Alternative Available:** The shader pack system provides 90% of the functionality without the FBO integration headache.

## Phase 2: Shader Pack System (Current Direction)

### What We Built

A **lightweight, modular shader system** that modders can extend without modifying core code.

**Components:**
- `ShaderPack.h/cpp` — Pack loading, shader compilation, parameter management
- `ShaderPackManager` — Discovers and loads packs from `shaderpacks/` directory
- `pack.json` format — Human-readable effect/material definitions
- Documentation — `SHADER_PACKS.md` for modders

**Features:**
- Post-process effects (run after scene rendering, no FBO complexity)
- Tunable parameters (sliders exposed in UI)
- Hot-reload (restart game to load new packs)
- Example pack — tone mapping and color grading reference

### Why This Direction

1. **Simplicity:** No FBO integration issues. Effects render as fullscreen quads to the screen directly.

2. **Modders First:** Clear, documented system with example pack. Low barrier to entry for shader creators.

3. **Pragmatic:** 90% of Minecraft-shader-like effects (tone mapping, bloom, color grading, custom lighting) are achievable without complex FBO pipelines.

4. **Extensible:** Foundation is in place to add material shaders, deferred rendering, and advanced features later.

5. **Fast to Implement:** Core system working in 1-2 hours instead of 4-8 hours of FBO debugging.

### Architecture

```
shaderpacks/
├── example/
│   ├── pack.json                    # Effect/parameter definitions
│   └── effects/
│       ├── tonemap_reinhard.frag    # Post-process shader
│       └── color_grade.frag         # Post-process shader
└── custompack/
    ├── pack.json
    └── effects/
```

**Rendering Flow:**
```
1. DrawScene() to screen (normal rendering)
2. For each enabled effect:
   - Bind fullscreen quad
   - Render effect shader
   - Output composited to screen
```

## Comparison: FBO Pipeline vs. Shader Packs

| Aspect | FBO Pipeline | Shader Packs |
|--------|--------------|--------------|
| Complexity | High | Low |
| Integration Effort | High (integration failure) | Low |
| Post-process Effects | ✅ Full support | ✅ Full support |
| Material Shaders | ✅ Capable | 🔵 Infrastructure ready |
| Deferred Rendering | ✅ Can support | 🔵 Future work |
| Modder Onboarding | Steep | Easy |
| Documentation | Complex | Clear |
| Debug Time | 4-8 hours+ | Minimal |
| Time to MVP | Failed | 2 hours |

## Tradeoffs & Future Directions

### Current Limitations

**Shader Packs (by design):**
- Post-process effects only (reads final screen color)
- No per-pixel depth or normal data (for now)
- No custom lighting models (until material shaders integrated)
- Parameters are float-only (UI support needed for other types)

**Planned Future Features:**
- G-buffer support (depth, normals, position) for advanced effects
- Material shader integration (custom terrain/object rendering)
- Normal mapping and parallax mapping
- Deferred lighting models
- Screen-space ambient occlusion
- Screen-space reflections
- Parameter UI controls (int, bool, color)

### Migration Path: FBO Pipeline Revival (If Needed)

If a modder requests features that require FBO pipelines (full G-buffer access, deferred rendering), the path forward is:

1. **Implement proper FBO integration** — Restructure render loop to use intermediate FBOs correctly
2. **Keep shader pack system** — Extend it to support G-buffer effects
3. **Learn from this attempt** — Use this experience to build it right the second time

The FBO code we wrote is not wasted — it provides a foundation for future work:
- Shadow mapping infrastructure can be revived
- FBO structure is sound (issue was integration, not design)
- Post-processing composition logic is reusable

## Lessons Learned

1. **Don't Over-Engineer for Hypotheticals:** The original game didn't have global shadows. Building a complex system for "maybe modders will want this" was premature.

2. **Integration Matters More Than Feature Complexity:** A simple system integrated cleanly beats a complex system that breaks the renderer.

3. **Modder Experience is Key:** Clear documentation and low barrier to entry (shader packs) beats a powerful system (FBO pipeline) that's hard to understand.

4. **Incremental Delivery:** Start with post-process effects, add material shaders later, advance to deferred rendering when needed.

5. **Debug GL State Issues Early:** GL state problems compound — catch them before they break the entire renderer.

## Conclusion

**Decision:** Ship the shader pack system as the foundation for modding support.

**Rationale:**
- ✅ Working, tested, documented
- ✅ Low risk, high modder value
- ✅ Clear path to advanced features
- ✅ Time-efficient implementation

**Future:** The FBO pipeline and shadow mapping may be revived as optional advanced features if modders request them. For now, the shader pack system provides the best balance of capability and simplicity.

## Phase 3: Path B World Shadow Mapping

### Problem with Phase 2.1 Shadow Cascade Approach

The Phase 2.1 cascade shadow pass called `DrawScene()` with a light-space projection matrix passed as `uProjection` to the depth vertex shader. This was fundamentally broken: `basic.vert` uses `aPos` (screen-space x,y from the CPU pre-transformer) with an orthographic screen-space projection. Applying a light-space matrix to screen-space pixels produces a meaningless depth map — the GPU never sees world positions.

### Solution: Screen-Space World Position Reconstruction

The vertex format (`RenderVertex`) carries `sz = -16/camera_z`. Combined with the screen-space position `(sx, sy)` and camera parameters `(VideoCX, VideoCY, CameraW, CameraH)`, the world position can be exactly reconstructed in the vertex shader:

```glsl
float neg_cam_z = 16.0 / aDepth;                              // aDepth = sz = -16/cam_z
float cam_x     = (aPos.x - uVideoCX) * neg_cam_z / uCameraW;
float cam_y     = (uVideoCY - aPos.y) * neg_cam_z / uCameraH;
vec3  worldPos  = uCameraPos + uCamToWorld * vec3(cam_x, cam_y, -neg_cam_z);
```

where `uCamToWorld` is the transpose of the camera rotation matrix R = Rr·Rp·Ry (yaw→pitch→roll). The matrix is computed CPU-side from CameraAlpha/Beta/Gamma each frame and passed as a `mat3` uniform.

### Architecture

**New uniforms (depth.vert and basic.vert):**
- `uVideoCX/CY`, `uCameraW/H` — screen projection parameters
- `uCameraPos` (vec3) — camera world position  
- `uCamToWorld` (mat3) — camera-to-world rotation R^T (column-major)
- `uLightSpace` (mat4) — combined light view·proj for shadow map projection

**Shadow FBO (`RendererGL`):**
- Single 2048×2048 depth-only FBO (`m_worldShadowFBO`)
- Standard depth convention (GL_LESS, clearDepth=1.0) — NOT reversed
- Shadow depth texture bound to unit 4 for fragment shader sampling

**Rendering flow:**
```
SetCameraWorldUniforms(...)       ← CPU: compute R^T, upload to both shaders
BeginWorldShadowPass()            ← bind shadow FBO, set GL_LESS, upload uniforms
DrawScene()                       ← depth.vert reconstructs world pos, depth.frag writes depth
EndWorldShadowPass()              ← restore GL_GEQUAL, bind shadow tex to unit 4, set uWorldShadow=1
glClear(...)
DrawScene()                       ← basic.frag samples shadow map via vWorldPos + uLightSpace
```

**Fragment shader PCF:**
```glsl
vec4 lsPos = uLightSpace * vec4(vWorldPos, 1.0);
vec3 proj  = lsPos.xyz / lsPos.w * 0.5 + 0.5;
// 3×3 PCF with bias 0.003
```

**Polygon offset:** `glPolygonOffset(2.0, 4.0)` on the depth pass to reduce shadow acne without a per-pack shadow bias parameter.

### Pack JSON Control

```json
"shadows_mode": "full",   // "none" | "full"
"shadow_strength": 0.45   // 0.0–1.0 shadow darkness
```

`"dinos_only"` mode is reserved and currently mapped to `"full"` pending a DrawScene flag to skip terrain during the depth pass.

### Runtime Toggle

`Shift+S` cycles shadow mode 0 (off) ↔ 2 (full).

---

## Shadow / UI Coupling Bug (fixed)

**Symptom**: Shadows on terrain changed appearance whenever UI text popped up, the weapon was drawn or holstered, the map was displayed, or Escape was pressed.

**Root cause 1 — PhongMap/EnvMap sampling shadow with wrong world position** (`Hunt2.cpp` `DrawPostObjects`):

`RenderModelClipPhongMap` and `RenderModelClipEnvMap` have non-zero `sz` values, so `vRhw < 1.0` and the fragment shadow gate fires. They were rendered AFTER `d3dSetHUDMode(FALSE)` at the original line 746, meaning `uWorldShadow=1` was active. PhongMap/EnvMap use additive blend (`BLEND_ONE`) and sample the shadow map with the weapon's reconstructed world position (near camera). The result was additively composited onto terrain, and its brightness changed depending on whether the weapon position was in shadow — coupling weapon shadow state to terrain appearance. **Fix**: moved `d3dSetHUDMode(FALSE)` to after both PhongMap and EnvMap passes so `uWorldShadow=0` is maintained through them.

**Root cause 2 — stale GL_TEXTURE0 binding from HUD/UI draws corrupting foliage shadow alpha** (`RendererGL.cpp`, `renderd3d.cpp`):

## Phase 4: Depth Capture + God Rays

### Depth capture (gateway feature)

`RunPostOverlay` now captures the backbuffer **depth** alongside color
(`glCopyTexImage2D` with `GL_DEPTH_COMPONENT24` into `s_depthTex`). This is the
enabler for all depth-aware post effects (god rays now; SSAO, volumetric height
fog, DoF, SSR later).

**Depth semantics (reversed convention):** cleared = 0.0, sky plane writes
sz ≈ 0.0001, world geometry writes `sz = -16/cam_z` which stays ≥ ~0.001 even at
maximum view range. So `depth < 0.0004` reliably classifies a pixel as sky.

### God rays (screen-space crepuscular rays)

Mitchell radial-blur technique, two half-res passes added to the post chain:

1. **Mask pass** (`FS_GODRAY_MASK` → fboC): sky pixels (from depth) weighted by
   quadratic falloff around the sun's screen position × `godrays_color`.
2. **Radial blur** (`FS_GODRAY_BLUR` → fboD): 48 samples marched from each pixel
   toward the sun with per-sample decay.
3. **Composite**: added to the scene before exposure/tone mapping so ACES
   compresses shaft highlights (texture unit 2 on the existing composite pass).

**Sun screen projection (CPU, in RunPostOverlay):** world sun direction
(`m_sunDirWorld`, from `SunShadowK`) is rotated into camera space with the
transpose of `m_camToWorld` (R^T stored column-major ⟹ `R[i][j] = m[i*3+j]`),
then projected with the same VideoCX/CY + CameraW/H convention the depth shader
inverts for world-pos reconstruction. Texture V is flipped (`1 - screenY/H`)
because the captured texture has row 0 at the screen bottom. Rays fade with an
edge factor as the sun leaves the screen and are skipped entirely (`grIntensity=0`)
when the sun is behind the camera.

**Prerequisite change (Hunt2.cpp flatscreen path):** `SetCameraWorldUniforms` +
`SetSunDirection` are now called every flatscreen frame regardless of shadow
mode — previously they only ran when `GetShadowMode() > 0`, which would have
left god rays with stale camera data when shadows were disabled.

**Scope:** flatscreen only (ApplyPostProcess is not called in VR) and day only
(post chain is skipped when `OptDayNight == 2`).

### Sun position misalignment (fixed post-Phase 4)

**Symptom:** god rays originated from a point visibly higher in the sky than the
rendered sun disk — noticeably wrong during morning and afternoon hunts.

**Root cause:** `SetSunDirection` was called with `(-SunShadowK, 1.0f, -SunShadowK)`.
`SunShadowK` is the *dino drop-shadow polygon offset* constant (0.7 morning / 0.5
noon / −0.7 afternoon) — it is not the authoritative sun angle. The actual sun disk
is drawn by `RenderSun(nv.x, nv.y, nv.z)` where `nv = RotateVector(Sun3dPos)`.
`Sun3dPos` is the world-space sun position vector set per time-of-day in `Game.cpp`:
morning `(−4048, 2048, −4048)` (elevation ≈19°) vs. the `SunShadowK`-derived
direction `(−0.7, 1, −0.7)` (elevation ≈45°) — a 26° vertical error.

**Fix:** `SetSunDirection(Sun3dPos.x, Sun3dPos.y, Sun3dPos.z)` in `Hunt2.cpp`.
`SetSunDirection` normalises the vector, so the magnitude of `Sun3dPos` is
irrelevant. This also aligns height fog sun scattering and CSM shadow direction
with the visual sun (more physically correct shadows, especially in the morning).

## Phase 5: Volumetric Height Fog

Depth-aware exponential height fog with sun forward scattering (`FS_HEIGHTFOG`),
the first effect to use full per-pixel world reconstruction in the post chain.

**Per-pixel world reconstruction in post:** the fog shader inverts the screen
projection exactly like `depth.vert`, but from the captured depth texture:
`cam_z = -16/depth`, pixel → camera ray via VideoCX/CY + CameraW/H, camera →
world via `uCamToWorld`. Sky pixels (depth < 0.0004) use a capped ray length of
2× the view radius so the horizon hazes out and hides the terrain draw-distance
edge, while looking up stays clear.

**Analytic fog integral (Quilez-style):** density `rho(y) = a·exp(-(y-anchor)·b)`
integrated along the ray has closed form
`a·exp(-(camY-anchor)·b) · (1-exp(-b·rd.y·t))/(b·rd.y)`; the exponent is clamped
to ±60 to avoid fp32 overflow on long downward rays. Anchor = `MapMinY*ctHScale`
(the map's lowest terrain — same horizon reference RenderSkyPlane uses), passed
per-frame from Hunt2.cpp via `SetHeightFogWorldParams` along with `ctViewR*256`.

**Pipeline placement:** full-res pass (scene + depth → `s_fogFBO`, GL_RGB8) right
after the shared depth capture and **before** bloom extraction; downstream passes
sample `sceneSrc` (the fogged texture when fog is active, `s_sceneTex` otherwise),
so bloom and tone mapping operate on the fogged image. The depth capture is now
shared between height fog and god rays — captured once when either is active.

**Sun scattering:** `pow(max(dot(rd, sunDir), 0), sun_power)` lerps the fog color
toward `heightfog_suncolor`, giving the fog a warm glow around the sun direction
that pairs with the god rays.

## Phase 6: SSAO

Depth-only Alchemy-style ambient occlusion (`FS_SSAO` + `FS_SSAO_BLUR`), no
G-buffer required.

**Technique:** view-space position from the captured depth (`z = 16/depth`,
inverse screen projection), normal from `cross(dFdx(P), dFdy(P))` flipped to face
the camera. 12 samples on a golden-angle spiral disk with per-pixel hash rotation;
each sample's occlusion is `max(0, dot(v,N) - 0.02z) / (|v|² + (0.1·radius)²)` —
the squared-distance falloff doubles as the range check, so distant geometry and
sky samples contribute nothing. The sample radius is specified in world units and
projected to pixels per-fragment (clamped 2–80 px), keeping AO scale consistent
at any distance.

**Noise removal:** 5×5 bilateral blur weighted by spatial Gaussian × linear-depth
similarity (relative 8% + 16 GU tolerance) — smooths the hash-rotation noise
without bleeding AO across silhouette edges.

**Pipeline placement:** both passes run half-res into new `GL_R8` targets
(`s_fboE` raw → `s_fboF` blurred) right after the shared depth capture. The
blurred AO is applied inside the existing full-res scene pass (`FS_HEIGHTFOG`,
unit 2): `scene *= mix(1, ao, strength)` **before** the fog mix, since occlusion
is a surface property and fog scattering above it stays unoccluded. The scene
pass now runs when either fog or SSAO is active, with `uDensity=0` acting as a
fog passthrough when only SSAO is on.

**Limitation (by design):** AO multiplies final scene color rather than just the
ambient lighting term — standard for post-process SSAO in a non-deferred
pipeline. Strength 0.7 default keeps sunlit surfaces from looking dirty.

### Banding fix (post-Phase 6)

**Symptom:** faint horizontal contour lines on the ground that follow the player,
most visible in debug modes (flat surfaces, no texture noise to mask them).

**Root cause:** the scene-pass target was `GL_RGB8` — the slow height-fog gradient
quantized into 1/255 steps (iso-distance contours, hence camera-tracking), and the
composite's unsharp-mask sharpen amplified each band-step edge into a visible line.

**Fix:** scene-pass target upgraded to `GL_RGB16F`, SSAO targets to `GL_R16F`
(same class of issue once AO multiplies the scene), and the composite now applies
a ±0.5 LSB hash dither before output — this also removes pre-existing 8-bit
banding in the sky gradient at the final backbuffer quantization.

### Terrain crack dashes (post-Phase 6)

**Symptom (second artifact):** after the banding fix, short horizontal dashes
remained on the ground at terrain-tile-row spacings, tracking the camera.

**Root cause:** hairline T-junction cracks at terrain tile/LOD seams — 1px gaps
where the sky-plane depth (~0.0001) shows through between two ground rows. The
height fog reads those pixels as sky-distance rays and paints them with heavy
fog. (SSAO was ruled out analytically: its depth-proportional bias `0.02·z`
swamps anything a tile-seam crease can produce.)

**Fix:** selective crack-fill in `FS_HEIGHTFOG` — a sky-depth pixel whose
horizontal OR vertical neighbour pair are BOTH solid geometry is a crack, not
sky, and takes the farther neighbour's depth. True sky is unaffected (at the
horizon, the side neighbours are sky too, so no pair qualifies). Note the raw
colour crack (sky tint through the 1px gap) is a pre-existing engine artifact
that only a mesh-level terrain fix would remove; the fog no longer amplifies it.

### Camera-tracking dash lines — root cause: half-res depth texel ambiguity

**Symptom:** faint horizontal dash lines on the ground tracking the camera,
persisting through multiple plausible-but-wrong fixes (shadow bias, noise hash,
normals, blur weighting). Isolated with the `ssao_debug` pack param, which
renders the raw AO buffer: the lines were IN the AO buffer — perfectly straight,
content-independent, evenly spaced rows.

**Root cause:** the half-res SSAO pass reads the full-res depth texture, and
every half-res pixel center lands EXACTLY on a full-res texel boundary
(`(j+0.5)/halfH × fullH = 2j+1`). NEAREST sampling at an exact boundary tips on
float rounding, and the rounding direction flips at certain rows. On those rows
the reconstruction pairs one texel's view ray with the neighbouring row's depth —
on receding ground that puts the reconstructed point half a depth-step off the
surface, the whole kernel registers false occlusion, and the entire row darkens.
A first-attempt fix (`floor(uv·size)+0.5` snap) moved the same coin-flip into
the `floor()` (exact-integer argument) and striped vertical silhouettes per
column instead.

**Fix (FS_SSAO `viewPos` + FS_SSAO_BLUR `lz`):** deterministic texel selection —
compute the half-res cell index first (`floor(uv·halfSize)` evaluates at i+0.5,
never boundary-ambiguous), then map to a fixed corner of the 2×2 full-res block:
`uv = (floor(uv·fullSize·0.5)·2 + 0.5)/fullSize`. Ray and depth then always
describe the same texel, so the reconstructed point lies on the real surface
regardless of which texel a boundary case would have picked. Normal-tap stride
is one half-res texel (2 full-res) — a 1-texel step could land inside the same
quantized block and produce a zero tangent → NaN normal.

**Useful by-products of the hunt** (kept, all genuine improvements):
- `ssao_debug` pack param — composite bypass that shows the raw AO buffer
- robust side-choosing normals in FS_SSAO (no garbage normals at depth edges)
- gradient-aware bilateral AO blur (full 2D blur on continuous slopes; a naive
  |Δz| test rejected vertical neighbours on receding ground, degenerating to a
  horizontal-only blur that smeared noise into streaks)
- interleaved gradient noise for kernel rotation + composite dither (the classic
  `fract(sin(dot))` hash bands at large pixel coords)
- god-ray mask crack-fill (hairline terrain cracks read as sky and streaked
  under the radial blur when the sun sat low)
- height fog crack-fill (same cracks fogged at sky distance → dashes)
- slope-scaled shadow bias in `basic.frag` (`bias *= 1 + 1.5·clamp(tanθ, 0, 4)`,
  computed in uniform control flow) — proper CSM practice, prevents acne on
  sun-tilted slopes
- sharpen coring in the composite (unsharp mask gated off below ~2/255 deltas)
- 16F intermediate targets + output dither (banding removal)

`DrawTextWithFont` and `DrawBitmap` bind `m_bitmapTexture` to `GL_TEXTURE0` and did not restore the previous binding. `FlushWorldSpaceShadow` (called at `EndWorldShadowPass`) reads `GL_TEXTURE0` for alpha-testing foliage geometry in the world-space shadow batch. The frame after any text or HUD draw, the foliage in the shadow pass was alpha-tested against the text/bitmap texture instead of its own foliage texture — changing which tree pixels wrote shadow depth, and therefore changing tree shadow patterns on the terrain. **Fix**: save/restore `GL_TEXTURE0` in `DrawTextWithFont` and `DrawBitmap` (`glGetIntegerv(GL_TEXTURE_BINDING_2D)` + restore after draw). Additionally, the object render loop in `renderd3d.cpp` now explicitly calls `glBindTexture` on unit 0 immediately after each `d3dSetTexture`, so `FlushWorldSpaceShadow` always uses the correct model texture even before the first HUD draw of the session.

## Phase 7: Water Material

Animated refractive water that replaces the flat retro water texture. Active only on
the GL path (D3D retains its original look), disabled automatically underwater and
in VR stereo.

### Architecture

**Scene capture:** `BeginWaterPass(allow, timeSec)` (called at the top of
`RenderWater()` after flushing any pending batch) copies the current color and
depth buffers into two persistent textures via `glCopyTexImage2D`:
- `m_waterSceneTex` (unit 5) — full scene color at the moment the water starts
  rendering; provides the "world seen through water" for refraction.
- `m_waterDepthTex` (unit 6) — depth at the same moment; used to compute
  per-pixel underwater path length.

`EndWaterPass()` is called after `d3dEndBufferG` flushes the last water tile and
before `RenderWCircles()` so ripple circles revert to the normal shader.

**Fragment shader (`basic.frag`, `uWaterMode == 1`):**
1. Animated surface normal from three summed cosine waves (analytic gradient
   derivatives for smooth normals without finite differences). Spatial scale 350 GU
   (~2.6 m), slow speeds (0.40/0.55/0.85 t-multipliers) to avoid tight spirals.
2. Depth-scaled refraction: undistorted depth sampled first at `suv` to get
   approximate water depth; UV offset `N.xz × (18/zSelf)` is then scaled by
   `clamp(wdist/120, 0, 1)` so shallow water (bottom clearly visible) has no
   distortion while deep water ripples the scene.
3. Absorption: `1 − exp(−wdist × clarity)` blends the refracted scene toward
   `uWaterDeepColor`, plus a constant `baseAbsorb = 0.38` so even zero-depth
   water has a visible teal tint and reads as water rather than clear glass.
4. Schlick Fresnel sky reflection + sun glint; reflectivity uniform scales the
   Fresnel weight to keep the sky contribution subtle.
5. Shoreline foam: narrow `smoothstep` band in shallow areas, broken up with the
   water tile's own texture as noise.

**Uniforms exposed to pack.json:** `water_enabled`, `water_wave_strength`,
`water_clarity`, `water_deep_color_r/g/b`, `water_foam_width`,
`water_reflectivity`.

### Tuning history and lessons

| Issue | Cause | Fix |
|-------|-------|-----|
| Too swirly | Wave spatial scale 96 GU was too fine; three-wave interference creates spiral patterns | Scale to 350 GU; reduce direction-vector magnitudes and secondary-wave amplitudes |
| Floating / sky-colored | Reflectivity 0.85 overwhelmed the water color with pale sky | Reduce to 0.35 |
| Ground visibly moving | Refraction UV shift applied uniformly even in 1-pixel-deep water | Depth-scale the offset — zero in shallow, full in deep |
| Too transparent | baseAbsorb 0.12 not enough; water_clarity 0.0025 absorbed too slowly | baseAbsorb → 0.38; clarity → 0.005 |
| Shadow flicker (every other frame) | CSM shadow block had no `uWaterMode` guard; flat water tile depth lands on the shadow-map precision boundary, flipping the shadow test frame-by-frame | Add `&& uWaterMode == 0` to the shadow block; shadow is already visible via the captured scene through refraction |

### Key invariant
The water tile geometry must **not** receive the CSM shadow directly — doing so
double-counts the shadow (the captured scene already has the shadow on the terrain)
and causes precision-boundary flicker because a flat horizontal surface sits on the
exact decision boundary of the depth comparison. The shadow on the underwater
terrain is already visible through refraction.
