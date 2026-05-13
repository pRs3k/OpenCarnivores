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

    // Apply post-processing effects to the current framebuffer
    // Should be called after scene rendering, before UI/weapon rendering
    void ApplyEffects();

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

    std::vector<EffectShader> m_effects;
    uint32_t m_fsQuadVao, m_fsQuadVbo;   // Full-screen quad for post-processing

    int m_width, m_height;
    bool m_initialized;

    // Utility: render full-screen quad with given shader
    void RenderFullscreenQuad(uint32_t shaderProgram);

    // Load shader from file (supports hot-reload)
    uint32_t LoadPostProcessShader(const char* shaderName);

    // Compose two FBOs with given blend mode
    void Compose(FramebufferObject* src, FramebufferObject* dst, CompositionMode mode);
};
