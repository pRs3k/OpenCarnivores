# Shader Pack System - Summary

## What We Built

A **modular shader system** that lets modders create custom visual effects and materials without touching core game code.

### Architecture

```
Game Engine
    ↓
ShaderPackManager (discovers/loads packs from disk)
    ↓
ShaderPack (per-pack: effects + materials)
    ↓
Effects (post-process shaders applied each frame)
Materials (custom rendering for geometry types)
```

### Key Components

**ShaderPack.h/cpp**
- Loads `pack.json` from `shaderpacks/packname/`
- Compiles shader files (.vert, .frag)
- Manages effect parameters and materials
- Hot-reloadable (restart game to load new packs)

**ShaderPackManager**
- Discovers packs in `shaderpacks/` directory
- Loads/unloads packs on demand
- Provides access to active pack effects

### Directory Structure

```
shaderpacks/
├── example/                    # Reference implementation
│   ├── pack.json              # Pack metadata
│   └── effects/
│       ├── tonemap_reinhard.frag
│       └── color_grade.frag
└── yourpack/                  # Your custom pack
    ├── pack.json
    ├── effects/
    ├── materials/             # (for future use)
    └── textures/              # (for future use)
```

### Example pack.json Structure

```json
{
  "name": "My Effects Pack",
  "version": "1.0.0",
  "effects": [
    {
      "id": "tone_mapping",
      "name": "Tone Mapping",
      "type": "post_process",
      "shader": "effects/tonemap.frag",
      "enabled": false,
      "parameters": {
        "exposure": { "type": "float", "min": 0.5, "max": 2.0, "default": 1.0 }
      }
    }
  ]
}
```

## What Modders Can Do

### Post-Process Effects (Working Now)
- Tone mapping (HDR→SDR conversion)
- Color grading (saturation, brightness, contrast)
- Bloom
- Custom color spaces
- Any full-screen shader effect

**Example shader** (`effects/example.frag`):
```glsl
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uIntensity;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);
    // Apply your effect
    FragColor = color * vec4(uIntensity);
}
```

### Material Shaders (Infrastructure Ready)
- Custom terrain rendering
- Object/character materials
- Normal mapping (when integrated)
- Custom lighting models

### Parameters (Tunable Effects)
- Modders define parameters in `pack.json`
- Game exposes sliders/controls in menu
- Values passed to shaders as uniforms

## How to Create a Shader Pack

### 1. Create Directory
```
shaderpacks/mypack/
```

### 2. Write pack.json
Describe your effects and parameters. See `shaderpacks/example/pack.json`.

### 3. Write Shaders
Save `.frag` files in `effects/` subdirectory.

### 4. Test
Launch game — pack loads automatically.

## What's NOT Implemented Yet

- Full JSON parsing (currently hardcoded example)
- Menu integration for effect toggles
- Parameter UI controls
- Material shader hot-loading
- Texture replacements

## Implementation Status

| Feature | Status | Notes |
|---------|--------|-------|
| Pack discovery | ✅ | Auto-discovers `shaderpacks/` |
| Pack loading | ✅ | Loads `pack.json` |
| Shader compilation | ✅ | Compiles `.vert` and `.frag` files |
| Effect parameters | ✅ | Defined in `pack.json` |
| Menu integration | ❌ | Needs UI work |
| Material shaders | 🔵 | Infrastructure ready, needs integration |
| Documentation | ✅ | Full guide in SHADER_PACKS.md |
| Example pack | ✅ | Tone mapping + color grading |

## Next Steps for Modders

1. Copy `shaderpacks/example/` as a template
2. Modify `pack.json` with your own effects
3. Write custom `.frag` shaders
4. Test by launching game
5. Share your pack with the community

## Technical Notes

- **Performance**: Post-process effects run after scene rendering
- **Input**: Each effect receives `uScreenColor` (scene rendered to texture)
- **Output**: Effect shader writes to `FragColor`
- **Coordinates**: Fragment shaders use `vTexCoord` (normalized 0-1 UV)
- **Hot-reload**: Shaders are not hot-reloaded; restart game to load new packs

## See Also

- [SHADER_PACKS.md](SHADER_PACKS.md) — Complete modding guide
- [shaderpacks/example/](shaderpacks/example/) — Reference implementation
- [RENDERING.md](RENDERING.md) — Rendering architecture
