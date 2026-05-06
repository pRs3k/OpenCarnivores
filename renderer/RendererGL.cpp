// SOURCEPORT: OpenGL 3.3 Core renderer backend for Carnivores 2.
// This replaces the Direct3D 6 execute buffer rendering model with modern OpenGL.
// The game engine does all vertex transformation on the CPU, so we receive
// pre-transformed screen-space vertices (like D3DTLVERTEX) and just need to
// rasterize them with the correct texture, fog, and blending.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max
#include "RendererGL.h"
#include "../Materials.h"
#include "../CustomMaterials.h"
#include "../HotReload.h"
#include "../VFS.h"
#include <cstring>
#include <cstdio>
#include <string>

// Global renderer instance
IRenderer* g_Renderer = nullptr;

// Game globals — updated by SetVideoMode whenever window is resized
extern int WinW, WinH;

// ============================================================
// Shader source — handles pre-transformed 2D vertices
// ============================================================

static const char* vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;       // screen-space x, y
layout(location = 1) in float aDepth;    // z (depth)
layout(location = 2) in vec4 aColor;     // diffuse RGBA
layout(location = 3) in vec4 aSpecular;  // specular RGBA (A = fog)
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;

noperspective out vec4  vColor;
noperspective out float vFog;
// SOURCEPORT: two UV paths.
// vTexCoord (smooth) is used only for screen-space LOD derivatives.
// vTexCoordR / vRhw gives perspective-correct UV via explicit fragment-shader
// division, bypassing NVIDIA's hardware rasterizer storage for smooth varyings
// which may use reduced precision and produce a systematic UV bias at certain
// depth values (manifesting as a headlamp brightness ring on NVIDIA).
smooth        out vec2  vTexCoord;
noperspective out vec2  vTexCoordR;  // aTexCoord * rhw  (rhw = aDepth/16)
noperspective out float vRhw;        // rhw = aDepth/16

void main() {
    vec4 pos_ndc = uProjection * vec4(aPos, aDepth, 1.0);

    // SOURCEPORT: perspective-correct attribute interpolation.
    // D3D6 TL vertices carry rhw = sz/16 (= 1/camera_z) which the rasterizer uses
    // for perspective-correct UV/color interpolation.  With w_clip=1 (orthographic),
    // GL interpolates all varyings linearly in screen space — as the camera rotates,
    // vertex screen positions change while UVs stay fixed at TCMIN/TCMAX per vertex,
    // causing the interpolated UV at every fragment to drift.  This is the "ground
    // morphing and swimming" visible when standing still and looking around.
    // Fix: set w_clip = 1/rhw = 16/sz so GL's rasterizer sees the real camera-space
    // depth and applies perspective division automatically.  After perspective division
    // NDC position = pos_ndc.xyz (unchanged), but varyings are now correct.
    // Guard: HUD/sky/2D geometry uses sz=0 (aDepth=0) — keep w=1 for those.
    if (aDepth > 0.0) {
        float rhw = max(aDepth, 0.01) / 16.0;
        float w   = 1.0 / rhw;
        gl_Position  = vec4(pos_ndc.xyz * w, w);
        vTexCoordR   = aTexCoord * rhw;
        vRhw         = rhw;
    } else {
        gl_Position  = pos_ndc;
        vTexCoordR   = aTexCoord;
        vRhw         = 1.0;
    }

    vColor    = aColor;
    vTexCoord = aTexCoord;
    vFog      = aSpecular.a;
}
)";

static const char* fragmentShaderSrc = R"(
#version 330 core
noperspective in vec4  vColor;
noperspective in float vFog;
smooth        in vec2  vTexCoord;
noperspective in vec2  vTexCoordR;
noperspective in float vRhw;

uniform sampler2D uTexture;
uniform bool uFogEnabled;
uniform vec4 uFogColor;
uniform bool uAlphaTest;
// SOURCEPORT: runtime brightness multiplier (1.0 = neutral, 0.0 = black, 2.0 = double-bright)
uniform float uBrightness;

// SOURCEPORT: PBR override path. When uPBR is set, the bound texture at unit 0
// is the albedo; units 1/2/3 supply tangent-space normal, metallic+roughness
// (glTF convention: metallic in R, roughness in G), and AO. Tangent frame is
// rebuilt from screen-space derivatives so the retail vertex format needs no
// tangent/bitangent attributes. Lighting = vertex-baked irradiance (vColor)
// as diffuse + Cook-Torrance GGX specular against a hardcoded sun direction.
uniform bool      uPBR;
uniform sampler2D uNormalMap;
uniform sampler2D uMRMap;
uniform sampler2D uAOMap;
uniform float     uMetallicFactor;
uniform float     uRoughnessFactor;
uniform vec3      uSunDirView;     // normalized; camera-ish space approximation

// SOURCEPORT: debug visualization mode (toggled at runtime with F8).
// 0 = normal, 1 = PBR disabled (Lambert only), 2 = PBR-active fragments shown as magenta.
uniform int uDebugMode;

out vec4 FragColor;

// Christian Schüler's cotangent-frame trick — builds tangent basis from
// derivatives of position+UV so we don't need per-vertex tangents.
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T,T), dot(B,B)));
    return mat3(T * invmax, B * invmax, N);
}

float DistributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Perspective-correct UV via explicit division (full 32-bit precision).
    vec2 vTC = vTexCoordR / vRhw;

    ivec2 tsz = textureSize(uTexture, 0);

    // SOURCEPORT: LOD computation split by geometry type.
    // For alpha-tested geometry (foliage/trees): use depth-based LOD derived from
    // camera-space Z (= 1/vRhw).  Screen-space derivatives (dFdx/dFdy) change with
    // headbob because all vertex screen-Y positions oscillate, making dFdy(vTexCoord)
    // oscillate, LOD oscillate, and foliage sample from mip 1+ (slightly different
    // colour) → the whole scene darkens while walking → headlamp appears by contrast.
    // Camera depth (rv.z) is unaffected by a vertical headbob for a level camera, so
    // depth-based LOD is stable and eliminates the strobe/headlamp entirely.
    // For opaque geometry (terrain): screen-space derivative LOD is fine; terrain has
    // no mipmaps so the lod value is irrelevant (textureLod always returns level 0).
    float lod;
    if (uAlphaTest) {
        float camZ = 1.0 / max(vRhw, 1e-6);
        lod = max(0.0, 0.5 * log2(camZ / 512.0) - 0.75);
    } else {
        vec2 dt_x = dFdx(vTexCoord) * vec2(tsz);
        vec2 dt_y = dFdy(vTexCoord) * vec2(tsz);
        float rho2 = max(dot(dt_x, dt_x), dot(dt_y, dt_y));
        lod = max(0.0, 0.5 * log2(max(rho2, 1e-10)) - 0.75);
    }
    vec4 texel = textureLod(uTexture, vTC, lod);

    if (uAlphaTest && uDebugMode != 9) {
        vec2 atUV = (floor(vTC * vec2(tsz)) + 0.5) / vec2(tsz);
        if (textureLod(uTexture, atUV, 0.0).a < 0.5) discard;
    }

    vec4 color = texel * vColor;
    if (!uAlphaTest) { color.a = 1.0; } else { color.a = vColor.a; }

    if (uDebugMode == 2 && uPBR) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (uDebugMode == 3) { FragColor = vec4(vColor.rgb, 1.0); return; }
    if (uDebugMode == 4) { FragColor = vec4(texel.rgb,  1.0); return; }
    if (uDebugMode == 5) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
    if (uDebugMode == 6) { FragColor = vec4(textureLod(uTexture, vTC, 0.0).rgb, 1.0); return; }
    if (uDebugMode == 7) { float l = lod / 4.0; FragColor = vec4(l, 0.0, max(0.0,1.0-l), 1.0); return; }
    if (uDebugMode == 8) { FragColor = vec4(fract(vTC), 0.0, 1.0); return; }
    if (uDebugMode == 9) { FragColor = vec4(textureLod(uTexture, vTC, lod).rgb, 1.0); return; }
    // SOURCEPORT: mode 10 = terrain texture in color, foliage/alpha-test geometry as solid gray.
    // If headlamp appears here, headlamp is in terrain. If not, headlamp is foliage-only.
    if (uDebugMode == 10) {
        if (uAlphaTest) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
        FragColor = vec4(texel.rgb, 1.0); return;
    }
    // SOURCEPORT: mode 11 = UV fract at 100x magnification. A 0.001 UV drift (invisible at 1x
    // in mode 8) shows as 0.1 color-unit phase shift here — confirms or rules out sub-texel drift.
    if (uDebugMode == 11) { FragColor = vec4(fract(vTC * 100.0), 0.0, 1.0); return; }

    // SOURCEPORT: PBR path — Cook-Torrance GGX on top of retail vertex light.
    // vColor.rgb already encodes per-vertex Lambert against the sun + ambient,
    // so we treat it as diffuse irradiance and add a physically-plausible
    // specular on top. Tangent frame comes from screen-space derivatives so
    // no per-vertex tangent attribute is needed.
    if (uPBR && uDebugMode != 1) {
        vec3  albedo    = texel.rgb;
        vec2  mr        = texture(uMRMap, vTC).rg;
        float metallic  = mr.r * uMetallicFactor;
        float roughness = max(mr.g * uRoughnessFactor, 0.04);
        float ao        = texture(uAOMap, vTC).r;

        vec3 nTS = texture(uNormalMap, vTC).xyz * 2.0 - 1.0;
        vec3 N0  = vec3(0.0, 0.0, 1.0);
        vec3 p   = vec3(gl_FragCoord.xy, gl_FragCoord.z * 1000.0);
        mat3 TBN = cotangentFrame(N0, p, vTC);
        vec3 N   = normalize(TBN * nTS);

        vec3 V = vec3(0.0, 0.0, 1.0);
        vec3 L = normalize(uSunDirView);
        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 1e-4);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        vec3  F = FresnelSchlick(VdotH, F0);
        float D = DistributionGGX(NdotH, roughness);
        float G = GeometrySmith(NdotV, NdotL, roughness);
        vec3 spec = (D * G * F) / (4.0 * NdotV * max(NdotL, 1e-4) + 1e-4);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        vec3 diffuse  = kD * albedo * vColor.rgb;
        vec3 specular = spec * NdotL;
        color.rgb = (diffuse + specular) * ao;
    }

    // SOURCEPORT: hue-preserving brightness clamp.
    vec3 bright = color.rgb * uBrightness;
    float maxC  = max(max(bright.r, bright.g), bright.b);
    if (maxC > 1.0) bright /= maxC;
    color.rgb = bright;
    if (uFogEnabled) color.rgb = mix(uFogColor.rgb, color.rgb, vFog);

    FragColor = color;
}
)";

// ============================================================
// Helper: compile a shader and check for errors
// ============================================================

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
    }
    return shader;
}

// ============================================================
// Convert 16-bit RGB565 pixel to RGBA8888
// ============================================================

static inline uint32_t RGB565toRGBA(uint16_t c) {
    if (c == 0) return 0x00000000; // Color key: black = transparent
    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
    uint32_t b = ((c)       & 0x1F) * 255 / 31;
    return (0xFF << 24) | (b << 16) | (g << 8) | r; // ABGR for GL
}

static inline uint32_t RGB555toRGBA(uint16_t c) {
    if (c == 0) return 0x00000000; // Color key: black = transparent
    uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x1F) * 255 / 31;
    uint32_t b = ((c)       & 0x1F) * 255 / 31;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

// ============================================================
// RendererGL implementation
// ============================================================

RendererGL::RendererGL() {}

RendererGL::~RendererGL() {
    Shutdown();
}

