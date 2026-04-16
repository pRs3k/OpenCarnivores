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
#include <cstring>
#include <cstdio>

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

out vec4 vColor;
out vec2 vTexCoord;
out float vFog;

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
        float w = 16.0 / aDepth;   // = 1/rhw = -camera_z
        gl_Position = vec4(pos_ndc.xyz * w, w);
    } else {
        gl_Position = pos_ndc;
    }

    vColor = aColor;
    vTexCoord = aTexCoord;
    // Fog factor stored in specular alpha (255 = no fog, 0 = full fog)
    vFog = aSpecular.a;
}
)";

static const char* fragmentShaderSrc = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in float vFog;

uniform sampler2D uTexture;
uniform bool uFogEnabled;
uniform vec4 uFogColor;
uniform bool uAlphaTest;
// SOURCEPORT: runtime brightness multiplier (1.0 = neutral, 0.0 = black, 2.0 = double-bright)
uniform float uBrightness;

out vec4 FragColor;

void main() {
    vec4 texel = texture(uTexture, vTexCoord);

    // fproc2 (alpha-test pass): discard color-key pixels (RGB555 c=0 decoded as alpha=0).
    // SOURCEPORT: threshold 0.1 instead of 0.5 — game textures have binary alpha (0 or 1),
    // so at full-res this makes no difference. At mip levels, averaging 0/1 pixels yields
    // intermediate values; 0.5 would discard leaves with <50% coverage (foliage vanishes);
    // 0.1 keeps them visible down to 10% coverage, eliminating the flicker/pop.
    if (uAlphaTest && texel.a < 0.1)
        discard;

    vec4 color = texel * vColor;

    // SOURCEPORT: Alpha compositing matches the original D3D6 pipeline:
    // - fproc1 (uAlphaTest=false): D3D6 renders these with alpha blending DISABLED —
    //   all pixels are fully opaque. RGB555 c=0 (decoded alpha=0) must still render
    //   solid (weapon grips, compass body, binocular frame). Force alpha=1.
    // - fproc2 (uAlphaTest=true): use vertex alpha. sfOpacity faces have vColor.a=1.0
    //   (opaque after color-key discard). sfTransparent faces have vColor.a≈0.44
    //   (semi-transparent overlay, e.g. binocular lens vignette).
    if (!uAlphaTest) {
        color.a = 1.0;
    } else {
        color.a = vColor.a;
    }

    // Apply brightness (clamped to [0,1] to avoid HDR blow-out on non-HDR displays)
    color.rgb = clamp(color.rgb * uBrightness, 0.0, 1.0);

    // Apply fog
    if (uFogEnabled) {
        color.rgb = mix(uFogColor.rgb, color.rgb, vFog);
    }

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

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

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
    CreateBuffers();

    // Initial GL state
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL); // Carnivores uses GREATEREQUAL depth test
    glClearDepth(0.0);      // Clear to 0 since we use GEQUAL
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

void RendererGL::CompileShaders() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);

    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vs);
    glAttachShader(m_shaderProgram, fs);
    glLinkProgram(m_shaderProgram);

    GLint success;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_shaderProgram, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
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

    glUniform1f(m_locBrightness, 1.0f); // default: neutral (no change)

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

    m_frameCounter++;
}

void RendererGL::EndFrame() {
    SDL_GL_SwapWindow(m_window);
}

void RendererGL::ClearBuffers() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// --- Texture management ---

GLuint RendererGL::UploadTexture16(void* data, int w, int h) {
    // Convert 16-bit RGB565/555 to 32-bit RGBA
    std::vector<uint32_t> rgba(w * h);
    uint16_t* src = (uint16_t*)data;
    for (int i = 0; i < w * h; i++) {
        rgba[i] = m_isRGB565 ? RGB565toRGBA(src[i]) : RGB555toRGBA(src[i]);
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);

    return tex;
}

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

        GLuint tex = UploadTexture16(lpData, w, h);
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

void RendererGL::SetBrightness(float b) {
    // SOURCEPORT: runtime brightness applied in shader. b=1.0 = neutral, 0.0=black, 2.0=double.
    // Replaces the old BrightenTexture(OptBrightness) bake so the slider is live.
    m_brightness = b;
    glUniform1f(m_locBrightness, b);
}

