// SOURCEPORT: custom-shader material system. See CustomMaterials.h.
//
// Design notes:
//  • Minimal directive-per-line parser — JSON was overkill and adds a
//    dependency; modders only need a handful of key/value pairs.
//  • Programs are cached by shader-name so two materials that reference
//    "custom_dino" share the same compiled GL program.
//  • Custom-program uniforms are re-applied on every bind (Apply) because
//    the default engine program lives in the same GL context and will have
//    clobbered the last state.
//  • Reload rebuilds the Material in place; the key stays stable so the
//    renderer's hMaterialCustom pointer doesn't dangle across reloads.

#include "CustomMaterials.h"

#include <glad/gl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "stb_image.h"   // STB_IMAGE_IMPLEMENTATION lives in TextureOverrides.cpp
#include "VFS.h"

// Forward decl — RendererGL's restore routine. After Apply() we switch GL
// away from the default program; Unapply() asks the renderer to re-bind its
// own program and push its uniforms back.
extern class RendererGL* g_glRenderer;

namespace {

// ─── Data ─────────────────────────────────────────────────────────────────────

struct UniformVal {
    std::string name;
    int         kind;   // 1=float, 2=vec2, 3=vec3, 4=vec4
    float       v[4];
    GLint       loc;    // cached per-program
};

struct TextureBinding {
    std::string name;     // uniform sampler name
    std::string path;     // for hot-reload diff + logging
    GLuint      texId;    // GL texture object
    GLint       loc;      // uniform location in the program
    int         unit;     // sampler unit (1..N; unit 0 is the engine albedo)
};

struct Program {
    GLuint id             = 0;
    GLint  locProjection  = -1;
    GLint  locTextureUnit0 = -1;   // "uTexture" — the engine's albedo binding
};

} // anon

namespace CustomMaterials {

struct Material {
    std::string              shaderName;
    std::string              basePath;   // .material file path, for reload
    Program                  prog;
    std::vector<TextureBinding> textures;
    std::vector<UniformVal>     uniforms;
};

} // namespace CustomMaterials

namespace {

std::unordered_map<uintptr_t, CustomMaterials::Material*> g_reg;
std::unordered_map<std::string, Program>                  g_progCache;   // keyed by shaderName

// ─── Helpers ──────────────────────────────────────────────────────────────────

void Log(const char* s) { std::fputs(s, stdout); }

std::string SlurpFile(const char* path) {
    // SOURCEPORT: route shader/material reads through VFS for mod overrides.
    std::string resolved = VFS::ResolveRead(path);
    std::ifstream f(resolved, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

GLuint CompileStage(GLenum type, const char* src, const char* label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        char msg[2560];
        std::snprintf(msg, sizeof(msg), "[CustomMaterials] %s compile failed:\n%s\n", label, log);
        Log(msg);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Build (or fetch from cache) a linked GL program for `shaderName`.
// Looks for shaders/<name>.vert and shaders/<name>.frag on disk.
Program BuildProgram(const std::string& shaderName) {
    auto it = g_progCache.find(shaderName);
    if (it != g_progCache.end() && it->second.id) return it->second;

    std::string vsPath = "shaders/" + shaderName + ".vert";
    std::string fsPath = "shaders/" + shaderName + ".frag";
    std::string vs = SlurpFile(vsPath.c_str());
    std::string fs = SlurpFile(fsPath.c_str());
    if (vs.empty() || fs.empty()) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "[CustomMaterials] shader files not found: %s / %s\n",
            vsPath.c_str(), fsPath.c_str());
        Log(msg);
        return {};
    }

    GLuint v = CompileStage(GL_VERTEX_SHADER,   vs.c_str(), vsPath.c_str());
    GLuint f = CompileStage(GL_FRAGMENT_SHADER, fs.c_str(), fsPath.c_str());
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return {};
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        char msg[2560];
        std::snprintf(msg, sizeof(msg), "[CustomMaterials] link failed for %s:\n%s\n",
                      shaderName.c_str(), log);
        Log(msg);
        glDeleteProgram(prog);
        return {};
    }

    Program p;
    p.id              = prog;
    p.locProjection   = glGetUniformLocation(prog, "uProjection");
    p.locTextureUnit0 = glGetUniformLocation(prog, "uTexture");

    g_progCache[shaderName] = p;

    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "[CustomMaterials] program '%s' linked (id=%u)\n", shaderName.c_str(), p.id);
    Log(msg);
    return p;
}

