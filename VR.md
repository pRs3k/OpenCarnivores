# Virtual Reality Support

OpenXR stereo rendering is fully implemented. The stereo eye loop lives in `Hunt2.cpp` (`ShowVideo` VR branch) and renders the scene twice per frame with per-eye projection matrices into runtime-allocated swapchain FBOs.

## Implemented components

- **OpenXR session**: `XR.cpp` — opens an XR session, samples HMD pose + per-eye projection matrices, manages swapchain textures, submits rendered eye textures to the runtime for compositor distortion.
- **Stereo rendering hook**: `Hunt2.cpp` — scene rendered twice per frame (one `for (int xrEye…)` loop) with per-eye view/projection matrices injected via `g_glRenderer->UpdateProjection()`.
- **Head-tracking as camera source**: HMD pose drives `CameraAlpha`/`CameraBeta` each frame via `XR::SampleVR()`.

## Asymmetric per-eye FOV (Quest 3 / OpenXR)

OpenXR supplies four independent tangent half-angles per eye (`angleLeft`, `angleRight`, `angleUp`, `angleDown`). On Quest 3 the horizontal FOV is strongly asymmetric:

| Eye | Nasal (inward) tan | Temporal (outward) tan |
|---|---|---|
| Left | ≈ 0.839 (≈ 40°) | ≈ 1.376 (≈ 54°) |
| Right | ≈ 0.839 (≈ 40°) | ≈ 1.376 (≈ 54°) |

The principal point shifts toward the nasal side:
- `VideoCX = tan_nasal / (tan_nasal + tan_temporal) × WinW ≈ 0.38 × WinW` (right eye)
- `CameraW = WinW / (tan_nasal + tan_temporal)`

The temporal half-offset from the principal point is `WinW − VideoCX ≈ 0.62 × WinW`, which is **larger** than `VideoCX`. Any code that uses `VideoCX` as a proxy for the horizontal half-FOV is implicitly using the nasal (narrower) side and will under-serve the temporal (wider) side.

### Close-object stereo split (diplopia at ~3ft)

ClipA clips the **right** screen boundary (temporal side) and ClipC clips the **left** boundary (nasal side for the right eye; temporal for the left eye). The nasal half-FOV is ≈40° (tangent ≈0.839) vs the temporal ≈54° (tangent ≈1.376).

For an object at ~3ft (≈122 GU) the per-eye IPD offset (~±4 GU) shifts its extremities by ~4/122 ≈ 2° laterally per eye. A body ±100 GU wide spans ≈40° — right at the nasal clip threshold. The right eye's ClipC (nasal=left at 40°) clips the object's left extremity; the left eye's ClipA (nasal=right at 40°) clips the object's right extremity. Each eye sees a different half of the object → the brain can't fuse them → object appears split into two.

**Fix** (`mathematics.cpp` `InitClips()`): both ClipA and ClipC use `xx_wide = max(xx_left, xx_right) * 1.05` (temporal tangent + 5% margin) for their clip planes. Geometry between nasal (40°) and temporal (54°) on the nasal side is no longer software-clipped; GL viewport handles anything beyond the physical screen edge. The transparent-corner fix (ClipA using temporal not nasal for the right boundary) is unaffected because `xx_wide ≥ xx_right` always.

### Fixes applied

**Sky plane UV** (`renderd3d.cpp` `RenderSkyPlane()`):
- `sx1 = −VideoCX` — left-edge offset from principal point ✓ (nasal)
- `sx2 = +(WinW − VideoCX)` — right-edge offset ✓ (temporal, fixed from `+VideoCX`)
- Cylindrical TV mapping: `tv = (ry·sy2 + rz) / q` at `sx=0` for both screen edges (fixed from flat-plane formula `(rx·sx + ry·sy2 + rz)/q`). The flat-plane formula has `rx = −0.002·p·CameraH·sin(yaw)`, which shears V horizontally whenever the camera is yawed — causing the sky texture to rock left/right as the player turns. Evaluating tv only at the principal point (`sx=0`) eliminates the sx term and gives a cylindrical sky where horizontal texture features appear at constant screen-y.
- **Flatscreen sky (working)**: Sky rendering uses `RotateVVector` to transform tangent basis vectors `tx`/`ty` and plane normal `nv` by camera yaw+pitch. The plane position `vbase` is damped (divided by 128) so the sky scrolls slowly while walking but doesn't jump when rotating. This produces visually pleasing sky with correct perspective.

