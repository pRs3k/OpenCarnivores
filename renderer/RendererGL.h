#pragma once
// SOURCEPORT: OpenGL 3.3 Core renderer backend for Carnivores 2.
// Replaces Direct3D 6 execute buffers with modern GL.
// Accepts pre-transformed screen-space vertices from the game's CPU pipeline.

#include "Renderer.h"
#include <glad/gl.h>
#include <SDL.h>
#include <unordered_map>
#include <vector>

// Forward declaration
class PostProcessingPipeline;

class RendererGL : public IRenderer {
public:
    RendererGL();
    ~RendererGL() override;

    bool Init(void* windowHandle, int width, int height) override;
    void Shutdown() override;

    void BeginFrame() override;
    void EndFrame() override;
    void ClearBuffers() override;
    void RestoreEngineGLState() override;

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
    void SetDepthMask(bool write);
    void SetBrightness(float b);   // SOURCEPORT: live brightness uniform (1.0=neutral)
    // SOURCEPORT: override uProjection with a caller-supplied column-major mat4.
    // Used by the VR stereo path to switch between per-eye and flat projections
    // without calling BeginFrame() (which would also reset the viewport cache).
    void UpdateProjection(const float* mat16);
    // SOURCEPORT: stencil modes for weapon/overlay isolation.
    // 0=off, 1=write (mark drawn pixels with ref=1), 2=test (only draw where ref==1).
    void SetStencilMode(int mode);
    void SetHUDMode(bool enabled);
    // SOURCEPORT: debug visualization mode. 0=normal, 1=PBR off, 2=PBR magenta.
    void SetDebugMode(int mode);
    // SOURCEPORT: upload screen-space fog parameters (call once per frame after CAMERAINFOG update).
    void SetFogParams(float fogYBeginGU, float fogTransp, float fogLimit,
                      float cameraY, int cameraInFog,
                      float videoCY, float winH, float cameraH);

    // SOURCEPORT: bind a PBR material's supplementary maps (normal/MR/AO) to
    // texture units 1/2/3 and enable the PBR branch in the fragment shader.
    // Pass nullptr to fall back to the retail Lambert path. Call this AFTER
    // the base-color texture has been bound to unit 0.
    void BindMaterial(const void* material);   // Materials::Material*

    // SOURCEPORT: switch the active GL program to a mod-supplied custom
    // shader (from a .material file). Pass nullptr to restore the engine's
    // default program. Because GL uniforms are per-program and persist, the
    // engine's fog/alpha/brightness/PBR state is preserved across the swap.
    void BindCustomMaterial(const void* customMaterial);  // CustomMaterials::Material*

