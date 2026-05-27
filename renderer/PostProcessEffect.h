#pragma once

#include <cstdint>

// SOURCEPORT: Simple post-process effect renderer (no FBO capture required)
// Renders fullscreen quad with a shader after scene is complete

class PostProcessEffect {
public:
    // Initialize fullscreen quad VAO/VBO
    static void Initialize();

    // Compile and render a post-process shader
    // Reads from currently bound framebuffer, renders result to screen
    static void ApplyShader(const char* fragmentShaderPath, float intensity = 1.0f);

    // Cleanup
    static void Shutdown();

private:
    static uint32_t s_fsQuadVao;
    static uint32_t s_fsQuadVbo;
    static bool s_initialized;

    static uint32_t CompileShader(const char* path);
};