- **VR sky (unsolved)**: When the player turns their head in VR, the sky textures visibly shift or oscillate rather than staying locked to world position. The flatscreen approach's RotateVVector rotation + camera-relative damped vbase produces yaw-coupling artifacts in VR. Root cause: the flat-plane perspective-projection math inherently couples sky appearance to camera orientation. No UV formula can simultaneously satisfy "world-locked" and "always visible from any angle." Multiple failed fix approaches: cylindrical UV (assumed qx=0), fixed world position (plane became a line when perpendicular), large tx/ty (texture tiled badly), pitch-only vbase (sky still moved), per-eye head-centre rendering (sky stayed camera-locked).

**Recommended future approach**: Replace the flat-plane sky with a simple hemisphere or spherical dome 3D model positioned at a fixed world location (e.g., far behind the player), rendered through the normal geometry pipeline. This would provide correct perspective, visibility, and true world-locking without special-case math.

- Head-centre `CameraX/Z` used for sky rendering (`g_vrCamCenterX/Z`) so the sky has no IPD-offset parallax between eyes.

**`FOVK` / `hFovCull`** — horizontal early-cull key (`Hunt2.cpp`):
- All three `FOVK =` assignments use `CameraW / (max(VideoCX, WinW−VideoCX) × 1.25f)`
- `hFovCull` local in `PreCashGroundModel()` uses the same formula
- Cull threshold is now `tan ≈ 1.72`, safely outside the `1.376` temporal edge

**Frustum clip planes** (`mathematics.cpp` `InitClips()`):
- ClipB (bottom) and ClipD (top) use separate `yy_bot`/`yy_top` tangents for VR vertical asymmetry
- ClipA clips the **right** screen boundary; ClipC clips the **left** screen boundary
- ClipA uses `xx_right = (WinW - VideoCX + 1) / CameraW` (temporal tangent ≈ 1.376 for Quest 3 right eye)
- ClipC uses `xx_left  = (VideoCX + 1) / CameraW` (nasal tangent ≈ 0.839 for Quest 3 right eye)
- Root cause of "transparent square in bottom-right corner": old code used `xx = (VideoCX+1)/CameraW` for **both** planes. For the asymmetric right eye (`VideoCX ≈ 0.38 × WinW`), ClipA clipped the right side at `scrx ≈ 0.76 × WinW` instead of `WinW`. Close terrain tiles (r ≤ 8, rendered by `DrawTPlaneClip`) in the temporal-right region were software-clipped before reaching GL, leaving the bottom-right corner empty.

### Terrain rendering architecture note

Two code paths render terrain tiles:
- **`DrawTPlane`** (r > 8, far tiles): submits pre-projected vertices directly; GL viewport clips anything outside the screen. No software frustum clipping.
- **`DrawTPlaneClip`** (r ≤ 8, close tiles): applies software clip planes (ClipA/B/C/D/Z) before submission. Incorrect clip plane bounds directly cause missing geometry in the corners for close terrain.

The `±(ctViewR+3)` loop bounds in `PreCashGroundModel()` and the `ctViewR`-radius ring in `RenderGround()` are sufficient for the actual visible frustum — extending them to cover `htan×ctViewR` tiles caused a measurable performance regression without fixing the bottom-right corner (the gap was from ClipA, not missing tile generation).

## GL state pitfall — XR::EndFrame

