#version 330 core
// Example: Reinhard tone mapping for HDR to SDR conversion

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uScreenColor;
uniform float uExposure;
uniform float uWhite;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);

    // Apply exposure
    vec3 hdr = color.rgb * uExposure;

    // Reinhard tone mapping with white point
    vec3 ldr = hdr * (1.0 + hdr / (uWhite * uWhite)) / (1.0 + hdr);

    // Apply gamma correction
    ldr = pow(ldr, vec3(1.0 / 2.2));

    FragColor = vec4(ldr, color.a);
}
