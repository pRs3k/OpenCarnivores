#version 330 core
// SOURCEPORT: screen-space reflections shader (Phase 2.3).
// Currently a placeholder. Final implementation: ray tracing on-screen using depth/normal buffers.

uniform sampler2D uScreenColor;
uniform sampler2D uScreenDepth;
uniform sampler2D uScreenNormal;
uniform float uIntensity;

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    // Placeholder: pass through color unchanged
    FragColor = texture(uScreenColor, vTexCoord);
}