// --- 2D operations ---

void RendererGL::DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData, bool colorKey, int srcH) {
    if (!lpData || w <= 0 || h <= 0) return;

    // Actual source dimensions: srcW × uploadH.
    // If srcH == 0 the caller hasn't specified a source height; fall back to h
    // (legacy callers where src and dest are the same size).
    int uploadH = (srcH > 0) ? srcH : h;
    int uploadW = srcW;

    // Convert 16-bit source to RGBA (only the actual source pixels)
    std::vector<uint32_t> rgba(uploadW * uploadH);
    uint16_t* src = (uint16_t*)lpData;
    for (int row = 0; row < uploadH; row++) {
        for (int col = 0; col < uploadW; col++) {
            uint16_t pixel = src[row * srcW + col];
            rgba[row * uploadW + col] = m_isRGB565 ? RGB565toRGBA(pixel) : RGB555toRGBA(pixel);
        }
    }

    // Upload source texture at its natural size
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, uploadW, uploadH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
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
void RendererGL::DrawText(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;

    extern HFONT fnt_Small;

    // SOURCEPORT: Scale font proportionally to screen height so text stays readable at
    // high resolutions (fnt_Small is 14px, designed for 600px reference height).
    static HFONT s_scaledFont = NULL;
    static int   s_scaledWinH = 0;
    if (s_scaledWinH != WinH) {
        if (s_scaledFont) { DeleteObject(s_scaledFont); s_scaledFont = NULL; }
        int fh = std::max(14, 14 * WinH / 600);
        int fw = std::max(5,   5 * WinH / 600);
        s_scaledFont = CreateFontA(fh, fw, 0, 0, 100, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        s_scaledWinH = WinH;
    }
    HFONT useFont = s_scaledFont ? s_scaledFont : fnt_Small;

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
    bih.biHeight      = -th;  // negative = top-down
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    BITMAPINFO bi = {};
    bi.bmiHeader = bih;

    void* dibBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &dibBits, NULL, 0);
    if (!hbmp) { SelectObject(hdc, hOldFont); DeleteDC(hdc); return; }
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hbmp);

    // Clear to black (will be the transparent key)
    int stride = (tw * 3 + 3) & ~3;
    memset(dibBits, 0, stride * th);

    // Render text
    SetBkMode(hdc, TRANSPARENT);
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color      ) & 0xFF;
    SetTextColor(hdc, RGB(r, g, b));
    TextOutA(hdc, 0, 0, text, (int)strlen(text));

    // Convert BGR DIB pixels to RGBA (black = transparent)
    std::vector<uint32_t> rgba(tw * th);
    uint8_t* src = (uint8_t*)dibBits;
    for (int py = 0; py < th; py++) {
        for (int px = 0; px < tw; px++) {
            uint8_t bv = src[py * stride + px * 3 + 0];
            uint8_t gv = src[py * stride + px * 3 + 1];
            uint8_t rv = src[py * stride + px * 3 + 2];
            uint8_t av = (rv | gv | bv) ? 255 : 0;
            rgba[py * tw + px] = ((uint32_t)av << 24) | ((uint32_t)rv << 16) |
                                  ((uint32_t)gv <<  8) | bv;
        }
    }

    // Cleanup GDI objects
    SelectObject(hdc, hOldBmp);
    SelectObject(hdc, hOldFont);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    // Upload to shared bitmap texture and draw quad
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    // Data is BGRA in memory (GDI DIB is BGR; rv/gv/bv packed into uint32 little-endian)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + tw,  y1 = y0 + th;

    RenderVertex quad[6];
    auto fill = [&](RenderVertex& v, float px, float py, float u, float tv) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = 0xFFFFFFFF; v.specular = 0xFF000000;
        v.tu = u; v.tv = tv;
    };
    fill(quad[0], x0, y0, 0.0f, 0.0f);
    fill(quad[1], x1, y0, 1.0f, 0.0f);
    fill(quad[2], x1, y1, 1.0f, 1.0f);
    fill(quad[3], x0, y0, 0.0f, 0.0f);
    fill(quad[4], x1, y1, 1.0f, 1.0f);
    fill(quad[5], x0, y1, 0.0f, 1.0f);

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