`XR::EndFrame()` resets several GL state variables for the OpenXR compositor's benefit after submitting swapchain images:

```
glDepthFunc(GL_LESS)    // engine expects GL_GEQUAL (reversed depth)
glClearDepth(1.0)       // engine expects 0.0
glDisable(GL_BLEND)
glUseProgram(0)
```

These changes persist into the **next frame's** VR eye-render loop. The engine uses a reversed depth convention (`GL_GEQUAL`, `glClearDepth(0.0)`, near=1 far=0). With `GL_LESS` + `clearDepth=1.0` active:
- Sky geometry (depth ≈ 0) is drawn first and writes near-zero values into the depth buffer.
- All subsequent geometry (walls, terrain, depth ≈ 0.1–0.4) fails `GL_LESS` against those values and is silently discarded.
- Result: geometry is invisible wherever sky was drawn — "holes in walls."

**Fix**: `RendererGL::RestoreEngineGLState()` (added to `Renderer.h` as a pure virtual, implemented in `RendererGL.cpp`) restores `GL_GEQUAL`, `glClearDepth(0.0)`, `GL_BLEND`, and the engine's shader program. It is called in `Hunt2.cpp` immediately before the per-eye loop, after `XR::EndFrame()` has run for the previous frame.

## World scale and IPD

Game world units: **~133 GU/m** (game units per real-world metre). Derived from `HeadY = 220 GU` (standing eye height) ÷ 1.65 m. The IPD eye-position offset in `XR.cpp::GetEyeCameraSetup` uses `kGUperM = 220/1.65 ≈ 133` to convert OpenXR metres to game units. Using a larger scale (e.g. 256) doubles the virtual IPD, shrinks the perceived world, and causes excessive near-object stereo disparity → eye strain.

## Comfort and UX
- 6DoF locomotion + snap turn options for VR comfort.
- Partially implemented: Q/E snap-turn ±30° in SDL key-down handler — works in flatscreen now, will bind to controller thumbstick-click in HMD build.
- World-space UI: `Interface.cpp` draws to 2D screen coords — needs a canvas layer that can render to a quad in 3D for VR.

## Audio for VR
- HRTF (Head-Related Transfer Function): see [AUDIO.md](AUDIO.md) for HRTF toggle (`ALC_HRTF_SOFT`).
- EFX reverb zones for spatial immersion: see [AUDIO.md](AUDIO.md).

## Weapon overlay smear fix

`DrawPostObjects()` renders the weapon in three passes:
1. **Base pass** — `RenderNearModel` with `SetStencilMode(1)`: writes stencil=1 at every rasterized weapon pixel.
2. **PhongMap pass** — `RenderModelClipPhongMap` with `SetStencilMode(2)`: additive specular overlay, reads stencil, renders only where stencil=1.
3. **EnvMap pass** — `RenderModelClipEnvMap` with `SetStencilMode(2)`: additive environment-map overlay, same stencil test.

On flat-screen the stencil test correctly restricts additive overlays to weapon pixels. In VR the eye FBOs previously had `GL_DEPTH_COMPONENT24` (no stencil buffer). `glEnable(GL_STENCIL_TEST)` against an FBO with no stencil attachment is a no-op — all fragments pass — so PhongMap/EnvMap painted on every pixel the weapon model's triangles projected onto, including terrain and sky. The depth buffer had been cleared by `Hardware_ZBuffer(FALSE)` at the start of `DrawPostObjects`, so `GL_GEQUAL` passed everywhere (all depths ≥ 0.0). This produced a bright smear over the terrain in the weapon's screen region that trailed visibly when the head moved.

**Fix** (`XR.cpp`): Depth renderbuffer format changed from `GL_DEPTH_COMPONENT24` to `GL_DEPTH24_STENCIL8`, attached as `GL_DEPTH_STENCIL_ATTACHMENT`. Eye FBO clear in `Hunt2.cpp` extended with `GL_STENCIL_BUFFER_BIT` so stencil is reset to 0 each frame before `DrawPostObjects` runs.

