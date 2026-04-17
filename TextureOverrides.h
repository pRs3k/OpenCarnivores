#pragma once
// SOURCEPORT: 32-bit PNG/TGA/BMP/JPEG texture override registry.
// Keyed by the CPU pointer of the original RGB555 texture buffer — when the
// renderer is about to upload that buffer, it checks the registry first and
// uploads the 32-bit override instead. Purely additive: with no registered
// overrides, the game renders identically to the 16-bit-only path.

#include <cstdint>

namespace TextureOverrides {

// Load a PNG/TGA/BMP/JPEG file and register it as the replacement for the
// RGB555 buffer at `key`. The decoded RGBA is retained until Shutdown().
// Returns true on success.
bool RegisterFromFile(void* key, const char* imagePath);

// Returns decoded RGBA8 buffer + dimensions if an override exists for key,
// or nullptr. The buffer is owned by the registry — do not free.
// Memory layout matches gl_UploadTexture16 output: R in low byte, A in high.
const uint32_t* Get(void* key, int* outW, int* outH);

// Convenience: derive "<stem>.png" from a source file path (e.g.
// "HUNTDAT\\BINOCUL.3DF" → "HUNTDAT\\BINOCUL.png") and try to register it.
// Returns true if the sibling existed and was successfully loaded.
bool TryRegisterSibling(void* key, const char* sourcePath);

// Convenience: try basePath + {".png", ".tga", ".bmp", ".jpg"} in order and
// register the first one that exists. basePath must NOT already have an
// extension (e.g. "AREA1_tex_00", not "AREA1_tex_00.png").
bool TryRegisterWithExts(void* key, const char* basePath);

// Free all decoded buffers.
void Shutdown();

} // namespace TextureOverrides
