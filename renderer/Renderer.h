#pragma once
// SOURCEPORT: Abstract renderer interface for Carnivores 2 source port.
// The original engine submits pre-transformed, lit vertices (screen-space)
// with 16-bit RGB565/555 textures. All backends must accept this format.

#include <cstdint>

// Vertex format matching the original D3DTLVERTEX layout.
// All coordinates are in screen space (pre-transformed by the game's CPU).
struct RenderVertex {
    float sx, sy;       // Screen position
    float sz;           // Depth (for Z-buffer)
    float rhw;          // Reciprocal of homogeneous W
    uint32_t color;     // ARGB8888 diffuse color (vertex lighting + alpha)
    uint32_t specular;  // ARGB8888 specular (alpha channel = fog factor)
    float tu, tv;       // Texture coordinates
};

// Blend mode constants (matching original D3D6 values used by the game)
enum RenderBlendMode {
    BLEND_ZERO          = 1,
    BLEND_ONE           = 2,
    BLEND_SRCALPHA      = 5,
    BLEND_INVSRCALPHA   = 6,
};

// Abstract renderer interface.
// Implementations: RendererD3D6 (original), RendererGL (new OpenGL 3.3).
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // --- Lifecycle ---
    virtual bool Init(void* windowHandle, int width, int height) = 0;
    virtual void Shutdown() = 0;

    // --- Frame management ---
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;   // Present/flip
    virtual void ClearBuffers() = 0;

    // --- Texture management ---
    // Upload a 16-bit (RGB565 or RGB555) texture. Returns a handle.
    // w, h are dimensions. lpData points to w*h WORDs.
    virtual void SetTexture(void* lpData, int w, int h) = 0;

    // --- Vertex buffer (main buffer — terrain, models) ---
    virtual RenderVertex* LockVertexBuffer() = 0;
    virtual void UnlockAndDrawTriangles(int triCount1, int triCount2) = 0;
    // triCount1 = normal triangles, triCount2 = color-keyed/alpha triangles

    // --- Geometry buffer (smaller — UI overlays, clipped geometry) ---
    virtual RenderVertex* LockGeometryBuffer() = 0;
    virtual void UnlockAndDrawGeometry(int vertexCount, bool colorKey) = 0;

    // --- Render state ---
    virtual void SetRenderStates(bool zWrite, int dstBlend) = 0;
    virtual void SetFogEnabled(bool enabled) = 0;
    virtual void SetFogColor(uint32_t color) = 0;
    virtual void SetLinearFilter(bool enabled) = 0;
    virtual void SetAlphaTest(bool enabled) = 0;
    virtual void SetZBufferEnabled(bool enabled) = 0;

    // --- 2D operations ---
    virtual void DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData) = 0;
    virtual void DrawText(int x, int y, const char* text, uint32_t color) = 0;
    virtual void DrawFullscreenRect(uint32_t argbColor) = 0;

    // --- Z-buffer control ---
    virtual void ClearZBuffer() = 0;
    virtual float GetDepthAt(int x, int y) = 0;

    // --- Screenshot ---
    virtual void CopyBackBuffer(void* dest, int x, int y, int w, int h) = 0;

    // --- Display info ---
    virtual bool IsRGB565() const = 0;
    virtual int  GetTextureMemory() const = 0;
};

// Global renderer instance — set during initialization
extern IRenderer* g_Renderer;
