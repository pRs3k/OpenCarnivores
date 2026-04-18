// SOURCEPORT: 32-bit + BCn DDS texture override registry — see TextureOverrides.h.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#include "TextureOverrides.h"
#include "hunt.h"
#include "VFS.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

// GL format constants we may reference without pulling in glad here.
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT        0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT        0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT        0x83F3
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT  0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT  0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT  0x8C4F
#define GL_COMPRESSED_RED_RGTC1                 0x8DBB
#define GL_COMPRESSED_RG_RGTC2                  0x8DBD
#define GL_COMPRESSED_RGBA_BPTC_UNORM           0x8E8C
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM     0x8E8D
#endif

namespace {
struct Entry { uint32_t* rgba; int w; int h; };
std::unordered_map<void*, Entry> g_reg;
std::unordered_map<void*, TextureOverrides::CompressedTex> g_regDDS;

void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

// --- DDS header structs (POD, tightly packed on-disk layout) -----------------

#pragma pack(push, 1)
struct DDSPixelFormat {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};
struct DDSHeader {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDSPixelFormat ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};
struct DDSHeaderDXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

constexpr uint32_t kMagicDDS  = 0x20534444; // 'DDS '
constexpr uint32_t kFourCC_DXT1 = 0x31545844;
constexpr uint32_t kFourCC_DXT3 = 0x33545844;
constexpr uint32_t kFourCC_DXT5 = 0x35545844;
constexpr uint32_t kFourCC_ATI1 = 0x31495441; // BC4U
constexpr uint32_t kFourCC_BC4U = 0x55344342;
constexpr uint32_t kFourCC_ATI2 = 0x32495441; // BC5U
constexpr uint32_t kFourCC_BC5U = 0x55354342;
constexpr uint32_t kFourCC_DX10 = 0x30315844;

constexpr uint32_t kDDPF_FOURCC = 0x4;

// Map DDS/DX10 format → { GL format, block-bytes } or { 0, 0 } if unsupported.
struct FmtInfo { uint32_t gl; uint32_t blk; };

FmtInfo FourCCToGL(uint32_t cc) {
    switch (cc) {
        case kFourCC_DXT1: return { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8  };
        case kFourCC_DXT3: return { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 16 };
        case kFourCC_DXT5: return { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16 };
        case kFourCC_ATI1:
        case kFourCC_BC4U: return { GL_COMPRESSED_RED_RGTC1,          8  };
        case kFourCC_ATI2:
        case kFourCC_BC5U: return { GL_COMPRESSED_RG_RGTC2,           16 };
        default:           return { 0, 0 };
    }
}

FmtInfo DXGIToGL(uint32_t fmt) {
    // DXGI_FORMAT_* constants — subset covering the common BCn formats.
    switch (fmt) {
        case 71: return { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,        8  }; // BC1_UNORM
        case 72: return { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT,  8  }; // BC1_UNORM_SRGB
        case 74: return { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,        16 }; // BC2_UNORM
        case 75: return { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT,  16 }; // BC2_UNORM_SRGB
        case 77: return { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,        16 }; // BC3_UNORM
        case 78: return { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,  16 }; // BC3_UNORM_SRGB
        case 80: return { GL_COMPRESSED_RED_RGTC1,                 8  }; // BC4_UNORM
        case 83: return { GL_COMPRESSED_RG_RGTC2,                  16 }; // BC5_UNORM
        case 98: return { GL_COMPRESSED_RGBA_BPTC_UNORM,           16 }; // BC7_UNORM
        case 99: return { GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,     16 }; // BC7_UNORM_SRGB
        default: return { 0, 0 };
    }
}

size_t LevelSize(int w, int h, uint32_t blk) {
    int bw = (w + 3) / 4; if (bw < 1) bw = 1;
    int bh = (h + 3) / 4; if (bh < 1) bh = 1;
    return (size_t)bw * (size_t)bh * (size_t)blk;
}

} // anon

