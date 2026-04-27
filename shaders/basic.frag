#version 330 core
noperspective in vec4  vColor;
noperspective in float vFog;
smooth        in vec2  vTexCoord;
noperspective in vec2  vTexCoordR;
noperspective in float vRhw;

uniform sampler2D uTexture;
uniform bool uFogEnabled;
uniform vec4 uFogColor;
uniform bool uAlphaTest;
uniform float uBrightness;

// PBR override path. When uPBR is set, units 1/2/3 supply tangent-space
// normal, metallic+roughness (glTF: metallic=R, roughness=G), and AO.
uniform bool      uPBR;
uniform sampler2D uNormalMap;
uniform sampler2D uMRMap;
uniform sampler2D uAOMap;
uniform float     uMetallicFactor;
uniform float     uRoughnessFactor;
uniform vec3      uSunDirView;

// SOURCEPORT: debug visualization mode (toggled at runtime with F8).
// 0 = normal, 1 = PBR disabled (Lambert only), 2 = PBR-active fragments shown as magenta.
uniform int uDebugMode;

out vec4 FragColor;

mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1  = dFdx(p);
    vec3 dp2  = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T,T), dot(B,B)));
    return mat3(T * invmax, B * invmax, N);
}

float DistributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 1e-7);
}
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return NdotV / (NdotV * (1.0 - k) + k);
}
float GeometrySmith(float NdotV, float NdotL, float roughness) {
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Perspective-correct UV via explicit division (full 32-bit precision).
    vec2 vTC = vTexCoordR / vRhw;

    ivec2 tsz  = textureSize(uTexture, 0);
    // SOURCEPORT: LOD computation split by geometry type.
    // For alpha-tested geometry (foliage/trees): use depth-based LOD derived from
    // camera-space Z (= 1/vRhw). Screen-space derivatives change with headbob
    // because all vertex screen-Y positions oscillate, making dFdy(vTexCoord)
    // oscillate, LOD oscillate, and foliage sample from mip 1+ (slightly different
    // colour) -> the whole scene darkens while walking -> headlamp appears by contrast.
    // Camera depth (rv.z) is unaffected by a vertical headbob for a level camera, so
    // depth-based LOD is stable and eliminates the strobe/headlamp entirely.
    // For opaque geometry (terrain): screen-space derivative LOD is fine; terrain has
    // no mipmaps so the lod value is irrelevant (textureLod always returns level 0).
    float lod;
    if (uAlphaTest) {
        float camZ = 1.0 / max(vRhw, 1e-6);
        lod = max(0.0, 0.5 * log2(camZ / 512.0) - 0.75);
    } else {
        vec2  dt_x = dFdx(vTexCoord) * vec2(tsz);
        vec2  dt_y = dFdy(vTexCoord) * vec2(tsz);
        float rho2 = max(dot(dt_x, dt_x), dot(dt_y, dt_y));
        lod = max(0.0, 0.5 * log2(max(rho2, 1e-10)) - 0.75);
    }
    vec4 texel = textureLod(uTexture, vTC, lod);

    if (uAlphaTest && uDebugMode != 9) {
        vec2 atUV = (floor(vTC * vec2(tsz)) + 0.5) / vec2(tsz);
        if (textureLod(uTexture, atUV, 0.0).a < 0.5) discard;
    }

    vec4 color = texel * vColor;
    if (!uAlphaTest) color.a = 1.0; else color.a = vColor.a;

    // SOURCEPORT: debug visualization modes (F8 cycles in-game):
    //   1 = PBR disabled (Lambert only)
    //   2 = PBR-active fragments shown as solid magenta
    //   3 = vertex color only (isolates VMap.Light / baked terrain brightness)
    //   4 = raw texel only (isolates texture sampling / mip selection)
    //   5 = solid mid-gray (geometry coverage check — circle persists = geometry artifact)
    if (uDebugMode == 2 && uPBR) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (uDebugMode == 3) { FragColor = vec4(vColor.rgb, 1.0); return; }
    if (uDebugMode == 4) { FragColor = vec4(texel.rgb,  1.0); return; }
    if (uDebugMode == 5) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
    // SOURCEPORT: mode 6 = force mip 0 (LOD=0) regardless of derivatives.
    // If this eliminates the headlamp, the issue is LOD-related despite textureLod fix.
    // If the headlamp persists even here, the cause is UV coordinates or something else.
    if (uDebugMode == 6) { FragColor = vec4(textureLod(uTexture, vTC, 0.0).rgb, 1.0); return; }
    if (uDebugMode == 7) { float l = lod / 4.0; FragColor = vec4(l, 0.0, max(0.0,1.0-l), 1.0); return; }
    if (uDebugMode == 8) { FragColor = vec4(fract(vTC), 0.0, 1.0); return; }
    if (uDebugMode == 9) { FragColor = vec4(textureLod(uTexture, vTC, lod).rgb, 1.0); return; }
    // SOURCEPORT: mode 10 = terrain texture in color, foliage/alpha-test geometry as solid gray.
    // If headlamp appears here, headlamp is in terrain. If not, headlamp is foliage-only.
    if (uDebugMode == 10) {
        if (uAlphaTest) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
        FragColor = vec4(texel.rgb, 1.0); return;
    }
    // SOURCEPORT: mode 11 = UV fract at 100x magnification. A 0.001 UV drift (invisible at 1x
    // in mode 8) shows as 0.1 color-unit phase shift here — confirms or rules out sub-texel drift.
    if (uDebugMode == 11) { FragColor = vec4(fract(vTC * 100.0), 0.0, 1.0); return; }
    // SOURCEPORT: mode 12 = vFog factor as grayscale.
    // If a radial circle is visible here while walking, fog is driving the headlamp.
    // vFog=1 (white) = no fog; vFog=0 (black) = full fog.
    if (uDebugMode == 12) { FragColor = vec4(vFog, vFog, vFog, 1.0); return; }
    // SOURCEPORT: mode 13 = normal render with fog disabled.
    // If the headlamp disappears here (vs mode 0), fog is confirmed as the cause.

    if (uPBR && uDebugMode != 1) {
        vec3  albedo    = texel.rgb;
        vec2  mr        = texture(uMRMap, vTC).rg;
        float metallic  = mr.r * uMetallicFactor;
        float roughness = max(mr.g * uRoughnessFactor, 0.04);
        float ao        = texture(uAOMap, vTC).r;

        vec3 nTS = texture(uNormalMap, vTC).xyz * 2.0 - 1.0;
        vec3 N0  = vec3(0.0, 0.0, 1.0);
        vec3 p   = vec3(gl_FragCoord.xy, gl_FragCoord.z * 1000.0);
        mat3 TBN = cotangentFrame(N0, p, vTC);
        vec3 N   = normalize(TBN * nTS);

        vec3 V = vec3(0.0, 0.0, 1.0);
        vec3 L = normalize(uSunDirView);
        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 1e-4);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        vec3  F = FresnelSchlick(VdotH, F0);
        float D = DistributionGGX(NdotH, roughness);
        float G = GeometrySmith(NdotV, NdotL, roughness);
        vec3 spec = (D * G * F) / (4.0 * NdotV * max(NdotL, 1e-4) + 1e-4);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        vec3 diffuse  = kD * albedo * vColor.rgb;
        vec3 specular = spec * NdotL;
        color.rgb = (diffuse + specular) * ao;
    }

    // SOURCEPORT: hue-preserving brightness clamp. A naive clamp to [0,1]
    // clips red and green before blue, turning saturated browns/oranges
    // (e.g. partGround = #B4824B) into yellow at elevated uBrightness.
    // Scaling down uniformly when any channel exceeds 1.0 preserves hue.
    vec3 bright = color.rgb * uBrightness;
    float maxC  = max(max(bright.r, bright.g), bright.b);
    if (maxC > 1.0) bright /= maxC;
    color.rgb = bright;
    if (uFogEnabled && uDebugMode != 13) color.rgb = mix(uFogColor.rgb, color.rgb, vFog);

    FragColor = color;
}
