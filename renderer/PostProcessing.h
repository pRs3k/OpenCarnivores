#pragma once

#include <cstdint>
#include <vector>

// SOURCEPORT: Post-processing pipeline for shader effects (bloom, tone mapping, SSR, shadows, etc.)
// All effects are disabled by default and user-configurable via menu/config.

class FramebufferObject {
public:
    FramebufferObject() : fbo(0), colorTex(0), depthTex(0), width(0), height(0) {}
    ~FramebufferObject();

    // Create FBO with color (GL_RGBA8) and optional depth texture
    bool Create(int w, int h, bool includeDepth = true);

    void Bind() const;
    void Unbind() const;
    void Clear(float r = 0.f, float g = 0.f, float b = 0.f, float a = 1.f);
    void BlitTo(FramebufferObject* dst) const;  // Blit color to another FBO or screen (if dst==null)

    uint32_t GetColorTexture() const { return colorTex; }
    uint32_t GetDepthTexture() const { return depthTex; }
    uint32_t GetFramebuffer() const { return fbo; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    uint32_t fbo, colorTex, depthTex;
    int width, height;
};

enum class CompositionMode {
    REPLACE,           // dst = src
    ADDITIVE,          // dst += src
    ALPHA_BLEND,       // dst = src*a + dst*(1-a)
    SCREEN,            // dst = 1 - (1-dst)*(1-src)  (lighter)
    MULTIPLY,          // dst = dst * src
    OVERLAY,           // depends on src brightness
};

class PostProcessingPipeline {
public:
    PostProcessingPipeline();
    ~PostProcessingPipeline();

    // Initialize pipeline for given resolution
    bool Initialize(int screenWidth, int screenHeight);

    // Resize all FBOs if screen resolution changes
    void Resize(int screenWidth, int screenHeight);

    // Bind the scene capture FBO; call once at start of frame before DrawScene.
    // Has no effect when in VR mode (VR uses its own per-eye FBOs).
    void BeginCapture();

    // Apply post-processing effects and composite to screen.
    // Call after the full frame (scene + HUD) is rendered.
    void ApplyEffects();

    // SOURCEPORT: Simple post-process effect - renders fullscreen quad with shader
    // Copies current back buffer to temp texture, applies shader, renders to screen
    void ApplySimpleEffect(const char* shaderPath, float intensity = 1.0f);

    bool IsInitialized() const { return m_initialized; }

    // Load/reload post-processing shaders from disk
    void HotReloadShaders();

    // Enable/disable individual effects
    void SetEffectEnabled(const char* effectName, bool enabled);
    bool IsEffectEnabled(const char* effectName) const;

    // Set effect-specific parameters (intensity, exposure, etc.)
    void SetEffectParameter(const char* effectName, const char* paramName, float value);

    // Get access to underlying FBOs for debugging
    FramebufferObject* GetSourceFBO() { return &m_sourceFBO; }
    FramebufferObject* GetIntermediate1() { return &m_intermediate1; }
    FramebufferObject* GetIntermediate2() { return &m_intermediate2; }

    // SOURCEPORT: Shadow map accessors for depth rendering and sampling
    FramebufferObject* GetShadowMap(int cascade) { return cascade >= 0 && cascade < SHADOW_CASCADES ? &m_shadowMaps[cascade] : nullptr; }
    const float* GetLightDirection() const { return m_lightDirection; }
    void SetLightDirection(float x, float y, float z) { m_lightDirection[0]=x; m_lightDirection[1]=y; m_lightDirection[2]=z; }
    const float* GetLightViewMatrix(int cascade) const { return cascade >= 0 && cascade < SHADOW_CASCADES ? m_lightViewMatrix[cascade] : nullptr; }
    const float* GetLightProjMatrix(int cascade) const { return cascade >= 0 && cascade < SHADOW_CASCADES ? m_lightProjMatrix[cascade] : nullptr; }
    void UpdateShadowMatrices(const float* cameraPos, const float* cameraDir, float fovY, float aspect);

    // Debug visualization of shadow maps
    void SetDebugShadowVisualization(int cascade) { m_debugShadowCascade = cascade; }
    int GetDebugShadowVisualization() const { return m_debugShadowCascade; }

private:
    struct EffectShader {
        const char* name;
        uint32_t program;
        bool enabled;
        bool loaded;
    };

    FramebufferObject m_sourceFBO;       // Main scene FBO (color + depth)
    FramebufferObject m_intermediate1;   // Work FBO for effect chains
    FramebufferObject m_intermediate2;   // Secondary work FBO

    // Bloom pipeline FBOs
    FramebufferObject m_bloomDownsampled;  // 1/4 resolution for bloom threshold/blur
    FramebufferObject m_bloomBlurH;        // Horizontal blur result
    FramebufferObject m_bloomBlurV;        // Vertical blur result (final bloom)

    // SOURCEPORT: Shadow mapping (Phase 2.1 - cascaded PCF shadows)
    static constexpr int SHADOW_CASCADES = 3;
    static constexpr int SHADOW_MAP_SIZE = 2048;
    FramebufferObject m_shadowMaps[SHADOW_CASCADES];  // Depth-only FBOs for light POV rendering
    float m_cascadeDistances[SHADOW_CASCADES];        // Distance ranges for each cascade
    float m_lightDirection[3] = {0.6f, 0.6f, 0.2f};   // Directional light direction (world space)
    float m_lightViewMatrix[SHADOW_CASCADES][16];     // Light view matrix per cascade
    float m_lightProjMatrix[SHADOW_CASCADES][16];     // Light projection matrix per cascade
    int m_debugShadowCascade = -1;                    // -1 = disabled, 0-2 = visualize cascade

    std::vector<EffectShader> m_effects;
    uint32_t m_fsQuadVao, m_fsQuadVbo;   // Full-screen quad for post-processing

    int m_width, m_height;
    bool m_initialized;
    bool m_captureActive = false;

    // Utility: render full-screen quad with given shader
    void RenderFullscreenQuad(uint32_t shaderProgram);

    // Load shader from file (supports hot-reload)
    uint32_t LoadPostProcessShader(const char* shaderName);

    // Compose two FBOs with given blend mode
    void Compose(FramebufferObject* src, FramebufferObject* dst, CompositionMode mode);
};
