#version 330 core
// SOURCEPORT: dynamic shadow mapping shader (Phase 2.1).
// Currently a placeholder. Final implementation: cascaded shadow mapping with PCF filtering.

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