bool RendererGL::Init(void* windowHandle, int width, int height) {
    // SOURCEPORT: Phase 4 — read display options from game globals
    extern int OptDisplayMode, OptVSync;

    m_width  = width;
    m_height = height;

    // Create SDL window with OpenGL context
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // SOURCEPORT: Request GL 4.1 Core — OpenXR runtimes (Meta Link, SteamVR)
    // require GL 4.0+; 4.1 is also the highest Core version supported on macOS.
    // All 3.3 code is forward-compatible: only the context version changes.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // SOURCEPORT: stencil for weapon/overlay masking

    // Build window flags based on OptDisplayMode:
    //   0 = windowed, 1 = fullscreen (exclusive), 2 = borderless fullscreen
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (OptDisplayMode == 1) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    } else if (OptDisplayMode == 2) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Borderless fullscreen: use desktop resolution
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            width    = dm.w;
            height   = dm.h;
            m_width  = width;
            m_height = height;
        }
    }

    m_window = SDL_CreateWindow(
        "OpenCarnivores",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        windowFlags
    );
    if (!m_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // For exclusive fullscreen, query the actual drawable size (may differ from requested)
    if (OptDisplayMode == 1) {
        SDL_GL_GetDrawableSize(m_window, &m_width, &m_height);
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    // Load OpenGL functions
    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!version) {
        fprintf(stderr, "gladLoadGL failed\n");
        return false;
    }

    // SOURCEPORT: VSync — adaptive (-1) falls back to regular (1) if unsupported
    if (OptVSync) {
        if (SDL_GL_SetSwapInterval(-1) < 0)
            SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    CompileShaders();

    // SOURCEPORT: register shader files for hot reload. Watches are cheap (one
    // stat() per file ~3× per second); callbacks only fire when mtime advances.
    HotReload::Watch("shaders/basic.vert", [this]() {
        CompileShaders();
        fprintf(stdout, "[HotReload] shader reloaded: shaders/basic.vert\n");
    });
    HotReload::Watch("shaders/basic.frag", [this]() {
        CompileShaders();
        fprintf(stdout, "[HotReload] shader reloaded: shaders/basic.frag\n");
    });

    CreateBuffers();

    // Initial GL state
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL); // Carnivores uses GREATEREQUAL depth test
    glClearDepth(0.0);      // Clear to 0 since we use GEQUAL
    // SOURCEPORT: the projection maps sz=0.25 (camera z=-64) to z_ndc=1 (GL far plane).
    // HUD weapon verts at z > -64 would be clipped by GL's far plane without this.
    // GL_DEPTH_CLAMP disables near/far z-clipping and just clamps depth to [0,1],
    // so close-to-camera stock/grip geometry renders correctly.
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); // Original game: D3DCULL_NONE

    // Create reusable texture for bitmap uploads
    glGenTextures(1, &m_bitmapTexture);

    // SOURCEPORT: 1x1 white texture — used as fallback when no texture is set (e.g. circle/particle rendering)
    {
        uint32_t white = 0xFFFFFFFF;
        glGenTextures(1, &m_whiteTexture);
        glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    fprintf(stderr, "RendererGL: OpenGL %d.%d initialized (%dx%d mode=%d vsync=%d)\n",
            GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version),
            m_width, m_height, OptDisplayMode, OptVSync);

    return true;
}

// SOURCEPORT: read a text file fully into a std::string, returning empty on miss.
// Used so `shaders/basic.{vert,frag}` can override the embedded shader source
// for dev hot reload; missing files fall back to the embedded strings silently.
static std::string ReadTextFile(const char* path) {
    // SOURCEPORT: route shader loads through VFS for mod overrides.
    FILE* f = VFS::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    if (n > 0) { s.resize((size_t)n); std::fread(&s[0], 1, (size_t)n, f); }
    std::fclose(f);
    return s;
}

void RendererGL::CompileShaders() {
    // SOURCEPORT: prefer shaders/basic.{vert,frag} on disk (hot-reloadable);
    // fall back to the embedded sources if either file is missing.
    std::string vsDisk = ReadTextFile("shaders/basic.vert");
    std::string fsDisk = ReadTextFile("shaders/basic.frag");
    const char* vsSrc = vsDisk.empty() ? vertexShaderSrc   : vsDisk.c_str();
    const char* fsSrc = fsDisk.empty() ? fragmentShaderSrc : fsDisk.c_str();

    GLuint vs = CompileShader(GL_VERTEX_SHADER,   vsSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        // Keep any previously-linked program active — hot-reload failure leaves
        // the game running on the last good shader.
        if (m_shaderProgram) return;
    } else {
        if (m_shaderProgram) glDeleteProgram(m_shaderProgram);
        m_shaderProgram = prog;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    glUseProgram(m_shaderProgram);
    m_locProjection = glGetUniformLocation(m_shaderProgram, "uProjection");
    m_locTexture    = glGetUniformLocation(m_shaderProgram, "uTexture");
    m_locFogEnabled = glGetUniformLocation(m_shaderProgram, "uFogEnabled");
    m_locFogColor   = glGetUniformLocation(m_shaderProgram, "uFogColor");
    m_locAlphaTest  = glGetUniformLocation(m_shaderProgram, "uAlphaTest");
    m_locBrightness = glGetUniformLocation(m_shaderProgram, "uBrightness");
    // uHUDMode removed — fproc1/fproc2 alpha handled unconditionally in shader

    // SOURCEPORT: PBR uniforms + sampler bindings for units 1/2/3.
    m_locPBR              = glGetUniformLocation(m_shaderProgram, "uPBR");
    m_locMetallicFactor   = glGetUniformLocation(m_shaderProgram, "uMetallicFactor");
    m_locRoughnessFactor  = glGetUniformLocation(m_shaderProgram, "uRoughnessFactor");
    m_locSunDirView       = glGetUniformLocation(m_shaderProgram, "uSunDirView");
    m_locDebugMode        = glGetUniformLocation(m_shaderProgram, "uDebugMode");
    GLint locNormal = glGetUniformLocation(m_shaderProgram, "uNormalMap");
    GLint locMR     = glGetUniformLocation(m_shaderProgram, "uMRMap");
    GLint locAO     = glGetUniformLocation(m_shaderProgram, "uAOMap");
    if (m_locTexture >= 0) glUniform1i(m_locTexture, 0);
    if (locNormal   >= 0)  glUniform1i(locNormal,    1);
    if (locMR       >= 0)  glUniform1i(locMR,        2);
    if (locAO       >= 0)  glUniform1i(locAO,        3);
    glUniform1i(m_locPBR, 0);
    glUniform1f(m_locMetallicFactor,  1.0f);
    glUniform1f(m_locRoughnessFactor, 1.0f);
    // Hardcoded screen-space sun (upper-right, toward viewer). Will make this
    // data-driven once world→view basis is plumbed.
    glUniform3f(m_locSunDirView, 0.4f, -0.5f, 0.8f);

    glUniform1f(m_locBrightness, 1.0f); // default: neutral (no change)
    if (m_locDebugMode >= 0) glUniform1i(m_locDebugMode, 0); // default: normal rendering

    // Set up orthographic projection matrix for screen-space rendering
    // Maps (0,0)-(WinW,WinH) to clip space in X/Y.
    // Z: _ZSCALE = -16 so sz = -16/camZ ranges from 0 (far) to 0.25 (near clip at z=-64).
    // SOURCEPORT: Set N=0, F=0.25 so the full sz range [0,0.25] maps to window depth [0,1],
    // using the entire depth buffer precision instead of just 12.5% of it.
    // With glClearDepth(0) and GL_GEQUAL, far=0 and near=1 is correct front-to-back ordering.
    float L = 0.0f, R = (float)m_width;
    float T = 0.0f, B = (float)m_height;
    float N = 0.0f, F = 0.25f;
    float proj[16] = {
        2.0f/(R-L),     0.0f,         0.0f,           0.0f,
        0.0f,           2.0f/(T-B),   0.0f,           0.0f,
        0.0f,           0.0f,         2.0f/(F-N),     0.0f,
        -(R+L)/(R-L),  -(T+B)/(T-B), -(F+N)/(F-N),   1.0f,
    };
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, proj);

    glUniform1i(m_locTexture, 0); // Texture unit 0
}

