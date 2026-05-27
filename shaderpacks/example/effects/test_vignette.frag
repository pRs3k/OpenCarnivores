#version 330 core
// Test effect: simple vignette (darkens edges)

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uIntensity;

void main() {
    // Simple vignette: darken based on distance from center
    vec2 uv = vTexCoord - 0.5;
    float vignette = 1.0 - length(uv) * 1.5;
    vignette = smoothstep(0.0, 1.0, vignette);

    // Get screen color and apply vignette
    vec4 color = texture(uScreenColor, vTexCoord);
    vec3 result = color.rgb * mix(1.0, vignette, uIntensity);

    FragColor = vec4(result, color.a);
}
