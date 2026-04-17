#version 330 core
layout(location = 0) in vec2 aPos;       // screen-space x, y
layout(location = 1) in float aDepth;    // z (depth)
layout(location = 2) in vec4 aColor;     // diffuse RGBA
layout(location = 3) in vec4 aSpecular;  // specular RGBA (A = fog)
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;

out vec4 vColor;
out vec2 vTexCoord;
out float vFog;

void main() {
    vec4 pos_ndc = uProjection * vec4(aPos, aDepth, 1.0);

    // Perspective-correct attribute interpolation. D3D6 TL vertices carry
    // rhw = sz/16 (= 1/camera_z); set w_clip = 1/rhw so GL applies perspective
    // division to varyings. Guard: HUD/sky uses aDepth=0 — keep w=1 there.
    if (aDepth > 0.0) {
        float w = 16.0 / aDepth;
        gl_Position = vec4(pos_ndc.xyz * w, w);
    } else {
        gl_Position = pos_ndc;
    }

    vColor = aColor;
    vTexCoord = aTexCoord;
    vFog = aSpecular.a;
}
