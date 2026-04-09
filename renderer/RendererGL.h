#pragma once
// SOURCEPORT: OpenGL 3.3 Core renderer backend for Carnivores 2.
// Replaces Direct3D 6 execute buffers with modern GL.
// Accepts pre-transformed screen-space vertices from the game's CPU pipeline.

#include "Renderer.h"
#include <glad/gl.h>
#include <SDL.h>
#include <unordered_map>
#include <vector>

class RendererGL : public IRenderer {
public:
    RendererGL();
    ~RendererGL() override;

    bool Init(void* windowHandle, int width, int height) override;
    void Shutdown() override;

    void BeginFrame() override;
    void EndFrame() override;
    void ClearBuffers() override;

    void SetTexture(void* lpData, int w, int h) override;

    RenderVertex* LockVertexBuffer() override;
    void UnlockAndDrawTriangles(int triCount1, int triCount2) override;

    RenderVertex* LockGeometryBuffer() override;
    void UnlockAndDrawGeometry(int vertexCount, bool colorKey) override;

    void SetRenderStates(bool zWrite, int dstBlend) override;
    void SetFogEnabled(bool enabled) override;
    void SetFogColor(uint32_t color) override;
    void SetLinearFilter(bool enabled) override;
    void SetAlphaTest(bool enabled) override;
    void SetZBufferEnabled(bool enabled) override;

    void DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData) override;
    void DrawText(int x, int y, const char* text, uint32_t color) override;
    void DrawFullscreenRect(uint32_t argbColor) override;

    void ClearZBuffer() override;
    float GetDepthAt(int x, int y) override;

    void CopyBackBuffer(void* dest, int x, int y, int w, int h) override;

    bool IsRGB565() const override;
    int  GetTextureMemory() const override;

    // SDL window accessor for the platform layer
    SDL_Window* GetWindow() const { return m_window; }

    // Returns a 1x1 white RGBA texture for untextured (vertex-color-only) rendering
    GLuint GetWhiteTexture();


private:
    void CompileShaders();
    void CreateBuffers();
    GLuint UploadTexture16(void* data, int w, int h);
    void FlushBatch(RenderVertex* verts, int vertexCount, bool alphaTest);

    SDL_Window*   m_window = nullptr;
    SDL_GLContext  m_glContext = nullptr;
    int m_width = 0, m_height = 0;

    // Shader program
    GLuint m_shaderProgram = 0;
    GLint  m_locProjection = -1;
    GLint  m_locTexture = -1;
    GLint  m_locFogEnabled = -1;
    GLint  m_locFogColor = -1;
    GLint  m_locAlphaTest = -1;

    // Vertex buffer objects
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    // Texture cache: maps (cpuAddr, w, h) → GL texture ID
    struct TexCacheEntry {
        GLuint texId;
        int lastUsed;
    };
    std::unordered_map<uintptr_t, TexCacheEntry> m_texCache;
    GLuint m_currentTexture = 0;
    int    m_frameCounter = 0;

    // Bitmap upload texture (reused for DrawBitmap/DrawText)
    GLuint m_bitmapTexture = 0;

    // 1x1 white texture for untextured (vertex-color-only) geometry
    GLuint m_whiteTexture = 0;

    // Vertex staging buffers
    static constexpr int MAX_MAIN_VERTICES = 1024 * 3;
    static constexpr int MAX_GEOM_VERTICES = 400 * 3;
    RenderVertex m_mainBuffer[MAX_MAIN_VERTICES];
    RenderVertex m_geomBuffer[MAX_GEOM_VERTICES];

    // Current render state
    bool     m_fogEnabled = false;
    float    m_fogColor[4] = {0};
    bool     m_linearFilter = true;
    bool     m_alphaTestEnabled = false;
    bool     m_zBufferEnabled = true;
    bool     m_zWriteEnabled = true;
    int      m_dstBlend = BLEND_INVSRCALPHA;
    bool     m_isRGB565 = true;

    // Fullscreen quad VAO for overlays
    GLuint m_fsQuadVao = 0;
    GLuint m_fsQuadVbo = 0;
};
