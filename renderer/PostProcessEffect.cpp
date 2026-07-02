#include "PostProcessEffect.h"
#include "glad/gl.h"
#include <cstdio>
#include <fstream>
#include <sstream>

uint32_t PostProcessEffect::s_fsQuadVao = 0;
uint32_t PostProcessEffect::s_fsQuadVbo = 0;
bool PostProcessEffect::s_initialized = false;

// Basic fullscreen quad vertex shader
static const char* QUAD_VERTEX_SHADER = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static std::string ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        fprintf(stderr, "[PostProcess] Failed to read: %s\n", path.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static uint32_t CompileShader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "[PostProcess] Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static uint32_t LinkProgram(uint32_t vs, uint32_t fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[PostProcess] Program link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void PostProcessEffect::Initialize() {
    if (s_initialized) return;

    // Fullscreen quad vertices and UVs
    float quadVertices[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };

    glGenVertexArrays(1, &s_fsQuadVao);
    glGenBuffers(1, &s_fsQuadVbo);

    glBindVertexArray(s_fsQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_fsQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    // Position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    // TexCoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float))); // NOLINT(performance-no-int-to-ptr)
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    s_initialized = true;
    fprintf(stdout, "[PostProcess] Fullscreen quad initialized\n");
}

uint32_t PostProcessEffect::CompileShader(const char* path) {
    std::string fsSource = ReadFile(path);
    if (fsSource.empty()) return 0;

    uint32_t vs = ::CompileShader(GL_VERTEX_SHADER, QUAD_VERTEX_SHADER);
    uint32_t fs = ::CompileShader(GL_FRAGMENT_SHADER, fsSource);

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    uint32_t prog = LinkProgram(vs, fs);
    if (prog) {
        fprintf(stdout, "[PostProcess] Compiled shader: %s\n", path);
    }
    return prog;
}

void PostProcessEffect::ApplyShader(const char* fragmentShaderPath, float intensity) {
    if (!s_initialized) Initialize();

    uint32_t program = CompileShader(fragmentShaderPath);
    if (!program) {
        fprintf(stderr, "[PostProcess] Failed to compile: %s\n", fragmentShaderPath);
        return;
    }

    // Enable blending for effect composition
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // Additive blend for bloom

    glUseProgram(program);
    glBindVertexArray(s_fsQuadVao);

    // Bind screen texture (read from what's already rendered)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);  // Bind default framebuffer as texture
    glUniform1i(glGetUniformLocation(program, "uScreenColor"), 0);
    glUniform1f(glGetUniformLocation(program, "uIntensity"), intensity);

    // Render fullscreen quad
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDeleteProgram(program);
}

void PostProcessEffect::Shutdown() {
    if (s_fsQuadVao) glDeleteVertexArrays(1, &s_fsQuadVao);
    if (s_fsQuadVbo) glDeleteBuffers(1, &s_fsQuadVbo);
    s_initialized = false;
}
