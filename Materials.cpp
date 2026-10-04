// SOURCEPORT: PBR material override registry. See Materials.h for scope.

#include "Materials.h"

#include <glad/gl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "stb_image.h"  // STB_IMAGE_IMPLEMENTATION lives in TextureOverrides.cpp
#include "VFS.h"

namespace {

// SOURCEPORT: decoded map awaiting GL upload. Assets can load before the GL
// context exists (direct `prj=` launch loads the area before
// Activate3DHardware), when glGenTextures is still a null glad pointer.
struct PendingMap {
    std::vector<unsigned char> rgba;
    int  w = 0, h = 0;
    bool srgb = false;
};

struct Entry {
    Materials::Material mat;
    PendingMap pending[3];   // normal, mr, ao — uploaded on first Get()
};

std::unordered_map<uintptr_t, Entry> g_registry; // NOLINT(bugprone-throwing-static-initialization)

bool GLReady() { return glad_glGenTextures != nullptr; }

GLuint UploadRGBA(const unsigned char* rgba, int w, int h, bool srgb) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum internalFmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

bool FileExists(const char* p) {
    FILE* f = VFS::fopen(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// SOURCEPORT: detect whether a normal map's alpha channel encodes height data
// by checking whether any alpha value differs from 255 (fully opaque).
// Standard normal maps saved without height data have uniform alpha=255.
static bool DetectHeightInAlpha(const unsigned char* px, int w, int h) {
    int total = w * h;
    for (int i = 0; i < total; ++i) {
        if (px[i * 4 + 3] != 255) return true;
    }
    return false;
}

// Returns true if the map exists. Uploads now when GL is ready (*outTex), else
// stashes the pixels in *pending for Materials::Get() to upload later.
bool LoadMapFile(const char* path, bool srgb, GLuint* outTex, PendingMap* pending,
                 bool* outHasHeight = nullptr) {
    *outTex = 0;
    // SOURCEPORT: resolve through VFS so mod folder PBR maps take priority.
    std::string resolved = VFS::ResolveRead(path);
    if (!FileExists(resolved.c_str())) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(resolved.c_str(), &w, &h, &comp, 4);
    if (!px) return false;
    if (outHasHeight) *outHasHeight = DetectHeightInAlpha(px, w, h);
    if (GLReady()) {
        *outTex = UploadRGBA(px, w, h, srgb);
    } else {
        pending->rgba.assign(px, px + (size_t)w * h * 4);
        pending->w = w; pending->h = h; pending->srgb = srgb;
    }
    stbi_image_free(px);
    char msg[512];
    std::snprintf(msg, sizeof(msg), "PBR map loaded: %s (%dx%d)\n", path, w, h);
    std::fputs(msg, stdout);
    return true;
}

void FlushPending(Entry& e) {
    uint32_t* ids[3] = { &e.mat.normalTex, &e.mat.mrTex, &e.mat.aoTex };
    for (int i = 0; i < 3; ++i) {
        PendingMap& pm = e.pending[i];
        if (pm.rgba.empty()) continue;
        *ids[i] = UploadRGBA(pm.rgba.data(), pm.w, pm.h, pm.srgb);
        std::vector<unsigned char>().swap(pm.rgba);
    }
}

bool RegisterFromStem(void* key, const std::string& stem) {
    std::string nrm = stem + "_normal.png";
    std::string mrp = stem + "_mr.png";
    std::string aop = stem + "_ao.png";

    // SOURCEPORT: detect height-in-alpha while loading normal map.
    bool hasHeight = false;
    GLuint nId = 0, mrId = 0, aoId = 0;
    PendingMap pend[3];
    bool n  = LoadMapFile(nrm.c_str(), /*srgb=*/false, &nId,  &pend[0], &hasHeight);
    bool mr = LoadMapFile(mrp.c_str(), /*srgb=*/false, &mrId, &pend[1]);
    bool ao = LoadMapFile(aop.c_str(), /*srgb=*/false, &aoId, &pend[2]);

    if (!n && !mr && !ao) return false;

    Entry& e = g_registry[(uintptr_t)key];
    // Replace any prior entry (GL ids leak — acceptable at asset-load scope).
    e.mat.normalTex       = nId;
    e.mat.mrTex           = mrId;
    e.mat.aoTex           = aoId;
    for (int i = 0; i < 3; ++i) e.pending[i] = std::move(pend[i]);
    e.mat.metallicFactor  = mr ? 1.0f : 0.0f;
    e.mat.roughnessFactor = 1.0f; // SOURCEPORT: removed identical ternary branches — roughness is always 1.0 regardless of mr
    // SOURCEPORT: enable parallax only when height data detected in normal map alpha.
    // 0.05 GU gives subtle depth on rock/terrain without swimming artefacts at oblique angles.
    e.mat.parallaxScale   = (n && hasHeight) ? 0.05f : 0.0f;
    if (n && hasHeight)
        std::fputs("PBR parallax enabled (height detected in normal map alpha)\n", stdout);
    return true;
}

} // anon

namespace Materials {

bool TryRegisterSibling(void* key, const char* sourcePath) {
    if (!key || !sourcePath) return false;
    std::string p = sourcePath;
    size_t dot = p.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? p : p.substr(0, dot);
    return RegisterFromStem(key, stem);
}

bool TryRegisterWithExts(void* key, const char* basePath) {
    if (!key || !basePath) return false;
    return RegisterFromStem(key, std::string(basePath));
}

const Material* Get(void* key) {
    auto it = g_registry.find((uintptr_t)key);
    if (it == g_registry.end()) return nullptr;
    FlushPending(it->second);   // SOURCEPORT: deferred upload (no-op once uploaded)
    return &it->second.mat;
}

void Shutdown() {
    for (auto& kv : g_registry) {
        if (kv.second.mat.normalTex) glDeleteTextures(1, &kv.second.mat.normalTex);
        if (kv.second.mat.mrTex)     glDeleteTextures(1, &kv.second.mat.mrTex);
        if (kv.second.mat.aoTex)     glDeleteTextures(1, &kv.second.mat.aoTex);
    }
    g_registry.clear();
}

} // namespace Materials
