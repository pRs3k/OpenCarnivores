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
// SOURCEPORT: normal map alpha encodes height for parallax mapping (1=raised, 0=flat).
uniform bool      uPBR;
uniform sampler2D uNormalMap;
uniform sampler2D uMRMap;
uniform sampler2D uAOMap;
uniform float     uMetallicFactor;
uniform float     uRoughnessFactor;
// SOURCEPORT: world-space sun direction (normalised). Replaces screen-space approximation.
uniform vec3      uSunDirWorld;
// SOURCEPORT: parallax height scale. 0 = disabled (default). ~0.05 for visible relief.
// Height is encoded in the alpha channel of uNormalMap (1=raised, 0=flat).
uniform float     uParallaxScale;

// SOURCEPORT: debug visualization mode (F8 cycles 0-9 in-game).
// 0 = normal          1 = PBR off         2 = PBR-pixels magenta
// 3 = vertex color    4 = solid gray       5 = fog factor grayscale
// 6 = fog disabled    7 = flat magenta     8 = shadow-map depth
// 9 = shadow factor
uniform int uDebugMode;

// SOURCEPORT: camera world position (shared with vertex shader reconstruction path).
// Used here for the PBR view direction: V = normalize(uCameraPos - vWorldPos).
uniform vec3 uCameraPos;

// SOURCEPORT: world shadow mapping.
// uWorldShadow gates shadow sampling; uShadowMapArray is bound to unit 4.
uniform bool            uWorldShadow;
uniform sampler2DArrayShadow uShadowMapArray;   // SOURCEPORT: CSM — 3-layer depth array
uniform mat4                 uLightSpaceArr[3]; // SOURCEPORT: CSM — one matrix per cascade
uniform vec3                 uCascadeSplits;    // SOURCEPORT: CSM — x=C0 far, y=C1 far, z=unused (GU)
uniform vec3                 uCascadeBias;      // SOURCEPORT: CSM — per-cascade NDC bias x/y/z
uniform float     uShadowStrength;

out vec4 FragColor;

