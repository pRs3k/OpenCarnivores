#version 330 core
// SOURCEPORT: fullscreen quad vertex shader for post-processing effects.
// Accepts pre-transformed [-1, 1] screen-space positions and UVs from the fullscreen quad.

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