### Companion-window stencil clear

The companion window's `DrawPostObjects()` (at the end of the frame, default FBO) was missing `GL_STENCIL_BUFFER_BIT` in its `glClear` call. Stale stencil=1 marks from the previous frame's weapon screen position were not cleared, so PhongMap/EnvMap overlays painted at both old and new weapon positions — smear trail on the monitor.

**Fix** (`Hunt2.cpp`): companion window `glClear` extended with `GL_STENCIL_BUFFER_BIT`.

## Weapon reprojection smear (ASW during frame drops)

When the app misses its frame deadline the Quest compositor synthesises intermediate frames via Asynchronous SpaceWarp (ASW), warping the last submitted eye textures to the current head pose. The weapon is rendered head-centre (zero IPD parallax) but baked into the world-locked projection layer, so ASW warps it as if it were a world-space object → it trails during frame drops.

### Fix — `XR_KHR_composition_layer_depth` (`XR.cpp`)

Depth-based reprojection: submit the depth buffer alongside each eye's colour texture. The compositor's timewarp uses per-pixel depth to determine how far each pixel is from the camera and scales the warp correction proportionally. Weapon/HUD pixels have depth ≈ 1.0 (near plane in reversed-depth convention = ~0.01 m from camera); ASW applies near-zero warp to them, keeping the weapon approximately head-locked.

**Implementation:**
- `Init()`: probes `XR_KHR_composition_layer_depth` by trying `xrCreateInstance` with both extensions; falls back to `XR_KHR_opengl_enable`-only silently.
- `CreateSwapchains()`: if the extension is available, creates a `GL_DEPTH24_STENCIL8` depth swapchain per eye (same dimensions as colour).
- `AcquireEyeImage()`: acquires a depth swapchain image each frame and re-attaches it to the eye FBO as `GL_DEPTH_STENCIL_ATTACHMENT`, replacing the fallback RBO. If depth acquire fails, falls back to the RBO for that frame (no depth submission).
- `ReleaseEyeImage()`: fills `XrCompositionLayerDepthInfoKHR`, chains it to the projection view via `next`, then releases the depth swapchain image. Reversed depth encoding: `minDepth=1.0 → nearZ=0.01m`, `maxDepth=0.0 → farZ=10000m`.

The fallback RBO (`GL_DEPTH24_STENCIL8`) is kept and used whenever the depth extension is unavailable or a depth acquire fails, so the stencil-based weapon masking always works.

**Result**: depth submission did not visibly fix the weapon smear during frame drops. The smear under load is a fundamental limitation of rendering HUD elements into the world-locked projection layer — ASW warps the weapon as if it were world geometry regardless of its depth value. The proper fix is to render the weapon to a separate `XrCompositionLayerQuad` with `XR_COMPOSITION_LAYER_FLAG_BLEND_TEXTURE_SOURCE_ALPHA_BIT` submitted above the projection layer, or (simpler) to eliminate the frame drops themselves via CPU/GPU performance work. Depth submission is still retained as it improves reprojection quality for world geometry.

## VR performance

### Fixes applied

**Per-frame `PrintLog` spam in `DrawScene`** (`Hunt2.cpp`): Eight `PrintLog("DS:xxx\n")` calls guarded by `XR::StereoActive()` fired on every `DrawScene` call. Since `DrawScene` ran 3× per VR frame (eye 0, eye 1, companion), this was 24 file-write/flush operations per frame — ~1728/s at 72 Hz. Each `PrintLog` forces a file flush, causing measurable CPU stalls under load. **Removed**; `drawSceneStage` breadcrumbs kept for crash diagnosis. **Impact: Eliminates per-frame file I/O stalls.** Confirmed: CPU stall spikes eliminated during VR frame render.

