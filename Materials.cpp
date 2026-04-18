// SOURCEPORT: PBR material override registry. See Materials.h for scope.

#include "Materials.h"

#include <glad/gl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "stb_image.h"  // STB_IMAGE_IMPLEMENTATION lives in TextureOverrides.cpp
#include "VFS.h"

namespace {

struct Entry {
    Materials::Material mat;
};

std::unordered_map<uintptr_t, Entry> g_registry;

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

GLuint LoadMapFile(const char* path, bool srgb) {
    // SOURCEPORT: resolve through VFS so mod folder PBR maps take priority.
    std::string resolved = VFS::ResolveRead(path);
    if (!FileExists(resolved.c_str())) return 0;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(resolved.c_str(), &w, &h, &comp, 4);
    if (!px) return 0;
    GLuint id = UploadRGBA(px, w, h, srgb);
    stbi_image_free(px);
    char msg[512];
    std::snprintf(msg, sizeof(msg), "PBR map loaded: %s (%dx%d)\n", path, w, h);
    std::fputs(msg, stdout);
    return id;
}

bool RegisterFromStem(void* key, const std::string& stem) {
    std::string nrm = stem + "_normal.png";
    std::string mrp = stem + "_mr.png";
    std::string aop = stem + "_ao.png";

    GLuint n  = LoadMapFile(nrm.c_str(), /*srgb=*/false);
    GLuint mr = LoadMapFile(mrp.c_str(), /*srgb=*/false);
    GLuint ao = LoadMapFile(aop.c_str(), /*srgb=*/false);

    if (!n && !mr && !ao) return false;

    Entry& e = g_registry[(uintptr_t)key];
    // Replace any prior entry (GL ids leak — acceptable at asset-load scope).
    e.mat.normalTex       = n;
    e.mat.mrTex           = mr;
    e.mat.aoTex           = ao;
    e.mat.metallicFactor  = mr ? 1.0f : 0.0f;
    e.mat.roughnessFactor = mr ? 1.0f : 1.0f;
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