GLuint LoadTextureFile(const char* path) {
    std::string resolved = VFS::ResolveRead(path);
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(resolved.c_str(), &w, &h, &comp, 4);
    if (!px) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "[CustomMaterials] texture load failed: %s\n", path);
        Log(msg);
        return 0;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    // Assume sRGB for colour data; modders who want linear data can extend the
    // directive to `tex_linear` in the future.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(px);
    return t;
}

// Build the fully-qualified .material path from an asset sourcePath. Any
// extension on sourcePath is stripped; the suffix ".material" is appended.
std::string MaterialPathForSource(const char* sourcePath) {
    std::string p = sourcePath ? sourcePath : "";
    size_t dot = p.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? p : p.substr(0, dot);
    return stem + ".material";
}
std::string MaterialPathForBase(const char* basePath) {
    return std::string(basePath ? basePath : "") + ".material";
}

// Parse one .material file into `m`. Returns false on I/O error.
bool ParseFile(const std::string& path, CustomMaterials::Material& m) {
    std::ifstream in(VFS::ResolveRead(path.c_str()));
    if (!in) return false;

    m.shaderName.clear();
    m.textures.clear();
    m.uniforms.clear();

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // Strip comments
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ts(line);
        std::string tok;
        if (!(ts >> tok)) continue;

        if (tok == "shader") {
            ts >> m.shaderName;
        } else if (tok == "tex") {
            TextureBinding tb{};
            ts >> tb.name >> tb.path;
            if (tb.name.empty() || tb.path.empty()) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "[CustomMaterials] %s:%d — 'tex' needs <uniform> <path>\n",
                    path.c_str(), lineNo);
                Log(msg);
                continue;
            }
            m.textures.push_back(std::move(tb));
        } else if (tok == "float" || tok == "vec2" || tok == "vec3" || tok == "vec4") {
            UniformVal u{};
            u.kind = (tok == "float") ? 1 : (tok == "vec2") ? 2 : (tok == "vec3") ? 3 : 4;
            ts >> u.name;
            for (int i = 0; i < u.kind; ++i) ts >> u.v[i];
            if (ts.fail() || u.name.empty()) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "[CustomMaterials] %s:%d — bad '%s' directive\n",
                    path.c_str(), lineNo, tok.c_str());
                Log(msg);
                continue;
            }
            m.uniforms.push_back(std::move(u));
        } else {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "[CustomMaterials] %s:%d — unknown directive '%s'\n",
                path.c_str(), lineNo, tok.c_str());
            Log(msg);
        }
    }
    return !m.shaderName.empty();
}

// Build GL resources for a parsed material: look up program, load textures,
// cache uniform locations. Previously-owned textures are destroyed.
void Realize(CustomMaterials::Material& m) {
    // Destroy old textures (program is cached, don't delete).
    for (auto& t : m.textures) {
        if (t.texId) glDeleteTextures(1, &t.texId);
        t.texId = 0;
    }

    m.prog = BuildProgram(m.shaderName);
    if (!m.prog.id) return;

    // Resolve texture uniforms and load images.
    int unit = 1;   // unit 0 reserved for engine albedo
    for (auto& t : m.textures) {
        t.loc  = glGetUniformLocation(m.prog.id, t.name.c_str());
        t.unit = unit++;
        t.texId = LoadTextureFile(t.path.c_str());
    }
    // Resolve scalar/vector uniforms.
    for (auto& u : m.uniforms) {
        u.loc = glGetUniformLocation(m.prog.id, u.name.c_str());
    }
}

} // anon

// ─── Public API ───────────────────────────────────────────────────────────────

namespace CustomMaterials {

bool TryRegisterSibling(void* key, const char* sourcePath) {
    if (!key || !sourcePath) return false;
    std::string mpath = MaterialPathForSource(sourcePath);
    std::ifstream probe(VFS::ResolveRead(mpath.c_str()));
    if (!probe) return false;
    probe.close();

    auto& slot = g_reg[(uintptr_t)key];
    if (!slot) slot = new Material();
    slot->basePath = mpath;
    if (!ParseFile(mpath, *slot)) {
        delete slot;
        g_reg.erase((uintptr_t)key);
        return false;
    }
    Realize(*slot);
    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "[CustomMaterials] registered %s (shader=%s, %zu tex, %zu uniforms)\n",
        mpath.c_str(), slot->shaderName.c_str(),
        slot->textures.size(), slot->uniforms.size());
    Log(msg);
    return slot->prog.id != 0;
}

