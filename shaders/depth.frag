#version 330 core
// SOURCEPORT: Shadow depth fragment shader.
// Writes depth implicitly via gl_FragCoord.z (standard convention, GL_LESS).
// Alpha-tested geometry (foliage) discards transparent fragments so they
// cast correct leaf-shaped shadows rather than solid-rectangle shadows.

smooth in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform bool      uAlphaTest;

void main() {
    if (uAlphaTest) {
        vec4 t = texture(uTexture, vTexCoord);
        if (t.a < 0.5) discard;
    }
    // Depth written automatically from gl_FragCoord.z.
}