    void DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData, bool colorKey = true, int srcH = 0, const void* overrideKey = nullptr) override;
    void DrawText(int x, int y, const char* text, uint32_t color) override;
    int  MeasureText(const char* text);     // SOURCEPORT: width of text as rendered by DrawText
    void DrawTextMed(int x, int y, const char* text, uint32_t color); // SOURCEPORT: fnt_Midd style (weight=550, 16px/7px at 600p)
    int  MeasureTextMed(const char* text);  // SOURCEPORT: width of text as rendered by DrawTextMed
    void DrawTextBig(int x, int y, const char* text, uint32_t color); // SOURCEPORT: ~36px at 600p heading weight
    int  MeasureTextBig(const char* text);  // SOURCEPORT: width of text as rendered by DrawTextBig
    void DrawFullscreenRect(uint32_t argbColor) override;
    void FillRect(int x, int y, int w, int h, uint32_t argbColor) override;

    void ClearZBuffer() override;
    float GetDepthAt(int x, int y) override;

    void CopyBackBuffer(void* dest, int x, int y, int w, int h) override;

    bool IsRGB565() const override;
    int  GetTextureMemory() const override;

    // SDL window accessor for the platform layer
    SDL_Window* GetWindow() const { return m_window; }

    // Returns a 1x1 white RGBA texture for untextured (vertex-color-only) rendering
    GLuint GetWhiteTexture();

    // SOURCEPORT: post-processing pipeline accessor (overrides IRenderer)
    void* GetPostProcessingPipeline() const override { return m_postProcessingPipeline; }

    // Discard all cached GPU textures so they are re-uploaded on next use.
    // Call before LoadResources() whenever texture pixel data changes in-place
    // (e.g. BrightenTexture rewrites night/day mode pixels into the same buffers).
    void InvalidateTextureCache();

    // SOURCEPORT: query maximum anisotropic filtering level supported by hardware
    int GetMaxAnisotropy() const { return m_maxAnisotropy; }

    // SOURCEPORT: legacy cascade shadow API (kept for old call sites, superseded by Path B)
    void BeginShadowCascade(int cascade);
    void EndShadowPass();
    void GetLightTransform(int cascade, float* outViewMatrix, float* outProjMatrix);
    void SetDepthOnlyMode(bool enabled);

    // SOURCEPORT: Path B world shadow mapping.
    // Call SetCameraWorldUniforms once per frame (from Hunt2 which has the camera globals),
    // then BeginWorldShadowPass / draw / EndWorldShadowPass before the main scene.
    void SetCameraWorldUniforms(float vcx, float vcy, float cw, float ch,
                                float wx, float wy, float wz,
                                float ca, float sa, float cb, float sb, float cg, float sg);
    void BeginWorldShadowPass();
    void EndWorldShadowPass();
    // SOURCEPORT: must be called before BeginWorldShadowPass each frame so the
    // light matrix matches the game sun (derived from SunShadowK in renderd3d).
    void SetSunDirection(float x, float y, float z);

    // Shadow mode: 0=none, 1=dinos_only (future), 2=full
    void  SetShadowMode(int mode)     { m_shadowMode = mode; }
    int   GetShadowMode() const       { return m_shadowMode; }
    void  SetShadowStrength(float s)  { m_shadowStrength = s; }
    // SOURCEPORT: shadow ortho half-extent (GU); set from ctViewR*256 each frame.
    void  SetShadowRange(float r)     { m_shadowRange = r; }
    // SOURCEPORT: lets game-side render functions (RenderModelsList) skip
    // during the depth-only pass to avoid double-shadowing dino models.
    bool  IsShadowPassActive() const  { return m_shadowPassActive; }
    // SOURCEPORT: terrain shadow path reads these to transform camera-space→world-space.
    const float* GetCamToWorld()    const { return m_camToWorld; }
    const float* GetCameraWorldPos() const { return m_cameraWorldPos; }
    // SOURCEPORT: world-space shadow batch — submit a triangle with world-space
    // XYZ coordinates directly to the depth map, bypassing camera-space screen
    // reconstruction.  Used for objects behind the player (tree canopy when the
    // player turns away).  Call FlushWorldSpaceShadow() before changing textures.
    void  FlushWorldSpaceShadow();
    void  SubmitWorldSpaceShadowTriangle(
              float x0, float y0, float z0,
              float x1, float y1, float z1,
              float x2, float y2, float z2,
              float u0, float v0, float u1, float v1, float u2, float v2,
              bool alphaTest);

    // SOURCEPORT: Post-process overlay (incremental pipeline, Step 1+)
    bool m_postOverlayEnabled = false;
    void RunPostOverlay();

    void EnablePostOverlay(bool enable)   { m_postOverlayEnabled = enable; }
    bool IsPostOverlayEnabled() const     { return m_postOverlayEnabled; }
    void SetBloomThreshold(float t)       { m_bloomThreshold = t; }
    void SetBloomIntensity(float i)       { m_bloomIntensity = i; }

    // SOURCEPORT: tone mapping mode (applied in composite pass after bloom)
    // 0=off, 1=ACES filmic, 2=Reinhard
    void SetToneMappingMode(int mode) { m_toneMappingMode = mode; }
    int  GetToneMappingMode() const   { return m_toneMappingMode; }
    void SetExposure(float e)         { m_exposure = e; }

    // SOURCEPORT: color grading (contrast, saturation, lift, gain)
    void SetColorGradingEnabled(bool e)      { m_cgEnabled = e; }
    bool IsColorGradingEnabled() const       { return m_cgEnabled; }
    void SetCGSaturation(float s)            { m_cgSaturation = s; }
    void SetCGContrast(float c)              { m_cgContrast = c; }
    void SetCGLift(float r, float g, float b){ m_cgLift[0]=r; m_cgLift[1]=g; m_cgLift[2]=b; }
    void SetCGGain(float r, float g, float b){ m_cgGain[0]=r; m_cgGain[1]=g; m_cgGain[2]=b; }

    // SOURCEPORT: unsharp mask sharpening (0=off, ~0.6 = crisp, ~1.5 = max before halos)
    void  SetSharpenStrength(float s)  { m_sharpenStrength = s; }
    float GetSharpenStrength() const   { return m_sharpenStrength; }

