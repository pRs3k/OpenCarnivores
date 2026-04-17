// SOURCEPORT: 32-bit texture override registry — see TextureOverrides.h.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#include "TextureOverrides.h"
#include "hunt.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {
struct Entry { uint32_t* rgba; int w; int h; };
std::unordered_map<void*, Entry> g_reg;

void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }
} // anon

namespace TextureOverrides {

bool RegisterFromFile(void* key, const char* imagePath)
{
    if (!key || !imagePath) return false;

    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load(imagePath, &w, &h, &comp, 4);
    if (!data) return false;

    // stb with req_comp=4 returns R0 G0 B0 A0 R1 G1 B1 A1… — as little-endian
    // uint32_t that is 0xAABBGGRR, identical to gl_UploadTexture16's layout.
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
    const char* exts[] = { ".png", ".tga", ".bmp", ".jpg" };
    for (const char* ext : exts) {
        std::string candidate = std::string(basePath) + ext;
        if (RegisterFromFile(key, candidate.c_str())) return true;
    }
    return false;
}

void Shutdown()
{
    for (auto& kv : g_reg) delete[] kv.second.rgba;
    g_reg.clear();
}

} // namespace
