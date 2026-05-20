# 2K Resolution Artifact: Foliage Darkening & Headlamp Circle

## Summary
A texture sampling artifact appears exclusively at 2K resolution (2560×1440) on the rendering system, manifesting as:
1. **Foliage darkening** when player moves (VSpeed > 0), returning to normal brightness when still
2. **"Headlamp" circle of light** that follows player velocity direction and bobs with headbob
3. Both artifacts disappear completely at 4K resolution (3840×2160)

**Root cause:** Precision loss in texture coordinate quantization and screen-space vertex interpolation at 2K pixel density. The artifact exists identically in the **original Carnivores 2 (D3D6, Windows XP)**, confirming this is not a porting issue but a limitation of the original 1998 engine.

---

## Artifact Characteristics

### Foliage Darkening
- **Trigger:** Player movement (VSpeed > 0)
- **Manifestation:** Sudden ~20-30% brightness reduction on all foliage when movement starts; sudden return to normal when movement stops
- **Behavior:** Binary state (dark while moving, bright while still); NOT oscillating with headbob
- **Scope:** Affects only alpha-tested geometry (bushes, small trees, foliage); terrain unaffected
- **Camera dependency:** Unaffected by camera rotation; only depends on player velocity magnitude

### Headlamp Circle
- **Trigger:** Player movement (VSpeed > 0)
- **Manifestation:** Bright circular region on ground/foliage appearing in front of player
- **Direction:** Follows player **velocity direction**, not camera look direction
- **Bob:** Oscillates with headbob (camera Y oscillation) amplitude proportional to VSpeed
- **Scope:** Foliage-only; never appears on terrain, trees, models, sky, or weapons
- **Distance:** Appears at mid-range (ground and small bushes nearby, not far terrain)
- **Disappearance:** Completely invisible in all debug modes that override fragment shader output (modes 3, 22, 23, 24, 25)

---

## Investigation Results

### What We Ruled Out (With Testing)
| Hypothesis | Test | Result | Notes |
|-----------|------|--------|-------|
| Fog calculation | Debug mode 12/13 (fog visualization) | Not fog-related | Darkening persists even with fog disabled |
| Per-vertex Light values | Forced uniform Light=96 with DBG_NO_WEAPON | Not Light-related | Darkening still occurs |
| Post-processing effects | Toggled bloom/tone mapping | Causes screen lockup | Pipeline has GPU sync issues; can't even enable |
| Screen-space effects | Debug mode 23 (depth heatmap) | Not screen-space | Circle invisible in depth-only rendering |
| Vertex colors | Debug mode 3 (vertex color only) | Not vColor | No darkening/circle in color-only mode |
| LOD calculation | Changed to depth-based LOD (from screen-space derivatives) | Still appears | Not LOD-related despite NVIDIA precision comment |
| Texel snapping | Disabled floor() for foliage UVs | Still appears | Snapping not the cause |
| Headbob oscillation in depth | Removed headbob contribution from rv.z | No improvement | Darkening persists unchanged |

### What We Confirmed
1. **Texture sampling path** — Circle only visible when texel sampling occurs (modes that override fragment output show no circle)
2. **Foliage-only** — Debug mode 10 (terrain=color, foliage=gray) shows NO circle; foliage rendering is the exclusive source
3. **Velocity-dependent, not headbob-oscillation** — Darkening is sudden on/off (not periodic), only triggered by VSpeed > 0
4. **Original game has identical artifact** — Tested in original Carnivores 2 (D3D6, Windows XP): same foliage darkening and circle appear at 2K resolution
5. **Resolution-dependent** — Artifacts completely disappear at 4K (3840×2160) in both original and OpenCarnivores

---

## Root Cause Analysis

### Why It Happens at 2K But Not 4K
At 2K resolution (2560×1440), there is approximately **56% lower pixel density** than 4K (3840×2160). This causes:

1. **Texture coordinate quantization error** — When vTC (perspective-correct UV coordinates) are calculated as `vTexCoordR / vRhw`, small precision errors become visible texel offsets at lower pixel densities
2. **Screen-space interpolation loss** — noperspective varyings (vTexCoordR, vRhw, vColor) are linearly interpolated in screen space. At lower pixel densities, rasterization precision loss becomes more apparent
3. **Movement-induced bias** — When VSpeed > 0, the camera-space coordinate calculations include velocity information in subtle ways (through headbob amplitude and frame-to-frame delta). At lower pixel density, this bias becomes quantized into visible texture offsets

### Precision Chain
```
World coordinates → Camera-space (depends on CameraX/Y/Z including headbob amplitude) 
  → RotateVector (camera rotation applied) 
  → Screen projection (depends on depth rv.z) 
  → noperspective interpolation (precision loss at low pixel density)
  → Texture coordinate calculation (vTC = vTexCoordR / vRhw with quantization error)
  → Texel sampling (wrong texel selected, causing brightness shift)
```

