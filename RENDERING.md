# Rendering System

## Backends
- `renderd3d.cpp` — Direct3D 6 rendering backend.
- `Render3DFX.cpp` — 3Dfx Glide rendering backend.
- `RenderSoft.cpp` — Software rendering backend.
- `renderasm.cpp` — x86 ASM optimized software rendering routines.
- `Renderer.h` / `RendererGL.cpp` — SDL2 + OpenGL 3.3 Core backend (modern).

## Texture override registry
The renderer intercepts texture uploads to support high-res PNG/TGA/BMP/JPG overwrites of original 16-bit TGA assets:

- Registry keyed by CPU pointer of original RGB555 buffer → decoded RGBA8 + (w, h).
- Upload paths (`gl_UploadTexture16` → shared `gl_UploadRGBA` in renderd3d.cpp; `RendererGL::SetTexture` in RendererGL.cpp) check registry first.
- On hit: 32-bit override uploaded at any resolution with full 8-bit alpha and hardware mipmaps.
- On miss: existing 16-bit path runs unchanged.
- Foliage mip-fix preserved: transparent pixels filled with avg opaque color for levels 1..N, original at level 0.
- Loaders call `TextureOverrides::TryRegisterSibling(ptr, filePath)` or `TryRegisterWithExts(ptr, basePath)` immediately after loading 16-bit data (tries `.png`, `.tga`, `.bmp`, `.jpg` in order).

## Character rendering

**Dead character shadows** (`renderd3d.cpp` `RenderCharacterPost`):
- When a character dies (`Health==0`), shadows fade out during the death animation. Previously, an early return on the last animation frame prevented shadow rendering entirely, causing dead dinosaurs laying on the ground to cast no shadows.
- **Fix**: Removed the early return so shadows continue fading through the complete death animation cycle, including after the animation ends.

## Roadmap
- Formalize the renderer abstraction: move all `d3d*` functions behind the `Renderer` interface entirely, kill `renderd3d.cpp` glue so Vulkan/Metal/WebGPU backends become drop-in.
