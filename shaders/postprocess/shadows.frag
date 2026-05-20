#version 330 core
// SOURCEPORT: Cascaded PCF shadow mapping (Phase 2.1).
// Samples from 3 shadow map cascades with percentage-closer filtering.

uniform sampler2D uScreenColor;
uniform sampler2DArray uShadowMaps;          // Shadow maps for 3 cascades
uniform mat4 uLightViewProj[3];              // Light view-projection per cascade
uniform vec3 uCascadeDistances;              // Distance range for each cascade
uniform float uIntensity;                    // Shadow darkness (0=no shadow, 1=full)
uniform vec3 uLightDir;                      // Light direction (world space)

in vec2 vTexCoord;
out vec4 FragColor;

// PCF helper: sample shadow map with 4-tap filter
float sampleShadowPCF(sampler2DArray shadowMaps, vec3 shadowCoord, int cascade) {
    if (shadowCoord.z > 1.0 || shadowCoord.z < 0.0) return 1.0;  // Outside shadow map
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 || shadowCoord.y < 0.0 || shadowCoord.y > 1.0) return 1.0;

    float texelSize = 1.0 / 2048.0;  // Shadow map resolution
    float shadow = 0.0;
    float centerDepth = shadowCoord.z;

    // 4-tap PCF: sample corners of a 2x2 texel quad around shadow coord
    for (int x = -1; x <= 1; x += 2) {
        for (int y = -1; y <= 1; y += 2) {
            vec3 sampleCoord = shadowCoord + vec3(float(x) * texelSize, float(y) * texelSize, 0.0);
            float closestDepth = texture(shadowMaps, vec3(sampleCoord.xy, float(cascade))).r;
            shadow += (centerDepth - 0.005) <= closestDepth ? 1.0 : 0.0;  // Bias to reduce acne
        }
    }

    return shadow / 4.0;  // Average of 4 samples
}

void main() {
    vec4 screenColor = texture(uScreenColor, vTexCoord);

    // For now, placeholder: return color unchanged
    // Full implementation will:
    // 1. Convert screen position to world position using uScreenDepth
    // 2. Transform to light space for each cascade
    // 3. Select cascade based on depth
    // 4. Sample shadow map with PCF
    // 5. Modulate color by shadow factor
    FragColor = screenColor;
}
