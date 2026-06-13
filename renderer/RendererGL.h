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
    void BeginWorldShadowPass(int cascade = 0);
    void EndWorldShadowPass(int cascade = 0);
    // SOURCEPORT: must be called before BeginWorldShadowPass each frame so the
    // light matrix matches the game sun (derived from SunShadowK in renderd3d).
    void SetSunDirection(float x, float y, float z);

    // SOURCEPORT: CSM — public so Hunt2.cpp loop can use RendererGL::NUM_SHADOW_CASCADES.
    static constexpr int NUM_SHADOW_CASCADES_PUB = 3;

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

    // SOURCEPORT: apply post-process effects (bloom, tone mapping, color grading) to the
    // current back-buffer.  Call after DrawScene() but before any HUD/UI rendering.
    void ApplyPostProcess();
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

    // SOURCEPORT: screen-space god rays (crepuscular rays). Sun position is projected
    // from m_sunDirWorld using the camera uniforms cached by SetCameraWorldUniforms;
    // depth buffer captured per-frame identifies sky pixels as the light source mask.
    void SetGodRaysEnabled(bool e)             { m_godRaysEnabled = e; }
    bool GetGodRaysEnabled() const             { return m_godRaysEnabled; }
    void SetGodRayIntensity(float i)           { m_godRayIntensity = i; }
    void SetGodRayDensity(float d)             { m_godRayDensity = d; }
    void SetGodRayDecay(float d)               { m_godRayDecay = d; }
    void SetGodRayColor(float r, float g, float b) { m_godRayColor[0]=r; m_godRayColor[1]=g; m_godRayColor[2]=b; }

    // SOURCEPORT: volumetric height fog — depth-aware exponential height fog with
    // Mie-style forward scattering toward the sun. Applied full-res to the captured
    // scene before bloom extraction so bloom/tonemap operate on the fogged image.
    void SetHeightFogEnabled(bool e)    { m_heightFogEnabled = e; }
    bool GetHeightFogEnabled() const    { return m_heightFogEnabled; }
    void SetHeightFogDensity(float d)   { m_heightFogDensity = d; }
    void SetHeightFogFalloff(float f)   { m_heightFogFalloff = f; }
    void SetHeightFogSunPower(float p)  { m_heightFogSunPower = p; }
    void SetHeightFogColor(float r, float g, float b)    { m_heightFogColor[0]=r;    m_heightFogColor[1]=g;    m_heightFogColor[2]=b; }
    void SetHeightFogSunColor(float r, float g, float b) { m_heightFogSunColor[0]=r; m_heightFogSunColor[1]=g; m_heightFogSunColor[2]=b; }
    // Per-frame world params from game globals: fog anchor height (lowest terrain,
    // MapMinY*ctHScale — same horizon reference RenderSkyPlane uses) and view radius
    // in GU (ctViewR*256), which bounds the ray length used for sky pixels.
    void SetHeightFogWorldParams(float anchorY, float viewRangeGU) { m_fogAnchorY = anchorY; m_fogViewRange = viewRangeGU; }

    // SOURCEPORT: SSAO — depth-only screen-space ambient occlusion (Alchemy-style),
    // computed half-res from the captured depth, bilateral-blurred, and multiplied
    // into the scene before height fog in the same full-res scene pass.
    void SetSSAOEnabled(bool e)      { m_ssaoEnabled = e; }
    bool GetSSAOEnabled() const      { return m_ssaoEnabled; }
    void SetSSAOStrength(float s)    { m_ssaoStrength = s; }
    void SetSSAORadius(float r)      { m_ssaoRadius = r; }
    void SetSSAOIntensity(float i)   { m_ssaoIntensity = i; }
    void SetSSAODebug(bool d)        { m_ssaoDebug = d; }

    // SOURCEPORT: water material. BeginWaterPass captures the scene colour+depth from
    // the currently bound framebuffer (the underwater world is fully rendered by the
    // time RenderWater runs) and enables the water branch in basic.frag; EndWaterPass
    // disables it. `allow` is false when underwater or in VR — the retro water path
    // then renders unchanged.
    void BeginWaterPass(bool allow, float timeSec);
    void EndWaterPass();
    void SetWaterFXEnabled(bool e)        { m_waterFXEnabled = e; }
    bool GetWaterFXEnabled() const        { return m_waterFXEnabled; }
    void SetWaterWaveStrength(float s)    { m_waterWaveStrength = s; }
    void SetWaterClarity(float c)         { m_waterClarity = c; }
    void SetWaterDeepColor(float r, float g, float b) { m_waterDeepColor[0]=r; m_waterDeepColor[1]=g; m_waterDeepColor[2]=b; }
    void SetWaterFoamWidth(float w)       { m_waterFoamWidth = w; }
    void SetWaterReflectivity(float r)    { m_waterReflectivity = r; }

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
    GLint  m_locSunDirWorld     = -1;  // SOURCEPORT: world-space sun direction for PBR
    GLint  m_locParallaxScale   = -1;  // SOURCEPORT: parallax height scale (0=off)
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

    // SOURCEPORT: cached per-frame uniform locations — world-pos reconstruction
    // and shadow sampling.  Populated once in CompileShaders; used every frame.
    GLint  m_locVideoCX         = -1;
    GLint  m_locVideoCY         = -1;
    GLint  m_locCameraW         = -1;
    GLint  m_locCameraH         = -1;
    GLint  m_locCameraPos       = -1;
    GLint  m_locCamToWorld      = -1;
    GLint  m_locShadowMapArray  = -1;
    GLint  m_locLightSpaceArr   = -1;  // uLightSpaceArr[0] (upload 3 matrices)
    GLint  m_locShadowStrength  = -1;
    GLint  m_locCascadeBias     = -1;
    GLint  m_locCascadeSplits   = -1;
    // SOURCEPORT: cached depth-shader locations (m_depthShaderProgram), per cascade
    GLint  m_depthLocLightSpace = -1;
    GLint  m_depthLocVideoCX    = -1;
    GLint  m_depthLocVideoCY    = -1;
    GLint  m_depthLocCameraW    = -1;
    GLint  m_depthLocCameraH    = -1;
    GLint  m_depthLocCameraPos  = -1;
    GLint  m_depthLocCamToWorld = -1;
    GLint  m_depthLocTexture    = -1;
    // SOURCEPORT: cached world-space depth-shader locations (m_wsDepthProgram)
    GLint  m_wsLocLightSpace    = -1;
    GLint  m_wsLocAlphaTest     = -1;

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
    // SOURCEPORT: CSM — 3 cascades replace the single shadow map.
    static constexpr int   NUM_SHADOW_CASCADES = 3;
    static constexpr int   CASCADE_SHADOW_SIZE = 2048;  // per-cascade depth map resolution
    // Ortho half-extents as fractions of m_shadowRange.  C0=10%, C1=35%, C2=100%.
    // 1:3.5:2.9 quality ratios (vs old 1:4:4) — gentler jumps reduce visible seams.
    static constexpr float CASCADE_RANGE_FRACS[3] = {0.10f, 0.35f, 1.0f};
    RenderVertex m_mainBuffer[MAX_MAIN_VERTICES];
    RenderVertex m_geomBuffer[MAX_GEOM_VERTICES];


    // SOURCEPORT: CSM — 3-cascade shadow map.
    GLuint m_cascadeFBO[NUM_SHADOW_CASCADES] = {};  // one FBO per cascade
    GLuint m_cascadeDepthTex = 0;                   // GL_TEXTURE_2D_ARRAY, NUM_SHADOW_CASCADES layers × CASCADE_SHADOW_SIZE²
    float  m_cascadeLightMatrix[NUM_SHADOW_CASCADES][16] = {}; // light view-proj per cascade
    float  m_cascadeBiasNDC[NUM_SHADOW_CASCADES] = {};          // per-cascade NDC bias
    float  m_cascadeRanges[NUM_SHADOW_CASCADES] = {};            // ortho half-extents in GU
    int    m_currentCascade = 0;                                 // cascade being rendered
    int    m_shadowMode       = 0;      // 0=none, 2=full
    float  m_shadowStrength   = 0.5f;
    // SOURCEPORT: fixed shadow range (GU). Covers ctViewR up to 250 (max = (250+2)*256 = 64512).
    // Previously set dynamically from ctViewR each frame; that caused the ortho projection
    // scale to change continuously, which reshuffled which world region each shadow texel
    // covers and made shadows visibly resize/flicker. A fixed value keeps the projection
    // matrix identical every frame (only translation changes) so the texel-snapping fix works.
    float  m_shadowRange      = 65536.f;
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
    GLint  m_locWorldShadow        = -1;    // SOURCEPORT: uWorldShadow in m_shaderProgram — cached to avoid per-frame lookups
    bool   m_shadowsRendered       = false; // SOURCEPORT: true after EndWorldShadowPass(); cleared by BeginFrame()

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

    // SOURCEPORT: god ray state (driven by ShaderPack::Apply)
    bool     m_godRaysEnabled  = false;
    float    m_godRayIntensity = 0.5f;   // overall additive strength
    float    m_godRayDensity   = 0.9f;   // ray march length toward sun (0..1 of pixel→sun distance)
    float    m_godRayDecay     = 0.96f;  // per-sample falloff along the ray
    float    m_godRayColor[3]  = {1.0f, 0.92f, 0.75f}; // warm sunlight

    // SOURCEPORT: height fog state (driven by ShaderPack::Apply + per-frame world params)
    bool     m_heightFogEnabled  = false;
    float    m_heightFogDensity  = 0.00012f;  // extinction per GU at anchor height
    float    m_heightFogFalloff  = 0.0003f;   // density halves every ~2300 GU (~17 m) above anchor
    float    m_heightFogSunPower = 8.0f;      // forward-scattering exponent
    float    m_heightFogColor[3]    = {0.65f, 0.72f, 0.80f}; // cool morning haze
    float    m_heightFogSunColor[3] = {1.0f, 0.85f, 0.65f};  // warm glow toward the sun
    float    m_fogAnchorY   = 0.0f;       // lowest terrain height (GU)
    float    m_fogViewRange = 12000.0f;   // view radius (GU); sky rays use 2× this

    // SOURCEPORT: SSAO state (driven by ShaderPack::Apply)
    bool     m_ssaoEnabled   = false;
    float    m_ssaoStrength  = 0.7f;   // how much AO darkens the scene (0..1)
    float    m_ssaoRadius    = 120.0f; // world-space sample radius (GU, ~0.9 m)
    float    m_ssaoIntensity = 1.2f;   // occlusion gain before clamping
    bool     m_ssaoDebug     = false;  // visualize the blurred AO buffer

    // SOURCEPORT: water material state (driven by ShaderPack::Apply)
    bool     m_waterFXEnabled     = false;
    float    m_waterWaveStrength  = 0.35f;   // wave normal strength
    float    m_waterClarity       = 0.0025f; // absorption per GU (~63% at 3 m)
    float    m_waterDeepColor[3]  = {0.07f, 0.18f, 0.22f};
    float    m_waterFoamWidth     = 50.0f;   // shoreline foam band (GU, ~0.4 m)
    float    m_waterReflectivity  = 0.85f;
    bool     m_waterPassActive    = false;
    GLuint   m_waterSceneTex      = 0;       // scene colour captured before the water batch
    GLuint   m_waterDepthTex      = 0;       // scene depth captured before the water batch
    GLint    m_locWaterMode = -1, m_locWaterTime = -1, m_locWaterScene = -1, m_locWaterDepth = -1;
    GLint    m_locWaterScreenSize = -1, m_locWaterWave = -1, m_locWaterClarity = -1;
    GLint    m_locWaterDeepColor = -1, m_locWaterFoamWidth = -1, m_locWaterReflect = -1;

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
