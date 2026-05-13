# Virtual Reality Support

OpenXR stereo rendering is fully implemented. The stereo eye loop lives in `Hunt2.cpp` (`ShowVideo` VR branch) and renders the scene twice per frame with per-eye projection matrices into runtime-allocated swapchain FBOs.

## Setup: openxr_loader.dll requirement

**CRITICAL**: VR mode requires `openxr_loader.dll` in the build output directory (`build/Debug/` or `build/Release/`). This DLL is provided by Meta's OpenXR runtime.

### How to get openxr_loader.dll

**Option 1** (Recommended): Install **Meta Horizon Link** (formerly Meta Quest Link)
- Download from [Meta Developer](https://www.meta.com/developers/webxr/)
- Installs to: `C:/Program Files/Meta/MetaXREngine/openxr_loader.dll`
- CMakeLists.txt will auto-detect and copy it during build

**Option 2**: Manual copy
- Locate `openxr_loader.dll` from any OpenXR-enabled application or Meta runtime installation
- Copy to `build/Debug/` and `build/Release/` directories
- CMakeLists.txt will recognize it and include in build

**If DLL is missing**: The build will warn you, and VR will not launch (flatscreen still works). You'll see "Run-Time Check failure" or Meta Horizon Link failing to launch the game.

## Implemented components

- **OpenXR session**: `XR.cpp` — opens an XR session, samples HMD pose + per-eye projection matrices, manages swapchain textures, submits rendered eye textures to the runtime for compositor distortion.
- **Stereo rendering hook**: `Hunt2.cpp` — scene rendered twice per frame (one `for (int xrEye…)` loop) with per-eye view/projection matrices injected via `g_glRenderer->UpdateProjection()`.
- **Head-tracking as camera source**: HMD pose drives `CameraAlpha`/`CameraBeta` each frame via `XR::SampleVR()`.

## Asymmetric per-eye FOV (Quest 3 / OpenXR)

OpenXR supplies four independent tangent half-angles per eye (`angleLeft`, `angleRight`, `angleUp`, `angleDown`). Quest 3 has strongly asymmetric horizontal FOV (~40° nasal, ~54° temporal). The principal point shifts toward the nasal side, making `VideoCX` (screen-space center) offset from the geometric center. Code using `VideoCX` as a proxy for symmetric half-FOV will under-serve the wider temporal side.

### Close-object stereo split (diplopia at ~3ft)

Due to asymmetric FOV and IPD offset, close objects near the nasal edge can appear split between eyes (each eye sees different half, brain can't fuse). **Fix** (`mathematics.cpp` `InitClips()`): clip planes use the wider temporal tangent (+ 5% margin) instead of nasal, allowing GL viewport to handle edge clipping rather than software frustum clipping. This prevents the nasal-side geometry from being pre-clipped for one eye while visible to the other.

### Sky rendering

**Flatscreen**: Uses damped camera-relative plane positioning + yaw/pitch rotation to produce visually pleasing perspective without artifacts.

**VR (unsolved)**: Sky textures shift visibly when turning head, rather than staying locked to world position. Root cause: flat-plane perspective math inherently couples sky appearance to camera yaw. Previous attempts (cylindrical UV, fixed world position, pitch-only vbase) all failed. Head-centre camera position is used to eliminate IPD parallax.

**Recommended future approach**: Replace flat-plane sky with 3D dome/hemisphere model at fixed world position, rendered through normal geometry pipeline. This provides correct perspective and true world-locking without special-case UV math.

**`FOVK` / `hFovCull`** — horizontal early-cull threshold: uses temporal FOV (wider side) with margin to safely exclude geometry that would be off-screen without clipping geometry that's actually visible. Prevents the nasal edge from over-culling.

**Frustum clip planes** (`mathematics.cpp` `InitClips()`):
- ClipA/C use asymmetric clip bounds (temporal vs nasal tangent) for left/right screen edges
- ClipB/D use separate vertical tangents for top/bottom asymmetry
- **Previous bug** (bottom-right corner transparent): both planes incorrectly used the nasal tangent, over-clipping the temporal side before geometry reached GL. Close terrain was software-clipped away while remaining invisible.

### Terrain rendering architecture note

Two code paths:
- **`DrawTPlane`** (far tiles, r > 8): no software clipping; GL viewport handles screen edges.
- **`DrawTPlaneClip`** (close tiles, r ≤ 8): applies software clip planes before submission. Clip plane bugs directly cause visible corner artifacts.

Loop bounds in `PreCashGroundModel()` are sufficient for the visible frustum. Extending loop bounds didn't fix corner artifacts — the issue was always in ClipA/C bounds, not missing tile generation.

## GL state pitfall — XR::EndFrame

`XR::EndFrame()` resets GL state (depth func, blend, program) for the OpenXR compositor. These changes persist into the **next frame's** eye-render loop. The engine uses reversed depth (`GL_GEQUAL`, near=1 far=0), so with compositor's `GL_LESS` + `clearDepth=1.0` active, sky geometry fails depth test and all subsequent geometry becomes invisible ("holes in walls").

**Fix**: `RendererGL::RestoreEngineGLState()` restores engine GL state immediately before the per-eye loop. This is called after each `XR::EndFrame()` from the previous frame completes.

## World scale and IPD

Game world units: **~133 GU/m** (from `HeadY = 220 GU` ÷ 1.65 m standing eye height). This scale is critical: changing it affects perceived world size and IPD-induced stereo disparity. Larger scales (e.g. 256 GU/m) shrink the perceived world and double IPD disparity, causing eye strain at close distances.

## Comfort and UX
- 6DoF locomotion + snap turn options for VR comfort.
- Partially implemented: Q/E snap-turn ±30° in SDL key-down handler — works in flatscreen now, will bind to controller thumbstick-click in HMD build.
- World-space UI: `Interface.cpp` draws to 2D screen coords — needs a canvas layer that can render to a quad in 3D for VR.

## Audio for VR
- HRTF (Head-Related Transfer Function): see [AUDIO.md](AUDIO.md) for HRTF toggle (`ALC_HRTF_SOFT`).
- EFX reverb zones for spatial immersion: see [AUDIO.md](AUDIO.md).

## Weapon rendering in VR

Weapon rendering uses stencil buffer to restrict additive overlays (PhongMap, EnvMap) to weapon pixels only. **Bug**: VR eye FBOs lacked stencil buffer (`GL_DEPTH_COMPONENT24`), so overlays painted over terrain/sky. **Fix**: Changed to `GL_DEPTH24_STENCIL8` FBO format; clear stencil with `GL_STENCIL_BUFFER_BIT` each frame.

Companion window was missing `GL_STENCIL_BUFFER_BIT` in its clear call, causing overlay smear trails on monitor. **Fix**: Extended companion window clear with stencil bit.

## Weapon reprojection smear (ASW during frame drops)

When the app misses frame deadline, Quest compositor synthesises frames via ASW, warping submitted textures to current head pose. Weapon is head-locked but baked into world-locked projection layer, so ASW warps it as world geometry → trails during frame drops.

**Implemented feature** `XR_KHR_composition_layer_depth`: Submit depth buffer with eye textures so compositor's timewarp can scale warp correction per-pixel. Weapon pixels (near plane, depth ≈ 1.0) receive near-zero warp, staying approximately head-locked.

**Limitation**: Depth submission improves reprojection quality for world geometry but does not eliminate weapon smear under frame drops. Proper fix requires rendering weapon to separate `XrCompositionLayerQuad` submitted above projection layer, or eliminating frame drops via performance work.

## VR performance

### Optimizations applied

**Removed debug spam** (`DrawScene`): Eight debug `PrintLog` calls per frame caused file flushes (24/frame × 72 Hz = 1728/s). Eliminated CPU stalls.

**Companion window rendering**: In VR, `DrawScene` ran 3× per frame (eye 0, eye 1, companion) — pure waste since post-effects already ran per-eye. **Fix**: Blit eye 1's FBO to SDL default framebuffer; skip companion `DrawScene`. **Result**: `DrawScene` now runs 2× per frame (~33% CPU+GPU overhead reduction).

### Render distance cap in VR

**Problem**: Terrain rendering happens in a per-eye loop (Hunt2.cpp lines 2037–2180). Terrain tiles grow quadratically by distance O(r²). In stereo, this workload is doubled. Example: increasing `ctViewR` from 50 to 62 (24% increase) yields ~20,000 tiles per eye, totaling ~40,000 tiles per frame. At 30 FPS, user perceives a **sharp framerate cliff**—small slider adjustments cause dramatic FPS drops (60 FPS → 30 FPS with +10% distance).

**Optimization attempts that failed**:
1. **Terrain LOD (ProcessMap2 for distant tiles)**: Caused visible height popping, texture popping as camera moves, and grey holes in geometry where coarse (2×2) and detailed (1×1) tile grids don't align.
2. **Height morphing in transition zone**: Reduced but didn't eliminate artifacts; complex to tune without visual impact.
3. **Aggressive FOV culling**: Simple world-space offset culling (`if (fabs(x) < r * fovFactor)`) doesn't account for perspective projection, resulting in visible grey grid lines at screen edges.

**Solution**: Cap `ctViewR ≤ 110` in VR mode (`Game.cpp::ApplyViewRange()`). Flatscreen max is 250; VR cap trades visibility for stable 90 FPS. The cap is applied based on `XR::StereoActive()` check. Users at ~78% slider position (ctViewR ≈ 62) see ~30 FPS without cap; with cap and no LOD, achieve playable performance with clean visuals.

**Trade-off accepted**: Full visual fidelity (no LOD popping) at the cost of lower max render distance in VR. Matches real-world VR expectations (Quest 3 LOD distances are similarly constrained).

### Future optimisation opportunities

**`PreCashGroundModel` per-eye caching**: Currently scans terrain per-eye (2× per frame) for IPD-correct positioning. Caching eye 0 for eye 1 would introduce stereo-depth error (~5 px for tiles at 256 GU). Feasible only with dedicated low-error approximation.

**`CreateChMorphedModel` caching**: Currently called per character per eye, but morph results are identical. Adding frame guard would halve CPU cost (small `TCharacter` struct change).

**Head-locked weapon layer**: Weapon smear during ASW requires separate `XrCompositionLayerQuad` with head-locked pose, not baked into projection layer. Needs weapon-only FBO per frame.

## HUD overlay positioning for VR

Screen-space HUD (pause, exit popup, map) requires special handling to avoid convergence/double-image artifacts:

**Asymmetric principal point**: Each eye has different `VideoCX` (screen-space center). Overlays must center on per-eye principal point, not screen center, so they appear centered in each eye's view independently.

**Single-pass rendering**: Overlays render only on first eye (`!g_vrSecondEyePass`); second eye reuses first eye's rendering to avoid duplication. Exceptions: dynamically positioned overlays (like exit popup) may render per-eye if principal-point centered correctly.

**Scaling**: Overlays scaled down in VR (map 85%, exit popup 45%) for depth perception. Flatscreen uses larger scale (100%, 140%) for visibility.

**World-locked menus** (2.5m in front of player) are comfortable for VR convergence. Closer distances strain eyes; farther feels too distant.

## VR Binoculars Enhancement

When using binoculars in VR, the view magnifies via camera zoom (BinocularPower: 1.5–3.0×, +/- keys). Edge vignetting frames the view as realistic binocular:

**Vignette masking** (`Hunt2.cpp`):
- Four black bars (top/bottom/left/right) prevent peripheral world visibility
- Top bar 15% of screen height, bottom 35%, left/right 25% of width
- Rendered with `FillRect()` + GL_BLEND inside per-eye loop (active only `BINMODE=true`)

**Zoom mechanism**:
- `BinocularPower` global stores magnification level
- Applied to per-eye camera in eye loop: `CameraW/H *= BinocularPower` after per-eye setup
- Flatscreen applies same zoom earlier in render path

## Head roll (camera tilt) — rendering architecture

Head roll (`CameraGamma`, globals `cg`/`sg`) rotates around depth axis. Proper implementation requires understanding two separate pipelines:

**Roll extraction from quaternion**: Use `asinf(ry)` where `ry = 2*(qx*qy + qw*qz)`. Do NOT use `atan2f(ry, rx)` — it returns π for pure 180° yaw (no physical tilt), causing world flip. `asinf(ry)` correctly returns 0 for any yaw+pitch-only quaternion.

**Vertex displacement vs center**: Model/shadow centers are correctly rotated by `RotateVector`, but per-vertex displacements were added without roll transformation. This caused models to "dance" when player tilted head. **Fix**: Apply same roll formula to displacement before adding center. Five vertex loops affected: one in `RenderModel`, three in `RenderModelClip`/`RenderModelClipEnvMap`, one in `RenderShadowClip`.

**Yaw sign consistency**: `GetHeadOrientation` (locomotion) and `GetEyeCameraSetup` (rendering) must extract yaw with same sign formula. Opposite signs cause player to run backwards relative to head direction.

## 6DoF roomscale tracking

Head position from OpenXR (metres) is converted to game units and accumulated as room offset relative to session-start anchor. XZ offset is rotated by body yaw so forward movement respects player facing, not HMD looking direction. Offset is applied per-eye alongside IPD displacement. Reference resets on session start; first frame with valid pose anchors baseline.

## Independent weapon aiming (controller-relative pointing)

### Current state
The weapon is rendered **screen-locked** in `DrawPostObjects()` via `RenderNearModel()` at fixed screen coordinates (line 733). The aiming raycast origin is the player head, and direction is `PlayerAlpha + wpnDAlpha` / `PlayerBeta + wpnDBeta` (camera direction + weapon bobble). This locks the weapon to the camera view.

### Proposal
Allow the weapon to be positioned and aimed based on **controller pose** (6DoF hand tracking) independently of head orientation. When you point the controller, the weapon follows; turning your head doesn't move the gun. This is how modern VR shooters work (Half-Life: Alyx, etc.).

### Implementation plan

**Phase 1: Controller pose exposure** (1–2 hours)
- Add public function to `XR.h`: `bool GetControllerPose(int hand, float pos[3], float orient[4], bool& isActive);`
- Map `g_ctrl[hand].aimPose` (XrPosef, metres in OpenXR reference space) to game world coordinates:
  - Position: scale metres by `kGUperM ≈ 133` (same as head position in `GetHeadCenterPos()`)
  - Orientation: extract yaw/pitch/roll from quaternion using same method as `GetHeadOrientation()`
- **Already exists**: controller aim pose is sampled every frame in `SampleVR()` at line 1463–1479; `g_ctrl[0/1].aimValid` flags tracking state

**Phase 2: Weapon rendering mode** (2–3 hours)
- In `DrawPostObjects()` (Hunt2.cpp line 733), add conditional path:
  - **VR + controller active**: Get controller world position/rotation; transform to screen space using current projection matrix; render weapon at controller-relative grip offset; use controller yaw/pitch instead of `PlayerAlpha/Beta`
  - **VR without controller OR flatscreen**: Use existing screen-locked rendering
- Weapon bob/recoil (`wpnDAlpha`, `wpnDBeta`) needs rework: currently applied in screen space, would need world-space animation in controller mode

**Phase 3: Aiming raycast update** (1–2 hours)
- Modify `ProcessShoot()` (Hunt2.cpp ~line 1155):
  - Current: raycast origin is `PlayerX/Y+HeadY/Z` (head position)
  - New: when VR + controller active, use controller world position as origin
  - Direction: controller forward vector + weapon bob/recoil adjustments

**Phase 4: Collision and near-plane handling** (1 hour)
- Weapon model now clips geometry differently (no longer guaranteed screen-visible)
- Add near-plane clipping when controller is held too close to camera
- Verify weapon doesn't z-fight with player hands/body

**Phase 5: Testing and polish** (1–2 hours)
- Tune weapon grip offset (where gun attaches to hand) for comfort
- Compare aiming accuracy vs screen-locked mode
- Edge cases: controller tracking loss, low battery, grip transition

### Architecture notes

**Why feasible**:
- OpenXR controller poses already tracked and validated in `g_ctrl[]` every frame
- `GetControllerMenuCursor()` (XR.h line 197) already demonstrates controller-to-screen raycasting
- `RenderNearModel()` supports arbitrary position/rotation; only parameter passing changes
- Graceful fallback: screen-locked mode is the existing code path

**Risks**:
- **Medium**: Converting controller world pose to screen-space projection matrix coordinates for `RenderNearModel()` 
- **Medium**: Weapon perspective will feel different; aiming comfort depends on grip offset tuning
- **Low**: No changes to core game loop; can be feature-gated with console command
- **Low**: Asymmetric FOV (Quest 3) — controller position projects differently per eye, but `GetEyeCameraSetup()` already handles this for each eye

**Estimated effort**: 6–10 hours for working prototype; 12–15 hours including polish + edge cases.

## Graphics Quality Options

Two graphics quality settings added to Video Options (both flatscreen and VR):

**Anisotropy** (slider 1–4): Low/Medium/High/Max = 2x/4x/8x/hardware max. Level 4 queries GPU capability via `GL_MAX_TEXTURE_MAX_ANISOTROPY` (OpenGL standard max 16x).

**Supersampling** (slider 100–200%, default 100%): Render at `OptSSFactor%` of native resolution, downscale for display.

### Architecture

Settings are stored in globals (`OptAnisoLevel`, `OptSSFactor`) and persisted to `display.cfg` with version-aware fallback. Config version bumped for forward compatibility.

**Anisotropy**: Maps option level (1–3) to fixed values (2x/4x/8x); level 4 queries hardware max. Implementation in RendererGL; can be toggled via code comment.

**Supersampling**:
- **VR**: ✓ Enabled — Eye FBO scaled by multiplier; OpenXR compositor downscales for display.
- **Flatscreen**: Disabled — FBO approach caused rendering failures; requires architectural redesign.

**Fog & Map Scale**: Fog uses global toggle; map scale remains hardcoded (not user-tunable).

## Remaining work
- Input abstraction: SDL3 or OpenXR input layer to replace `_KeyFlags` bitfield with an action-binding layer so VR controllers, gamepads, and rebindable keyboards all route through it.
- World-space UI: port `Interface.cpp` screen-coord drawing to a 3D quad canvas for VR.
- **Independent weapon aiming**: See section above; allows controller-relative gun pointing instead of screen-locked aiming.