void RendererGL::CreateBuffers() {
    // Main VAO/VBO for rendering pre-transformed vertices
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Allocate enough for the larger of the two buffers
    glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex) * MAX_MAIN_VERTICES, nullptr, GL_DYNAMIC_DRAW);

    // Vertex layout matching RenderVertex struct
    // location 0: vec2 pos (sx, sy)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, sx));
    // location 1: float depth (sz)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, sz));
    // location 2: vec4 color — D3D stores as ARGB (0xAARRGGBB), in memory [BB,GG,RR,AA].
    // Use GL_BGRA so OpenGL remaps bytes to (R=RR, G=GG, B=BB, A=AA) correctly.
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, color));
    // location 3: vec4 specular — same ARGB layout
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, specular));
    // location 4: vec2 texcoord (tu, tv)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, tu));
    glBindVertexArray(0);
}

void RendererGL::InvalidateTextureCache() {
    for (auto& pair : m_texCache)
        glDeleteTextures(1, &pair.second.texId);
    m_texCache.clear();
    m_currentTexture = 0;
}

void RendererGL::Shutdown() {
    // Clean up textures
    for (auto& pair : m_texCache) {
        glDeleteTextures(1, &pair.second.texId);
    }
    m_texCache.clear();

    if (m_bitmapTexture) glDeleteTextures(1, &m_bitmapTexture);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_fsQuadVbo) glDeleteBuffers(1, &m_fsQuadVbo);
    if (m_fsQuadVao) glDeleteVertexArrays(1, &m_fsQuadVao);
    if (m_shaderProgram) glDeleteProgram(m_shaderProgram);

    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

// --- Frame management ---

void RendererGL::BeginFrame() {
    glUseProgram(m_shaderProgram);

    // SOURCEPORT: Sync viewport and internal dims to WinW×WinH every frame.
    // SetVideoMode() may update WinW/WinH to the actual drawable size after Init()
    // (HiDPI, SDL scaling), so the viewport and DrawFullscreenRect quad must follow.
    if (m_width != WinW || m_height != WinH) {
        m_width  = WinW;
        m_height = WinH;
        glViewport(0, 0, WinW, WinH);
    }

    // Rebuild projection every frame so it always matches the current WinW×WinH
    // game coordinate space (updated by SetVideoMode on every resize).
    float R = (float)WinW, B = (float)WinH;
    float N = 0.0f,        F = 0.25f;
    float proj[16] = {
        2.0f/R,   0.0f,   0.0f,          0.0f,
        0.0f,    -2.0f/B, 0.0f,          0.0f,
        0.0f,     0.0f,   2.0f/(F-N),    0.0f,
       -1.0f,     1.0f,  -(F+N)/(F-N),   1.0f,
    };
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, proj);
    // SOURCEPORT: cache for CustomMaterials::Apply; custom programs need the
    // same screen-space projection to render pre-transformed vertices correctly.
    std::memcpy(m_projMatrix, proj, sizeof(proj));

    m_frameCounter++;
}

void RendererGL::EndFrame() {
    SDL_GL_SwapWindow(m_window);
}

void RendererGL::UpdateProjection(const float* mat16) {
    glUseProgram(m_shaderProgram);
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, mat16);
    std::memcpy(m_projMatrix, mat16, 16 * sizeof(float));
}

void RendererGL::ClearBuffers() {
    glStencilMask(0xFF); // must unmask stencil writes before clearing
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// --- Texture management ---

// SOURCEPORT: sRGB-correct mipmap generation (mirrors renderd3d.cpp version).
// glGenerateMipmap averages in gamma space → higher mips are perceptually
// darker → GPU picks a darker mip when camera moves, creating a "circle of
// light" effect (near tiles stay on mip 0; far tiles go to darker high mips).
static inline float rgl_srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static inline float rgl_linear_to_srgb(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c < 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}
static void rgl_GenerateLinearMipmaps(const uint32_t* level0, int w0, int h0)
{
    std::vector<uint32_t> src(level0, level0 + w0 * h0);
    int w = w0, h = h0;
    for (int lv = 1; w > 1 || h > 1; ++lv) {
        int nw = (w > 1) ? w / 2 : 1;
        int nh = (h > 1) ? h / 2 : 1;
        std::vector<uint32_t> dst(nw * nh);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0, a = 0;
                int cnt = 0;
                for (int dy = 0; dy < 2 && (y*2+dy) < h; ++dy) {
                    for (int dx = 0; dx < 2 && (x*2+dx) < w; ++dx) {
                        uint32_t c = src[(y*2+dy)*w + (x*2+dx)];
                        r += rgl_srgb_to_linear((c        & 0xFF) / 255.0f);
                        g += rgl_srgb_to_linear(((c >> 8) & 0xFF) / 255.0f);
                        b += rgl_srgb_to_linear(((c >>16) & 0xFF) / 255.0f);
                        a += ((c >> 24) & 0xFF) / 255.0f;
                        ++cnt;
                    }
                }
                float inv = 1.0f / cnt;
                auto pack = [](float v) -> uint32_t {
                    int i = (int)(v * 255.5f);
                    return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i);
                };
                dst[y*nw + x] = pack(rgl_linear_to_srgb(r*inv))
                              | (pack(rgl_linear_to_srgb(g*inv)) << 8)
                              | (pack(rgl_linear_to_srgb(b*inv)) << 16)
                              | (pack(a*inv)                     << 24);
            }
        }
        glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA8, nw, nh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, dst.data());
        src = std::move(dst);
        w = nw; h = nh;
    }
}

