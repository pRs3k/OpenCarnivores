#version 330 core
// SOURCEPORT: bloom effect shader (Phase 2.2).
// Currently a placeholder. Final implementation: blur bright pixels and composite back.

uniform sampler2D uScreenColor;
uniform float uIntensity;

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    // Placeholder: pass through color unchanged
    FragColor = texture(uScreenColor, vTexCoord);
}
