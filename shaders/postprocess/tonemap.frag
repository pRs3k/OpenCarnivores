#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uScreenColor;
uniform float uExposure;

void main() {
    vec4 color = texture(uScreenColor, vTexCoord);
    
    // Apply exposure
    vec3 mapped = color.rgb * uExposure;
    
    // Reinhard tone mapping
    mapped = mapped / (mapped + vec3(1.0));
    
    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / 2.2));
    
    fragColor = vec4(mapped, color.a);
}