GLuint RendererGL::UploadTexture16(void* data, int w, int h) {
    // Convert 16-bit RGB565/555 to 32-bit RGBA
    std::vector<uint32_t> rgba(w * h);
    uint16_t* src = (uint16_t*)data;
    for (int i = 0; i < w * h; i++) {
        rgba[i] = m_isRGB565 ? RGB565toRGBA(src[i]) : RGB555toRGBA(src[i]);
    }

    // SOURCEPORT: only transparent textures (foliage) get a mip chain.
    // Opaque textures use GL_LINEAR — no mip selection, driver LOD bias irrelevant.
    // See renderd3d.cpp::gl_UploadRGBA for the full explanation.
    bool hasTransparency = false;
    for (int i = 0; i < w * h; i++)
        if ((rgba[i] >> 24) == 0) { hasTransparency = true; break; }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (hasTransparency)
        rgl_GenerateLinearMipmaps(rgba.data(), w, h);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // SOURCEPORT: Set MAX_LEVEL to actual mip count so textureLod() in the shader
    // clamps safely — non-mipmap textures land on level 0 for any computed LOD.
    {
        int maxLvl = 0;
        for (int s = std::max(w, h); s > 1; s >>= 1, maxLvl++);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, hasTransparency ? maxLvl : 0);
    }
    GLenum minFilter = hasTransparency
        ? (m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
        : (m_linearFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);

    return tex;
}

#include "../TextureOverrides.h"

void RendererGL::SetTexture(void* lpData, int w, int h) {
    uintptr_t key = (uintptr_t)lpData;

    auto it = m_texCache.find(key);
    if (it != m_texCache.end()) {
        m_currentTexture = it->second.texId;
        it->second.lastUsed = m_frameCounter;
    } else {
        // Evict old entries if cache is too large
        if (m_texCache.size() > 128) {
            int oldest = m_frameCounter;
            uintptr_t oldestKey = 0;
            for (auto& p : m_texCache) {
                if (p.second.lastUsed < oldest) {
                    oldest = p.second.lastUsed;
                    oldestKey = p.first;
                }
            }
            if (oldestKey) {
                glDeleteTextures(1, &m_texCache[oldestKey].texId);
                m_texCache.erase(oldestKey);
            }
        }

        // SOURCEPORT: prefer BCn DDS override, then 8-bit RGBA override,
        // finally fall back to the retail 16-bit decode.
        GLuint tex = 0;
        if (const TextureOverrides::CompressedTex* ct = TextureOverrides::GetCompressed(lpData)) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            int lw = ct->w, lh = ct->h;
            for (int i = 0; i < ct->mipCount; ++i) {
                glCompressedTexImage2D(GL_TEXTURE_2D, i, (GLenum)ct->glFormat,
                                       lw, lh, 0,
                                       (GLsizei)ct->mipSizes[i],
                                       ct->data + ct->mipOffsets[i]);
                lw = (lw > 1) ? lw >> 1 : 1;
                lh = (lh > 1) ? lh >> 1 : 1;
            }
            // Single-mip DDS: generate the rest in linear space
            if (ct->mipCount <= 1) {
                std::vector<uint32_t> tmp(ct->w * ct->h);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
                rgl_GenerateLinearMipmaps(tmp.data(), ct->w, ct->h);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  ct->mipCount - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
            // SOURCEPORT: anisotropic filtering — see renderd3d.cpp::gl_SetAnisotropy
            {
                static GLfloat maxAniso_ = -1.0f;
                if (maxAniso_ < 0.0f) {
                    if (GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic)
                        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso_);
                    else maxAniso_ = 0.0f;
                }
                if (maxAniso_ > 1.0f) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                                                     maxAniso_ < 16.0f ? maxAniso_ : 16.0f);
            }
        } else {
            int ow = 0, oh = 0;
            const uint32_t* over = TextureOverrides::Get(lpData, &ow, &oh);
            if (over) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
                rgl_GenerateLinearMipmaps(over, ow, oh);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
            } else {
                tex = UploadTexture16(lpData, w, h);
            }
        }
        m_texCache[key] = { tex, m_frameCounter };
        m_currentTexture = tex;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_currentTexture);
}

// --- Vertex buffer operations ---

RenderVertex* RendererGL::LockVertexBuffer() {
    return m_mainBuffer;
}

void RendererGL::UnlockAndDrawTriangles(int triCount1, int triCount2) {
    int totalVerts = (triCount1 + triCount2) * 3;
    if (totalVerts <= 0) return;

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, totalVerts * sizeof(RenderVertex), m_mainBuffer);

    // SOURCEPORT: do NOT rebind texture here — d3dSetTexture already bound it.
    // m_currentTexture is stale since terrain/model rendering bypasses SetTexture().

    // Draw normal triangles (no alpha test)
    if (triCount1 > 0) {
        glUniform1i(m_locAlphaTest, 0);
        glDrawArrays(GL_TRIANGLES, 0, triCount1 * 3);
    }

    // Draw color-keyed triangles (with alpha test)
    if (triCount2 > 0) {
        glUniform1i(m_locAlphaTest, 1);
        glDrawArrays(GL_TRIANGLES, triCount1 * 3, triCount2 * 3);
    }

    glBindVertexArray(0);
}

RenderVertex* RendererGL::LockGeometryBuffer() {
    return m_geomBuffer;
}

