# OpenCarnivores Shader Packs

This directory contains **modular shader packs** that extend OpenCarnivores' visual appearance.

## Getting Started

1. **Copy a shader pack folder here**: `shaderpacks/yourpackname/`
2. **Launch the game** — packs load automatically
3. **Enable effects in the menu** — Shaders > Choose Pack > Enable/Configure Effects

## Creating Your Own Pack

See [SHADER_PACKS.md](../SHADER_PACKS.md) for the complete modding guide.

**Quick start**:
- Copy the `example/` folder as a template
- Edit `pack.json` to describe your effects
- Add `.frag` shader files in `effects/` directory
- Test by loading your pack in-game

## Example Pack

The `example/` folder includes:
- Reinhard tone mapping (HDR to SDR conversion)
- Color grading (saturation, brightness, contrast)
- Reference implementations of common effects

## Sharing Your Pack

1. Test thoroughly in-game
2. Document any parameters/usage in a README
3. Share on modding platforms or GitHub
4. Include your `pack.json` so users can see what it offers

## Limitations & Future Features

**Currently supported**:
- Post-process screen effects (fullscreen shaders)
- Effect parameters (float sliders)

**Coming soon**:
- Material shaders (custom terrain/object rendering)
- Normal mapping and parallax mapping
- Custom lighting models
- Deferred rendering support

---

**Questions?** See the full documentation in [SHADER_PACKS.md](../SHADER_PACKS.md).
