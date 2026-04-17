// Example custom-material vertex shader.
//
// Mirrors the engine's default vertex layout (shaders/basic.vert). Custom
// shaders MUST use these exact attribute locations — the engine binds them
// when drawing, and changing them will give you garbage.
//
//   0 vec2  aPos         — screen-space x, y (pre-transformed by CPU)
//   1 float aDepth       — z
//   2 vec4  aColor       — diffuse RGBA (vertex light + any tint)
//   3 vec4  aSpecular    — specular RGBA; .a doubles as the fog factor (0..1)
//   4 vec2  aTexCoord
//
// The engine always provides `uProjection` (mat4) and binds the albedo to
// texture unit 0 as `uTexture` (sampler2D). Everything else is yours.

#version 330 core
layout(location = 0) in vec2  aPos;
layout(location = 1) in float aDepth;
layout(location = 2) in vec4  aColor;
layout(location = 3) in vec4  aSpecular;
layout(location = 4) in vec2  aTexCoord;

uniform mat4 uProjection;

out vec4 vColor;
out vec2 vTexCoord;
out float vFog;

void main() {
    vec4 pos_ndc = uProjection * vec4(aPos, aDepth, 1.0);
    // Same rhw-style perspective restore as basic.vert — needed for correct
    // texture/colour interpolation on the pre-transformed geometry.
    if (aDepth > 0.0) {
        float w = 16.0 / aDepth;
        gl_Position = vec4(pos_ndc.xyz * w, w);
    } else {
        gl_Position = pos_ndc;
    }
    vColor    = aColor;
    vTexCoord = aTexCoord;
    vFog      = aSpecular.a;
}