**Companion window triple scene render** (`Hunt2.cpp`): In VR, `DrawScene` was called for eye 0, eye 1, and the SDL companion window — 3× CPU terrain projection + GPU render per frame. The companion window (`DrawPostObjects` + `ShowControlElements` included) already ran per-eye; re-rendering it from scratch for the monitor was pure waste. **Fixed by:**
- Blitting eye 1's FBO to the default SDL framebuffer inside the eye loop, immediately before `XR::ReleaseEyeImage(1)` while the swapchain image is still valid.
- Replacing the companion `DrawScene` call with a depth+stencil clear only (color left from blit).
- Skipping the post-loop `DrawHMap` / `DrawPostObjects` / `ShowControlElements` that would have overdrawn the blit and double-rendered the weapon.

**Result**: `DrawScene` now runs twice per frame (one per eye) instead of three times. **Impact: ~33% reduction in per-frame CPU+GPU scene rendering overhead.** Confirmed: noticeable frame rate improvement in dense scenes (many dinosaurs).

### Architecture notes for future optimisation

**`PreCashGroundModel` runs per eye**: The `(2*(ctViewR+3))²` terrain-scan loop (up to ~256k iterations at `ctViewR=250`) runs once per `DrawScene` → twice per VR frame. The loop projects tile positions into camera space using per-eye camera position (IPD-offset). Caching eye 0's result for eye 1 would introduce ~5 px stereo position error for tiles at 256 GU — noticeable as flat/incorrect stereo depth for near terrain. Not safe to skip without a purpose-built low-error approximation.

**`CreateChMorphedModel` per character per eye**: `Render3DHardwarePosts` calls `CreateChMorphedModel` for every visible character (dinosaur) on every `DrawScene` call. The morph result for a given animation frame is identical between eyes. Adding a per-character `lastMorphedFrame` guard (like the `LastAniTime` guard on static animated objects in `_RenderObject`) would halve the morph CPU cost at the expense of a small struct change to `TCharacter`.

**Head-locked weapon layer**: The weapon smear during ASW (frame drops) cannot be fixed at the depth-submission level — the weapon must be submitted as a separate `XrCompositionLayerQuad` with a head-locked pose rather than baked into the world-locked projection layer. Requires rendering the weapon to a dedicated small FBO each frame and submitting it as a second layer in `EndFrame`.

## HUD overlay positioning for VR

2D screen-space HUD overlays (pause menu, exit popup, map) require special handling in VR to avoid convergence issues and double-image artifacts:

**Per-eye principal point centering** (`Hunt2.cpp` `DrawPostObjects`):
- Exit popup (`EXITMODE`): use `VideoCX - dw/2` for X centering, not `(WinW - dw)/2`. Each eye has a different principal point (`VideoCX` varies per-eye); centering at screen midpoint causes the overlay to appear off-center in one eye. Positioned at `WinH / 2.75` for eye-level placement. On VR: scaled to 45% for perspective depth. On flatscreen: 100% scale for visibility.
- Map (`DrawHMap` in `renderd3d.cpp`): scale down to 85% in VR (`if (XR::StereoActive()) mapScaleF *= 0.85f`) to push further from player's face.
- Call icon (`ChCallTime` in `DrawPostObjects`): On VR, positioned using angular offset system aligned with compass/wind indicators: `X = callerCX + 0.48*CameraW` (right side, 27.5° right of forward) and `Y = callerCY - 0.20*CameraH` (above eye level), using native resolution. On flatscreen, positioned at top-right corner (original position: 10px from right edge, 7px from top), scaled to 140% for visibility.

**Single-pass HUD rendering** (`Hunt2.cpp` `DrawPostObjects`):
- Pause/exit overlays only render when `!g_vrSecondEyePass` to avoid per-eye duplication. The first eye's rendering is used for both eyes' display, preventing double images and texture coordinate divergence.
- Exception: overlays that need to be dynamically positioned (like the exit popup) should render in both eyes if centered correctly relative to per-eye principal points, so each eye sees the overlay centered in its own view.

