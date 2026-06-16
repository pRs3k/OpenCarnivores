#include "PostProcessing.h"
#include "glad/gl.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

extern void PrintLog(const char* msg);

static GLuint CompilePostShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        PrintLog(log);
    }
    return s;
}

static GLuint LinkPostProgram(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        PrintLog(log);
        glDeleteProgram(p); p = 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

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

    // SOURCEPORT: Create shadow map FBOs (depth-only, fixed size)
    for (int i = 0; i < SHADOW_CASCADES; i++) {
        if (!m_shadowMaps[i].Create(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, true)) {
            PrintLog("[PostProcessing] WARNING: Shadow map creation failed\n");
            // Continue anyway — shadows will simply not render, rest of pipeline works
        }
        m_cascadeDistances[i] = 100.0f * (i + 1);  // 100, 200, 300 GU
    }

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
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(uintptr_t)(2 * sizeof(float))); // NOLINT(performance-no-int-to-ptr)
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

void PostProcessingPipeline::ApplySimpleEffect(const char* shaderPath, float intensity) {
    if (!m_initialized || !m_fsQuadVao) return;

    // Read shader file
    std::ifstream shaderFile(shaderPath);
    if (!shaderFile) {
        fprintf(stderr, "[PostProcessing] Failed to load shader: %s\n", shaderPath);
        return;
    }
    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    std::string fsSource = buffer.str();

    // Compile shaders
    GLuint vs = CompilePostShader(GL_VERTEX_SHADER,
        "#version 330 core\n"
        "layout(location=0)in vec2 aPos;layout(location=1)in vec2 aTexCoord;"
        "out vec2 vTexCoord;void main(){gl_Position=vec4(aPos,0,1);vTexCoord=aTexCoord;}");
    GLuint fs = CompilePostShader(GL_FRAGMENT_SHADER, fsSource.c_str());

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    GLuint prog = LinkPostProgram(vs, fs);
    if (!prog) return;

    // Copy back buffer to texture
    GLuint screenTex;
    glGenTextures(1, &screenTex);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, m_width, m_height, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Render fullscreen quad with effect
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(prog);
    glBindVertexArray(m_fsQuadVao);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glUniform1i(glGetUniformLocation(prog, "uScreenColor"), 0);
    glUniform1f(glGetUniformLocation(prog, "uIntensity"), intensity);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDeleteProgram(prog);
    glDeleteTextures(1, &screenTex);
}

