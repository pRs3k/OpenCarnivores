#version 330 core
// SOURCEPORT: test post-processing shader — converts to grayscale for pipeline verification.
// Phase 2 will implement actual effect shaders (shadows, bloom, tone mapping, SSR).

uniform sampler2D uScreenColor;
uniform float uIntensity;

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);

    // Convert to grayscale using luminance formula
    float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));

    // Lerp between original color and grayscale based on intensity
    vec3 desaturated = mix(color.rgb, vec3(gray), uIntensity);

    FragColor = vec4(desaturated, color.a);
}