void RendererGL::UnlockAndDrawGeometry(int vertexCount, bool colorKey) {
    if (vertexCount <= 0) return;

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(RenderVertex), m_geomBuffer);

    // SOURCEPORT: do NOT rebind texture here — d3dSetTexture / glBindTexture in
    // DrawTPlaneClip's texture-change block already set the correct texture.

    // SOURCEPORT: do NOT override uAlphaTest here — d3dStartBufferG()/d3dStartBufferGBMP()
    // already set the correct alpha-test state via SetAlphaTest(true).  Overriding with
    // colorKey=false would force uAlphaTest=0 → color.a=1.0 in the shader, making water
    // (which uses vertex alpha for transparency) fully opaque.
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);

    glBindVertexArray(0);
}

// --- Render state ---

void RendererGL::SetRenderStates(bool zWrite, int dstBlend) {
    m_zWriteEnabled = zWrite;
    m_dstBlend = dstBlend;
    glDepthMask(zWrite ? GL_TRUE : GL_FALSE);

    GLenum glDst;
    switch (dstBlend) {
        case BLEND_ZERO:        glDst = GL_ZERO; break;
        case BLEND_ONE:         glDst = GL_ONE; break;
        case BLEND_SRCALPHA:    glDst = GL_SRC_ALPHA; break;
        case BLEND_INVSRCALPHA: glDst = GL_ONE_MINUS_SRC_ALPHA; break;
        default:                glDst = GL_ONE_MINUS_SRC_ALPHA; break;
    }
    glBlendFunc(GL_SRC_ALPHA, glDst);
}

void RendererGL::SetFogEnabled(bool enabled) {
    m_fogEnabled = enabled;
    glUniform1i(m_locFogEnabled, enabled ? 1 : 0);
}

void RendererGL::SetFogColor(uint32_t color) {
    m_fogColor[0] = ((color >> 16) & 0xFF) / 255.0f;
    m_fogColor[1] = ((color >> 8)  & 0xFF) / 255.0f;
    m_fogColor[2] = ((color)       & 0xFF) / 255.0f;
    m_fogColor[3] = 1.0f;
    glUniform4fv(m_locFogColor, 1, m_fogColor);
    glClearColor(m_fogColor[0], m_fogColor[1], m_fogColor[2], 1.0f);
}

void RendererGL::SetLinearFilter(bool enabled) {
    m_linearFilter = enabled;
    // Will apply to next texture bind
}

void RendererGL::SetAlphaTest(bool enabled) {
    m_alphaTestEnabled = enabled;
    glUniform1i(m_locAlphaTest, enabled ? 1 : 0);
}

GLuint RendererGL::GetWhiteTexture() {
    return m_whiteTexture;
}

void RendererGL::SetHUDMode(bool /*enabled*/) {
    // SOURCEPORT: no-op — fproc1 alpha is now always 1.0 in the shader (no HUD mode needed)
}

void RendererGL::SetZBufferEnabled(bool enabled) {
    m_zBufferEnabled = enabled;
    if (enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(m_zWriteEnabled ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
}

void RendererGL::SetDepthMask(bool write) {
    // SOURCEPORT: set depth write without touching depth test enable state.
    // Used by additive overlay passes (EnvMap/PhongMap) so they read depth
    // (occluded by closer geometry like the player's arm) but don't write it.
    m_zWriteEnabled = write;
    glDepthMask(write ? GL_TRUE : GL_FALSE);
}

void RendererGL::SetStencilMode(int mode) {
    // SOURCEPORT: stencil isolation for weapon overlays (PhongMap/EnvMap).
    // Mode 1 (write): mark every rasterized weapon pixel with stencil=1.
    // Mode 2 (test): only render fragments where stencil==1 (weapon pixels only).
    // Mode 0 (off): disable stencil entirely.
    // Without this, the additive overlay depth values (~0.64, very close) always
    // pass GL_GEQUAL against terrain depth (~0.125, far), painting the specular
    // texture over the ground and creating the "headlamp" artifact while walking.
    switch (mode) {
        case 1:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case 2:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0x00); // read-only stencil
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            break;
        default:
            glStencilMask(0xFF);
            glDisable(GL_STENCIL_TEST);
            break;
    }
}

void RendererGL::SetBrightness(float b) {
    // SOURCEPORT: runtime brightness applied in shader. b=1.0 = neutral, 0.0=black, 2.0=double.
    // Replaces the old BrightenTexture(OptBrightness) bake so the slider is live.
    m_brightness = b;
    glUniform1f(m_locBrightness, b);
}

void RendererGL::SetDebugMode(int mode) {
    if (m_locDebugMode >= 0) glUniform1i(m_locDebugMode, mode);
}

void RendererGL::BindMaterial(const void* materialPtr) {
    const Materials::Material* m = static_cast<const Materials::Material*>(materialPtr);
    // Only enable PBR when a normal map exists — without it the tangent-frame
    // path contributes no perturbation and specular would be flat lit.
    if (!m || m->normalTex == 0) {
        if (m_pbrActive) {
            glUniform1i(m_locPBR, 0);
            m_pbrActive = false;
        }
        return;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)m->normalTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m->mrTex ? (GLuint)m->mrTex : GetWhiteTexture());
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m->aoTex ? (GLuint)m->aoTex : GetWhiteTexture());
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(m_locMetallicFactor,  m->metallicFactor);
    glUniform1f(m_locRoughnessFactor, m->roughnessFactor);
    glUniform1i(m_locPBR, 1);
    m_pbrActive = true;
}

void RendererGL::BindCustomMaterial(const void* materialPtr) {
    const CustomMaterials::Material* cm =
        static_cast<const CustomMaterials::Material*>(materialPtr);
    if (!cm) {
        // Restore the engine's default program. GL uniforms are per-program
        // so fog/alpha/brightness/PBR state in the default program is still
        // intact — nothing else to re-push.
        if (m_customProgramActive) {
            glUseProgram(m_shaderProgram);
            m_customProgramActive = false;
        }
        return;
    }
    CustomMaterials::Apply(cm, m_projMatrix);
    m_customProgramActive = true;
}

// --- 2D operations ---

