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
  "shadow_strength": 0.45
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

### shadows_mode

- `"none"` — no world shadows (original game look)
- `"full"` — full scene shadow map rendered from sun direction; all geometry (terrain + models) casts and receives shadows
- `"dinos_only"` — reserved for future use; currently treated as `"full"`

---

**Questions?** See the reference pack in `shaderpacks/default/pack.json`.
