// SOURCEPORT: OpenGL 3.3 Core renderer backend for Carnivores 2.
// This replaces the Direct3D 6 execute buffer rendering model with modern OpenGL.
// The game engine does all vertex transformation on the CPU, so we receive
// pre-transformed screen-space vertices (like D3DTLVERTEX) and just need to
// rasterize them with the correct texture, fog, and blending.

#include "RendererGL.h"
#include <cstring>
#include <cstdio>

// Global renderer instance
IRenderer* g_Renderer = nullptr;

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
    // Convert screen-space to clip-space using orthographic projection
    gl_Position = uProjection * vec4(aPos, aDepth, 1.0);
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

out vec4 FragColor;

void main() {
    vec4 texel = texture(uTexture, vTexCoord);

    // Alpha test: discard fully transparent pixels (color key)
    if (uAlphaTest && texel.a < 0.5)
        discard;

    // Modulate texture with vertex color
    vec4 color = texel * vColor;

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
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
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

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
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

    glUniform1i(m_locAlphaTest, colorKey ? 1 : 0);
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

// --- 2D operations ---

void RendererGL::DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData) {
    if (!lpData || w <= 0 || h <= 0) return;

    // Convert 16-bit source to RGBA
    std::vector<uint32_t> rgba(w * h);
    uint16_t* src = (uint16_t*)lpData;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t pixel = src[row * srcW + col];
            rgba[row * w + col] = m_isRGB565 ? RGB565toRGBA(pixel) : RGB555toRGBA(pixel);
        }
    }

    // Upload to texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

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
    glUniform1i(m_locAlphaTest, 1); // Enable alpha test for color-keyed bitmaps
    glUniform1i(m_locFogEnabled, 0);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

void RendererGL::DrawText(int x, int y, const char* text, uint32_t color) {
    // Minimal text rendering — for now, this is a no-op.
    // Full text rendering requires a font atlas, which will be implemented
    // when SDL_ttf or a bitmap font system is added.
    // The D3D6 backend used GDI TextOut on the backbuffer DC, which has
    // no direct equivalent in OpenGL.
    (void)x; (void)y; (void)text; (void)color;
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
    fillVert(quad[1], (float)m_width, 0);
    fillVert(quad[2], (float)m_width, (float)m_height);
    fillVert(quad[3], 0, 0);
    fillVert(quad[4], (float)m_width, (float)m_height);
    fillVert(quad[5], 0, (float)m_height);

    // Draw without depth test, with blending, no texture
    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(m_locAlphaTest, 0);
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
