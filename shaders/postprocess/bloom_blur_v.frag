#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uScreenColor;
uniform int uBlurRadius;

void main() {
    vec2 texelSize = 1.0 / textureSize(uScreenColor, 0);
    vec4 result = vec4(0.0);
    float weight = 0.0;
    
    for (int i = -uBlurRadius; i <= uBlurRadius; ++i) {
        float gaussWeight = exp(-float(i * i) / (2.0 * float(uBlurRadius * uBlurRadius)));
        result += texture(uScreenColor, vTexCoord + vec2(0.0, float(i) * texelSize.y)) * gaussWeight;
        weight += gaussWeight;
    }
    
    fragColor = result / weight;
}
