#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uScreenColor;
uniform float uThreshold;
uniform float uKnee;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);
    float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));

    // Soft threshold with knee curve
    float softness = uThreshold * uKnee;
    float bright = smoothstep(uThreshold - softness, uThreshold + softness, lum);

    // Output bright pixels only
    fragColor = vec4(color.rgb * bright, 1.0);
}
