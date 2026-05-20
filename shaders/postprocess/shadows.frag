#version 330 core
// SOURCEPORT: Cascaded PCF shadow mapping (Phase 2.1).
// Placeholder: renders screen color unchanged.
// Full implementation will:
//   1. Access screen depth to reconstruct world position
//   2. Transform to light space for each cascade
//   3. Select cascade based on distance from camera
//   4. Sample shadow maps with PCF filtering
//   5. Modulate color by shadow factor
//
// Currently waits for depth pass rendering to be wired into Hunt2.cpp

uniform sampler2D uScreenColor;
uniform sampler2D uScreenDepth;              // Depth buffer for world position reconstruction
uniform sampler2DArray uShadowMaps;          // Shadow maps for 3 cascades (when depth pass implemented)
uniform mat4 uLightViewProj[3];              // Light view-projection per cascade
uniform vec3 uCascadeDistances;              // Distance range [cascade0_far, cascade1_far, cascade2_far]
uniform float uIntensity;                    // Shadow darkness (0=no shadow, 1=full)
uniform vec3 uLightDir;                      // Light direction (normalized, world space)

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    vec4 screenColor = texture(uScreenColor, vTexCoord);

    // Placeholder: return color unchanged until depth pass is wired
    // When depth pass is ready:
    //   1. Read depth from uScreenDepth at vTexCoord
    //   2. Reconstruct world position from depth
    //   3. Select cascade based on distance from camera
    //   4. Transform to light space using uLightViewProj[cascade]
    //   5. Sample uShadowMaps layer cascade at transformed coords with PCF
    //   6. Modulate screenColor.rgb by shadow factor and uIntensity

    FragColor = screenColor;
}
