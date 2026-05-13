#include "PostProcessing.h"
#include "glad/gl.h"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>

// Forward declarations from RendererGL
extern uint32_t CompileShaderProgram(const char* vertSource, const char* fragSource);
extern void PrintLog(char* msg);

// ─── FramebufferObject ────────────────────────────────────────────────────────

FramebufferObject::~FramebufferObject() {
    if (colorTex) glDeleteTextures(1, &colorTex);
    if (depthTex) glDeleteTextures(1, &depthTex);
    if (fbo) glDeleteFramebuffers(1, &fbo);
}

bool FramebufferObject::Create(int w, int h, bool includeDepth) {
    width = w;
    height = h;

    // SOURCEPORT: Safety check for invalid dimensions (can happen in VR initialization)
    if (w <= 0 || h <= 0) {
        PrintLog("ERROR: FBO create with invalid dimensions\n");
        return false;
    }

    // Create color texture
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Check for GL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        PrintLog("ERROR: FBO color texture creation failed\n");
        glDeleteTextures(1, &colorTex);
        colorTex = 0;
        return false;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create depth texture if requested
    if (includeDepth) {
        glGenTextures(1, &depthTex);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

        err = glGetError();
        if (err != GL_NO_ERROR) {
            PrintLog("ERROR: FBO depth texture creation failed\n");
            glDeleteTextures(1, &depthTex);
            glDeleteTextures(1, &colorTex);
            depthTex = 0;
            colorTex = 0;
            return false;
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // Create framebuffer
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    if (depthTex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        PrintLog("ERROR: FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        if (depthTex) glDeleteTextures(1, &depthTex);
        fbo = 0;
        colorTex = 0;
        depthTex = 0;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void FramebufferObject::Bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
}

void FramebufferObject::Unbind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FramebufferObject::Clear(float r, float g, float b, float a) {
    Bind();
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | (depthTex ? GL_DEPTH_BUFFER_BIT : 0));
}

void FramebufferObject::BlitTo(FramebufferObject* dst) const {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    if (dst) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
    } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);  // Screen
    }
    glBlitFramebuffer(0, 0, width, height, 0, 0, (dst ? dst->width : width), (dst ? dst->height : height),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

// ─── PostProcessingPipeline ───────────────────────────────────────────────────

PostProcessingPipeline::PostProcessingPipeline()
    : m_fsQuadVao(0), m_fsQuadVbo(0), m_width(0), m_height(0), m_initialized(false) {
}

PostProcessingPipeline::~PostProcessingPipeline() {
    if (m_fsQuadVao) glDeleteVertexArrays(1, &m_fsQuadVao);
    if (m_fsQuadVbo) glDeleteBuffers(1, &m_fsQuadVbo);
    for (auto& eff : m_effects) {
        if (eff.program) glDeleteProgram(eff.program);
    }
}

bool PostProcessingPipeline::Initialize(int screenWidth, int screenHeight) {
    m_width = screenWidth;
    m_height = screenHeight;

    // Create FBOs
    if (!m_sourceFBO.Create(m_width, m_height, true)) return false;
    if (!m_intermediate1.Create(m_width, m_height, false)) return false;
    if (!m_intermediate2.Create(m_width, m_height, false)) return false;

    // Create bloom pipeline FBOs at 1/4 resolution
    int bloomW = m_width / 2;
    int bloomH = m_height / 2;
    if (!m_bloomDownsampled.Create(bloomW, bloomH, false)) return false;
    if (!m_bloomBlurH.Create(bloomW, bloomH, false)) return false;
    if (!m_bloomBlurV.Create(bloomW, bloomH, false)) return false;

    // Create fullscreen quad
    float quadVertices[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };

    glGenVertexArrays(1, &m_fsQuadVao);
    glGenBuffers(1, &m_fsQuadVbo);
    glBindVertexArray(m_fsQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fsQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // SOURCEPORT: Register placeholder effects for Phase 2
    // Actual effects will be loaded from shaders/postprocess/*.frag
    m_effects.push_back({ "shadows", 0, false, false });
    m_effects.push_back({ "bloom", 0, false, false });
    m_effects.push_back({ "tonemap", 0, false, false });
    m_effects.push_back({ "ssr", 0, false, false });

    m_initialized = true;
    PrintLog("[PostProcessing] Pipeline initialized\n");
    return true;
}

void PostProcessingPipeline::Resize(int screenWidth, int screenHeight) {
    if (m_width == screenWidth && m_height == screenHeight) return;

    m_width = screenWidth;
    m_height = screenHeight;

    // Recreate FBOs with new size
    m_sourceFBO = FramebufferObject();
    m_intermediate1 = FramebufferObject();
    m_intermediate2 = FramebufferObject();
    m_bloomDownsampled = FramebufferObject();
    m_bloomBlurH = FramebufferObject();
    m_bloomBlurV = FramebufferObject();

    if (!m_sourceFBO.Create(m_width, m_height, true)) PrintLog("ERROR: Resize failed to create sourceFBO\n");
    if (!m_intermediate1.Create(m_width, m_height, false)) PrintLog("ERROR: Resize failed to create intermediate1\n");
    if (!m_intermediate2.Create(m_width, m_height, false)) PrintLog("ERROR: Resize failed to create intermediate2\n");

    int bloomW = m_width / 2;
    int bloomH = m_height / 2;
    if (!m_bloomDownsampled.Create(bloomW, bloomH, false)) PrintLog("ERROR: Resize failed to create bloomDownsampled\n");
    if (!m_bloomBlurH.Create(bloomW, bloomH, false)) PrintLog("ERROR: Resize failed to create bloomBlurH\n");
    if (!m_bloomBlurV.Create(bloomW, bloomH, false)) PrintLog("ERROR: Resize failed to create bloomBlurV\n");
}

void PostProcessingPipeline::ApplyEffects() {
    if (!m_initialized) return;

    // SOURCEPORT: Phase 2 bloom & tone mapping effects
    // Load shaders on first call
    static uint32_t thresholdProgram = 0;
    static uint32_t blurHProgram = 0;
    static uint32_t blurVProgram = 0;
    static uint32_t tonemapProgram = 0;
    static bool shadersLoaded = false;

    if (!shadersLoaded) {
        thresholdProgram = LoadPostProcessShader("bloom_threshold");
        blurHProgram = LoadPostProcessShader("bloom_blur_h");
        blurVProgram = LoadPostProcessShader("bloom_blur_v");
        tonemapProgram = LoadPostProcessShader("tonemap");
        shadersLoaded = true;
    }

    // SOURCEPORT: Phase 2 post-processing effects (currently minimal/disabled for stability)
    // Note: Bloom and tone mapping are disabled by default; infrastructure is in place
    // but effects composition requires additional work for proper framebuffer handling.
    // Effects can be safely enabled once compositing architecture is refined.
}

void PostProcessingPipeline::SetEffectEnabled(const char* effectName, bool enabled) {
    for (auto& eff : m_effects) {
        if (strcmp(eff.name, effectName) == 0) {
            eff.enabled = enabled;
            return;
        }
    }
}

bool PostProcessingPipeline::IsEffectEnabled(const char* effectName) const {
    for (const auto& eff : m_effects) {
        if (strcmp(eff.name, effectName) == 0) {
            return eff.enabled;
        }
    }
    return false;
}

void PostProcessingPipeline::SetEffectParameter(const char* effectName, const char* paramName, float value) {
    // SOURCEPORT: Placeholder for Phase 2 effect parameters
    // (intensity, exposure, quality, etc.)
}

void PostProcessingPipeline::HotReloadShaders() {
    // SOURCEPORT: Detect and reload post-processing shaders from disk
    // Integrates with HotReload system similar to custom material shaders
}

void PostProcessingPipeline::RenderFullscreenQuad(uint32_t shaderProgram) {
    glUseProgram(shaderProgram);
    glBindVertexArray(m_fsQuadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

uint32_t PostProcessingPipeline::LoadPostProcessShader(const char* shaderName) {
    // SOURCEPORT: Shader loading disabled for now
    return 0;
}

void PostProcessingPipeline::Compose(FramebufferObject* src, FramebufferObject* dst, CompositionMode mode) {
    // SOURCEPORT: Composite source FBO to destination using specified blend mode
    if (!src) return;

    int targetWidth = m_width, targetHeight = m_height;
    if (dst) {
        dst->Bind();
        targetWidth = dst->GetWidth();
        targetHeight = dst->GetHeight();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
    }

    if (mode == CompositionMode::REPLACE) {
        // Simple blit for REPLACE mode
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src->GetFramebuffer());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst ? dst->GetFramebuffer() : 0);
        glBlitFramebuffer(0, 0, src->GetWidth(), src->GetHeight(),
                         0, 0, targetWidth, targetHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, dst ? dst->GetFramebuffer() : 0);
    } else {
        // For blending modes, would need a shader to sample source texture
        // For now, skip implementation (used by bloom via manual rendering)
    }
}
