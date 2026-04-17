#pragma once
// SOURCEPORT: PBR material override registry.
//
// Keyed by the CPU pointer of the original RGB555 texture buffer (same key
// space as TextureOverrides). When a base-color texture has sibling PBR
// maps (`_normal.png`, `_mr.png`, `_ao.png`), they are loaded and registered
// here; the renderer then enables a Cook-Torrance GGX path when binding
// that texture. Purely additive — assets without PBR siblings render through
// the original Lambert+vertex-light path unchanged.
//
// Sibling naming (stb_image formats: PNG/TGA/BMP/JPEG):
//   <stem>_normal.png   — tangent-space normal map (RGB, +Y up / OpenGL style)
//   <stem>_mr.png       — metallic (R) + roughness (G), glTF convention
//   <stem>_ao.png       — ambient occlusion (R channel)
//
// Tangent basis is reconstructed in the fragment shader from screen-space
// derivatives of position and UV (Christian Schüler technique), so the
// original .CAR/.3DF vertex format needs no tangent attribute.

#include <cstdint>

namespace Materials {

struct Material {
    uint32_t normalTex;   // GL texture ID; 0 if absent
    uint32_t mrTex;       // metallic (R) + roughness (G); 0 if absent
    uint32_t aoTex;       // 0 if absent
    float    metallicFactor;
    float    roughnessFactor;
};

// Probe for PBR siblings next to sourcePath (e.g. "HUNTDAT\\TREX.CAR" →
// "HUNTDAT\\TREX_normal.png"). Registers whatever subset of maps exists.
// Returns true if at least one map was loaded.
bool TryRegisterSibling(void* key, const char* sourcePath);

// Probe for basePath + "_normal.png" / "_mr.png" / "_ao.png". basePath must
// NOT already have an extension.
bool TryRegisterWithExts(void* key, const char* basePath);

// Returns the material for `key`, or nullptr if no PBR maps were registered.
const Material* Get(void* key);

// Free all GL textures. Safe to call with no registered materials.
void Shutdown();

} // namespace Materials
