#version 330 core
// Simple bloom: brightens bright pixels and adds glow

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uIntensity;
uniform float uThreshold;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);

    // Calculate brightness (luminance)
    float brightness = dot(color.rgb, vec3(0.299, 0.587, 0.114));

    // Extract bright pixels
    float bloom = max(0.0, brightness - uThreshold);

    // Simple gaussian-ish blur by sampling nearby pixels
    vec2 texelSize = 1.0 / vec2(1920.0, 1080.0);  // Adjust for your resolution
    vec3 blurred = vec3(0.0);
    float count = 0.0;

    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * texelSize * 2.0;
            vec4 sampleColor = texture(uScreenColor, vTexCoord + offset);
            float sampleBright = dot(sampleColor.rgb, vec3(0.299, 0.587, 0.114));
            float weight = exp(-float(x*x + y*y) / 2.0);

            blurred += sampleColor.rgb * max(0.0, sampleBright - uThreshold) * weight;
            count += weight;
        }
    }

    blurred /= count;

    // Add bloom glow to original color
    vec3 result = color.rgb + blurred * uIntensity;

    FragColor = vec4(result, color.a);
}