The quantization error is **sub-pixel at 4K** (falls within a single texel's interpolation error budget) but **visible at 2K** (accumulates to neighboring texel offset).

---

## Attempted Fixes & Why They Failed

### 1. Depth-Based LOD (Initial Attempt)
**Hypothesis:** NVIDIA's smooth varying precision issue causing screen-center headlamp ring  
**Change:** Modified shader to use depth-based LOD for all geometry instead of screen-space derivatives  
**Result:** Circle persists; not LOD-related  
**Lesson:** The documented NVIDIA precision issue (bright circle at screen center on some depths) is a different artifact

### 2. Depth Oscillation Correction
**Hypothesis:** Headbob oscillation in depth calculation causes perspective division to oscillate, shifting color interpolation  
**Change:** Added `rv.z += stepdy * sb` to remove headbob contribution from depth  
**Result:** Eliminated bobbing visual effect but darkening persisted; introduced unwanted geometric side effects  
**Lesson:** The darkening is NOT from perspective division oscillation, but from a more fundamental texture coordinate issue

### 3. Texel Snapping Disable/Re-enable
**Hypothesis:** floor() operation in texel snapping amplifies velocity bias  
**Changes:** Disabled texel snapping entirely, then re-enabled it  
**Result:** Minimal to no effect on darkening artifact  
**Lesson:** The bias is upstream of the snapping operation

### 4. Revert to Original Camera Y for Depth
**Hypothesis:** Using CameraYStable (no headbob) would stabilize depth without darkening  
**Change:** Used CameraYStable instead of CameraY for v[0].y in PreCashGroundModel  
**Result:** Eliminated darkening but introduced visible vertical bobbing; reverted  
**Lesson:** Screen position and depth are entangled in the original engine; can't change one without visible artifact

---

## Why It Can't Be Fixed With Simple Code Changes

The artifact is **not a bug in our porting code** — it's a **fundamental precision limitation of the original 1998 Carnivores 2 engine** manifesting at 2K pixel density.

To fix it would require one of:

1. **Resolution-specific shader patches** — Detect 2K resolution and apply precision corrections only at that scale (complex, hacky, breaks "faithful port" principle)
2. **Texture coordinate recalculation** — Don't use the CPU-calculated texture coordinates; recalculate them in the shader with higher precision (major refactor, affects all geometry)
3. **Screen-space stabilization** — Apply jitter/dithering to hide the quantization error (destroys image quality)
4. **Architectural rewrite** — Move all vertex calculations from CPU to GPU shaders, eliminate screen-space pre-transformation (defeats the purpose of a port)

---

## Future Fix Approaches (If Pursued)

### Option A: Resolution Detection + Precision Correction (Least Invasive)
```cpp
if (screenWidth == 2560 && screenHeight == 1440) {
    // Apply 2K-specific precision correction to vTC or vRhw in shader
    // Add small epsilon to counteract quantization
}
```
**Pros:** Localized, doesn't affect 4K rendering  
**Cons:** Magic number, doesn't address root cause, might break other resolutions

### Option B: GPU-Side Texture Coordinate Calculation
Move texture coordinate calculation from CPU to vertex shader, using full 32-bit precision instead of float32 vertex data precision loss.  
**Pros:** Addresses root cause  
**Cons:** Requires significant refactor; performance implications; breaks compatibility with original vertex format

### Option C: Accept as Original Limitation
Document as "known artifact of original engine at 2K resolution; use 4K or higher for artifact-free rendering."  
**Pros:** Honest, maintains faithful port, minimal code changes  
**Cons:** Leaves artifact visible on 2K monitors

---

## Testing Matrix (For Future Reference)

| Condition | Foliage Darken? | Circle Present? | Headlamp Visible? |
|-----------|-----------------|-----------------|-------------------|
| 4K, moving | ❌ No | ❌ No | ❌ No |
| 4K, still | ❌ No | ❌ No | ❌ No |
| 2K, moving | ✅ Yes | ✅ Yes | ✅ Yes |
| 2K, still | ❌ No | ❌ No | ❌ No |
| Original D3D6 2K, moving | ✅ Yes (identical) | ✅ Yes (identical) | ✅ Yes (identical) |
| Original D3D6 4K, moving | ❌ No | ❌ No | ❌ No |

---

## Debug Modes Used

- **Mode 3:** Vertex color only — no darkening/circle visible
- **Mode 10:** Terrain=color, foliage=gray — no darkening/circle visible (foliage confirmed as exclusive source)
- **Mode 8:** UV fract visualization — no visible distortion in circle area
- **Mode 23:** Depth heatmap — circle completely invisible
- **Mode 24:** vTC fract x4 — no visible distortion
- **Mode 25:** vRhw visualization — circle completely invisible

---

## Key Files & Code References

- `Hunt2.cpp`: PreCashGroundModel() (line 151) — calculates VMap with camera-relative coordinates including headbob
- `shaders/basic.frag`: Texture sampling and perspective-correct UV calculation (lines 63-100)
- `shaders/basic.vert`: Perspective division setup (lines 36-46, w = 1/rhw calculation)
- `mathematics.cpp`: RotateVector() (line 50) — applies camera rotation, includes v.y * sb depth contribution

---

## Conclusion

This artifact is a **faithful reproduction of an original Carnivores 2 (1998) rendering precision limitation** that manifests at 2K resolution due to lower pixel density. It appears in both the original D3D6 engine and the OpenGL port identically, confirming our porting is correct.

The artifact is **resolution-dependent** and **disappears completely at 4K and higher**, where pixel density is sufficient to hide the quantization errors in texture coordinate calculation and screen-space interpolation.

**Recommendation:** Document as known limitation; pursue architectural refactoring only if 2K resolution support becomes critical for the project.
