#version 330 core
// SOURCEPORT: composite shader — adds blurred bloom to the scene (tonemapped or raw).

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uScreenColor;  // scene (tonemapped if tone mapping is enabled)
uniform sampler2D uBloomColor;   // blurred bright-pixel mask (half-res, bilinear upscaled)
uniform float uIntensity;        // bloom blend factor (g_bloomIntensity)

void main() {
    vec4 scene = texture(uScreenColor, vTexCoord);
    vec4 bloom = texture(uBloomColor, vTexCoord);
    fragColor = vec4(scene.rgb + bloom.rgb * uIntensity, scene.a);
}