bool TryRegisterWithExts(void* key, const char* basePath) {
    if (!key || !basePath) return false;
    std::string mpath = MaterialPathForBase(basePath);
    std::ifstream probe(VFS::ResolveRead(mpath.c_str()));
    if (!probe) return false;
    probe.close();

    auto& slot = g_reg[(uintptr_t)key];
    if (!slot) slot = new Material();
    slot->basePath = mpath;
    if (!ParseFile(mpath, *slot)) {
        delete slot;
        g_reg.erase((uintptr_t)key);
        return false;
    }
    Realize(*slot);
    return slot->prog.id != 0;
}

const Material* Get(void* key) {
    auto it = g_reg.find((uintptr_t)key);
    return it == g_reg.end() ? nullptr : it->second;
}

const char* const* GetWatchPaths(const char* basePath) {
    static std::string p0, p1, p2, p3;
    static const char* arr[5];
    p0 = MaterialPathForBase(basePath);
    // Could also watch shader files — but those are global and hot-reloaded
    // via the main RendererGL shader-watch path when they happen to be named
    // "basic.{vert,frag}". Custom shader reload is triggered by editing the
    // .material file itself (re-Realize calls BuildProgram which re-uses the
    // cached program; for full shader hot-reload, delete shaders/<name>.vert's
    // cache entry — left as a small TODO).
    arr[0] = p0.c_str();
    arr[1] = nullptr;
    return arr;
}

void Reload(void* key) {
    auto it = g_reg.find((uintptr_t)key);
    if (it == g_reg.end()) return;
    Material& m = *it->second;
    // Drop the shader-program cache entry for this shader name so BuildProgram
    // re-reads the .vert/.frag from disk — lets modders edit shaders AND the
    // .material together and see both pick up on save.
    auto pit = g_progCache.find(m.shaderName);
    if (pit != g_progCache.end()) {
        if (pit->second.id) glDeleteProgram(pit->second.id);
        g_progCache.erase(pit);
    }
    if (!ParseFile(m.basePath, m)) return;
    Realize(m);
    Log("[CustomMaterials] reloaded\n");
}

void ReloadByBasePath(const char* basePath) {
    std::string target = MaterialPathForBase(basePath);
    for (auto& kv : g_reg) {
        if (kv.second->basePath == target) { Reload((void*)kv.first); return; }
    }
}

void Apply(const Material* m, const float* projMatrix) {
    if (!m || !m->prog.id) return;
    glUseProgram(m->prog.id);
    if (m->prog.locProjection >= 0 && projMatrix)
        glUniformMatrix4fv(m->prog.locProjection, 1, GL_FALSE, projMatrix);
    if (m->prog.locTextureUnit0 >= 0)
        glUniform1i(m->prog.locTextureUnit0, 0);

    for (const auto& t : m->textures) {
        if (t.loc < 0 || !t.texId) continue;
        glActiveTexture(GL_TEXTURE0 + t.unit);
        glBindTexture(GL_TEXTURE_2D, t.texId);
        glUniform1i(t.loc, t.unit);
    }
    glActiveTexture(GL_TEXTURE0);

    for (const auto& u : m->uniforms) {
        if (u.loc < 0) continue;
        switch (u.kind) {
            case 1: glUniform1f (u.loc, u.v[0]); break;
            case 2: glUniform2f (u.loc, u.v[0], u.v[1]); break;
            case 3: glUniform3f (u.loc, u.v[0], u.v[1], u.v[2]); break;
            case 4: glUniform4f (u.loc, u.v[0], u.v[1], u.v[2], u.v[3]); break;
        }
    }
}

void Unapply() {
    // Restoration is handled by RendererGL::RestoreDefaultProgram — see
    // BindCustomMaterial(nullptr) in RendererGL.cpp.
    (void)g_glRenderer;
}

void Shutdown() {
    for (auto& kv : g_reg) {
        for (auto& t : kv.second->textures)
            if (t.texId) glDeleteTextures(1, &t.texId);
        delete kv.second;
    }
    g_reg.clear();
    for (auto& kv : g_progCache) {
        if (kv.second.id) glDeleteProgram(kv.second.id);
    }
    g_progCache.clear();
}

} // namespace CustomMaterials
