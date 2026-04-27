#version 330 core
layout(location = 0) in vec2 aPos;       // screen-space x, y
layout(location = 1) in float aDepth;    // z (depth)
layout(location = 2) in vec4 aColor;     // diffuse RGBA
layout(location = 3) in vec4 aSpecular;  // specular RGBA (A = fog)
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;

noperspective out vec4  vColor;
noperspective out float vFog;
// SOURCEPORT: two UV paths.
// vTexCoord (smooth) is used only for screen-space LOD derivatives.
// vTexCoordR / vRhw gives perspective-correct UV via explicit fragment-shader
// division, bypassing NVIDIA's hardware rasterizer storage for smooth varyings
// which may use reduced precision and produce a systematic UV bias at certain
// depth values (manifesting as a headlamp brightness ring on NVIDIA).
smooth        out vec2  vTexCoord;
noperspective out vec2  vTexCoordR;  // aTexCoord * rhw  (rhw = aDepth/16)
noperspective out float vRhw;        // rhw = aDepth/16

void main() {
    vec4 pos_ndc = uProjection * vec4(aPos, aDepth, 1.0);

    // SOURCEPORT: perspective-correct attribute interpolation.
    // D3D6 TL vertices carry rhw = sz/16 (= 1/camera_z) which the rasterizer uses
    // for perspective-correct UV/color interpolation.  With w_clip=1 (orthographic),
    // GL interpolates all varyings linearly in screen space — as the camera rotates,
    // vertex screen positions change while UVs stay fixed at TCMIN/TCMAX per vertex,
    // causing the interpolated UV at every fragment to drift.  This is the "ground
    // morphing and swimming" visible when standing still and looking around.
    // Fix: set w_clip = 1/rhw = 16/sz so GL's rasterizer sees the real camera-space
    // depth and applies perspective division automatically.  After perspective division
    // NDC position = pos_ndc.xyz (unchanged), but varyings are now correct.
    // Guard: HUD/sky/2D geometry uses sz=0 (aDepth=0) — keep w=1 for those.
    if (aDepth > 0.0) {
        float rhw = max(aDepth, 0.01) / 16.0;
        float w   = 1.0 / rhw;
        gl_Position  = vec4(pos_ndc.xyz * w, w);
        vTexCoordR   = aTexCoord * rhw;
        vRhw         = rhw;
    } else {
        gl_Position  = pos_ndc;
        vTexCoordR   = aTexCoord;
        vRhw         = 1.0;
    }

    vColor    = aColor;
    vTexCoord = aTexCoord;
    vFog      = aSpecular.a;
}
