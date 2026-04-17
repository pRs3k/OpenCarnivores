// Example custom-material fragment shader.
//
// Shows three things modders are likely to want:
//   1. Sampling the engine-provided albedo off unit 0 (`uTexture`).
//   2. Using a mod-supplied extra texture bound by a `tex` directive
//      (here: `uDetail`, a high-frequency noise overlay — optional; if the
//      modder doesn't declare it, stb_image never loads it and the sampler
//      quietly reads black, which is fine thanks to the kDetailMix knob).
//   3. Per-material tuning via `float` / `vec3` directives (`uTint`, `uMix`).
//
// You still own fog / alpha-test / brightness if you want them — the engine
// does NOT inject those automatically into a custom program.

#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in float vFog;

uniform sampler2D uTexture;   // engine albedo (always bound)
uniform sampler2D uDetail;    // optional overlay — declared in .material
uniform vec3      uTint;      // multiplicative tint
uniform float     uMix;       // detail blend 0..1

out vec4 FragColor;

void main() {
    vec4 base   = texture(uTexture, vTexCoord) * vColor;
    vec3 detail = texture(uDetail,  vTexCoord * 4.0).rgb;
    vec3 rgb    = mix(base.rgb, base.rgb * detail, clamp(uMix, 0.0, 1.0));
    rgb        *= uTint;
    // Fog: mod-authors can reuse the vFog varying the same way basic.frag does.
    rgb = mix(vec3(0.55, 0.65, 0.75), rgb, clamp(vFog, 0.0, 1.0));
    FragColor = vec4(rgb, base.a);
}
