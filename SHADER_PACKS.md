# Shader Packs

Modders can create custom shader packs to extend the visual appearance of OpenCarnivores.

## Directory Structure

Shader packs live in `shaderpacks/` directory:

```
shaderpacks/
├── mypack/
│   ├── pack.json          # Pack metadata
│   ├── materials/         # Custom material shaders
│   │   ├── terrain.vert
│   │   ├── terrain.frag
│   │   ├── object.vert
│   │   └── object.frag
│   ├── effects/           # Post-process screen effects
│   │   ├── tonemap.frag
│   │   ├── bloom.frag
│   │   └── color_grade.frag
│   └── textures/          # Optional: replacement textures
│       └── (PNG/TGA files)
```

## Pack Metadata (pack.json)

```json
{
  "name": "My Shader Pack",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "A beautiful shader pack",
  "target_game_version": "1.0.0",
  "effects": [
    {
      "id": "tonemap",
      "name": "Tone Mapping",
      "type": "post_process",
      "shader": "effects/tonemap.frag",
      "enabled": false,
      "parameters": {
        "exposure": { "type": "float", "min": 0.5, "max": 2.0, "default": 1.0 }
      }
    },
    {
      "id": "bloom",
      "name": "Bloom",
      "type": "post_process",
      "shader": "effects/bloom.frag",
      "enabled": false,
      "parameters": {
        "threshold": { "type": "float", "min": 0.0, "max": 2.0, "default": 0.8 },
        "intensity": { "type": "float", "min": 0.0, "max": 2.0, "default": 1.0 }
      }
    }
  ],
  "materials": [
    {
      "id": "terrain",
      "type": "terrain",
      "vertex_shader": "materials/terrain.vert",
      "fragment_shader": "materials/terrain.frag"
    }
  ]
}
```

## Material Shaders

Custom material shaders replace the default rendering for specific geometry types.

### Terrain Material

`materials/terrain.vert`:
```glsl
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in float aDepth;
layout(location = 2) in vec4 aColor;
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    vec4 pos_clip = uProjection * vec4(aPos, aDepth, 1.0);
    gl_Position = pos_clip;
    vTexCoord = aTexCoord;
    vColor = aColor;
}
```

`materials/terrain.frag`:
```glsl
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    vec4 texel = texture(uTexture, vTexCoord);
    FragColor = texel * vColor;
}
```

## Post-Process Effects

Screen-space effects that process the final rendered frame.

Example `effects/tonemap.frag`:
```glsl
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uExposure;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);
    // Apply tone mapping
    vec3 mapped = vec3(1.0) - exp(-color.rgb * uExposure);
    FragColor = vec4(mapped, color.a);
}
```

## Loading and Using Packs

1. Place your shader pack in `shaderpacks/yourname/`
2. Run the game — packs are loaded automatically
3. Use the **Shaders** menu to enable/disable effects and adjust parameters

## Actual pack.json Format (current)

The real `pack.json` is a flat JSON file with these top-level keys:

```json
{
  "name": "My Pack",
  "description": "Description shown in UI",
  "author": "Author Name",

  "bloom_enabled": true,
  "bloom_threshold": 0.78,
  "bloom_intensity": 1.5,

  "tonemap_mode": "aces",
  "exposure": 1.0,

  "colorgrade_enabled": true,
  "colorgrade_saturation": 1.2,
  "colorgrade_contrast": 1.1,
  "colorgrade_lift_r": 0.0,
  "colorgrade_lift_g": 0.0,
  "colorgrade_lift_b": 0.0,
  "colorgrade_gain_r": 1.0,
  "colorgrade_gain_g": 1.0,
  "colorgrade_gain_b": 1.0,

  "sharpen_strength": 0.6,

  "shadows_mode": "full",
  "shadow_strength": 0.45,

  "godrays_enabled": true,
  "godrays_intensity": 0.5,
  "godrays_density": 0.9,
  "godrays_decay": 0.96,
  "godrays_color_r": 1.0,
  "godrays_color_g": 0.92,
  "godrays_color_b": 0.75,

  "heightfog_enabled": true,
  "heightfog_density": 0.00012,
  "heightfog_falloff": 0.0003,
  "heightfog_sun_power": 8.0,
  "heightfog_color_r": 0.65,
  "heightfog_color_g": 0.72,
  "heightfog_color_b": 0.80,
  "heightfog_suncolor_r": 1.0,
  "heightfog_suncolor_g": 0.85,
  "heightfog_suncolor_b": 0.65,

  "ssao_enabled": true,
  "ssao_strength": 0.7,
  "ssao_radius": 120.0,
  "ssao_intensity": 1.2,
  "ssao_debug": false,

  "water_enabled": true,
  "water_wave_strength": 0.18,
  "water_clarity": 0.005,
  "water_deep_color_r": 0.07,
  "water_deep_color_g": 0.18,
  "water_deep_color_b": 0.22,
  "water_foam_width": 50.0,
  "water_reflectivity": 0.35
}
```

### Effect Reference

