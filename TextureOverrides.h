#pragma once
// SOURCEPORT: 32-bit PNG/TGA/BMP/JPEG + BCn DDS texture override registry.
// Keyed by the CPU pointer of the original RGB555 texture buffer — when the
// renderer is about to upload that buffer, it checks the registry first and
// uploads the override instead. Purely additive: with no registered overrides,
// the game renders identically to the 16-bit-only path.

#include <cstdint>
#include <cstddef>

namespace TextureOverrides {

// --- 8-bit-per-channel uncompressed path (PNG/TGA/BMP/JPEG via stb_image) ---

// Load a PNG/TGA/BMP/JPEG file and register it as the replacement for the
// RGB555 buffer at `key`. The decoded RGBA is retained until Shutdown().
// Returns true on success.
bool RegisterFromFile(void* key, const char* imagePath);

// Returns decoded RGBA8 buffer + dimensions if an override exists for key,
// or nullptr. The buffer is owned by the registry — do not free.
// Memory layout matches gl_UploadTexture16 output: R in low byte, A in high.
const uint32_t* Get(void* key, int* outW, int* outH);

// --- BCn compressed path (.dds — BC1/BC2/BC3/BC4/BC5/BC7, DX10 header aware) ---

// A parsed DDS with its full mip chain ready for glCompressedTexImage2D.
// `data` owns the concatenated mip levels in top-down order (level 0 first).
// `mipOffsets[i]` / `mipSizes[i]` locate level i within `data`; `mipCount`
// is clamped to the natural count for (w,h) so an under-specified DDS still
// uploads the levels it provides.
struct CompressedTex {
    uint8_t* data;
    size_t   dataSize;
    int      w, h;
    int      mipCount;
    uint32_t glFormat;           // GL_COMPRESSED_* — see TextureOverrides.cpp
    uint32_t blockBytes;         // 8 (BC1/BC4) or 16 (BC2/BC3/BC5/BC7)
    size_t   mipOffsets[16];
    size_t   mipSizes[16];
};

// Parse `ddsPath` as a DDS file and register its compressed mip chain under
// `key`. Returns false if the file is missing, malformed, or uses a format
// this loader does not support (see format table in TextureOverrides.cpp).
bool RegisterDDS(void* key, const char* ddsPath);

// Returns the parsed compressed mip chain for `key`, or nullptr. Owned by
// the registry.
const CompressedTex* GetCompressed(void* key);

// --- Convenience extension-probing loaders ---

// Convenience: derive "<stem>.png" (etc.) from a source file path (e.g.
// "HUNTDAT\\BINOCUL.3DF" → "HUNTDAT\\BINOCUL.<ext>") and try to register it.
// Returns true if any of the candidate siblings existed and loaded.
bool TryRegisterSibling(void* key, const char* sourcePath);

// Convenience: try basePath + {".dds", ".png", ".tga", ".bmp", ".jpg"} in
// order and register the first one that exists. .dds is preferred because
// BCn textures use ~4–6× less VRAM than uncompressed RGBA at 4K. basePath
// must NOT already have an extension (e.g. "AREA1_tex_00", not
// "AREA1_tex_00.png").
bool TryRegisterWithExts(void* key, const char* basePath);

// Free all decoded buffers.
void Shutdown();

} // namespace TextureOverrides
