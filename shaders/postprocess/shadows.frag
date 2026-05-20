#version 330 core
// SOURCEPORT: Cascaded PCF shadow mapping (Phase 2.1).
// Samples from 3 shadow map cascades (separate 2D textures) with 4-tap PCF filtering.

uniform sampler2D uScreenColor;
uniform sampler2D uScreenDepth;              // Depth buffer for light evaluation
uniform sampler2D uShadowMap0;               // Shadow map cascade 0
uniform sampler2D uShadowMap1;               // Shadow map cascade 1
uniform sampler2D uShadowMap2;               // Shadow map cascade 2
uniform mat4 uLightViewProj[3];              // Light view-projection per cascade
uniform vec3 uCascadeDistances;              // Distance range [cascade0_far, cascade1_far, cascade2_far]
uniform float uIntensity;                    // Shadow darkness (0=no shadow, 1=full)
uniform vec3 uLightDir;                      // Light direction (normalized, world space)

in vec2 vTexCoord;
out vec4 FragColor;

// PCF filtering: sample 4-tap around center
float sampleShadowPCF(sampler2D shadowMap, vec2 shadowCoord, float centerDepth) {
    // Clamp to shadow map bounds
    if (centerDepth > 1.0 || centerDepth < 0.0) return 1.0;
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 || shadowCoord.y < 0.0 || shadowCoord.y > 1.0) return 1.0;

    float texelSize = 1.0 / 2048.0;
    float shadow = 0.0;

    // 4-tap PCF: corners of 2×2 texel quad around shadow coord
    for (int x = -1; x <= 1; x += 2) {
        for (int y = -1; y <= 1; y += 2) {
            vec2 sampleCoord = shadowCoord + vec2(float(x) * texelSize, float(y) * texelSize);
            float closestDepth = texture(shadowMap, sampleCoord).r;
            // Shadow bias to reduce acne
            shadow += (centerDepth - 0.005) <= closestDepth ? 1.0 : 0.0;
        }
    }

    return shadow / 4.0;
}

void main() {
    vec4 screenColor = texture(uScreenColor, vTexCoord);
    float screenDepth = texture(uScreenDepth, vTexCoord).r;

    // Early exit: no shadow on far/unwritten pixels
    if (screenDepth >= 0.9999) {
        FragColor = screenColor;
        return;
    }

    // Approximate camera distance from depth using game's reversed depth convention.
    // screenDepth range: [0=far (sky), 1=near (close geometry)]
    // Estimate distance: inverted depth approximates log distance
    float approxDist = 1.0 - screenDepth;
    if (approxDist < 0.001) approxDist = 0.001;
    float estimatedDistance = 1.0 / (approxDist * 16.0);  // Rough conversion to GU

    // Select cascade and sampler based on estimated distance
    int cascade = 0;
    sampler2D shadowMap = uShadowMap0;

    if (estimatedDistance < uCascadeDistances.x) {
        cascade = 0;
        shadowMap = uShadowMap0;
    } else if (estimatedDistance < uCascadeDistances.y) {
        cascade = 1;
        shadowMap = uShadowMap1;
    } else {
        cascade = 2;
        shadowMap = uShadowMap2;
    }

    // Transform to light space using selected cascade
    vec3 screenPos = vec3(vTexCoord, screenDepth);
    vec4 lightSpacePos = uLightViewProj[cascade] * vec4(vTexCoord * 2.0 - 1.0, estimatedDistance, 1.0);
    vec3 shadowCoord = lightSpacePos.xyz / lightSpacePos.w;

    // NDC to texture space: [-1,1] → [0,1]
    shadowCoord.xy = shadowCoord.xy * 0.5 + 0.5;

    // Sample shadow with PCF
    float shadowFactor = sampleShadowPCF(shadowMap, shadowCoord.xy, shadowCoord.z);

    // Modulate color by shadow: 1.0 = fully lit, 0.0 = fully shadowed
    float shadowModulation = mix(1.0, shadowFactor, uIntensity);
    FragColor = vec4(screenColor.rgb * shadowModulation, screenColor.a);
}