| Key | Values | Description |
|-----|--------|-------------|
| `bloom_enabled` | true/false | Screen-blend bloom |
| `bloom_threshold` | 0.0–1.0 | Luminance threshold for bloom extraction |
| `bloom_intensity` | 0.0–3.0 | Bloom brightness multiplier |
| `tonemap_mode` | "none", "aces", "reinhard" | Tone mapping curve |
| `exposure` | 0.5–2.0 | Pre-tone-map exposure multiplier |
| `colorgrade_enabled` | true/false | Enable color grading |
| `colorgrade_saturation` | 0.0–2.0 | 1.0 = neutral |
| `colorgrade_contrast` | 0.5–2.0 | S-curve contrast; 1.0 = neutral |
| `colorgrade_lift_r/g/b` | −0.2–0.2 | Shadow color tint per channel |
| `colorgrade_gain_r/g/b` | 0.5–2.0 | Highlight scale per channel |
| `sharpen_strength` | 0.0–1.5 | Unsharp mask; 0 = off |
| `shadows_mode` | "none", "full" | World shadow mapping mode |
| `shadow_strength` | 0.0–1.0 | Shadow darkness; 0.45 is subtle |
| `godrays_enabled` | true/false | Screen-space sun shafts (crepuscular rays) |
| `godrays_intensity` | 0.0–2.0 | Overall ray brightness; 0.5 is subtle |
| `godrays_density` | 0.5–1.0 | Ray march length toward the sun |
| `godrays_decay` | 0.9–1.0 | Falloff along the ray; higher = longer shafts |
| `godrays_color_r/g/b` | 0.0–1.0 | Ray tint; defaults to warm sunlight |
| `heightfog_enabled` | true/false | Volumetric height fog (valley mist + horizon haze) |
| `heightfog_density` | 0.00005–0.0005 | Fog thickness at the map's lowest terrain |
| `heightfog_falloff` | 0.0001–0.001 | How fast fog thins with altitude (per GU); 0.0003 ≈ halves every 17 m |
| `heightfog_sun_power` | 2–32 | Forward-scatter focus; higher = tighter sun glow |
| `heightfog_color_r/g/b` | 0.0–1.0 | Base fog color; defaults to cool morning haze |
| `heightfog_suncolor_r/g/b` | 0.0–1.0 | Fog tint when looking toward the sun |
| `ssao_enabled` | true/false | Screen-space ambient occlusion |
| `ssao_strength` | 0.0–1.0 | How much AO darkens the scene |
| `ssao_radius` | 50–300 | Sample radius in world units (120 ≈ 0.9 m) |
| `ssao_intensity` | 0.5–3.0 | Occlusion gain; higher = darker crevices |
| `ssao_debug` | true/false | Development aid: render the raw AO buffer instead of the scene |
| `water_enabled` | true/false | Animated refractive water (GL only; no-op in VR and underwater) |
| `water_wave_strength` | 0.0–0.6 | Surface normal tilt from waves; 0 = perfectly flat mirror |
| `water_clarity` | 0.001–0.02 | Absorption per GU of underwater depth; 0.005 ≈ teal at 1 m |
| `water_deep_color_r/g/b` | 0.0–1.0 | Color of fully absorbed deep water |
| `water_foam_width` | 0–200 | Shoreline foam band in GU (0 = disabled) |
| `water_reflectivity` | 0.0–1.0 | Scales Fresnel sky reflection; 0.35 = subtle shimmer |

### shadows_mode

- `"none"` — no world shadows (original game look)
- `"full"` — full scene shadow map rendered from sun direction; all geometry (terrain + models) casts and receives shadows
- `"dinos_only"` — reserved for future use; currently treated as `"full"`

### godrays

Screen-space crepuscular rays: sky pixels around the sun's screen position are
extracted using the captured depth buffer, then radially blurred toward the sun
and added to the scene before tone mapping. Rays fade automatically when the sun
moves behind the camera or off-screen, and are inactive at night and in VR
(post-processing runs flatscreen-only).

### heightfog

Volumetric height fog: per-pixel world position is reconstructed from the captured
depth buffer and an exponential height-density fog is integrated analytically along
each view ray. Fog is anchored at the map's lowest terrain, so it pools in valleys
and thins with altitude; looking toward the sun tints the fog with `heightfog_suncolor`
(Mie-style forward scattering). Applied before bloom and tone mapping. Layers on top
of the game's retro distance fog — keep `heightfog_density` subtle. Inactive at night
and in VR, like all post effects.

### ssao

Screen-space ambient occlusion, computed entirely from the captured depth buffer
(no G-buffer needed): surfaces near corners, crevices, dense foliage, and contact
points darken in proportion to how much surrounding geometry encloses them. Grounds
dinos and props against the terrain and adds depth inside vegetation. Computed at
half resolution with a depth-aware blur (no halos around silhouettes), then applied
to the scene **before** height fog so mist is not darkened. `ssao_radius` is in
world units — larger values give broad soft shading, smaller values give tight
contact shadows.

### water

Animated refractive water surface that replaces the flat retro water texture on the
GL path. The scene and depth are captured just before the water batch starts so
each pixel can refract the world beneath it and compute the underwater path length
for colour absorption. Refraction distortion is depth-scaled — shallow areas near
the shore stay still (the sandy bottom is clearly visible), while deeper water
shimmers. Schlick Fresnel adds a subtle sky reflection and sun glint. A narrow
shoreline foam band can be enabled via `water_foam_width`.

The water material is automatically disabled underwater, in VR stereo mode, and on
the D3D path — the original retro look is preserved in those contexts.

**Shadows:** the CSM shadow is intentionally not applied directly to the water
surface. Tree and terrain shadows are already visible through the refracted scene
capture; applying CSM again would double-count the darkness and cause
precision-boundary flicker on the flat water geometry.

---

**Questions?** See the reference pack in `shaderpacks/default/pack.json`.