void RendererGL::DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData, bool colorKey, int srcH, const void* overrideKey) {
    if (!lpData || w <= 0 || h <= 0) return;

    // Actual source dimensions: srcW × uploadH.
    // If srcH == 0 the caller hasn't specified a source height; fall back to h
    // (legacy callers where src and dest are the same size).
    int uploadH = (srcH > 0) ? srcH : h;
    int uploadW = srcW;

    // SOURCEPORT: menu/HUD override path. If TextureOverrides has a 32-bit PNG/
    // TGA/BMP/JPEG registered for this picture, upload it at its native
    // resolution instead of decoding the retail 16-bit TGA. UVs are 0..1 so any
    // size maps correctly onto the destination quad; high-res menu art drops in
    // with no other code changes. `overrideKey`, when supplied, wins over
    // `lpData` — menu pictures pass `&pic` because pic.lpImage is heap-recycled
    // across ReleaseResources (cross-asset bleed bug otherwise).
    int overW = 0, overH = 0;
    void* lookupKey = overrideKey ? const_cast<void*>(overrideKey) : lpData;
    const uint32_t* over = TextureOverrides::Get(lookupKey, &overW, &overH);

    // Convert 16-bit source to RGBA only when we don't have an override.
    std::vector<uint32_t> rgba;
    if (!over) {
        rgba.resize(uploadW * uploadH);
        uint16_t* src = (uint16_t*)lpData;
        for (int row = 0; row < uploadH; row++) {
            for (int col = 0; col < uploadW; col++) {
                uint16_t pixel = src[row * srcW + col];
                rgba[row * uploadW + col] = m_isRGB565 ? RGB565toRGBA(pixel) : RGB555toRGBA(pixel);
            }
        }
    }

    // Upload source texture at its natural size (override dims may differ).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    if (over) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, overW, overH, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, uploadW, uploadH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Draw a textured quad at screen position
    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + w, y1 = y0 + h;

    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py, float u, float tv) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = 0xFFFFFFFF; v.specular = 0xFF000000;
        v.tu = u; v.tv = tv;
    };
    fillVert(quad[0], x0, y0, 0.0f, 0.0f);
    fillVert(quad[1], x1, y0, 1.0f, 0.0f);
    fillVert(quad[2], x1, y1, 1.0f, 1.0f);
    fillVert(quad[3], x0, y0, 0.0f, 0.0f);
    fillVert(quad[4], x1, y1, 1.0f, 1.0f);
    fillVert(quad[5], x0, y1, 0.0f, 1.0f);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(m_locAlphaTest, colorKey ? 1 : 0);
    glUniform1i(m_locFogEnabled, 0);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

// SOURCEPORT: Render text using GDI into a temp DIB, then upload as GL texture.
// Uses fnt_Small for all menu and HUD text. Performance is acceptable for
// non-realtime menu use; in-game calls are infrequent.
// SOURCEPORT: shared scaled font — built once per WinH change, used by DrawText and MeasureText.
// fnt_Small is 14px/5px wide designed for 600px height; scale both dimensions with WinH.
static HFONT  s_scaledFont  = NULL;
static int    s_scaledWinH  = 0;

static HFONT GetScaledFont() {
    extern HFONT fnt_Small;
    if (s_scaledWinH != WinH) {
        if (s_scaledFont) { DeleteObject(s_scaledFont); s_scaledFont = NULL; }
        int fh = std::max(14, 14 * WinH / 600);
        int fw = std::max(5,   5 * WinH / 600);
        s_scaledFont = CreateFontA(fh, fw, 0, 0, 100, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        s_scaledWinH = WinH;
    }
    return s_scaledFont ? s_scaledFont : fnt_Small;
}

// SOURCEPORT: big heading font — ~36px at 600p, Bold. Used for menu screen headings.
static HFONT  s_bigFont    = NULL;
static int    s_bigWinH    = 0;

static HFONT GetBigFont() {
    if (s_bigWinH != WinH) {
        if (s_bigFont) { DeleteObject(s_bigFont); s_bigFont = NULL; }
        int fh = std::max(36, 36 * WinH / 600);
        int fw = std::max(14, 14 * WinH / 600);
        s_bigFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, NULL);
        s_bigWinH = WinH;
    }
    return s_bigFont ? s_bigFont : GetScaledFont();
}

// SOURCEPORT: menu font — fnt_Midd style: weight=550 (semibold), 16px/7px at 600p, scaled by WinH/600.
static HFONT  s_menuFont   = NULL;
static int    s_menuWinH   = 0;

static HFONT GetMenuFont() {
    if (s_menuWinH != WinH) {
        if (s_menuFont) { DeleteObject(s_menuFont); s_menuFont = NULL; }
        int fh = std::max(16, 16 * WinH / 600);
        int fw = std::max(7,   7 * WinH / 600);
        // SOURCEPORT: NULL face name → system default sans-serif (matches original fnt_Midd style).
        // Weight=700 (Bold) to match the medium-bold appearance of the original menu text.
        s_menuFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, NULL);
        s_menuWinH = WinH;
    }
    return s_menuFont ? s_menuFont : GetScaledFont();
}

