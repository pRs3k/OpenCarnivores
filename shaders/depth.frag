#version 330 core
// SOURCEPORT: Depth-only fragment shader for shadow map rendering.
// For alpha-tested geometry (foliage), samples texture to determine opacity.

uniform sampler2D uTexture;
uniform bool uAlphaTest;

smooth in vec2 vTexCoord;
noperspective in float vDepth;

out float fragDepth;

void main() {
    // For foliage (alpha-tested geometry), check texture alpha
    if (uAlphaTest) {
        vec4 texel = texture(uTexture, vTexCoord);
        // Alpha test: discard if transparent
        if (texel.a < 0.5) discard;
    }

    // Output normalized linear depth (0=near, 1=far)
    // The depth is written automatically by GL_DEPTH_ATTACHMENT,
    // but we also output for potential debugging/PCF custom filtering.
    fragDepth = gl_FragCoord.z;
}
