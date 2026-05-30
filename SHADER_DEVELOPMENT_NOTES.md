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

`DrawTextWithFont` and `DrawBitmap` bind `m_bitmapTexture` to `GL_TEXTURE0` and did not restore the previous binding. `FlushWorldSpaceShadow` (called at `EndWorldShadowPass`) reads `GL_TEXTURE0` for alpha-testing foliage geometry in the world-space shadow batch. The frame after any text or HUD draw, the foliage in the shadow pass was alpha-tested against the text/bitmap texture instead of its own foliage texture — changing which tree pixels wrote shadow depth, and therefore changing tree shadow patterns on the terrain. **Fix**: save/restore `GL_TEXTURE0` in `DrawTextWithFont` and `DrawBitmap` (`glGetIntegerv(GL_TEXTURE_BINDING_2D)` + restore after draw). Additionally, the object render loop in `renderd3d.cpp` now explicitly calls `glBindTexture` on unit 0 immediately after each `d3dSetTexture`, so `FlushWorldSpaceShadow` always uses the correct model texture even before the first HUD draw of the session.