private:
    void CompileShaders();
    void CreateBuffers();
    GLuint UploadTexture16(void* data, int w, int h);
    void FlushBatch(RenderVertex* verts, int vertexCount, bool alphaTest);

    // SOURCEPORT: supersampling FBO management
    void CreateSSAFramebuffer(int width, int height);
    void DestroySSAFramebuffer();
    void BindSSAFramebuffer();
    void UnbindAndDownscaleSSA();

    SDL_Window*   m_window = nullptr;
    SDL_GLContext  m_glContext = nullptr;
    int m_width = 0, m_height = 0;

    // Shader program
    GLuint m_shaderProgram = 0;
    GLuint m_depthShaderProgram = 0;    // SOURCEPORT: Depth-only shader for shadow pass
    GLint  m_locProjection = -1;
    GLint  m_locTexture = -1;
    GLint  m_locFogEnabled = -1;
    GLint  m_locFogColor = -1;
    GLint  m_locAlphaTest  = -1;
    GLint  m_locHUDMode    = -1;
    GLint  m_locBrightness = -1;
    GLint  m_locPBR             = -1;
    GLint  m_locMetallicFactor  = -1;
    GLint  m_locRoughnessFactor = -1;
    GLint  m_locSunDirView      = -1;
    GLint  m_locDebugMode       = -1;
    bool   m_pbrActive          = false;
    // SOURCEPORT: screen-space fog uniform locations
    GLint  m_locFogYBeginGU     = -1;
    GLint  m_locFogTransp       = -1;
    GLint  m_locFogLimit        = -1;
    GLint  m_locFogCameraY      = -1;
    GLint  m_locCameraInFog     = -1;
    GLint  m_locFogVideoCY      = -1;
    GLint  m_locFogWinH         = -1;
    GLint  m_locFogCameraH      = -1;

    // SOURCEPORT: cached projection matrix for CustomMaterials::Apply. Updated
    // every BeginFrame alongside the default-program uProjection uniform.
    float  m_projMatrix[16]     = {0};
    bool   m_customProgramActive = false;

    // Vertex buffer objects
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    // Texture cache: maps content hash → GL texture ID
    struct TexCacheEntry {
        GLuint texId;
        int lastUsed;
    };
    std::unordered_map<uint64_t, TexCacheEntry> m_texCache;
    GLuint m_currentTexture = 0;
    int    m_frameCounter = 0;

    // Bitmap upload texture (reused for DrawBitmap/DrawText)
    GLuint m_bitmapTexture = 0;

    // 1x1 white texture for untextured (vertex-color-only) geometry
    GLuint m_whiteTexture = 0;

    // SOURCEPORT: cached maximum anisotropic filtering level from hardware
    int m_maxAnisotropy = 1;

    // SOURCEPORT: supersampling FBO for flatscreen (render at higher res, downscale to window)
    GLuint m_ssaFBO = 0;            // framebuffer object for supersampled rendering
    GLuint m_ssaTexture = 0;        // color texture for FBO
    GLuint m_ssaDepth = 0;          // depth renderbuffer for FBO
    int    m_ssaWidth = 0;          // scaled rendering width
    int    m_ssaHeight = 0;         // scaled rendering height
    float  m_ssaScale = 1.0f;       // current scale factor (for comparison)

    // Vertex staging buffers
    static constexpr int MAX_MAIN_VERTICES = 4096 * 3;
    static constexpr int MAX_GEOM_VERTICES = 4096 * 3;  // SOURCEPORT: expanded for DrawTerrainGL bucketed batches
    RenderVertex m_mainBuffer[MAX_MAIN_VERTICES];
    RenderVertex m_geomBuffer[MAX_GEOM_VERTICES];


    // SOURCEPORT: Path B world shadow map FBO (depth-only, standard convention)
    GLuint m_worldShadowFBO      = 0;
    GLuint m_worldShadowDepthTex = 0;
    static constexpr int WORLD_SHADOW_SIZE = 2048;
    int    m_shadowMode       = 0;      // 0=none, 2=full
    float  m_shadowStrength   = 0.5f;
    float  m_shadowRange      = 16000.f; // SOURCEPORT: ortho half-extent (GU); updated from ctViewR
    float  m_shadowBiasNDC    = 0.001f;  // SOURCEPORT: computed in BeginWorldShadowPass, uploaded in End
    float  m_worldLightMatrix[16] = {}; // combined light view-proj (column-major)
    // Camera-to-world uniforms — filled by SetCameraWorldUniforms each frame
    float  m_camToWorld[9]      = {};   // R^T column-major mat3
    float  m_cameraWorldPos[3]  = {};
    float  m_unifVideoCX = 0.f, m_unifVideoCY = 0.f;
    float  m_unifCameraW = 1.f, m_unifCameraH = 1.f;
    // Sun direction (world space, pointing TOWARD the sun)
    float  m_sunDirWorld[3] = {0.4f, 0.8f, 0.3f};
    // SOURCEPORT: true while the depth-only shadow pass owns DrawScene().
    // UnlockAndDraw* re-asserts m_depthShaderProgram before each draw so that
    // BindCustomMaterial(nullptr) / UpdateProjection() etc. cannot stomp it.
    bool   m_shadowPassActive      = false;
    bool   m_shadowGeomSuppressed  = false; // SOURCEPORT: true while zWrite=false (water/blend) during depth pass
    GLint  m_depthLocAlphaTest     = -1;    // uAlphaTest location in m_depthShaderProgram

    // SOURCEPORT: world-space shadow batch (ws_depth.vert path)
    GLuint m_wsDepthProgram  = 0;
    GLuint m_wsVAO           = 0;
    GLuint m_wsVBO           = 0;
    struct WSShadowVert { float x, y, z, u, v; };
    static constexpr int MAX_WS_SHADOW_VERTS = MAX_GEOM_VERTICES;
    WSShadowVert m_wsShadowBuffer[MAX_WS_SHADOW_VERTS];
    int          m_wsShadowCount = 0;
    bool         m_wsAlphaTest   = false;

    // SOURCEPORT: post-process effect state (driven by ShaderPack::Apply)
    float    m_bloomThreshold  = 0.78f;
    float    m_bloomIntensity  = 1.5f;
    int      m_toneMappingMode = 0;   // 0=off, 1=ACES, 2=Reinhard
    float    m_exposure        = 1.0f;

    // SOURCEPORT: sharpen state
    float    m_sharpenStrength = 0.0f;

    // SOURCEPORT: color grading state
    bool     m_cgEnabled     = false;
    float    m_cgSaturation  = 1.2f;               // slight boost by default
    float    m_cgContrast    = 1.1f;               // slight punch by default
    float    m_cgLift[3]     = {0.0f, 0.0f, 0.0f}; // shadow tint (neutral)
    float    m_cgGain[3]     = {1.0f, 1.0f, 1.0f}; // highlight scale (neutral)

    // Current render state
    bool     m_fogEnabled = false;
    float    m_fogColor[4] = {0};
    bool     m_linearFilter = true;
    bool     m_alphaTestEnabled = false;
    bool     m_zBufferEnabled = true;
    bool     m_zWriteEnabled = true;
    int      m_dstBlend = BLEND_INVSRCALPHA;
    bool     m_isRGB565 = true;
    float    m_brightness = 1.0f;

    // Fullscreen quad VAO for overlays
    GLuint m_fsQuadVao = 0;
    GLuint m_fsQuadVbo = 0;

    // SOURCEPORT: Post-processing pipeline for bloom, tone mapping, SSR, shadows
    PostProcessingPipeline* m_postProcessingPipeline = nullptr;
};
