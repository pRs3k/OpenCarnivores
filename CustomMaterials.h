#pragma once
// SOURCEPORT: data-driven custom-shader material system.
//
// Mods can attach a custom GLSL shader to any texture by dropping a
// `<stem>.material` file next to the asset (e.g. `HUNTDAT/TREX.material`).
// The file is line-based, whitespace-separated, with `#` comments:
//
//     # trex.material — shiny scales
//     shader  custom_dino
//     tex     uDiffuse       HUNTDAT/TREX_albedo.png
//     tex     uNormal        HUNTDAT/TREX_normal.png
//     float   uShininess     0.85
//     vec3    uTintColor     1.0 0.92 0.85
//     vec4    uRimLight      0.4 0.5 0.7 1.0
//
// Directives:
//   shader <name>               — loads shaders/<name>.vert + .frag at runtime
//   tex    <uniform> <path>     — stb_image → next free unit (1..N); unit 0 is
//                                 the engine's standard albedo binding
//   float  <uniform> <x>        — float uniform
//   vec2   <uniform> <x> <y>    — vec2 uniform
//   vec3   <uniform> <x> <y> <z>
//   vec4   <uniform> <x> <y> <z> <w>
//
// Vertex attribute locations are fixed and match `shaders/basic.vert`:
//   0 vec2 aPos   1 float aDepth   2 vec4 aColor   3 vec4 aSpecular   4 vec2 aTexCoord
// The engine also always sets `uProjection` (mat4) and binds the albedo to
// texture unit 0 as `uTexture`. Everything else is up to the modder.
//
// Strictly additive:
//   • No .material file  → existing Lambert / PBR paths run unchanged.
//   • .material present  → wins over the PBR auto-path for that texture key.
//
// Registry is keyed by the CPU pointer of the original RGB555 buffer, same
// as TextureOverrides/Materials.

#include <cstdint>

namespace CustomMaterials {

struct Material;  // opaque; defined in CustomMaterials.cpp

// Probe for `<stem>.material` next to sourcePath (e.g. "HUNTDAT\\TREX.CAR"
// → "HUNTDAT\\TREX.material"). Returns true if a file was loaded and the
// shader compiled+linked.
bool TryRegisterSibling(void* key, const char* sourcePath);

// Probe for `<basePath>.material`. basePath must NOT have an extension.
bool TryRegisterWithExts(void* key, const char* basePath);

// Returns the material registered for `key`, or nullptr.
const Material* Get(void* key);

// Returns the paths the registry is watching for this key, for HotReload.
// Null-terminated array in a static buffer (reused per call, not thread-safe).
const char* const* GetWatchPaths(const char* basePath);

// Re-parse the material file associated with `key` and rebuild the program +
// textures + uniform state. No-op if `key` has no registered material.
void Reload(void* key);

// Re-parse whichever material file on disk matches `basePath.material`. Used
// by HotReload callbacks that don't carry the key directly.
void ReloadByBasePath(const char* basePath);

// Switches GL to the material's program, binds its textures, pushes its
// uniforms. Pass nullptr to restore the engine's default program.
// projMatrix is the 4×4 column-major projection the caller is currently
// using — the custom program's `uProjection` is set to this.
// albedoUnit0 is the GL texture currently bound to unit 0; the material
// leaves it alone but may want to sample it from `uTexture`.
void Apply(const Material* m, const float* projMatrix);

// Restore the engine's default shader program + uniforms.
void Unapply();

// Free all GL programs and textures.
void Shutdown();

} // namespace CustomMaterials