namespace TextureOverrides {

// ============================================================================
// Uncompressed RGBA path
// ============================================================================

bool RegisterFromFile(void* key, const char* imagePath)
{
    if (!key || !imagePath) return false;

    int w = 0, h = 0, comp = 0;
    // SOURCEPORT: route through VFS so mod folders override retail sibling files.
    std::string resolved = VFS::ResolveRead(imagePath);
    stbi_uc* data = stbi_load(resolved.c_str(), &w, &h, &comp, 4);
    if (!data) return false;

    uint32_t* buf = new uint32_t[w * h];
    std::memcpy(buf, data, (size_t)w * h * 4);
    stbi_image_free(data);

    auto it = g_reg.find(key);
    if (it != g_reg.end()) { delete[] it->second.rgba; }
    g_reg[key] = { buf, w, h };

    char msg[512];
    std::snprintf(msg, sizeof(msg), "Texture override loaded: %s (%dx%d)\n", imagePath, w, h);
    Log(msg);
    return true;
}

const uint32_t* Get(void* key, int* outW, int* outH)
{
    auto it = g_reg.find(key);
    if (it == g_reg.end()) return nullptr;
    if (outW) *outW = it->second.w;
    if (outH) *outH = it->second.h;
    return it->second.rgba;
}

// ============================================================================
// BCn DDS path
// ============================================================================

bool RegisterDDS(void* key, const char* ddsPath)
{
    if (!key || !ddsPath) return false;

    FILE* f = VFS::fopen(ddsPath, "rb");
    if (!f) return false;

    // Magic + header
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || magic != kMagicDDS) {
        std::fclose(f); return false;
    }
    DDSHeader hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.dwSize != sizeof(hdr)) {
        std::fclose(f); return false;
    }

    // Resolve compressed format
    FmtInfo fi = { 0, 0 };
    bool isDX10 = (hdr.ddspf.dwFlags & kDDPF_FOURCC) && hdr.ddspf.dwFourCC == kFourCC_DX10;
    if (isDX10) {
        DDSHeaderDXT10 ext;
        if (std::fread(&ext, sizeof(ext), 1, f) != 1) { std::fclose(f); return false; }
        fi = DXGIToGL(ext.dxgiFormat);
    } else if (hdr.ddspf.dwFlags & kDDPF_FOURCC) {
        fi = FourCCToGL(hdr.ddspf.dwFourCC);
    }
    if (fi.gl == 0) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "DDS override skipped (unsupported format): %s\n", ddsPath);
        Log(msg);
        std::fclose(f); return false;
    }

    // Compute mip chain. DDS is permitted to omit mips; clamp to what the file
    // actually stores by trusting dwMipMapCount when the caps flag is set,
    // otherwise treat as single level.
    int w = (int)hdr.dwWidth, h = (int)hdr.dwHeight;
    int declared = (hdr.dwMipMapCount > 0) ? (int)hdr.dwMipMapCount : 1;
    if (declared > 16) declared = 16; // OpenGL 3.3 textures cap well below 2^16.

    CompressedTex ct{};
    ct.w = w; ct.h = h; ct.glFormat = fi.gl; ct.blockBytes = fi.blk;

    size_t total = 0;
    int levels = 0;
    int lw = w, lh = h;
    for (int i = 0; i < declared; ++i) {
        size_t sz = LevelSize(lw, lh, fi.blk);
        ct.mipOffsets[i] = total;
        ct.mipSizes[i]   = sz;
        total += sz;
        ++levels;
        if (lw == 1 && lh == 1) break;
        lw = (lw > 1) ? lw >> 1 : 1;
        lh = (lh > 1) ? lh >> 1 : 1;
    }
    ct.mipCount = levels;
    ct.dataSize = total;

    // Read remaining bytes. If the file is shorter than expected, bail.
    ct.data = (uint8_t*)std::malloc(total);
    if (!ct.data) { std::fclose(f); return false; }
    size_t got = std::fread(ct.data, 1, total, f);
    std::fclose(f);
    if (got != total) {
        std::free(ct.data);
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "DDS override truncated (%zu/%zu bytes): %s\n", got, total, ddsPath);
        Log(msg);
        return false;
    }

    auto it = g_regDDS.find(key);
    if (it != g_regDDS.end()) { std::free(it->second.data); g_regDDS.erase(it); }
    g_regDDS[key] = ct;

    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "DDS override loaded: %s (%dx%d, %d mips, fmt=0x%04X, %zu KiB)\n",
        ddsPath, w, h, levels, fi.gl, total / 1024);
    Log(msg);
    return true;
}

const CompressedTex* GetCompressed(void* key)
{
    auto it = g_regDDS.find(key);
    if (it == g_regDDS.end()) return nullptr;
    return &it->second;
}

// ============================================================================
// Extension-probing convenience
// ============================================================================

bool TryRegisterSibling(void* key, const char* sourcePath)
{
    if (!sourcePath) return false;
    std::string p = sourcePath;
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    return TryRegisterWithExts(key, p.substr(0, dot).c_str());
}

bool TryRegisterWithExts(void* key, const char* basePath)
{
    if (!basePath) return false;
    // .dds first — BCn is ~4–6× smaller in VRAM than uncompressed RGBA.
    {
        std::string candidate = std::string(basePath) + ".dds";
        if (RegisterDDS(key, candidate.c_str())) return true;
    }
    const char* exts[] = { ".png", ".tga", ".bmp", ".jpg" };
    for (const char* ext : exts) {
        std::string candidate = std::string(basePath) + ext;
        if (RegisterFromFile(key, candidate.c_str())) return true;
    }
    return false;
}

bool TryRegisterMenuPicture(void* key, const char* basePath)
{
    // SOURCEPORT: menu-picture variant — `.tga` intentionally omitted because
    // LoadPictureTGA's own source is a .tga, and probing it here would cause
    // the retail asset to be "loaded as its own override" (wasted RGBA decode).
    if (!basePath) return false;
    {
        std::string candidate = std::string(basePath) + ".dds";
        if (RegisterDDS(key, candidate.c_str())) return true;
    }
    const char* exts[] = { ".png", ".bmp", ".jpg" };
    for (const char* ext : exts) {
        std::string candidate = std::string(basePath) + ext;
        if (RegisterFromFile(key, candidate.c_str())) return true;
    }
    return false;
}

bool Has(void* key)
{
    return g_reg.find(key) != g_reg.end() || g_regDDS.find(key) != g_regDDS.end();
}

void Unregister(void* key)
{
    auto it = g_reg.find(key);
    if (it != g_reg.end()) { delete[] it->second.rgba; g_reg.erase(it); }
    auto dd = g_regDDS.find(key);
    if (dd != g_regDDS.end()) { std::free(dd->second.data); g_regDDS.erase(dd); }
}

void Shutdown()
{
    for (auto& kv : g_reg)    delete[] kv.second.rgba;
    for (auto& kv : g_regDDS) std::free(kv.second.data);
    g_reg.clear();
    g_regDDS.clear();
}

} // namespace
