#version 330 core
// SOURCEPORT: Debug visualization - display shadow map contents

uniform sampler2D uScreenColor;
uniform sampler2D uShadowMap0;
uniform sampler2D uShadowMap1;
uniform sampler2D uShadowMap2;
uniform int uDebugCascade;  // Which cascade to visualize (0-2)

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    // First, just show cascade selector as solid color to verify debug is running
    // If this works, we can add depth sampling
    if (uDebugCascade == 0) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // Red = cascade 0
    } else if (uDebugCascade == 1) {
        FragColor = vec4(0.0, 1.0, 0.0, 1.0);  // Green = cascade 1
    } else if (uDebugCascade == 2) {
        FragColor = vec4(0.0, 0.0, 1.0, 1.0);  // Blue = cascade 2
    } else {
        FragColor = vec4(1.0, 1.0, 0.0, 1.0);  // Yellow = off/invalid
    }
}