// SOURCEPORT: CSM — PCF 3×3 shadow sample for one cascade layer.
// Accesses global uniforms directly (legal in GLSL 330 for non-sampler parameters).
// Returns [0,1]: 0=fully lit, 1=fully in shadow. Fragments outside the cascade
// ortho frustum (proj outside [0,1]³) return 0 so they appear lit.
float sampleCascade(int cas, vec3 worldPos) {
    float bias = (cas == 0) ? uCascadeBias.x
               : (cas == 1) ? uCascadeBias.y : uCascadeBias.z;
    vec4 lsPos = uLightSpaceArr[cas] * vec4(worldPos, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w * 0.5 + 0.5;
    if (proj.z < 0.0 || proj.z >= 1.0 ||
        proj.x < 0.0 || proj.x >  1.0 ||
        proj.y < 0.0 || proj.y >  1.0) return 0.0;
    vec2 ts = 1.0 / vec2(textureSize(uShadowMapArray, 0).xy);
    float shadow = 0.0;
    for (int ox = -1; ox <= 1; ++ox)
        for (int oy = -1; oy <= 1; ++oy)
            shadow += 1.0 - texture(uShadowMapArray,
                vec4(proj.x + float(ox)*ts.x,
                     proj.y + float(oy)*ts.y,
                     float(cas), proj.z - bias));
    return shadow / 9.0;
}

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
    // SOURCEPORT: always use vertex alpha — opaque geometry has vColor.a=1.0 so this is
    // a no-op for terrain/models.  Dino shadow polygons carry vColor.a = Al/255 (≈0.376)
    // so they blend semi-transparently rather than being stamped at full opacity.
    color.a = vColor.a;

    // SOURCEPORT: early-exit debug modes (F8).
    if (uDebugMode == 2 && uPBR) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; } // PBR magenta
    if (uDebugMode == 3) { FragColor = vec4(vColor.rgb, 1.0); return; }              // vertex color
    if (uDebugMode == 4) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }          // solid gray
    if (uDebugMode == 7) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }          // flat magenta

    if (uPBR && uDebugMode != 1) {
        // SOURCEPORT: world-space PBR. All vectors computed in world space so that
        // lighting is correct regardless of camera orientation.
        vec3 albedo = texel.rgb;

        // SOURCEPORT: view direction in world space.
        // uCameraPos is already available from the shadow reconstruction uniforms.
        vec3 V = normalize(uCameraPos - vWorldPos);

        // SOURCEPORT: geometric surface normal from world-space position derivatives.
        // dFdx/dFdy of vWorldPos give world-space tangent directions along the screen axes.
        // Their cross product is the geometric normal; gl_FrontFacing corrects the sign
        // for back-face scenarios (transparent or double-sided surfaces).
        vec3 geoN = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
        if (dot(geoN, V) < 0.0) geoN = -geoN;

        // SOURCEPORT: build TBN in world space using position+UV derivatives.
        // cotangentFrame derives tangent/bitangent from dFdx/dFdy of p and uv,
        // so passing vWorldPos gives a world-space basis aligned with texture UVs.
        mat3 TBN = cotangentFrame(geoN, vWorldPos, vTC);

        // SOURCEPORT: parallax mapping from normal map alpha (1=raised, 0=flat).
        // Project view direction onto tangent/bitangent to get UV offset direction.
        // Skip when uParallaxScale==0 (standard normal maps with flat alpha).
        vec2 pUV = vTC;
        if (uParallaxScale > 0.0) {
            float h   = texture(uNormalMap, vTC).a;  // 1=surface level, 0=recessed
            float vTx = dot(TBN[0], V);              // V projected onto tangent
            float vTy = dot(TBN[1], V);              // V projected onto bitangent
            float vTz = max(dot(TBN[2], V), 0.1);   // V projected onto normal (clamp near 0°)
            pUV = vTC + vec2(vTx, vTy) / vTz * (h - 0.5) * uParallaxScale;
        }

        vec3 nTS    = texture(uNormalMap, pUV).xyz * 2.0 - 1.0;
        vec2 mr     = texture(uMRMap,     pUV).rg;
        float ao    = texture(uAOMap,     pUV).r;

        float metallic  = mr.r * uMetallicFactor;
        float roughness = max(mr.g * uRoughnessFactor, 0.04);

        // SOURCEPORT: perturb geometric normal by normal map sample (world space).
        vec3 N = normalize(TBN * nTS);

        vec3 L = normalize(uSunDirWorld);
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
        // SOURCEPORT: CSM — select cascade by horizontal world-space distance from camera.
        float distXZ = length(vWorldPos.xz - uCameraPos.xz);
        int cascade = (distXZ < uCascadeSplits.x) ? 0 : (distXZ < uCascadeSplits.y) ? 1 : 2;

        // SOURCEPORT: debug mode 8/9 — show depth/UV for the selected cascade.
        if (uDebugMode == 8 || uDebugMode == 9) {
            vec4 _lsPos = uLightSpaceArr[cascade] * vec4(vWorldPos, 1.0);
            vec3 _proj  = _lsPos.xyz / _lsPos.w * 0.5 + 0.5;
            if (uDebugMode == 8) { FragColor = vec4(_proj.zzz, 1.0); return; }
            if (uDebugMode == 9) { FragColor = vec4(_proj.xy, 0.0, 1.0); return; }
        }

        float shadow = sampleCascade(cascade, vWorldPos);

        // SOURCEPORT: CSM blend zone — cross-fade the last 30% of each cascade
        // into the next to hide the hard seam at the split boundary.
        float blend0Start = uCascadeSplits.x * 0.7;
        float blend1Start = uCascadeSplits.y * 0.7;
        if (cascade == 0 && distXZ >= blend0Start) {
            float t = clamp((distXZ - blend0Start) / (uCascadeSplits.x - blend0Start), 0.0, 1.0);
            shadow = mix(shadow, sampleCascade(1, vWorldPos), t);
        } else if (cascade == 1 && distXZ >= blend1Start) {
            float t = clamp((distXZ - blend1Start) / (uCascadeSplits.y - blend1Start), 0.0, 1.0);
            shadow = mix(shadow, sampleCascade(2, vWorldPos), t);
        }

        color.rgb *= 1.0 - shadow * uShadowStrength;
    }

    // SOURCEPORT: mode 5 = fog factor grayscale (1=no fog, 0=full fog).
    if (uFogEnabled && uDebugMode == 5) { FragColor = vec4(vFog, vFog, vFog, 1.0); return; }

    if (uFogEnabled && uDebugMode != 6) color.rgb = mix(uFogColor.rgb, color.rgb, vFog);

    FragColor = color;
}