**Menu quads (world-locked)** (`XR.cpp` `AcquireMenuImage`):
- World-locked menu distance: 2.5m in front of player is comfortable (increased from original 2m). Greater distances (3.5m) feel too far; closer distances cause convergence strain.

## Head roll (camera tilt) — rendering architecture

Head roll (`CameraGamma`, globals `cg`/`sg`) is a rotation around the depth axis. Applying it correctly requires understanding the two separate vertex-transform pipelines:

### RotateVector vs RotateVVector

The engine has two rotation functions with **opposite yaw sign conventions**:

| Function | File | Yaw x-term | Used for |
|---|---|---|---|
| `RotateVector` | `mathematics.cpp` | `+v.z * sa` | Terrain tiles, water, object billboard positions |
| `RotateVVector` | `renderd3d.cpp` | `−v.z * sa` | Sky/cloud plane tangent vectors (software renderer only) |

Because the yaw signs are opposite, the same roll formula applied to both produces **opposite horizontal displacement** on screen. `RotateVVector` is effectively dead code in the GL/VR path — no model rendering calls it — so its formula does not matter for VR.

### Roll extraction — use `asinf(ry)`, not `atan2f(ry, rx)`

In `XR.cpp` `GetHeadOrientation` and `GetEyeCameraSetup`, roll is extracted from the right-vector Y-component `ry = 2*(qx*qy + qw*qz)`:

```cpp
gamma = asinf(fmaxf(-1.f, fminf(1.f, ry)));   // CORRECT
// gamma = atan2f(ry, rx);                      // WRONG — gives π for pure 180° yaw
```

`atan2f(ry, rx)` returns π whenever `rx = −1, ry = 0` (which happens at any pure 180° yaw with zero actual tilt), causing the entire world to flip upside down when turning around. `asinf(ry)` is always 0 for any pure yaw+pitch quaternion because `ry` is algebraically zero when no physical tilt is present.

### Roll in model and shadow rendering — vertex displacement, not center

`RenderModel` / `RenderModelClip` / `RenderShadowClip` (`renderd3d.cpp`) receive the model/shadow center position `(x0, y0, z0)` already in rolled camera space (computed by `RotateVector`). They then compute per-vertex displacements using model yaw (`al`) and camera pitch (`bt`), but the displacement is added directly to the rolled center without itself being rolled:

```
rVertex.x = (p.x*ca + p.z*sa) + x0   ← x0 is rolled, displacement is NOT
rVertex.y = (p.y*cb − vz*sb)  + y0   ← same
```

This makes every vertex sit in the wrong position relative to its (correctly-rolled) center — models and shadows visually "dance" when the player tilts their head while terrain stays correct.

**Fix**: apply the same roll formula (`dx*cg + dy*sg`, `dy*cg − dx*sg`) to the displacement before adding the center. Inside these functions, `ca`/`sa`/`cb`/`sb` are **local** variables (shadowing globals), but `cg`/`sg` are **not** declared locally and correctly resolve to the global camera roll. There are five vertex loops that needed this fix — one in `RenderModel`, three across `RenderModelClip`/`RenderModelClipEnvMap`, and one in `RenderShadowClip`.

### Yaw sign consistency (`GetHeadOrientation` vs `GetEyeCameraSetup`)

`GetHeadOrientation` feeds `PlayerAlpha` (locomotion direction); `GetEyeCameraSetup` feeds `CameraAlpha` (render direction). Both must use the same sign for yaw:

```cpp
yaw = atan2f(fx, -fz);   // CORRECT — both functions
// yaw = -atan2f(fx, -fz); // WRONG — negating in one but not the other makes the
                           //   player run backwards relative to the camera direction
```

## 6DoF roomscale tracking

