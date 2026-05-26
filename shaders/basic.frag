#version 330 core
noperspective in vec4  vColor;
noperspective in float vFog;
smooth        in vec2  vTexCoord;
noperspective in vec2  vTexCoordR;
noperspective in float vRhw;
noperspective in vec3  vWorldPos;

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

// SOURCEPORT: debug visualization mode (F8 cycles 0-9 in-game).
// 0 = normal          1 = PBR off         2 = PBR-pixels magenta
// 3 = vertex color    4 = solid gray       5 = fog factor grayscale
// 6 = fog disabled    7 = flat magenta     8 = shadow-map depth
// 9 = shadow factor
uniform int uDebugMode;

// SOURCEPORT: world shadow mapping.
// uWorldShadow gates shadow sampling; uShadowMap is bound to unit 4.
// uLightSpace projects world position to shadow NDC for depth comparison.
uniform bool      uWorldShadow;
uniform sampler2D uShadowMap;
uniform mat4      uLightSpace;
uniform float     uShadowStrength;

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
    // SOURCEPORT: Use depth-based LOD for all geometry. Screen-space derivative LOD
    // on NVIDIA hardware exhibits reduced precision in smooth varyings (vTexCoord),
    // creating a systematic UV bias that manifests as a bright circle in screen-center
    // on some depths. Depth-based LOD (from noperspective vRhw) is unaffected by
    // NVIDIA's rasterizer precision and eliminates the headlamp on all GPUs.
    float lod;
    {
        float camZ = 1.0 / max(vRhw, 1e-6);
        lod = max(0.0, 0.5 * log2(camZ / 512.0) - 0.75);
    }
    // SOURCEPORT: for alpha-tested geometry (foliage) snap UV to the nearest texel
    // centre and always sample mip level 0.
    // Two issues prevented faithful leaf rendering:
    //   1. Bilinear UV blending sampled adjacent transparent texels, darkening leaf edges.
    //      Snapping to texel centres eliminates that, matching D3D D3DFILTER_NEAREST.
    //   2. Trilinear mip blending: alpha test uses mip 0 but colour with lod>0 blends
    //      in mip 1, producing dark checkerboard patches at medium range.
    //      Forcing colour to mip 0 eliminates the mip-consistency mismatch.
    vec2 sampleUV = uAlphaTest
        ? (floor(vTC * vec2(tsz)) + 0.5) / vec2(tsz)
        : vTC;
    float sampleLOD = uAlphaTest ? 0.0 : lod;
    vec4 texel = textureLod(uTexture, sampleUV, sampleLOD);

    if (uAlphaTest) {
        if (texel.a < 0.5) discard;
    }

    vec4 color = texel * vColor;
    if (!uAlphaTest) color.a = 1.0; else color.a = vColor.a;

    // SOURCEPORT: early-exit debug modes (F8).
    if (uDebugMode == 2 && uPBR) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; } // PBR magenta
    if (uDebugMode == 3) { FragColor = vec4(vColor.rgb, 1.0); return; }              // vertex color
    if (uDebugMode == 4) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }          // solid gray
    if (uDebugMode == 7) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }          // flat magenta

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
    // clips red and green before blue, turning saturated browns/oranges into yellow.
    // Scaling down uniformly when any channel exceeds 1.0 preserves hue.
    vec3 bright = color.rgb * uBrightness;
    float maxC  = max(max(bright.r, bright.g), bright.b);
    if (maxC > 1.0) bright /= maxC;
    color.rgb = bright;

    // SOURCEPORT: world shadow map sampling (PCF 3x3).
    // Only runs when shadow pass was rendered this frame (uWorldShadow=true).
    // vRhw < 1.0 gates shadow to 3D geometry only: 2D/HUD vertices (aDepth=0)
    // produce vRhw=1.0 and must not be shadowed — world-origin vWorldPos for a
    // fullscreen rect would land inside the frustum and darken sun-blind quads.
    if (uWorldShadow && uShadowStrength > 0.0 && vRhw < 1.0) {
        vec4 lsPos = uLightSpace * vec4(vWorldPos, 1.0);
        vec3 proj  = lsPos.xyz / lsPos.w;
        proj = proj * 0.5 + 0.5;  // NDC [-1,1] → UV [0,1]

        // SOURCEPORT: mode 8 = shadow-map depth sampled at this fragment's UV (clamped,
        // no frustum gate). White=unwritten/far, gray/black=geometry recorded by depth pass.
        // If the whole screen is white the depth pass produced no data.
        if (uDebugMode == 8) {
            float d = texture(uShadowMap, clamp(proj.xy, 0.0, 1.0)).r;
            FragColor = vec4(d, d, d, 1.0);
            return;
        }
        // SOURCEPORT: mode 9 = raw shadow UV as RG gradient (no frustum gate).
        // proj.x→R, proj.y→G. Fully red+green = center of shadow frustum (proj=0.5,0.5).
        // Uniform colour everywhere = vWorldPos is constant (world-pos reconstruction broken).
        if (uDebugMode == 9) {
            FragColor = vec4(proj.x, proj.y, 0.0, 1.0);
            return;
        }

        // SOURCEPORT: proj.z >= 0.0 guards against behind-light geometry:
        // straddling triangles with a behind-camera vertex produce vWorldPos≈0
        // after interpolation; without this lower bound, proj.z < 0 still
        // passes the gate and spuriously darkens everything in that view.
        if (proj.z >= 0.0 && proj.z < 1.0 && proj.x >= 0.0 && proj.x <= 1.0 &&
                                              proj.y >= 0.0 && proj.y <= 1.0) {
            // SOURCEPORT: slope-based bias via screen-space derivatives.
            // dFdx/dFdy give the rate of depth change per pixel in shadow space;
            // their magnitude represents the shadow-map slope at this fragment.
            // This auto-scales with view distance and terrain steepness, eliminating
            // banding without Peter-panning. A small floor prevents acne on flat ground.
            float slopeBias = max(abs(dFdx(proj.z)), abs(dFdy(proj.z))) * 3.0;
            float bias = max(slopeBias, 0.0002);
            float shadow = 0.0;
            vec2 ts = 1.0 / vec2(textureSize(uShadowMap, 0));
            for (int ox = -1; ox <= 1; ++ox) {
                for (int oy = -1; oy <= 1; ++oy) {
                    float d = texture(uShadowMap, proj.xy + vec2(ox, oy) * ts).r;
                    shadow += (proj.z - bias > d) ? 1.0 : 0.0;
                }
            }
            shadow /= 9.0;
            color.rgb *= 1.0 - shadow * uShadowStrength;
        }
    }

    // SOURCEPORT: mode 5 = fog factor grayscale (1=no fog, 0=full fog).
    if (uFogEnabled && uDebugMode == 5) { FragColor = vec4(vFog, vFog, vFog, 1.0); return; }

    if (uFogEnabled && uDebugMode != 6) color.rgb = mix(uFogColor.rgb, color.rgb, vFog);

    FragColor = color;
}
