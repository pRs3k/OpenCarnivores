#version 330 core
// Example: Color grading (saturation, brightness, contrast)

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uSaturation;
uniform float uBrightness;
uniform float uContrast;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);
    vec3 rgb = color.rgb;

    // Apply brightness
    rgb *= uBrightness;

    // Apply contrast (center around 0.5)
    rgb = (rgb - 0.5) * uContrast + 0.5;

    // Apply saturation
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(lum), rgb, uSaturation);

    // Clamp to valid range
    rgb = clamp(rgb, 0.0, 1.0);

    FragColor = vec4(rgb, color.a);
}