Head position from `XR::GetHeadCenterPos` (OpenXR reference space, metres) is converted to game units (`kGUperM = 220/1.65 ≈ 133`) and accumulated as a room offset relative to a reference anchor captured at session start. The XZ offset is rotated by `g_vrBodyYaw` so leaning forward always moves in the body-facing direction regardless of where the HMD is looking.

The offset is applied **per-eye** to the camera position alongside the IPD displacement:

```cpp
CameraX = saveX + eyeDX + g_vrRoomOffsetX;
CameraY = saveY + eyeDY + g_vrRoomOffsetY;
CameraZ = saveZ + eyeDZ + g_vrRoomOffsetZ;
```

`g_vrHeadRefSet` is reset when the VR session starts; the first frame with valid views anchors the reference.

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

Added two new global graphics options to Video Options that apply to both flatscreen and VR:

### Implemented UI controls (`Menu.cpp` lines 1631–1815)

Added to the standard VIDEO panel after Brightness:
1. **Anisotropy**: Slider 1–4 (Low/Medium/High/Max = 2x/4x/8x/hardware max). Levels 1-3 fixed; level 4 queries GPU capability via `GL_MAX_TEXTURE_MAX_ANISOTROPY` (OpenGL standard max is 16x).
2. **Supersampling**: Slider 100–200% (default 100%). Applies to both flatscreen and VR: render at `OptSSFactor%` of native resolution, downscale to native for display.

### Persistence (`Game.cpp`)

- **SaveDisplayConfig()**: Appends quality options (aniso, supersampling) to `display.cfg`.
- **LoadDisplayConfig()**: Reads quality options with version-aware fallback — v2 configs load with sensible defaults (aniso=2, supersampling=100%).
- **Version bump**: `kDisplayVer` changed from 2 to 3 for forward compatibility.

### Variables (`Hunt.h`)

```c
_EXTORNOT int  OptAnisoLevel;   // 1=Low (2x), 2=Med (4x), 3=High (8x), 4=Max (16x)
_EXTORNOT int  OptSSFactor;     // 100–200, eye FBO supersampling multiplier
```

### Notes

- **Map Scale**: Kept hardcoded at 45% in VR (line 5577 `renderd3d.cpp`) — not user-tunable.
- **Fog**: Uses global `FOGENABLE` toggle for both modes; no separate VR fog control.

### Rendering Integration Status

**Menu UI & Configuration**: ✓ Complete
- Sliders added to Video Options menu (Menu.cpp lines 1796–1828)
- Global variables declared (Hunt.h line 934)
- Defaults initialized and config persistence implemented (Game.cpp)

**Rendering Code**:
- **Anisotropy**: Code implemented (RendererGL.cpp lines 814–839) but currently disabled via commented-out code block. Maps `OptAnisoLevel` (1–3) to fixed values (2x, 4x, 8x); level 4 queries hardware maximum via `GL_MAX_TEXTURE_MAX_ANISOTROPY` (OpenGL standard max is 16x across all vendors). Can be re-enabled when anisotropy query in Init() is uncommented.
- **Supersampling**:
  - **VR**: ✓ Enabled — Eye FBO dimensions scaled by `OptSSFactor / 100.0f` in `XR.cpp` (lines 961–965). OpenXR compositor downscales rendered textures for display.
  - **Flatscreen**: ⏸ Disabled — FBO-based rendering code is present (RendererGL.cpp lines 1510–1577) but commented out. The flatscreen FBO rendering approach caused black-screen rendering failures and requires architectural redesign for safe implementation. Default to native resolution rendering until fixed.

## Remaining work
- Input abstraction: SDL3 or OpenXR input layer to replace `_KeyFlags` bitfield with an action-binding layer so VR controllers, gamepads, and rebindable keyboards all route through it.
- World-space UI: port `Interface.cpp` screen-coord drawing to a 3D quad canvas for VR.
- **Independent weapon aiming**: See section above; allows controller-relative gun pointing instead of screen-locked aiming.