// SOURCEPORT: shared text-rendering core — renders text with a given GDI font as a GL quad.
static void DrawTextWithFont(int x, int y, const char* text, uint32_t color, HFONT useFont,
                             GLuint m_bitmapTexture, GLuint m_vao, GLuint m_vbo,
                             bool m_zBufferEnabled, GLint m_locAlphaTest, GLint m_locFogEnabled,
                             bool m_fogEnabled) {
    // Create a temporary memory DC to measure and render the text
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return;
    HFONT hOldFont = (HFONT)SelectObject(hdc, useFont);

    // Measure text extent
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    int tw = sz.cx > 0 ? sz.cx : 1;
    int th = sz.cy > 0 ? sz.cy : 1;

    // Create 24-bit DIB (top-down)
    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = tw;
    bih.biHeight      = -th;
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    BITMAPINFO bi = {};
    bi.bmiHeader = bih;

    void* dibBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &dibBits, NULL, 0);
    if (!hbmp) { SelectObject(hdc, hOldFont); DeleteDC(hdc); return; }
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hbmp);

    int stride = (tw * 3 + 3) & ~3;
    memset(dibBits, 0, stride * th);

    // SOURCEPORT: render white text on black so R channel = glyph coverage (0–255).
    // Use that as alpha and store the requested color constant — gives proper
    // anti-aliased edges and makes the black shadow pass actually visible.
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, text, (int)strlen(text));

    uint8_t cr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t cg = (uint8_t)((color >>  8) & 0xFF);
    uint8_t cb = (uint8_t)((color      ) & 0xFF);

    std::vector<uint32_t> rgba(tw * th);
    uint8_t* src = (uint8_t*)dibBits;
    for (int py = 0; py < th; py++) {
        for (int px2 = 0; px2 < tw; px2++) {
            uint8_t lum = src[py * stride + px2 * 3 + 2]; // R == G == B for white text
            // alpha = glyph coverage; RGB = constant requested color → correct blend
            rgba[py * tw + px2] = ((uint32_t)lum << 24) | ((uint32_t)cr << 16) |
                                   ((uint32_t)cg  <<  8) | cb;
        }
    }

    SelectObject(hdc, hOldBmp);
    SelectObject(hdc, hOldFont);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + tw,  y1 = y0 + th;

    RenderVertex quad[6];
    auto fill2 = [&](RenderVertex& v, float px2, float py, float u, float tv) {
        v.sx = px2; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = 0xFFFFFFFF; v.specular = 0xFF000000;
        v.tu = u; v.tv = tv;
    };
    fill2(quad[0], x0, y0, 0.0f, 0.0f);
    fill2(quad[1], x1, y0, 1.0f, 0.0f);
    fill2(quad[2], x1, y1, 1.0f, 1.0f);
    fill2(quad[3], x0, y0, 0.0f, 0.0f);
    fill2(quad[4], x1, y1, 1.0f, 1.0f);
    fill2(quad[5], x0, y1, 0.0f, 1.0f);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

int RendererGL::MeasureText(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetScaledFont();
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

int RendererGL::MeasureTextMed(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetMenuFont();
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

void RendererGL::DrawText(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    DrawTextWithFont(x, y, text, color, GetScaledFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextMed(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    DrawTextWithFont(x, y, text, color, GetMenuFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextBig(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    DrawTextWithFont(x, y, text, color, GetBigFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

int RendererGL::MeasureTextBig(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetBigFont();
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

// SOURCEPORT: Draw a filled rectangle at screen position (x,y) with size (w,h).
void RendererGL::FillRect(int x, int y, int w, int h, uint32_t argbColor) {
    float a = ((argbColor >> 24) & 0xFF) / 255.0f;
    float r = ((argbColor >> 16) & 0xFF) / 255.0f;
    float g = ((argbColor >>  8) & 0xFF) / 255.0f;
    float b = ((argbColor      ) & 0xFF) / 255.0f;

    // Store as ARGB in uint32; GL_BGRA attrib reads memory bytes [B,G,R,A] (LE) = correct
    uint32_t vc = ((uint32_t)(a*255) << 24) | ((uint32_t)(r*255) << 16) |
                  ((uint32_t)(g*255) <<  8) |  (uint32_t)(b*255);

    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + w,   y1 = y0 + h;

    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = vc; v.specular = 0xFF000000;
        v.tu = 0.0f; v.tv = 0.0f;
    };
    fillVert(quad[0], x0, y0); fillVert(quad[1], x1, y0); fillVert(quad[2], x1, y1);
    fillVert(quad[3], x0, y0); fillVert(quad[4], x1, y1); fillVert(quad[5], x0, y1);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    // SOURCEPORT: use alpha test = 1 so vColor.a drives blend transparency (white texture
    // has texel.a=1.0, so the discard never fires — vertex alpha is used directly).
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

void RendererGL::DrawFullscreenRect(uint32_t argbColor) {
    float a = ((argbColor >> 24) & 0xFF) / 255.0f;
    float r = ((argbColor >> 16) & 0xFF) / 255.0f;
    float g = ((argbColor >> 8)  & 0xFF) / 255.0f;
    float b = ((argbColor)       & 0xFF) / 255.0f;

    uint32_t vertColor =
        (uint32_t)(a * 255) << 24 |
        (uint32_t)(b * 255) << 16 |  // Note: GL reads as RGBA bytes, but our
        (uint32_t)(g * 255) << 8  |  // vertex attrib is BGRA from the D3D convention
        (uint32_t)(r * 255);

    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = argbColor; v.specular = 0xFF000000;
        v.tu = 0.0f; v.tv = 0.0f;
    };
    fillVert(quad[0], 0, 0);
    fillVert(quad[1], (float)WinW, 0);
    fillVert(quad[2], (float)WinW, (float)WinH);
    fillVert(quad[3], 0, 0);
    fillVert(quad[4], (float)WinW, (float)WinH);
    fillVert(quad[5], 0, (float)WinH);

    // Draw without depth test, with blending, no texture
    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    // SOURCEPORT: uAlphaTest=1 so vColor.a drives blend transparency; white texture
    // (texel.a=1.0) never triggers the discard, so vertex alpha passes through directly.
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    // Bind a 1x1 white texture so the shader just uses vertex color
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

// --- Z-buffer control ---

void RendererGL::ClearZBuffer() {
    glClear(GL_DEPTH_BUFFER_BIT);
}

float RendererGL::GetDepthAt(int x, int y) {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return 0.f;
    float depth = 0.f;
    glReadPixels(x, m_height - 1 - y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    return depth;
}

// --- Screenshot ---

void RendererGL::CopyBackBuffer(void* dest, int x, int y, int w, int h) {
    if (!dest || w <= 0 || h <= 0) return;
    // Read as 16-bit RGB565 into the destination buffer
    std::vector<uint32_t> rgba(w * h);
    glReadPixels(x, m_height - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // Convert RGBA to RGB565 and flip vertically
    uint16_t* dst = (uint16_t*)dest;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t c = rgba[(h - 1 - row) * w + col];
            uint32_t r = (c >> 0)  & 0xFF;
            uint32_t g = (c >> 8)  & 0xFF;
            uint32_t b = (c >> 16) & 0xFF;
            dst[row * w + col] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

// --- Display info ---

bool RendererGL::IsRGB565() const {
    return m_isRGB565;
}

int RendererGL::GetTextureMemory() const {
    // Report 64MB — modern GPUs have plenty
    return 64 * 1024 * 1024;
}