void PostProcessingPipeline::ApplyEffects() {
    if (!m_initialized || !m_captureActive) return;
    m_captureActive = false;

    static uint32_t thresholdProg = 0, blurHProg = 0, blurVProg = 0;
    static uint32_t tonemapProg = 0, compositeProg = 0;
    static bool shadersLoaded = false;
    if (!shadersLoaded) {
        thresholdProg = LoadPostProcessShader("bloom_threshold");
        blurHProg     = LoadPostProcessShader("bloom_blur_h");
        blurVProg     = LoadPostProcessShader("bloom_blur_v");
        tonemapProg   = LoadPostProcessShader("tonemap");
        compositeProg = LoadPostProcessShader("bloom");
        shadersLoaded = true;
    }

    extern bool  g_enableBloom, g_enableToneMapping, g_enableShadows;
    extern float g_bloomThreshold, g_bloomKnee, g_bloomIntensity, g_tonemapExposure;
    extern float g_shadowIntensity;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glBindVertexArray(m_fsQuadVao);

    GLuint sceneTex = m_sourceFBO.GetColorTexture();
    GLuint depthTex = m_sourceFBO.GetDepthTexture();
    GLuint bloomTex = 0;

    // SOURCEPORT: Phase 2.1 - Apply shadows (must be first to darken base scene)
    static uint32_t shadowProg = 0;
    static uint32_t shadowDebugProg = 0;
    static bool shadowProgLoaded = false;
    if (!shadowProgLoaded) {
        shadowProg = LoadPostProcessShader("shadows");
        shadowDebugProg = LoadPostProcessShader("shadows_debug");
        shadowProgLoaded = true;
    }

    // Debug visualization takes precedence
    if (m_debugShadowCascade >= 0 && m_debugShadowCascade < 3 && shadowDebugProg) {
        m_intermediate1.Bind();
        glUseProgram(shadowDebugProg);

        // Bind 3 shadow map depth textures
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[0].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowDebugProg, "uShadowMap0"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[1].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowDebugProg, "uShadowMap1"), 3);

        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[2].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowDebugProg, "uShadowMap2"), 4);

        glUniform1i(glGetUniformLocation(shadowDebugProg, "uDebugCascade"), m_debugShadowCascade);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        sceneTex = m_intermediate1.GetColorTexture();
    } else if (g_enableShadows && shadowProg && depthTex) {
        m_intermediate1.Bind();
        glUseProgram(shadowProg);

        // Bind scene color
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glUniform1i(glGetUniformLocation(shadowProg, "uScreenColor"), 0);

        // Bind depth buffer
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glUniform1i(glGetUniformLocation(shadowProg, "uScreenDepth"), 1);

        // Bind 3 shadow map depth textures as separate 2D textures
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[0].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowProg, "uShadowMap0"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[1].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowProg, "uShadowMap1"), 3);

        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_shadowMaps[2].GetDepthTexture());
        glUniform1i(glGetUniformLocation(shadowProg, "uShadowMap2"), 4);

        // Set shadow uniforms
        glUniform1f(glGetUniformLocation(shadowProg, "uIntensity"), g_shadowIntensity);
        glUniform3f(glGetUniformLocation(shadowProg, "uLightDir"),
                   m_lightDirection[0], m_lightDirection[1], m_lightDirection[2]);
        glUniform3f(glGetUniformLocation(shadowProg, "uCascadeDistances"),
                   m_cascadeDistances[0], m_cascadeDistances[1], m_cascadeDistances[2]);

        // Set light view-projection matrices
        for (int i = 0; i < 3; i++) {
            char locName[64];
            snprintf(locName, sizeof(locName), "uLightViewProj[%d]", i);
            glUniformMatrix4fv(glGetUniformLocation(shadowProg, locName), 1, GL_FALSE, m_lightProjMatrix[i]);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        sceneTex = m_intermediate1.GetColorTexture();  // Use shadowed result for next effect
    }

    // Bloom extraction + separable blur at half resolution
    if (g_enableBloom && thresholdProg && blurHProg && blurVProg) {
        m_bloomDownsampled.Bind();
        glUseProgram(thresholdProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glUniform1i(glGetUniformLocation(thresholdProg, "uScreenColor"), 0);
        glUniform1f(glGetUniformLocation(thresholdProg, "uThreshold"), g_bloomThreshold);
        glUniform1f(glGetUniformLocation(thresholdProg, "uKnee"), g_bloomKnee);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        m_bloomBlurH.Bind();
        glUseProgram(blurHProg);
        glBindTexture(GL_TEXTURE_2D, m_bloomDownsampled.GetColorTexture());
        glUniform1i(glGetUniformLocation(blurHProg, "uScreenColor"), 0);
        glUniform1i(glGetUniformLocation(blurHProg, "uBlurRadius"), 8);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        m_bloomBlurV.Bind();
        glUseProgram(blurVProg);
        glBindTexture(GL_TEXTURE_2D, m_bloomBlurH.GetColorTexture());
        glUniform1i(glGetUniformLocation(blurVProg, "uScreenColor"), 0);
        glUniform1i(glGetUniformLocation(blurVProg, "uBlurRadius"), 8);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        bloomTex = m_bloomBlurV.GetColorTexture();
    }

    // Output to screen: tonemap and/or composite bloom
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);

    if (g_enableToneMapping && g_enableBloom && tonemapProg && compositeProg && bloomTex) {
        // Tonemap into intermediate, then composite bloom on top
        m_intermediate1.Bind();
        glUseProgram(tonemapProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glUniform1i(glGetUniformLocation(tonemapProg, "uScreenColor"), 0);
        glUniform1f(glGetUniformLocation(tonemapProg, "uExposure"), g_tonemapExposure);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
        glUseProgram(compositeProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_intermediate1.GetColorTexture());
        glUniform1i(glGetUniformLocation(compositeProg, "uScreenColor"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTex);
        glUniform1i(glGetUniformLocation(compositeProg, "uBloomColor"), 1);
        glUniform1f(glGetUniformLocation(compositeProg, "uIntensity"), g_bloomIntensity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else if (g_enableToneMapping && tonemapProg) {
        glUseProgram(tonemapProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glUniform1i(glGetUniformLocation(tonemapProg, "uScreenColor"), 0);
        glUniform1f(glGetUniformLocation(tonemapProg, "uExposure"), g_tonemapExposure);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else if (g_enableBloom && compositeProg && bloomTex) {
        glUseProgram(compositeProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glUniform1i(glGetUniformLocation(compositeProg, "uScreenColor"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTex);
        glUniform1i(glGetUniformLocation(compositeProg, "uBloomColor"), 1);
        glUniform1f(glGetUniformLocation(compositeProg, "uIntensity"), g_bloomIntensity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        // No effects active: blit scene directly to screen
        m_sourceFBO.BlitTo(nullptr);
    }

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
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
    auto readFile = [](const char* path) -> std::string {
        std::ifstream f(path);
        if (!f) return {};
        return { std::istreambuf_iterator<char>(f), {} };
    };
    std::string vert = readFile("shaders/postprocess/quad.vert");
    std::string frag = readFile((std::string("shaders/postprocess/") + shaderName + ".frag").c_str());
    if (vert.empty() || frag.empty()) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR: [PostProcessing] Could not load shader: %s\n", shaderName);
        PrintLog(msg);
        return 0;
    }
    GLuint vs = CompilePostShader(GL_VERTEX_SHADER,   vert.c_str());
    GLuint fs = CompilePostShader(GL_FRAGMENT_SHADER, frag.c_str());
    return LinkPostProgram(vs, fs);
}

void PostProcessingPipeline::BeginCapture() {
    if (!m_initialized) return;
    m_sourceFBO.Bind();
    m_captureActive = true;
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

void PostProcessingPipeline::UpdateShadowMatrices(const float* cameraPos, const float* /*cameraDir*/,
                                                   float /*fovY*/, float /*aspect*/) {
    // SOURCEPORT: Compute light view/projection matrices for each shadow cascade.
    // Uses logarithmic cascade splitting: split camera frustum into N cascades
    // where cascade i covers depths [nearPlane * split^i, nearPlane * split^(i+1)]
    // This distributes cascade resolution proportionally to visual importance.

    float nearPlane = 1.0f;
    float farPlane = 10000.0f;  // Far clip distance in GU
    float splitLambda = 0.95f;  // Blends linear (0) and logarithmic (1) splitting

    // Normalize light direction
    float lightLen = std::sqrt(m_lightDirection[0]*m_lightDirection[0] +
                              m_lightDirection[1]*m_lightDirection[1] +
                              m_lightDirection[2]*m_lightDirection[2]);
    float lightDir[3] = { m_lightDirection[0]/lightLen,
                          m_lightDirection[1]/lightLen,
                          m_lightDirection[2]/lightLen };

    for (int i = 0; i < SHADOW_CASCADES; i++) {
        // Compute cascade distance split using logarithmic split
        float linearSplit = nearPlane + (farPlane - nearPlane) * (float)(i+1) / SHADOW_CASCADES;
        float logSplit = nearPlane * std::pow(farPlane / nearPlane, (float)(i+1) / SHADOW_CASCADES);
        float cascadeFar = linearSplit * (1.0f - splitLambda) + logSplit * splitLambda;
        m_cascadeDistances[i] = cascadeFar;

        // Light view matrix: look along light direction from a point above the scene
        // Position light "behind" the cascade center to get good shadow depth resolution
        // SOURCEPORT: cascadeCenter removed (dead store — lightDist derives from cascadeFar only)
        float lightDist = cascadeFar * 2.0f;  // Position light 2x cascade far plane away

        float lightPos[3] = { cameraPos[0] - lightDir[0] * lightDist,
                              cameraPos[1] - lightDir[1] * lightDist,
                              cameraPos[2] - lightDir[2] * lightDist };

        // Build light view matrix (looking toward light direction from light position)
        // Simplified approach: use light direction as forward, construct orthogonal basis
        float forward[3] = { -lightDir[0], -lightDir[1], -lightDir[2] };  // Toward camera

        // Use world Y as reference for "up" unless parallel to forward
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float dot = forward[0]*up[0] + forward[1]*up[1] + forward[2]*up[2];
        if (std::abs(dot) > 0.95f) {  // Nearly parallel, use X instead
            up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
        }

        // Compute right = forward × up
        float right[3] = { forward[1]*up[2] - forward[2]*up[1],
                           forward[2]*up[0] - forward[0]*up[2],
                           forward[0]*up[1] - forward[1]*up[0] };
        float rightLen = std::sqrt(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
        right[0]/=rightLen; right[1]/=rightLen; right[2]/=rightLen;

        // Recompute up = right × forward (now orthogonal)
        up[0] = right[1]*forward[2] - right[2]*forward[1];
        up[1] = right[2]*forward[0] - right[0]*forward[2];
        up[2] = right[0]*forward[1] - right[1]*forward[0];
        float upLen = std::sqrt(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
        up[0]/=upLen; up[1]/=upLen; up[2]/=upLen;

        // Assemble view matrix: [right up forward pos]
        // GL view matrix maps world space to view space: -Z forward, +Y up, +X right
        float* view = m_lightViewMatrix[i];
        view[0] = right[0];   view[4] = right[1];   view[8] = right[2];   view[12] = 0;
        view[1] = up[0];      view[5] = up[1];      view[9] = up[2];      view[13] = 0;
        view[2] = forward[0]; view[6] = forward[1]; view[10] = forward[2]; view[14] = 0;
        view[3] = 0;          view[7] = 0;          view[11] = 0;          view[15] = 1;

        // Apply translation: -dot(right, lightPos) etc.
        view[12] = -(right[0]*lightPos[0] + right[1]*lightPos[1] + right[2]*lightPos[2]);
        view[13] = -(up[0]*lightPos[0] + up[1]*lightPos[1] + up[2]*lightPos[2]);
        view[14] = -(forward[0]*lightPos[0] + forward[1]*lightPos[1] + forward[2]*lightPos[2]);

        // Orthographic projection: fit cascade frustum
        // For simplicity, use fixed view size; ideally computed from frustum geometry
        float projSize = 1500.0f;  // GU (covers ~3000x3000 area in world space)
        float* proj = m_lightProjMatrix[i];
        float invSize = 1.0f / projSize;
        proj[0]  = invSize; proj[1]  = 0;       proj[2]  = 0;         proj[3]  = 0;
        proj[4]  = 0;       proj[5]  = invSize; proj[6]  = 0;         proj[7]  = 0;
        proj[8]  = 0;       proj[9]  = 0;       proj[10] = -2.0f/lightDist; proj[11] = 0;
        proj[12] = 0;       proj[13] = 0;       proj[14] = -1.0f;     proj[15] = 1.0f;
    }
}
