#version 330 core

// SOURCEPORT: sky reconstruction vertex shader - full-screen quad

layout(location = 0) in vec2 aPos;  // Normalized screen coordinates [-1, 1]

noperspective out vec2 vTexCoord;

uniform float uWinW;
uniform float uWinH;

void main() {
    // Render at far plane (depth = 0.0 in reversed-Z convention) so geometry renders in front
    gl_Position = vec4(aPos, 0.0, 1.0);

    // Convert from normalized [-1, 1] to pixel coordinates [0, WinW] x [0, WinH]
    vTexCoord = vec2((aPos.x + 1.0) * 0.5 * uWinW, (1.0 - aPos.y) * 0.5 * uWinH);
}
