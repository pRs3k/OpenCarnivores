#version 330 core
// SOURCEPORT: Depth-only shader for shadow map rendering (light POV).
// Renders scene from light's perspective to capture depth values.

layout(location = 0) in vec2 aPos;       // screen-space x, y (game coords)
layout(location = 1) in float aDepth;    // z (game depth)
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;    // Light's view-projection matrix
uniform bool uAlphaTest;     // Whether to discard transparent fragments

noperspective out float vDepth;
smooth out vec2 vTexCoord;

void main() {
    vec4 pos_clip = uProjection * vec4(aPos, aDepth, 1.0);
    gl_Position = pos_clip;

    vDepth = aDepth;
    vTexCoord = aTexCoord;
}
