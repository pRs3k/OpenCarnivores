// SOURCEPORT: OpenGL 3.3 Core renderer backend for Carnivores 2.
// This replaces the Direct3D 6 execute buffer rendering model with modern OpenGL.
// The game engine does all vertex transformation on the CPU, so we receive
// pre-transformed screen-space vertices (like D3DTLVERTEX) and just need to
// rasterize them with the correct texture, fog, and blending.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max
#include "RendererGL.h"
#include "PostProcessing.h"
#include "ShaderPack.h"
#include "../Materials.h"
#include "../CustomMaterials.h"
#include "../HotReload.h"
#include "../VFS.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>

namespace XR { bool StereoActive(); }

// Global renderer instance
IRenderer* g_Renderer = nullptr;

// Game globals — updated by SetVideoMode whenever window is resized
extern int WinW, WinH;

// ============================================================
// Shader source — handles pre-transformed 2D vertices
// ============================================================

static const char* vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;       // screen-space x, y
layout(location = 1) in float aDepth;    // z (depth)
layout(location = 2) in vec4 aColor;     // diffuse RGBA
layout(location = 3) in vec4 aSpecular;  // specular RGBA (A = fog)
layout(location = 4) in vec2 aTexCoord;

uniform mat4 uProjection;

noperspective out vec4  vColor;
noperspective out float vFog;
// SOURCEPORT: two UV paths.
// vTexCoord (smooth) is used only for screen-space LOD derivatives.
// vTexCoordR / vRhw gives perspective-correct UV via explicit fragment-shader
// division, bypassing NVIDIA's hardware rasterizer storage for smooth varyings
// which may use reduced precision and produce a systematic UV bias at certain
// depth values (manifesting as a headlamp brightness ring on NVIDIA).
smooth        out vec2  vTexCoord;
noperspective out vec2  vTexCoordR;  // aTexCoord * rhw  (rhw = aDepth/16)
noperspective out float vRhw;        // rhw = aDepth/16

void main() {
    vec4 pos_ndc = uProjection * vec4(aPos, aDepth, 1.0);

    // SOURCEPORT: perspective-correct attribute interpolation.
    // D3D6 TL vertices carry rhw = sz/16 (= 1/camera_z) which the rasterizer uses
    // for perspective-correct UV/color interpolation.  With w_clip=1 (orthographic),
    // GL interpolates all varyings linearly in screen space — as the camera rotates,
    // vertex screen positions change while UVs stay fixed at TCMIN/TCMAX per vertex,
    // causing the interpolated UV at every fragment to drift.  This is the "ground
    // morphing and swimming" visible when standing still and looking around.
    // Fix: set w_clip = 1/rhw = 16/sz so GL's rasterizer sees the real camera-space
    // depth and applies perspective division automatically.  After perspective division
    // NDC position = pos_ndc.xyz (unchanged), but varyings are now correct.
    // Guard: HUD/sky/2D geometry uses sz=0 (aDepth=0) — keep w=1 for those.
    if (aDepth > 0.0) {
        // SOURCEPORT: clamp was 0.01 (= 1600 GU clip), pinning vRhw for all distant
        // terrain to the same value and making camZ=1/vRhw report ≤1600 GU everywhere
        // past that radius → depth-based LOD stuck at ~0 → temporal aliasing at 2K
        // minification onset (~3200 GU) → "headlamp circle".  1e-4 moves the guard to
        // 160 000 GU (well beyond any render distance) so real depths pass through.
        float rhw = max(aDepth, 1e-4) / 16.0;
        float w   = 1.0 / rhw;
        gl_Position  = vec4(pos_ndc.xyz * w, w);
        vTexCoordR   = aTexCoord * rhw;
        vRhw         = rhw;
    } else {
        gl_Position  = pos_ndc;
        vTexCoordR   = aTexCoord;
        vRhw         = 1.0;
    }

    vColor    = aColor;
    vTexCoord = aTexCoord;
    vFog      = aSpecular.a;
}
)";

static const char* fragmentShaderSrc = R"(
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
// SOURCEPORT: runtime brightness multiplier (1.0 = neutral, 0.0 = black, 2.0 = double-bright)
uniform float uBrightness;

// SOURCEPORT: PBR override path. When uPBR is set, the bound texture at unit 0
// is the albedo; units 1/2/3 supply tangent-space normal, metallic+roughness
// (glTF convention: metallic in R, roughness in G), and AO. Tangent frame is
// rebuilt from screen-space derivatives so the retail vertex format needs no
// tangent/bitangent attributes. Lighting = vertex-baked irradiance (vColor)
// as diffuse + Cook-Torrance GGX specular against a hardcoded sun direction.
uniform bool      uPBR;
uniform sampler2D uNormalMap;
uniform sampler2D uMRMap;
uniform sampler2D uAOMap;
uniform float     uMetallicFactor;
uniform float     uRoughnessFactor;
// SOURCEPORT: world-space sun direction (normalised).
uniform vec3      uSunDirWorld;
// SOURCEPORT: parallax height scale (0=disabled). N/A in this fallback (no vWorldPos).
uniform float     uParallaxScale;

// SOURCEPORT: debug visualization mode (toggled at runtime with F8).
// 0 = normal, 1 = PBR disabled (Lambert only), 2 = PBR-active fragments shown as magenta.
uniform int uDebugMode;

// SOURCEPORT: screen-space fog (idea 4). Computed per-fragment from depth and
// reconstructed world Y so headbob (stepdy) never affects fog density.
// _ZSCALE=-16 => vRhw=1/|camZ| => camDist=1/vRhw is camera depth in GU.
// ctHScale=64 is hardcoded (compile-time constant in Hunt.h).
uniform float uFogYBeginGU;  // fog ceiling in GU = fptr->YBegin * 64
uniform float uFogTransp;    // density distance (fptr->Transp)
uniform float uFogLimit;     // max fog factor   (fptr->FLimit)
uniform float uFogCameraY;   // stable camera Y without headbob (CameraYStable)
uniform int   uCameraInFog;  // 1 = camera centre is below fog ceiling
uniform float uFogVideoCY;   // D3D screen centre Y in pixels (VideoCY)
uniform float uFogWinH;      // window height in pixels
uniform float uFogCameraH;   // perspective Y scale factor (CameraH)

out vec4 FragColor;

// Christian Schüler's cotangent-frame trick — builds tangent basis from
// derivatives of position+UV so we don't need per-vertex tangents.
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
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

    ivec2 tsz = textureSize(uTexture, 0);

    // SOURCEPORT: depth-based LOD for ALL geometry — eliminates headlamp circle.
    // Screen-space derivatives (dFdx/dFdy) oscillate with headbob because vertex
    // screen-Y positions oscillate each frame, making LOD oscillate frame-to-frame
    // and sampling different mip levels → temporal brightness variation that averages
    // to a persistent "headlamp circle" on both foliage and terrain (terrain now has
    // mipmaps via glGenerateMipmap, so LOD choice matters for terrain too).
    // Camera depth (1/vRhw) is unaffected by a vertical headbob on level ground, so
    // depth-based LOD is stable and eliminates the headlamp on all geometry types.
    float camZ = 1.0 / max(vRhw, 1e-6);
    float lod  = max(0.0, 0.5 * log2(camZ / 512.0) - 0.75);
    // SOURCEPORT: use depth-based LOD for all geometry and plain vTC (no texel-snap).
    vec2 sampleUV = vTC;
    float sampleLOD = lod;
    vec4 texel = textureLod(uTexture, sampleUV, sampleLOD);

    if (uAlphaTest && uDebugMode != 9) {
        if (texel.a < 0.5) discard;
    }

    vec4 color = texel * vColor;
    if (!uAlphaTest) { color.a = 1.0; } else { color.a = vColor.a; }

    if (uDebugMode == 2 && uPBR) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (uDebugMode == 3) { FragColor = vec4(vColor.rgb, 1.0); return; }
    if (uDebugMode == 4) { FragColor = vec4(texel.rgb,  1.0); return; }
    if (uDebugMode == 5) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
    if (uDebugMode == 6) { FragColor = vec4(textureLod(uTexture, sampleUV, 0.0).rgb, 1.0); return; }
    if (uDebugMode == 7) { float l = lod / 4.0; FragColor = vec4(l, 0.0, max(0.0,1.0-l), 1.0); return; }
    if (uDebugMode == 8) { FragColor = vec4(fract(vTC), 0.0, 1.0); return; }
    if (uDebugMode == 9) { FragColor = vec4(textureLod(uTexture, sampleUV, sampleLOD).rgb, 1.0); return; }
    // SOURCEPORT: mode 10 = terrain texture in color, foliage/alpha-test geometry as solid gray.
    // If headlamp appears here, headlamp is in terrain. If not, headlamp is foliage-only.
    if (uDebugMode == 10) {
        if (uAlphaTest) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; }
        FragColor = vec4(texel.rgb, 1.0); return;
    }
    // SOURCEPORT: mode 11 = UV fract at 100x magnification. A 0.001 UV drift (invisible at 1x
    // in mode 8) shows as 0.1 color-unit phase shift here — confirms or rules out sub-texel drift.
    if (uDebugMode == 11) { FragColor = vec4(fract(vTC * 100.0), 0.0, 1.0); return; }
    // SOURCEPORT: mode 14 = smooth vTexCoord (GL perspective-correct) at 100x magnification.
    // Companion to mode 11. If pillar is ABSENT here but present in mode 11, the error is in
    // the noperspective vTexCoordR/vRhw manual division — switch foliage to use vTexCoord.
    if (uDebugMode == 14) { FragColor = vec4(fract(vTexCoord * 100.0), 0.0, 1.0); return; }
    // SOURCEPORT: mode 15 = |vTexCoord - vTC| * 10 — UV disagreement between GL and manual path.
    // Bright regions show where the two perspective-correction methods produce different UVs.
    // If the pillar/circle shape matches the headlamp, this is the source of the UV drift.
    if (uDebugMode == 15) { FragColor = vec4(abs(vTexCoord - vTC) * 10.0, 0.0, 1.0); return; }
    // SOURCEPORT: mode 16 = vRhw grayscale (camera depth, near=bright).
    // Reveals which geometry has anomalous depth/rhw that might corrupt noperspective UV interp.
    if (uDebugMode == 16) { FragColor = vec4(vec3(clamp(vRhw * 16.0, 0.0, 1.0)), 1.0); return; }
    // SOURCEPORT: mode 17 = foliage invisible (all alpha-tested fragments discarded).
    // If headlamp circle disappears in this mode → circle is foliage-only, not terrain.
    if (uDebugMode == 17) { if (uAlphaTest) discard; FragColor = vec4(texel.rgb * vColor.rgb, 1.0); return; }
    // SOURCEPORT: mode 18 = terrain invisible (opaque fragments → solid gray).
    // If headlamp circle disappears in this mode → circle is terrain, not foliage.
    if (uDebugMode == 18) { if (!uAlphaTest) { FragColor = vec4(0.5, 0.5, 0.5, 1.0); return; } }
    // SOURCEPORT: mode 19 = opaque blending only (no premultiplied alpha).
    // If circle disappears here → it's an additive/special blend artifact.
    if (uDebugMode == 19) { FragColor = vec4(color.rgb, 1.0); return; }
    // SOURCEPORT: mode 20 = depth visualization (vRhw indicates camera distance).
    // Shows if there's an anomalous depth boundary that creates a circle artifact.
    if (uDebugMode == 20) { float d = clamp(vRhw * 8.0, 0.0, 1.0); FragColor = vec4(d, d, d, 1.0); return; }
    // SOURCEPORT: mode 21 = vertex Light heatmap (shows per-vertex brightness from lightmap).
    // If circle appears here → the circle is in the pre-computed Light values, not rendering.
    if (uDebugMode == 21) { float l = clamp(color.r / 255.0, 0.0, 1.0); FragColor = vec4(l, l, l, 1.0); return; }
    // SOURCEPORT: mode 22 = flat magenta (verify mode is working).
    // If screen is NOT bright magenta, mode 22 isn't executing.
    if (uDebugMode == 22) { FragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }

    // SOURCEPORT: PBR path — Cook-Torrance GGX on top of retail vertex light.
    // vColor.rgb already encodes per-vertex Lambert against the sun + ambient,
    // so we treat it as diffuse irradiance and add a physically-plausible
    // specular on top. Tangent frame comes from screen-space derivatives so
    // no per-vertex tangent attribute is needed.
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

        vec3 V = vec3(0.0, 0.0, 1.0);  // screen-forward approximation (no vWorldPos in fallback)
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

    // SOURCEPORT: hue-preserving brightness clamp.
    vec3 bright = color.rgb * uBrightness;
    float maxC  = max(max(bright.r, bright.g), bright.b);
    if (maxC > 1.0) bright /= maxC;
    color.rgb = bright;
    // SOURCEPORT: screen-space fog — replaces per-vertex vFog which pulsed with headbob.
    // Reconstruct world Y from gl_FragCoord.y + depth, then mirror CalcFogLevel formula.
    if (uFogEnabled && uDebugMode != 13) {
        float camDist = 1.0 / max(vRhw, 1e-6);

        // D3D sy = WinH - gl_FragCoord.y; camRelY = (VideoCY - sy) * camDist / CameraH
        float camRelY = (uFogVideoCY - uFogWinH + gl_FragCoord.y) * camDist / max(uFogCameraH, 1.0);
        float worldY  = camRelY + uFogCameraY;

        const float kInvHScale = 1.0 / 64.0;  // 1/ctHScale
        float fla = -(worldY      - uFogYBeginGU) * kInvHScale;
        float flb = -(uFogCameraY - uFogYBeginGU) * kInvHScale;
        if (uCameraInFog == 0) flb = min(flb, 0.0);

        float fogFactor = 0.0;
        if (fla > 0.0 || flb > 0.0) {
            float d = camDist;
            if (fla < 0.0) { d *= flb / (flb - fla); fla = 0.0; }
            if (flb < 0.0) { d *= fla / (fla - flb); flb = 0.0; }
            float fl = (fla + flb) * (d + uFogTransp * 0.5) / max(uFogTransp, 1.0);
            fogFactor = clamp(fl / max(uFogLimit, 1.0), 0.0, 1.0);
        }
        if (uDebugMode == 12) { FragColor = vec4(fogFactor, fogFactor, fogFactor, 1.0); return; }
        color.rgb = mix(color.rgb, uFogColor.rgb, fogFactor);
    }

    FragColor = color;
}
)";

// ============================================================
// Helper: compile a shader and check for errors
// ============================================================

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
    }
    return shader;
}

// ============================================================
// Convert 16-bit RGB565 pixel to RGBA8888
// ============================================================

static inline uint32_t RGB565toRGBA(uint16_t c) {
    if (c == 0) return 0x00000000; // Color key: black = transparent
    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
    uint32_t b = ((c)       & 0x1F) * 255 / 31;
    // SOURCEPORT: RGB565 uses all 16 bits for color (no alpha channel).
    // Alpha always 0xFF (opaque). Only RGB555 has alpha bit (0x8000).
    return (0xFF << 24) | (b << 16) | (g << 8) | r; // ABGR for GL
}

static inline uint32_t RGB555toRGBA(uint16_t c) {
    if (c == 0) return 0x00000000; // Color key: black = transparent
    uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x1F) * 255 / 31;
    uint32_t b = ((c)       & 0x1F) * 255 / 31;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

// ============================================================
// RendererGL implementation
// ============================================================

RendererGL::RendererGL() {}

RendererGL::~RendererGL() {
    RendererGL::Shutdown(); // SOURCEPORT: explicit non-virtual call avoids virtual dispatch in destructor
}

bool RendererGL::Init(void* /*windowHandle*/, int width, int height) {
    // SOURCEPORT: Phase 4 — read display options from game globals
    extern int OptDisplayMode, OptVSync;

    m_width  = width;
    m_height = height;

    // Create SDL window with OpenGL context
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // SOURCEPORT: Request GL 4.1 Core — OpenXR runtimes (Meta Link, SteamVR)
    // require GL 4.0+; 4.1 is also the highest Core version supported on macOS.
    // All 3.3 code is forward-compatible: only the context version changes.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // SOURCEPORT: stencil for weapon/overlay masking

    // Build window flags based on OptDisplayMode:
    //   0 = windowed, 1 = fullscreen (exclusive), 2 = borderless fullscreen
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (OptDisplayMode == 1) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    } else if (OptDisplayMode == 2) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Borderless fullscreen: use desktop resolution
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            width    = dm.w;
            height   = dm.h;
            m_width  = width;
            m_height = height;
        }
    }

    m_window = SDL_CreateWindow(
        "OpenCarnivores",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        windowFlags
    );
    if (!m_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // For exclusive fullscreen, query the actual drawable size (may differ from requested)
    if (OptDisplayMode == 1) {
        SDL_GL_GetDrawableSize(m_window, &m_width, &m_height);
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    // Load OpenGL functions
    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!version) {
        fprintf(stderr, "gladLoadGL failed\n");
        return false;
    }

    // SOURCEPORT: VSync — adaptive (-1) falls back to regular (1) if unsupported
    if (OptVSync) {
        if (SDL_GL_SetSwapInterval(-1) < 0)
            SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    CompileShaders();

    // SOURCEPORT: register shader files for hot reload. Watches are cheap (one
    // stat() per file ~3× per second); callbacks only fire when mtime advances.
    HotReload::Watch("shaders/basic.vert", [this]() {
        CompileShaders();
        fprintf(stdout, "[HotReload] shader reloaded: shaders/basic.vert\n");
    });
    HotReload::Watch("shaders/basic.frag", [this]() {
        CompileShaders();
        fprintf(stdout, "[HotReload] shader reloaded: shaders/basic.frag\n");
    });

    CreateBuffers();

    // SOURCEPORT: force driver ISA (GPU machine-code) compilation for all shader
    // programs before the first game frame.  GL drivers (especially NVIDIA) defer
    // true compilation to the first draw call with a specific program+VAO layout;
    // zero-vertex draws trigger that work at init time so no per-frame stall occurs.
    {
        glBindVertexArray(m_vao);
        glUseProgram(m_shaderProgram);       glDrawArrays(GL_TRIANGLES, 0, 0);
        glUseProgram(m_depthShaderProgram);  glDrawArrays(GL_TRIANGLES, 0, 0);
        glBindVertexArray(m_wsVAO);
        glUseProgram(m_wsDepthProgram);      glDrawArrays(GL_TRIANGLES, 0, 0);
        glBindVertexArray(0);
        glUseProgram(m_shaderProgram);
        glFlush();  // submit draw commands so the driver can begin ISA compilation
    }

    // SOURCEPORT: Phase 1 post-processing pipeline initialization
    m_postProcessingPipeline = new PostProcessingPipeline();
    if (!m_postProcessingPipeline->Initialize(m_width, m_height)) {
        fprintf(stderr, "ERROR: PostProcessingPipeline initialization failed\n");
        return false;
    }

    // Initial GL state
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL); // Carnivores uses GREATEREQUAL depth test
    glClearDepth(0.0);      // Clear to 0 since we use GEQUAL
    // SOURCEPORT: the projection maps sz=0.25 (camera z=-64) to z_ndc=1 (GL far plane).
    // HUD weapon verts at z > -64 would be clipped by GL's far plane without this.
    // GL_DEPTH_CLAMP disables near/far z-clipping and just clamps depth to [0,1],
    // so close-to-camera stock/grip geometry renders correctly.
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); // Original game: D3DCULL_NONE

    // Create reusable texture for bitmap uploads
    glGenTextures(1, &m_bitmapTexture);

    // SOURCEPORT: 1x1 white texture — used as fallback when no texture is set (e.g. circle/particle rendering)
    {
        uint32_t white = 0xFFFFFFFF;
        glGenTextures(1, &m_whiteTexture);
        glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // SOURCEPORT: query max anisotropic filtering level for menu display
    if (GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic) {
        GLfloat maxAniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        m_maxAnisotropy = (int)maxAniso;
    } else {
        m_maxAnisotropy = 1;
    }

    fprintf(stderr, "RendererGL: OpenGL %d.%d initialized (%dx%d mode=%d vsync=%d aniso=%d)\n",
            GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version),
            m_width, m_height, OptDisplayMode, OptVSync, m_maxAnisotropy);

    // SOURCEPORT: CSM — create a 3-layer depth texture array and one FBO per cascade.
    // Each FBO renders to one layer of the array via glFramebufferTextureLayer.
    // sampler2DArrayShadow in basic.frag samples all 3 layers with a single binding.
    {
        glGenTextures(1, &m_cascadeDepthTex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_cascadeDepthTex);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
                     CASCADE_SHADOW_SIZE, CASCADE_SHADOW_SIZE, NUM_SHADOW_CASCADES,
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        // SOURCEPORT: GL_LINEAR + GL_COMPARE_REF_TO_TEXTURE enables hardware PCF on the array.
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.f, 1.f, 1.f, 1.f};
        glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);

        glGenFramebuffers(NUM_SHADOW_CASCADES, m_cascadeFBO);
        bool allOk = true;
        for (int c = 0; c < NUM_SHADOW_CASCADES; ++c) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_cascadeFBO[c]);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_cascadeDepthTex, 0, c);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "[RendererGL] Cascade %d FBO incomplete (0x%x)\n", c, st);
                allOk = false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (allOk)
            fprintf(stdout, "[RendererGL] CSM: %d cascades × %d² depth array created\n",
                    NUM_SHADOW_CASCADES, CASCADE_SHADOW_SIZE);

        glUseProgram(m_shaderProgram);
        glUniform1i(m_locShadowMapArray, 4);
        glUniform1i(m_locWorldShadow, 0);
        glUniform1f(m_locShadowStrength, m_shadowStrength);
    }

    // SOURCEPORT: auto-load shader pack named "default" if present in shaderpacks/
    {
        auto& spm = ShaderPackManager::Get();
        spm.DiscoverPacks();
        for (const auto& name : spm.GetAvailable()) {
            if (name == "default") {
                spm.ApplyPack("default", this);
                break;
            }
        }
    }

    // SOURCEPORT: pre-warm the bloom/tone-map post-process pipeline.
    // RunPostOverlay() lazily compiles 3 shader programs and allocates two
    // half-res ping/pong FBOs on its first call.  Calling it here during init
    // (on an empty backbuffer) front-loads the 50-200 ms driver compile cost so
    // the first frame with post-processing active does not produce a visible hitch.
    RunPostOverlay();

    return true;
}

// SOURCEPORT: read a text file fully into a std::string, returning empty on miss.
// Used so `shaders/basic.{vert,frag}` can override the embedded shader source
// for dev hot reload; missing files fall back to the embedded strings silently.
static std::string ReadTextFile(const char* path) {
    // SOURCEPORT: route shader loads through VFS for mod overrides.
    FILE* f = VFS::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    if (n > 0) { s.resize((size_t)n); std::fread(&s[0], 1, (size_t)n, f); }
    std::fclose(f);
    return s;
}

void RendererGL::CompileShaders() {
    // SOURCEPORT: prefer shaders/basic.{vert,frag} on disk (hot-reloadable);
    // fall back to the embedded sources if either file is missing.
    std::string vsDisk = ReadTextFile("shaders/basic.vert");
    std::string fsDisk = ReadTextFile("shaders/basic.frag");
    const char* vsSrc = vsDisk.empty() ? vertexShaderSrc   : vsDisk.c_str();
    const char* fsSrc = fsDisk.empty() ? fragmentShaderSrc : fsDisk.c_str();

    GLuint vs = CompileShader(GL_VERTEX_SHADER,   vsSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        // Keep any previously-linked program active — hot-reload failure leaves
        // the game running on the last good shader.
        if (m_shaderProgram) return;
    } else {
        if (m_shaderProgram) glDeleteProgram(m_shaderProgram);
        m_shaderProgram = prog;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    glUseProgram(m_shaderProgram);
    m_locProjection = glGetUniformLocation(m_shaderProgram, "uProjection");
    m_locTexture    = glGetUniformLocation(m_shaderProgram, "uTexture");
    m_locFogEnabled = glGetUniformLocation(m_shaderProgram, "uFogEnabled");
    m_locFogColor   = glGetUniformLocation(m_shaderProgram, "uFogColor");
    m_locAlphaTest  = glGetUniformLocation(m_shaderProgram, "uAlphaTest");
    m_locBrightness = glGetUniformLocation(m_shaderProgram, "uBrightness");
    // uHUDMode removed — fproc1/fproc2 alpha handled unconditionally in shader

    // SOURCEPORT: PBR uniforms + sampler bindings for units 1/2/3.
    m_locPBR              = glGetUniformLocation(m_shaderProgram, "uPBR");
    m_locMetallicFactor   = glGetUniformLocation(m_shaderProgram, "uMetallicFactor");
    m_locRoughnessFactor  = glGetUniformLocation(m_shaderProgram, "uRoughnessFactor");
    m_locSunDirWorld      = glGetUniformLocation(m_shaderProgram, "uSunDirWorld");
    m_locParallaxScale    = glGetUniformLocation(m_shaderProgram, "uParallaxScale");
    m_locDebugMode        = glGetUniformLocation(m_shaderProgram, "uDebugMode");
    m_locWorldShadow      = glGetUniformLocation(m_shaderProgram, "uWorldShadow"); // SOURCEPORT: cached; used by SetHUDMode and EndWorldShadowPass

    // SOURCEPORT: cache per-frame world-pos reconstruction + CSM shadow uniforms.
    // Avoids ~17 driver hash-map lookups per frame (SetCameraWorldUniforms +
    // EndWorldShadowPass) that each implicitly sync with shader state.
    m_locVideoCX        = glGetUniformLocation(m_shaderProgram, "uVideoCX");
    m_locVideoCY        = glGetUniformLocation(m_shaderProgram, "uVideoCY");
    m_locCameraW        = glGetUniformLocation(m_shaderProgram, "uCameraW");
    m_locCameraH        = glGetUniformLocation(m_shaderProgram, "uCameraH");
    m_locCameraPos      = glGetUniformLocation(m_shaderProgram, "uCameraPos");
    m_locCamToWorld     = glGetUniformLocation(m_shaderProgram, "uCamToWorld");
    m_locShadowMapArray = glGetUniformLocation(m_shaderProgram, "uShadowMapArray");
    m_locLightSpaceArr  = glGetUniformLocation(m_shaderProgram, "uLightSpaceArr[0]");
    m_locShadowStrength = glGetUniformLocation(m_shaderProgram, "uShadowStrength");
    m_locCascadeBias    = glGetUniformLocation(m_shaderProgram, "uCascadeBias");
    m_locCascadeSplits  = glGetUniformLocation(m_shaderProgram, "uCascadeSplits");

    // SOURCEPORT: water material uniform locations
    m_locWaterMode       = glGetUniformLocation(m_shaderProgram, "uWaterMode");
    m_locWaterTime       = glGetUniformLocation(m_shaderProgram, "uWaterTime");
    m_locWaterScene      = glGetUniformLocation(m_shaderProgram, "uWaterScene");
    m_locWaterDepth      = glGetUniformLocation(m_shaderProgram, "uWaterDepth");
    m_locWaterScreenSize = glGetUniformLocation(m_shaderProgram, "uWaterScreenSize");
    m_locWaterWave       = glGetUniformLocation(m_shaderProgram, "uWaterWave");
    m_locWaterClarity    = glGetUniformLocation(m_shaderProgram, "uWaterClarity");
    m_locWaterDeepColor  = glGetUniformLocation(m_shaderProgram, "uWaterDeepColor");
    m_locWaterFoamWidth  = glGetUniformLocation(m_shaderProgram, "uWaterFoamWidth");
    m_locWaterReflect    = glGetUniformLocation(m_shaderProgram, "uWaterReflect");

    // SOURCEPORT: screen-space fog uniform locations
    m_locFogYBeginGU  = glGetUniformLocation(m_shaderProgram, "uFogYBeginGU");
    m_locFogTransp    = glGetUniformLocation(m_shaderProgram, "uFogTransp");
    m_locFogLimit     = glGetUniformLocation(m_shaderProgram, "uFogLimit");
    m_locFogCameraY   = glGetUniformLocation(m_shaderProgram, "uFogCameraY");
    m_locCameraInFog  = glGetUniformLocation(m_shaderProgram, "uCameraInFog");
    m_locFogVideoCY   = glGetUniformLocation(m_shaderProgram, "uFogVideoCY");
    m_locFogWinH      = glGetUniformLocation(m_shaderProgram, "uFogWinH");
    m_locFogCameraH   = glGetUniformLocation(m_shaderProgram, "uFogCameraH");
    glUniform1f(m_locFogYBeginGU, 0.0f);
    glUniform1f(m_locFogTransp,   4000.0f);
    glUniform1f(m_locFogLimit,    1.0f);
    glUniform1f(m_locFogCameraY,  0.0f);
    glUniform1i(m_locCameraInFog, 0);
    glUniform1f(m_locFogVideoCY,  300.0f);
    glUniform1f(m_locFogWinH,     600.0f);
    glUniform1f(m_locFogCameraH,  400.0f);

    GLint locNormal = glGetUniformLocation(m_shaderProgram, "uNormalMap");
    GLint locMR     = glGetUniformLocation(m_shaderProgram, "uMRMap");
    GLint locAO     = glGetUniformLocation(m_shaderProgram, "uAOMap");
    if (m_locTexture >= 0) glUniform1i(m_locTexture, 0);
    if (locNormal   >= 0)  glUniform1i(locNormal,    1);
    if (locMR       >= 0)  glUniform1i(locMR,        2);
    if (locAO       >= 0)  glUniform1i(locAO,        3);
    glUniform1i(m_locPBR, 0);
    glUniform1f(m_locMetallicFactor,  1.0f);
    glUniform1f(m_locRoughnessFactor, 1.0f);
    // SOURCEPORT: sun direction initialised to noon; SetSunDirection overwrites each frame.
    glUniform3f(m_locSunDirWorld, -0.577f, 0.577f, -0.577f);
    glUniform1f(m_locParallaxScale, 0.0f);

    glUniform1f(m_locBrightness, 1.0f); // default: neutral (no change)
    if (m_locDebugMode >= 0) glUniform1i(m_locDebugMode, 0); // default: normal rendering

    // Set up orthographic projection matrix for screen-space rendering
    // Maps (0,0)-(WinW,WinH) to clip space in X/Y.
    // Z: _ZSCALE = -16 so sz = -16/camZ ranges from 0 (far) to 0.25 (near clip at z=-64).
    // SOURCEPORT: Set N=0, F=0.25 so the full sz range [0,0.25] maps to window depth [0,1],
    // using the entire depth buffer precision instead of just 12.5% of it.
    // With glClearDepth(0) and GL_GEQUAL, far=0 and near=1 is correct front-to-back ordering.
    float L = 0.0f, R = (float)m_width;
    float T = 0.0f, B = (float)m_height;
    float N = 0.0f, F = 0.25f;
    float proj[16] = {
        2.0f/(R-L),     0.0f,         0.0f,           0.0f,
        0.0f,           2.0f/(T-B),   0.0f,           0.0f,
        0.0f,           0.0f,         2.0f/(F-N),     0.0f,
        -(R+L)/(R-L),  -(T+B)/(T-B), -(F+N)/(F-N),   1.0f,
    };
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, proj);

    glUniform1i(m_locTexture, 0); // Texture unit 0

    // SOURCEPORT: Compile depth shader for shadow mapping
    std::string depthVsDisk = ReadTextFile("shaders/depth.vert");
    std::string depthFsDisk = ReadTextFile("shaders/depth.frag");
    const char* depthVsSrc = depthVsDisk.empty() ? "// Default depth vertex shader\n#version 330 core\nlayout(location=0) in vec2 aPos;\nlayout(location=1) in float aDepth;\nvoid main() { gl_Position = vec4(aPos, aDepth, 1.0); }" : depthVsDisk.c_str();
    const char* depthFsSrc = depthFsDisk.empty() ? "// Default depth fragment shader\n#version 330 core\nout float fragDepth;\nvoid main() { fragDepth = gl_FragCoord.z; }" : depthFsDisk.c_str();

    GLuint depthVs = CompileShader(GL_VERTEX_SHADER, depthVsSrc);
    GLuint depthFs = CompileShader(GL_FRAGMENT_SHADER, depthFsSrc);

    GLuint depthProg = glCreateProgram();
    glAttachShader(depthProg, depthVs);
    glAttachShader(depthProg, depthFs);
    glLinkProgram(depthProg);

    glGetProgramiv(depthProg, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(depthProg, sizeof(log), nullptr, log);
        fprintf(stderr, "Depth shader link error: %s\n", log);
        glDeleteProgram(depthProg);
        m_depthShaderProgram = 0;
    } else {
        if (m_depthShaderProgram) glDeleteProgram(m_depthShaderProgram);
        m_depthShaderProgram = depthProg;
        m_depthLocAlphaTest  = glGetUniformLocation(depthProg, "uAlphaTest");
        // SOURCEPORT: cache depth-shader locations — used 8× per cascade (24×/frame).
        m_depthLocLightSpace = glGetUniformLocation(depthProg, "uLightSpace");
        m_depthLocVideoCX    = glGetUniformLocation(depthProg, "uVideoCX");
        m_depthLocVideoCY    = glGetUniformLocation(depthProg, "uVideoCY");
        m_depthLocCameraW    = glGetUniformLocation(depthProg, "uCameraW");
        m_depthLocCameraH    = glGetUniformLocation(depthProg, "uCameraH");
        m_depthLocCameraPos  = glGetUniformLocation(depthProg, "uCameraPos");
        m_depthLocCamToWorld = glGetUniformLocation(depthProg, "uCamToWorld");
        m_depthLocTexture    = glGetUniformLocation(depthProg, "uTexture");
        fprintf(stderr, "[Shadow] depth prog=%u uLightSpace=%d uCamToWorld=%d uCameraPos=%d\n",
            depthProg, m_depthLocLightSpace, m_depthLocCamToWorld, m_depthLocCameraPos);
    }

    glDeleteShader(depthVs);
    glDeleteShader(depthFs);

    // SOURCEPORT: compile world-space depth shader (ws_depth.vert + depth.frag).
    // Used for objects behind the camera that cannot be reconstructed from screen-space.
    std::string wsVsDisk = ReadTextFile("shaders/ws_depth.vert");
    if (!wsVsDisk.empty()) {
        GLuint wsVs = CompileShader(GL_VERTEX_SHADER,  wsVsDisk.c_str());
        GLuint wsFs = CompileShader(GL_FRAGMENT_SHADER, depthFsSrc);
        GLuint wsProg = glCreateProgram();
        glAttachShader(wsProg, wsVs);
        glAttachShader(wsProg, wsFs);
        glLinkProgram(wsProg);
        GLint wsOk = 0; glGetProgramiv(wsProg, GL_LINK_STATUS, &wsOk);
        if (!wsOk) {
            char log[512]; glGetProgramInfoLog(wsProg, sizeof(log), nullptr, log);
            fprintf(stderr, "[Shadow] ws_depth link error: %s\n", log);
            glDeleteProgram(wsProg);
        } else {
            if (m_wsDepthProgram) glDeleteProgram(m_wsDepthProgram);
            m_wsDepthProgram  = wsProg;
            // SOURCEPORT: cache ws-depth locations — FlushWorldSpaceShadow fires per
            // alpha-test state change, potentially dozens of times per shadow frame.
            m_wsLocLightSpace = glGetUniformLocation(wsProg, "uLightSpace");
            m_wsLocAlphaTest  = glGetUniformLocation(wsProg, "uAlphaTest");
            glUseProgram(m_wsDepthProgram);
            glUniform1i(glGetUniformLocation(m_wsDepthProgram, "uTexture"), 0);
        }
        glDeleteShader(wsVs); glDeleteShader(wsFs);
    }
}

void RendererGL::CreateBuffers() {
    // Main VAO/VBO for rendering pre-transformed vertices
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Allocate enough for the larger of the two buffers
    glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex) * MAX_MAIN_VERTICES, nullptr, GL_DYNAMIC_DRAW);

    // Vertex layout matching RenderVertex struct
    // location 0: vec2 pos (sx, sy)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, sx));
    // location 1: float depth (sz)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, sz)); // NOLINT(performance-no-int-to-ptr)
    // location 2: vec4 color — D3D stores as ARGB (0xAARRGGBB), in memory [BB,GG,RR,AA].
    // Use GL_BGRA so OpenGL remaps bytes to (R=RR, G=GG, B=BB, A=AA) correctly.
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, color)); // NOLINT(performance-no-int-to-ptr)
    // location 3: vec4 specular — same ARGB layout
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, specular)); // NOLINT(performance-no-int-to-ptr)
    // location 4: vec2 texcoord (tu, tv)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, tu)); // NOLINT(performance-no-int-to-ptr)
    glBindVertexArray(0);

    // SOURCEPORT: world-space shadow VAO/VBO (layout: vec3 pos + vec2 uv)
    glGenVertexArrays(1, &m_wsVAO);
    glGenBuffers(1, &m_wsVBO);
    glBindVertexArray(m_wsVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_wsVBO);
    glBufferData(GL_ARRAY_BUFFER, MAX_WS_SHADOW_VERTS * sizeof(WSShadowVert), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WSShadowVert), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WSShadowVert), (void*)(3*sizeof(float))); // NOLINT(performance-no-int-to-ptr)
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void RendererGL::InvalidateTextureCache() {
    for (auto& pair : m_texCache)
        glDeleteTextures(1, &pair.second.texId);
    m_texCache.clear();
    m_currentTexture = 0;
}

void RendererGL::Shutdown() {
    // SOURCEPORT: Clean up post-processing pipeline
    if (m_postProcessingPipeline) {
        delete m_postProcessingPipeline;
        m_postProcessingPipeline = nullptr;
    }

    // Clean up textures
    for (auto& pair : m_texCache) {
        glDeleteTextures(1, &pair.second.texId);
    }
    m_texCache.clear();

    // SOURCEPORT: CSM cascade FBO/texture cleanup
    if (m_cascadeDepthTex) { glDeleteTextures(1, &m_cascadeDepthTex); m_cascadeDepthTex = 0; }
    for (int c = 0; c < NUM_SHADOW_CASCADES; ++c) {
        if (m_cascadeFBO[c]) { glDeleteFramebuffers(1, &m_cascadeFBO[c]); m_cascadeFBO[c] = 0; }
    }

    if (m_wsVBO) { glDeleteBuffers(1, &m_wsVBO); m_wsVBO = 0; }
    if (m_wsVAO) { glDeleteVertexArrays(1, &m_wsVAO); m_wsVAO = 0; }
    if (m_wsDepthProgram) { glDeleteProgram(m_wsDepthProgram); m_wsDepthProgram = 0; }
    if (m_bitmapTexture) glDeleteTextures(1, &m_bitmapTexture);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_fsQuadVbo) glDeleteBuffers(1, &m_fsQuadVbo);
    if (m_fsQuadVao) glDeleteVertexArrays(1, &m_fsQuadVao);
    if (m_shaderProgram) glDeleteProgram(m_shaderProgram);

    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

// --- Frame management ---

void RendererGL::BeginFrame() {
    glUseProgram(m_shaderProgram);

    // SOURCEPORT: Sync viewport and internal dims to WinW×WinH every frame.
    // SetVideoMode() may update WinW/WinH to the actual drawable size after Init()
    // (HiDPI, SDL scaling), so the viewport and DrawFullscreenRect quad must follow.
    if (m_width != WinW || m_height != WinH) {
        m_width  = WinW;
        m_height = WinH;
        glViewport(0, 0, WinW, WinH);
    } else {
        // Ensure viewport is set even if size didn't change
        glViewport(0, 0, WinW, WinH);
    }

    // Rebuild projection every frame so it always matches the current WinW×WinH
    // game coordinate space (updated by SetVideoMode on every resize).
    float R = (float)WinW, B = (float)WinH;
    float N = 0.0f,        F = 0.25f;
    float proj[16] = {
        2.0f/R,   0.0f,   0.0f,          0.0f,
        0.0f,    -2.0f/B, 0.0f,          0.0f,
        0.0f,     0.0f,   2.0f/(F-N),    0.0f,
       -1.0f,     1.0f,  -(F+N)/(F-N),   1.0f,
    };
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, proj);
    // SOURCEPORT: cache for CustomMaterials::Apply; custom programs need the
    // same screen-space projection to render pre-transformed vertices correctly.
    std::memcpy(m_projMatrix, proj, sizeof(proj));

    // SOURCEPORT: reset shadow flag each frame; EndWorldShadowPass sets it to 1.
    m_shadowsRendered = false;
    glUniform1i(m_locWorldShadow, 0);

    // SOURCEPORT: always render scene to default framebuffer (FBO 0).
    // RunPostOverlay() uses glCopyTexImage2D to capture the backbuffer after draw,
    // so no FBO capture before DrawScene() is needed. The old BeginCapture() path
    // routed the scene into a PostProcessingPipeline FBO which caused RunPostOverlay()
    // to copy an empty backbuffer (black bloom). Removed.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WinW, WinH);

    m_frameCounter++;
}

void RendererGL::EndFrame() {
    // SOURCEPORT: Post-process injection point — runs after all scene rendering,
    // before buffer swap. Pipeline is built up incrementally:
    //   Step 1 (current): verify fullscreen quad renders a visible tint
    //   Step 2: copy backbuffer → texture, render passthrough (no visible change)
    //   Step 3: bind FBO before scene, read it here
    //   Step 4+: real effects (bloom, vignette, etc.)
    // SOURCEPORT: post-processing moved to ApplyPostProcess(), called from the game
    // loop immediately after DrawScene() so UI elements drawn afterwards are unaffected.
    SDL_GL_SwapWindow(m_window);
}

// SOURCEPORT: apply bloom + tone mapping to the 3D scene captured in the back-buffer.
// Must be called after all 3D geometry is drawn but BEFORE any HUD/UI draws so that
// compass, wind meter, health bar, etc. are composited clean on top.
void RendererGL::ApplyPostProcess() {
    if (m_postOverlayEnabled || m_toneMappingMode != 0 || m_cgEnabled || m_godRaysEnabled
        || m_heightFogEnabled || m_ssaoEnabled) {
        RunPostOverlay();
    }
}

void RendererGL::UpdateProjection(const float* mat16) {
    glUseProgram(m_shaderProgram);
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, mat16);
    std::memcpy(m_projMatrix, mat16, 16 * sizeof(float));
}

void RendererGL::ClearBuffers() {
    glStencilMask(0xFF); // must unmask stencil writes before clearing
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void RendererGL::RestoreEngineGLState() {
    // SOURCEPORT: XR::EndFrame resets GL state for the compositor's benefit
    // (glUseProgram(0), glDepthFunc(GL_LESS), glClearDepth(1.0), glDisable(GL_BLEND)).
    // These persist into the next frame's VR eye-render loop, breaking the engine's
    // reversed depth convention: sky writes depth≈0, then GL_LESS rejects all closer
    // geometry (walls, terrain) because their depth > 0. Result: geometry invisible
    // where sky was drawn — "holes in walls". Restore all affected state here.
    glUseProgram(m_shaderProgram);
    glDepthFunc(GL_GEQUAL);   // reversed depth: near=1, far=0, clear=0
    glClearDepth(0.0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// --- Texture management ---

// SOURCEPORT: sRGB-correct mipmap generation (mirrors renderd3d.cpp version).
// glGenerateMipmap averages in gamma space → higher mips are perceptually
// darker → GPU picks a darker mip when camera moves, creating a "circle of
// light" effect (near tiles stay on mip 0; far tiles go to darker high mips).
static inline float rgl_srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static inline float rgl_linear_to_srgb(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c < 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}
static void rgl_GenerateLinearMipmaps(const uint32_t* level0, int w0, int h0)
{
    std::vector<uint32_t> src(level0, level0 + w0 * h0);
    int w = w0, h = h0;
    for (int lv = 1; w > 1 || h > 1; ++lv) {
        int nw = (w > 1) ? w / 2 : 1;
        int nh = (h > 1) ? h / 2 : 1;
        std::vector<uint32_t> dst(nw * nh);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0;
                int opaqueCnt = 0, transparentCnt = 0;
                for (int dy = 0; dy < 2 && (y*2+dy) < h; ++dy) {
                    for (int dx = 0; dx < 2 && (x*2+dx) < w; ++dx) {
                        uint32_t c = src[(y*2+dy)*w + (x*2+dx)];
                        uint8_t alpha = (c >> 24) & 0xFF;
                        // SOURCEPORT: For foliage with binary transparency, only average colors
                        // from opaque pixels. Transparent pixels are skipped entirely.
                        if (alpha > 127) {
                            r += rgl_srgb_to_linear((c        & 0xFF) / 255.0f);
                            g += rgl_srgb_to_linear(((c >> 8) & 0xFF) / 255.0f);
                            b += rgl_srgb_to_linear(((c >>16) & 0xFF) / 255.0f);
                            ++opaqueCnt;
                        } else {
                            ++transparentCnt;
                        }
                    }
                }
                // Majority voting: if more than half are opaque, output opaque with averaged color.
                // Otherwise output fully transparent to avoid dark semi-transparent artifacts.
                auto pack = [](float v) -> uint32_t {
                    int i = (int)(v * 255.5f);
                    return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i);
                };
                if (opaqueCnt > transparentCnt) {
                    // Majority opaque: average the opaque colors, output as fully opaque
                    float inv = 1.0f / opaqueCnt;
                    uint32_t rb = pack(rgl_linear_to_srgb(r*inv));
                    uint32_t gb = pack(rgl_linear_to_srgb(g*inv));
                    uint32_t bb = pack(rgl_linear_to_srgb(b*inv));
                    dst[y*nw + x] = rb | (gb << 8) | (bb << 16) | (255 << 24);  // Fully opaque
                } else {
                    dst[y*nw + x] = 0;  // Majority transparent or tie: output fully transparent
                }
            }
        }
        glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA8, nw, nh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, dst.data());
        src = std::move(dst);
        w = nw; h = nh;
    }
}

GLuint RendererGL::UploadTexture16(void* data, int w, int h) {
    // Convert 16-bit RGB565/555 to 32-bit RGBA
    std::vector<uint32_t> rgba(w * h);
    uint16_t* src = (uint16_t*)data;
    for (int i = 0; i < w * h; i++) {
        rgba[i] = m_isRGB565 ? RGB565toRGBA(src[i]) : RGB555toRGBA(src[i]);
    }

    // SOURCEPORT: all textures now get a full sRGB-correct mip chain.
    // Previously opaque (terrain) textures had MAX_LEVEL=0 to avoid the gamma-
    // incorrect glGenerateMipmap "circle of light" artifact. With rgl_GenerateLinearMipmaps
    // that artifact is gone, and without mipmaps terrain shimmers at 144 Hz (temporal
    // aliasing: each frame samples a different texel from mip 0). Foliage forced
    // sampleLOD=0 for the same reason; the shader now uses the computed depth-based
    // LOD so the mip chain is actually exercised and absorbs the per-frame jitter.
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    rgl_GenerateLinearMipmaps(rgba.data(), w, h);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {
        int maxLvl = 0;
        for (int s = std::max(w, h); s > 1; s >>= 1, maxLvl++);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLvl);
    }
    GLenum minFilter = m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
    if (m_maxAnisotropy > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, (GLfloat)m_maxAnisotropy);

    return tex;
}

#include "../TextureOverrides.h"

// FNV-1a sample hash: probes start, middle, and end of the 16-bit pixel data plus
// dimensions. Constant cost (~100 cycles) regardless of texture size.
static uint64_t hashTexContent(const void* data, int w, int h) {
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t* p = (const uint8_t*)data;
    size_t n = (size_t)w * h * 2;
    auto fnv = [&](size_t off, size_t len) {
        if (off + len > n) len = (off < n) ? n - off : 0;
        for (size_t i = 0; i < len; i++) { hash ^= p[off + i]; hash *= 1099511628211ULL; }
    };
    fnv(0,       32);
    fnv(n/2 - 16, 32);
    fnv(n - 32,  32);
    hash ^= (uint64_t)(unsigned)w * 2654435761ULL;
    hash ^= (uint64_t)(unsigned)h * 40503ULL;
    return hash;
}

void RendererGL::SetTexture(void* lpData, int w, int h) {
    uint64_t key = hashTexContent(lpData, w, h);

    auto it = m_texCache.find(key);
    if (it != m_texCache.end()) {
        m_currentTexture = it->second.texId;
        it->second.lastUsed = m_frameCounter;
    } else {
        // Evict old entries if cache is too large
        if (m_texCache.size() > 128) {
            int oldest = m_frameCounter;
            uint64_t oldestKey = 0;
            for (auto& p : m_texCache) {
                if (p.second.lastUsed < oldest) {
                    oldest = p.second.lastUsed;
                    oldestKey = p.first;
                }
            }
            if (oldestKey) {
                glDeleteTextures(1, &m_texCache[oldestKey].texId);
                m_texCache.erase(oldestKey);
            }
        }

        // SOURCEPORT: prefer BCn DDS override, then 8-bit RGBA override,
        // finally fall back to the retail 16-bit decode.
        GLuint tex = 0;
        if (const TextureOverrides::CompressedTex* ct = TextureOverrides::GetCompressed(lpData)) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            int lw = ct->w, lh = ct->h;
            for (int i = 0; i < ct->mipCount; ++i) {
                glCompressedTexImage2D(GL_TEXTURE_2D, i, (GLenum)ct->glFormat,
                                       lw, lh, 0,
                                       (GLsizei)ct->mipSizes[i],
                                       ct->data + ct->mipOffsets[i]);
                lw = (lw > 1) ? lw >> 1 : 1;
                lh = (lh > 1) ? lh >> 1 : 1;
            }
            // Single-mip DDS: generate the rest in linear space
            if (ct->mipCount <= 1) {
                std::vector<uint32_t> tmp(ct->w * ct->h);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
                rgl_GenerateLinearMipmaps(tmp.data(), ct->w, ct->h);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  ct->mipCount - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
            // SOURCEPORT: anisotropic filtering — controlled by OptAnisoLevel (1=2x, 2=4x, 3=8x, 4=max hardware)
            {
                extern int OptAnisoLevel;
                if (m_maxAnisotropy > 1) {
                    // Map OptAnisoLevel (1-3) to anisotropy values (2x, 4x, 8x); level 4 = hardware max
                    float desiredAniso;
                    if (OptAnisoLevel >= 4) {
                        desiredAniso = (float)m_maxAnisotropy;  // "Max" setting uses full hardware capability
                    } else {
                        desiredAniso = 1 << OptAnisoLevel;  // 2, 4, 8 for inputs 1, 2, 3
                    }
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, desiredAniso);
                }
            }
        } else {
            int ow = 0, oh = 0;
            const uint32_t* over = TextureOverrides::Get(lpData, &ow, &oh);
            if (over) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
                rgl_GenerateLinearMipmaps(over, ow, oh);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_linearFilter ? GL_LINEAR : GL_NEAREST);
            } else {
                tex = UploadTexture16(lpData, w, h);
            }
        }
        m_texCache[key] = { tex, m_frameCounter };
        m_currentTexture = tex;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_currentTexture);
}

// --- Vertex buffer operations ---

RenderVertex* RendererGL::LockVertexBuffer() {
    return m_mainBuffer;
}

void RendererGL::UnlockAndDrawTriangles(int triCount1, int triCount2) {
    int totalVerts = (triCount1 + triCount2) * 3;
    if (totalVerts <= 0) return;

    // SOURCEPORT: skip transparent geometry (water, env-map overlays) during shadow pass.
    // SetRenderStates(false,...) sets m_shadowGeomSuppressed; those surfaces must not
    // write to the shadow map or they cast a spurious shadow on the terrain below.
    if (m_shadowPassActive && m_shadowGeomSuppressed) return;

    // SOURCEPORT: re-assert depth program before every draw during the shadow pass.
    // BindCustomMaterial(nullptr) and UpdateProjection() both call glUseProgram(m_shaderProgram),
    // which would stomp the depth program set in BeginWorldShadowPass().
    if (m_shadowPassActive) glUseProgram(m_depthShaderProgram);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, totalVerts * sizeof(RenderVertex), m_mainBuffer);

    // Draw normal triangles (no alpha test)
    if (triCount1 > 0) {
        GLint alphaLoc = m_shadowPassActive ? m_depthLocAlphaTest : m_locAlphaTest;
        if (alphaLoc >= 0) glUniform1i(alphaLoc, 0);
        glDrawArrays(GL_TRIANGLES, 0, triCount1 * 3);
    }

    // Draw color-keyed triangles (with alpha test)
    if (triCount2 > 0) {
        GLint alphaLoc = m_shadowPassActive ? m_depthLocAlphaTest : m_locAlphaTest;
        if (alphaLoc >= 0) glUniform1i(alphaLoc, 1);
        glDrawArrays(GL_TRIANGLES, triCount1 * 3, triCount2 * 3);
    }

    glBindVertexArray(0);
}

RenderVertex* RendererGL::LockGeometryBuffer() {
    return m_geomBuffer;
}

void RendererGL::UnlockAndDrawGeometry(int vertexCount, bool /*colorKey*/) {
    if (vertexCount <= 0) return;

    // SOURCEPORT: skip transparent geometry during shadow pass (same guard as UnlockAndDrawTriangles).
    if (m_shadowPassActive && m_shadowGeomSuppressed) return;

    // SOURCEPORT: re-assert depth program before every draw during the shadow pass.
    // BindCustomMaterial(nullptr) and UpdateProjection() both call glUseProgram(m_shaderProgram),
    // which would stomp the depth program set in BeginWorldShadowPass().
    if (m_shadowPassActive) {
        glUseProgram(m_depthShaderProgram);
        // Mirror the alpha-test state (set by SetAlphaTest) into the depth shader.
        if (m_depthLocAlphaTest >= 0)
            glUniform1i(m_depthLocAlphaTest, m_alphaTestEnabled ? 1 : 0);
    }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(RenderVertex), m_geomBuffer);

    glDrawArrays(GL_TRIANGLES, 0, vertexCount);

    glBindVertexArray(0);
}

// --- Render state ---

void RendererGL::SetRenderStates(bool zWrite, int dstBlend) {
    // SOURCEPORT: during the shadow depth pass, track whether the game has entered
    // a transparent-geometry phase (zWrite=false = water, env-map overlays, etc.).
    // When suppressed, UnlockAndDraw* skip those draw calls so transparent surfaces
    // don't write to the shadow map and cast spurious shadows on the terrain below.
    // Depth mask stays on so opaque geometry drawn before/after keeps working.
    if (m_shadowPassActive) {
        m_shadowGeomSuppressed = !zWrite;
        return;
    }
    m_zWriteEnabled = zWrite;
    m_dstBlend = dstBlend;
    glDepthMask(zWrite ? GL_TRUE : GL_FALSE);

    GLenum glDst;
    switch (dstBlend) {
        case BLEND_ZERO:     glDst = GL_ZERO; break;
        case BLEND_ONE:      glDst = GL_ONE; break;
        case BLEND_SRCALPHA: glDst = GL_SRC_ALPHA; break;
        default:             glDst = GL_ONE_MINUS_SRC_ALPHA; break; // BLEND_INVSRCALPHA and unknown
    }
    glBlendFunc(GL_SRC_ALPHA, glDst);
}

void RendererGL::SetFogEnabled(bool enabled) {
    m_fogEnabled = enabled;
    // SOURCEPORT: depth shader has no fog uniforms — skip during shadow pass.
    // Using m_locFogEnabled (main-shader location) with depth shader active corrupts depth uniforms.
    if (m_shadowPassActive) return;
    glUniform1i(m_locFogEnabled, enabled ? 1 : 0);
}

void RendererGL::SetFogColor(uint32_t color) {
    m_fogColor[0] = ((color >> 16) & 0xFF) / 255.0f;
    m_fogColor[1] = ((color >> 8)  & 0xFF) / 255.0f;
    m_fogColor[2] = ((color)       & 0xFF) / 255.0f;
    m_fogColor[3] = 1.0f;
    // SOURCEPORT: depth shader has no fog — only push to GL when main shader owns the state.
    if (!m_shadowPassActive) {
        glUniform4fv(m_locFogColor, 1, m_fogColor);
    }
    glClearColor(m_fogColor[0], m_fogColor[1], m_fogColor[2], 1.0f);
}

// SOURCEPORT: upload screen-space fog parameters once per frame (after CAMERAINFOG update).
// Replaces per-vertex fog that pulsed with headbob.
void RendererGL::SetFogParams(float fogYBeginGU, float fogTransp, float fogLimit,
                               float cameraY, int cameraInFog,
                               float videoCY, float winH, float cameraH) {
    glUniform1f(m_locFogYBeginGU, fogYBeginGU);
    glUniform1f(m_locFogTransp,   fogTransp);
    glUniform1f(m_locFogLimit,    fogLimit);
    glUniform1f(m_locFogCameraY,  cameraY);
    glUniform1i(m_locCameraInFog, cameraInFog);
    glUniform1f(m_locFogVideoCY,  videoCY);
    glUniform1f(m_locFogWinH,     winH);
    glUniform1f(m_locFogCameraH,  cameraH);
}

void RendererGL::SetLinearFilter(bool enabled) {
    m_linearFilter = enabled;
    // Will apply to next texture bind
}

void RendererGL::SetAlphaTest(bool enabled) {
    m_alphaTestEnabled = enabled;
    // SOURCEPORT: during the shadow pass the depth shader is active; use its location.
    // d3dEndBufferG calls SetAlphaTest(false) after every geometry batch, including those
    // inside the shadow DrawScene.  Using m_locAlphaTest (main-shader location) with the
    // depth program bound would corrupt a depth-shader uniform at the same integer slot.
    if (m_shadowPassActive) {
        if (m_depthLocAlphaTest >= 0)
            glUniform1i(m_depthLocAlphaTest, enabled ? 1 : 0);
    } else {
        glUniform1i(m_locAlphaTest, enabled ? 1 : 0);
    }
}

GLuint RendererGL::GetWhiteTexture() {
    return m_whiteTexture;
}

void RendererGL::SetHUDMode(bool enabled) {
    // SOURCEPORT: 3D HUD models (weapon, compass, wind vane) have rhw < 1.0 and would
    // otherwise pass the shadow-sampling gate in basic.frag, receiving world shadows
    // that make no visual sense on HUD geometry.  Temporarily clear uWorldShadow while
    // these elements render; restore it when done so the next world-geometry batch is
    // still shaded correctly.
    // Always target m_shaderProgram explicitly — a custom material or post-process pass
    // may have left a different program bound, and glUniform* goes to whatever is current.
    if (m_locWorldShadow >= 0) {
        glUseProgram(m_shaderProgram);
        glUniform1i(m_locWorldShadow, enabled ? 0 : (m_shadowsRendered ? 1 : 0));
    }
}

void RendererGL::SetZBufferEnabled(bool enabled) {
    // SOURCEPORT: ignore depth-test disable during shadow pass — the depth-only
    // FBO requires depth test + write active throughout.  Skip the cached-state
    // update too so the pre-pass value is intact when the main render begins.
    if (m_shadowPassActive) return;
    m_zBufferEnabled = enabled;
    if (enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(m_zWriteEnabled ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
}

void RendererGL::SetDepthMask(bool write) {
    // SOURCEPORT: set depth write without touching depth test enable state.
    // Used by additive overlay passes (EnvMap/PhongMap) so they read depth
    // (occluded by closer geometry like the player's arm) but don't write it.
    if (m_shadowPassActive) return;  // SOURCEPORT: always write depth during shadow pass
    m_zWriteEnabled = write;
    glDepthMask(write ? GL_TRUE : GL_FALSE);
}

void RendererGL::SetStencilMode(int mode) {
    // SOURCEPORT: stencil isolation for weapon overlays (PhongMap/EnvMap).
    // Mode 1 (write): mark every rasterized weapon pixel with stencil=1.
    // Mode 2 (test): only render fragments where stencil==1 (weapon pixels only).
    // Mode 0 (off): disable stencil entirely.
    // Without this, the additive overlay depth values (~0.64, very close) always
    // pass GL_GEQUAL against terrain depth (~0.125, far), painting the specular
    // texture over the ground and creating the "headlamp" artifact while walking.
    switch (mode) {
        case 1:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            break;
        case 2:
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0x00); // read-only stencil
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            break;
        default:
            glStencilMask(0xFF);
            glDisable(GL_STENCIL_TEST);
            break;
    }
}

void RendererGL::SetBrightness(float b) {
    // SOURCEPORT: runtime brightness applied in shader. b=1.0 = neutral, 0.0=black, 2.0=double.
    // Replaces the old BrightenTexture(OptBrightness) bake so the slider is live.
    m_brightness = b;
    glUniform1f(m_locBrightness, b);
}

void RendererGL::SetDebugMode(int mode) {
    if (m_locDebugMode >= 0) glUniform1i(m_locDebugMode, mode);
}

void RendererGL::BindMaterial(const void* materialPtr) {
    // SOURCEPORT: depth shader has only ~9 uniforms (locations 0–8).  Main-shader
    // PBR locations (uPBR, uMetallicFactor, etc.) fall in the same integer range.
    // Writing them while the depth shader is the active program overwrites whatever
    // depth-shader uniform occupies that slot — typically a component of uCamToWorld
    // or uCameraPos — corrupting world-position reconstruction in the shadow pass.
    // Skip entirely; the depth shader doesn't use PBR anyway.
    if (m_shadowPassActive) return;
    const Materials::Material* m = static_cast<const Materials::Material*>(materialPtr);
    // Only enable PBR when a normal map exists — without it the tangent-frame
    // path contributes no perturbation and specular would be flat lit.
    if (!m || m->normalTex == 0) {
        if (m_pbrActive) {
            glUniform1i(m_locPBR, 0);
            glUniform1f(m_locParallaxScale, 0.0f);
            m_pbrActive = false;
        }
        return;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)m->normalTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m->mrTex ? (GLuint)m->mrTex : GetWhiteTexture());
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m->aoTex ? (GLuint)m->aoTex : GetWhiteTexture());
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(m_locMetallicFactor,  m->metallicFactor);
    glUniform1f(m_locRoughnessFactor, m->roughnessFactor);
    // SOURCEPORT: parallax scale from material (0 if normal map has flat alpha).
    glUniform1f(m_locParallaxScale, m->parallaxScale);
    glUniform1i(m_locPBR, 1);
    m_pbrActive = true;
}

void RendererGL::BindCustomMaterial(const void* materialPtr) {
    // SOURCEPORT: custom materials call CustomMaterials::Apply which calls glUseProgram
    // (switching away from m_depthShaderProgram).  During the shadow pass only the depth
    // shader must be active; UnlockAndDraw* re-asserts it before every draw, but any
    // uniform writes that happen between draws would land in the wrong program.  Suppress
    // the entire custom-material activation for the depth pass — the depth shader only
    // needs uLightSpace + camera uniforms already uploaded by BeginWorldShadowPass.
    if (m_shadowPassActive) return;
    const CustomMaterials::Material* cm =
        static_cast<const CustomMaterials::Material*>(materialPtr);
    if (!cm) {
        // Restore the engine's default program. GL uniforms are per-program
        // so fog/alpha/brightness/PBR state in the default program is still
        // intact — nothing else to re-push.
        if (m_customProgramActive) {
            glUseProgram(m_shaderProgram);
            m_customProgramActive = false;
        }
        return;
    }
    CustomMaterials::Apply(cm, m_projMatrix);
    m_customProgramActive = true;
}

// --- 2D operations ---

void RendererGL::DrawBitmap(int x, int y, int w, int h, int srcW, void* lpData, bool colorKey, int srcH, const void* overrideKey) {
    if (!lpData || w <= 0 || h <= 0) return;
    if (m_shadowPassActive) return;  // SOURCEPORT: guard — main-shader locs must not hit depth shader
    glUseProgram(m_shaderProgram);   // SOURCEPORT: ensure main shader owns any uniforms we set below

    // Actual source dimensions: srcW × uploadH.
    // If srcH == 0 the caller hasn't specified a source height; fall back to h
    // (legacy callers where src and dest are the same size).
    int uploadH = (srcH > 0) ? srcH : h;
    int uploadW = srcW;

    // SOURCEPORT: menu/HUD override path. If TextureOverrides has a 32-bit PNG/
    // TGA/BMP/JPEG registered for this picture, upload it at its native
    // resolution instead of decoding the retail 16-bit TGA. UVs are 0..1 so any
    // size maps correctly onto the destination quad; high-res menu art drops in
    // with no other code changes. `overrideKey`, when supplied, wins over
    // `lpData` — menu pictures pass `&pic` because pic.lpImage is heap-recycled
    // across ReleaseResources (cross-asset bleed bug otherwise).
    int overW = 0, overH = 0;
    void* lookupKey = overrideKey ? const_cast<void*>(overrideKey) : lpData;
    const uint32_t* over = TextureOverrides::Get(lookupKey, &overW, &overH);

    // Convert 16-bit source to RGBA only when we don't have an override.
    std::vector<uint32_t> rgba;
    if (!over) {
        rgba.resize(uploadW * uploadH);
        uint16_t* src = (uint16_t*)lpData;
        for (int row = 0; row < uploadH; row++) {
            for (int col = 0; col < uploadW; col++) {
                uint16_t pixel = src[row * srcW + col];
                rgba[row * uploadW + col] = m_isRGB565 ? RGB565toRGBA(pixel) : RGB555toRGBA(pixel);
            }
        }
    }

    // Upload source texture at its natural size (override dims may differ).
    // SOURCEPORT: save/restore the unit-0 binding so DrawBitmap does not leave
    // m_bitmapTexture on unit 0.  If it persists into the next frame's shadow pass,
    // FlushWorldSpaceShadow uses it for foliage alpha-test, corrupting tree shadows.
    GLint prevTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    if (over) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, overW, overH, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, uploadW, uploadH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Draw a textured quad at screen position
    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + w, y1 = y0 + h;

    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py, float u, float tv) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = 0xFFFFFFFF; v.specular = 0xFF000000;
        v.tu = u; v.tv = tv;
    };
    fillVert(quad[0], x0, y0, 0.0f, 0.0f);
    fillVert(quad[1], x1, y0, 1.0f, 0.0f);
    fillVert(quad[2], x1, y1, 1.0f, 1.0f);
    fillVert(quad[3], x0, y0, 0.0f, 0.0f);
    fillVert(quad[4], x1, y1, 1.0f, 1.0f);
    fillVert(quad[5], x0, y1, 0.0f, 1.0f);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(m_locAlphaTest, colorKey ? 1 : 0);
    glUniform1i(m_locFogEnabled, 0);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex0);  // SOURCEPORT: restore unit-0 binding
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

// SOURCEPORT: Render text using GDI into a temp DIB, then upload as GL texture.
// Uses fnt_Small for all menu and HUD text. Performance is acceptable for
// non-realtime menu use; in-game calls are infrequent.
// SOURCEPORT: shared scaled font — built once per WinH change, used by DrawText and MeasureText.
// fnt_Small is 14px/5px wide designed for 600px height; scale both dimensions with WinH.
static HFONT  s_scaledFont  = nullptr;
static int    s_scaledWinH  = 0;

static HFONT GetScaledFont() {
    extern HFONT fnt_Small;
    if (s_scaledWinH != WinH) {
        if (s_scaledFont) { DeleteObject(s_scaledFont); s_scaledFont = nullptr; }
        int fh = std::max(14, 14 * WinH / 600);
        int fw = std::max(5,   5 * WinH / 600);
        s_scaledFont = CreateFontA(fh, fw, 0, 0, 100, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        s_scaledWinH = WinH;
    }
    return s_scaledFont ? s_scaledFont : fnt_Small;
}

// SOURCEPORT: big heading font — ~36px at 600p, Bold. Used for menu screen headings.
static HFONT  s_bigFont    = nullptr;
static int    s_bigWinH    = 0;

static HFONT GetBigFont() {
    if (s_bigWinH != WinH) {
        if (s_bigFont) { DeleteObject(s_bigFont); s_bigFont = nullptr; }
        int fh = std::max(36, 36 * WinH / 600);
        int fw = std::max(14, 14 * WinH / 600);
        s_bigFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, nullptr);
        s_bigWinH = WinH;
    }
    return s_bigFont ? s_bigFont : GetScaledFont();
}

// SOURCEPORT: menu font — fnt_Midd style: weight=550 (semibold), 16px/7px at 600p, scaled by WinH/600.
static HFONT  s_menuFont   = nullptr;
static int    s_menuWinH   = 0;

static HFONT GetMenuFont() {
    if (s_menuWinH != WinH) {
        if (s_menuFont) { DeleteObject(s_menuFont); s_menuFont = nullptr; }
        int fh = std::max(16, 16 * WinH / 600);
        int fw = std::max(7,   7 * WinH / 600);
        // SOURCEPORT: nullptr face name → system default sans-serif (matches original fnt_Midd style).
        // Weight=700 (Bold) to match the medium-bold appearance of the original menu text.
        s_menuFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, nullptr);
        s_menuWinH = WinH;
    }
    return s_menuFont ? s_menuFont : GetScaledFont();
}

// SOURCEPORT: shared text-rendering core — renders text with a given GDI font as a GL quad.
static void DrawTextWithFont(int x, int y, const char* text, uint32_t color, HFONT useFont,
                             GLuint m_bitmapTexture, GLuint m_vao, GLuint m_vbo,
                             bool m_zBufferEnabled, GLint m_locAlphaTest, GLint m_locFogEnabled,
                             bool m_fogEnabled) {
    // Create a temporary memory DC to measure and render the text
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return;
    HFONT hOldFont = (HFONT)SelectObject(hdc, useFont);

    // Measure text extent
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    int tw = sz.cx > 0 ? sz.cx : 1;
    int th = sz.cy > 0 ? sz.cy : 1;

    // Create 24-bit DIB (top-down)
    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = tw;
    bih.biHeight      = -th;
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    BITMAPINFO bi = {};
    bi.bmiHeader = bih;

    void* dibBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hbmp) { SelectObject(hdc, hOldFont); DeleteDC(hdc); return; }
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hbmp);

    int stride = (tw * 3 + 3) & ~3;
    memset(dibBits, 0, stride * th);

    // SOURCEPORT: render white text on black so R channel = glyph coverage (0–255).
    // Use that as alpha and store the requested color constant — gives proper
    // anti-aliased edges and makes the black shadow pass actually visible.
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, text, (int)strlen(text));

    uint8_t cr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t cg = (uint8_t)((color >>  8) & 0xFF);
    uint8_t cb = (uint8_t)((color      ) & 0xFF);

    std::vector<uint32_t> rgba(tw * th);
    uint8_t* src = (uint8_t*)dibBits;
    for (int py = 0; py < th; py++) {
        for (int px2 = 0; px2 < tw; px2++) {
            uint8_t lum = src[py * stride + px2 * 3 + 2]; // R == G == B for white text
            // alpha = glyph coverage; RGB = constant requested color → correct blend
            rgba[py * tw + px2] = ((uint32_t)lum << 24) | ((uint32_t)cr << 16) |
                                   ((uint32_t)cg  <<  8) | cb;
        }
    }

    SelectObject(hdc, hOldBmp);
    SelectObject(hdc, hOldFont);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    // SOURCEPORT: save/restore the unit-0 binding — same reason as DrawBitmap.
    GLint prevTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + tw,  y1 = y0 + th;

    RenderVertex quad[6];
    auto fill2 = [&](RenderVertex& v, float px2, float py, float u, float tv) {
        v.sx = px2; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = 0xFFFFFFFF; v.specular = 0xFF000000;
        v.tu = u; v.tv = tv;
    };
    fill2(quad[0], x0, y0, 0.0f, 0.0f);
    fill2(quad[1], x1, y0, 1.0f, 0.0f);
    fill2(quad[2], x1, y1, 1.0f, 1.0f);
    fill2(quad[3], x0, y0, 0.0f, 0.0f);
    fill2(quad[4], x1, y1, 1.0f, 1.0f);
    fill2(quad[5], x0, y1, 0.0f, 1.0f);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex0);  // SOURCEPORT: restore unit-0 binding
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

int RendererGL::MeasureText(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetScaledFont();
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

int RendererGL::MeasureTextMed(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetMenuFont();
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

void RendererGL::DrawText(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    // SOURCEPORT: 2D drawing must not run during shadow pass — main-shader uniform
    // locations applied to the active depth shader would corrupt depth uniforms.
    if (m_shadowPassActive) return;
    glUseProgram(m_shaderProgram);  // SOURCEPORT: ensure main shader owns the uniforms
    DrawTextWithFont(x, y, text, color, GetScaledFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextMed(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    if (m_shadowPassActive) return;
    glUseProgram(m_shaderProgram);
    DrawTextWithFont(x, y, text, color, GetMenuFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextBig(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    if (m_shadowPassActive) return;
    glUseProgram(m_shaderProgram);
    DrawTextWithFont(x, y, text, color, GetBigFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

int RendererGL::MeasureTextBig(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetBigFont();
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return sz.cx;
}

// SOURCEPORT: Draw a filled rectangle at screen position (x,y) with size (w,h).
void RendererGL::FillRect(int x, int y, int w, int h, uint32_t argbColor) {
    if (m_shadowPassActive) return;  // SOURCEPORT: guard — main-shader locs must not hit depth shader
    glUseProgram(m_shaderProgram);   // SOURCEPORT: ensure main shader owns any uniforms we set below
    float a = ((argbColor >> 24) & 0xFF) / 255.0f;
    float r = ((argbColor >> 16) & 0xFF) / 255.0f;
    float g = ((argbColor >>  8) & 0xFF) / 255.0f;
    float b = ((argbColor      ) & 0xFF) / 255.0f;

    // Store as ARGB in uint32; GL_BGRA attrib reads memory bytes [B,G,R,A] (LE) = correct
    uint32_t vc = ((uint32_t)(a*255) << 24) | ((uint32_t)(r*255) << 16) |
                  ((uint32_t)(g*255) <<  8) |  (uint32_t)(b*255);

    float x0 = (float)x, y0 = (float)y;
    float x1 = x0 + w,   y1 = y0 + h;

    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = vc; v.specular = 0xFF000000;
        v.tu = 0.0f; v.tv = 0.0f;
    };
    fillVert(quad[0], x0, y0); fillVert(quad[1], x1, y0); fillVert(quad[2], x1, y1);
    fillVert(quad[3], x0, y0); fillVert(quad[4], x1, y1); fillVert(quad[5], x0, y1);

    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    // SOURCEPORT: use alpha test = 1 so vColor.a drives blend transparency (white texture
    // has texel.a=1.0, so the discard never fires — vertex alpha is used directly).
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

void RendererGL::DrawFullscreenRect(uint32_t argbColor) {
    if (m_shadowPassActive) return;  // SOURCEPORT: guard — main-shader locs must not hit depth shader
    glUseProgram(m_shaderProgram);   // SOURCEPORT: ensure main shader owns any uniforms we set below
    RenderVertex quad[6];
    auto fillVert = [&](RenderVertex& v, float px, float py) {
        v.sx = px; v.sy = py; v.sz = 0.0f; v.rhw = 1.0f;
        v.color = argbColor; v.specular = 0xFF000000;
        v.tu = 0.0f; v.tv = 0.0f;
    };
    fillVert(quad[0], 0, 0);
    fillVert(quad[1], (float)WinW, 0);
    fillVert(quad[2], (float)WinW, (float)WinH);
    fillVert(quad[3], 0, 0);
    fillVert(quad[4], (float)WinW, (float)WinH);
    fillVert(quad[5], 0, (float)WinH);

    // Draw without depth test, with blending, no texture
    bool prevDepth = m_zBufferEnabled;
    glDisable(GL_DEPTH_TEST);
    // SOURCEPORT: uAlphaTest=1 so vColor.a drives blend transparency; white texture
    // (texel.a=1.0) never triggers the discard, so vertex alpha passes through directly.
    glUniform1i(m_locAlphaTest, 1);
    glUniform1i(m_locFogEnabled, 0);

    // Bind a 1x1 white texture so the shader just uses vertex color
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

// --- Z-buffer control ---

void RendererGL::ClearZBuffer() {
    glClear(GL_DEPTH_BUFFER_BIT);
}

float RendererGL::GetDepthAt(int x, int y) {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return 0.f;
    float depth = 0.f;
    glReadPixels(x, m_height - 1 - y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    return depth;
}

// --- Screenshot ---

void RendererGL::CopyBackBuffer(void* dest, int x, int y, int w, int h) {
    if (!dest || w <= 0 || h <= 0) return;
    // Read as 16-bit RGB565 into the destination buffer
    std::vector<uint32_t> rgba(w * h);
    glReadPixels(x, m_height - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // Convert RGBA to RGB565 and flip vertically
    uint16_t* dst = (uint16_t*)dest;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t c = rgba[(h - 1 - row) * w + col];
            uint32_t r = (c >> 0)  & 0xFF;
            uint32_t g = (c >> 8)  & 0xFF;
            uint32_t b = (c >> 16) & 0xFF;
            dst[row * w + col] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

// --- Display info ---

bool RendererGL::IsRGB565() const {
    return m_isRGB565;
}

int RendererGL::GetTextureMemory() const {
    // Report 64MB — modern GPUs have plenty
    return 64 * 1024 * 1024;
}

// --- Supersampling FBO management (flatscreen only) ---

void RendererGL::CreateSSAFramebuffer(int width, int height) {
    DestroySSAFramebuffer();  // Clean up old FBO if it exists

    m_ssaWidth  = width;
    m_ssaHeight = height;

    // Create color texture
    glGenTextures(1, &m_ssaTexture);
    glBindTexture(GL_TEXTURE_2D, m_ssaTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create depth renderbuffer
    glGenRenderbuffers(1, &m_ssaDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_ssaDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    // Create FBO and attach
    glGenFramebuffers(1, &m_ssaFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssaTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_ssaDepth);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "RendererGL: SSA FBO incomplete (0x%X), falling back to direct rendering\n", status);
        DestroySSAFramebuffer();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RendererGL::DestroySSAFramebuffer() {
    if (m_ssaFBO) { glDeleteFramebuffers(1, &m_ssaFBO); m_ssaFBO = 0; }
    if (m_ssaTexture) { glDeleteTextures(1, &m_ssaTexture); m_ssaTexture = 0; }
    if (m_ssaDepth) { glDeleteRenderbuffers(1, &m_ssaDepth); m_ssaDepth = 0; }
    m_ssaWidth = 0;
    m_ssaHeight = 0;
}

void RendererGL::BindSSAFramebuffer() {
    if (!m_ssaFBO) {
        // Fallback: bind default framebuffer if SSA FBO unavailable
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaFBO);
    glViewport(0, 0, m_ssaWidth, m_ssaHeight);
}

void RendererGL::UnbindAndDownscaleSSA() {
    if (!m_ssaFBO) return;

    // Bind backbuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);

    // Blit FBO to backbuffer with downscaling
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ssaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_ssaWidth, m_ssaHeight, 0, 0, m_width, m_height,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// SOURCEPORT: Shadow mapping support (Phase 2.1)

void RendererGL::BeginShadowCascade(int cascade) {
    auto pipeline = (PostProcessingPipeline*)m_postProcessingPipeline;
    if (!pipeline || cascade < 0 || cascade >= 3) return;

    FramebufferObject* shadowFBO = pipeline->GetShadowMap(cascade);
    if (!shadowFBO) return;

    shadowFBO->Bind();
    glClearDepth(0.0);  // Set clear depth to 0.0 (far in reversed depth convention)
    shadowFBO->Clear(1.0f, 1.0f, 1.0f, 1.0f);  // Clear to white/far (depth=0 in reversed convention)

    // Update projection matrix to light space
    const float* lightView = pipeline->GetLightViewMatrix(cascade);
    const float* lightProj = pipeline->GetLightProjMatrix(cascade);
    if (lightView && lightProj) {
        // Compose view-projection matrix
        float viewProj[16];
        // SOURCEPORT: C = proj*view, column-major: C_cm[i*4+j] = Σk proj_cm[k*4+j]*view_cm[i*4+k]
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                viewProj[i*4+j] = 0;
                for (int k = 0; k < 4; k++) {
                    viewProj[i*4+j] += lightProj[k*4+j] * lightView[i*4+k];
                }
            }
        }
        glUseProgram(m_depthShaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(m_depthShaderProgram, "uProjection"), 1, GL_FALSE, viewProj);
    }

    SetDepthOnlyMode(true);
}

void RendererGL::EndShadowPass() {
    SetDepthOnlyMode(false);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glUseProgram(m_shaderProgram);
    glClearDepth(0.0);  // Restore reversed clear (far = 0.0)
    glDepthFunc(GL_GEQUAL);  // Restore reversed depth convention
}

void RendererGL::SetDepthOnlyMode(bool enabled) {
    if (enabled) {
        glUseProgram(m_depthShaderProgram);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  // Don't write color
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        // Use reversed depth convention (GL_GEQUAL) to match game's depth format
        // Shadow map will store 1=near, 0=far (matching scene's reversed convention)
        glDepthFunc(GL_GEQUAL);
        glClearDepth(0.0);  // Reversed: far = 0.0
    } else {
        glUseProgram(m_shaderProgram);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_GEQUAL);  // Restore reversed depth convention
        glClearDepth(0.0);  // Restore reversed clear (far = 0.0)
    }
}

void RendererGL::GetLightTransform(int cascade, float* outViewMatrix, float* outProjMatrix) {
    if (!outViewMatrix || !outProjMatrix) return;

    auto pipeline = (PostProcessingPipeline*)m_postProcessingPipeline;
    if (!pipeline) return;

    const float* viewMat = pipeline->GetLightViewMatrix(cascade);
    const float* projMat = pipeline->GetLightProjMatrix(cascade);

    if (viewMat) std::memcpy(outViewMatrix, viewMat, 16 * sizeof(float));
    else std::memset(outViewMatrix, 0, 16 * sizeof(float));

    if (projMat) std::memcpy(outProjMatrix, projMat, 16 * sizeof(float));
    else std::memset(outProjMatrix, 0, 16 * sizeof(float));
}

// SOURCEPORT: Path B world shadow mapping ────────────────────────────────────

void RendererGL::SetCameraWorldUniforms(float vcx, float vcy, float cw, float ch,
                                         float wx, float wy, float wz,
                                         float ca, float sa, float cb, float sb,
                                         float cg, float sg) {
    m_unifVideoCX = vcx; m_unifVideoCY = vcy;
    m_unifCameraW = cw;  m_unifCameraH = ch;
    m_cameraWorldPos[0] = wx; m_cameraWorldPos[1] = wy; m_cameraWorldPos[2] = wz;

    // Build camera-to-world rotation matrix R^T (column-major mat3).
    // R = Rr * Rp * Ry  transforms world-relative → camera space.
    // R columns (derived algebraically from RotateVector):
    //   col0 = (cg*ca+sg*sb*sa, -sg*ca+cg*sb*sa, -cb*sa)
    //   col1 = (sg*cb,           cg*cb,            sb   )
    //   col2 = (cg*sa-sg*sb*ca, -sg*sa-cg*sb*ca,  cb*ca)
    // R^T: col j of R^T = row j of R, so a[col*3+row]:
    //   col0 of R^T = row0 of R = (R[0,0], R[0,1], R[0,2])
    m_camToWorld[0] = cg*ca + sg*sb*sa;
    m_camToWorld[1] = sg*cb;
    m_camToWorld[2] = cg*sa - sg*sb*ca;
    // col1 of R^T = row1 of R
    m_camToWorld[3] = -sg*ca + cg*sb*sa;
    m_camToWorld[4] = cg*cb;
    m_camToWorld[5] = -sg*sa - cg*sb*ca;
    // col2 of R^T = row2 of R
    m_camToWorld[6] = -cb*sa;
    m_camToWorld[7] = sb;
    m_camToWorld[8] = cb*ca;

    // Push to main shader so vWorldPos is computed each frame.
    glUseProgram(m_shaderProgram);
    glUniform1f(m_locVideoCX, vcx);
    glUniform1f(m_locVideoCY, vcy);
    glUniform1f(m_locCameraW, cw);
    glUniform1f(m_locCameraH, ch);
    glUniform3fv(m_locCameraPos,   1,         m_cameraWorldPos);
    glUniformMatrix3fv(m_locCamToWorld, 1, GL_FALSE, m_camToWorld);
}

void RendererGL::FlushWorldSpaceShadow() {
    if (m_wsShadowCount == 0 || !m_wsDepthProgram || !m_wsVAO) return;
    glUseProgram(m_wsDepthProgram);
    glUniformMatrix4fv(m_wsLocLightSpace, 1, GL_FALSE, m_cascadeLightMatrix[m_currentCascade]);
    glUniform1i(m_wsLocAlphaTest, m_wsAlphaTest ? 1 : 0);
    glBindVertexArray(m_wsVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_wsVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_wsShadowCount * sizeof(WSShadowVert), m_wsShadowBuffer);
    glDrawArrays(GL_TRIANGLES, 0, m_wsShadowCount);
    glBindVertexArray(0);
    m_wsShadowCount = 0;
    // Restore screen-space depth program for subsequent draws
    if (m_shadowPassActive) glUseProgram(m_depthShaderProgram);
}

void RendererGL::SubmitWorldSpaceShadowTriangle(
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    float u0, float v0, float u1, float v1, float u2, float v2,
    bool alphaTest)
{
    // Flush on alpha-test state change or buffer full
    if (m_wsShadowCount > 0 && alphaTest != m_wsAlphaTest)
        FlushWorldSpaceShadow();
    if (m_wsShadowCount + 3 > MAX_WS_SHADOW_VERTS)
        FlushWorldSpaceShadow();
    // SOURCEPORT: FlushWorldSpaceShadow early-returns without draining when the
    // depth program/VAO is unavailable — drop the triangle rather than overflow.
    if (m_wsShadowCount + 3 > MAX_WS_SHADOW_VERTS)
        return;
    m_wsAlphaTest = alphaTest;
    m_wsShadowBuffer[m_wsShadowCount++] = {x0, y0, z0, u0, v0};
    m_wsShadowBuffer[m_wsShadowCount++] = {x1, y1, z1, u1, v1};
    m_wsShadowBuffer[m_wsShadowCount++] = {x2, y2, z2, u2, v2};
}

void RendererGL::BeginWaterPass(bool allow, float timeSec) {
    m_waterPassActive = false;
    if (!allow || !m_waterFXEnabled || m_shadowPassActive || !m_shaderProgram) return;

    // Capture the fully rendered scene (everything under/behind the water draws
    // before RenderWater) from the currently bound framebuffer at viewport size.
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;

    if (!m_waterSceneTex) {
        glGenTextures(1, &m_waterSceneTex);
        glBindTexture(GL_TEXTURE_2D, m_waterSceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!m_waterDepthTex) {
        glGenTextures(1, &m_waterDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_waterDepthTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // glCopyTexImage2D respecifies the texture each call — window resizes are free.
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_waterSceneTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, vp[0], vp[1], vp[2], vp[3], 0);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_waterDepthTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, vp[0], vp[1], vp[2], vp[3], 0);
    glActiveTexture(GL_TEXTURE0);

    glUseProgram(m_shaderProgram);
    glUniform1i(m_locWaterMode, 1);
    glUniform1f(m_locWaterTime, timeSec);
    glUniform1i(m_locWaterScene, 5);
    glUniform1i(m_locWaterDepth, 6);
    glUniform2f(m_locWaterScreenSize, (float)vp[2], (float)vp[3]);
    glUniform1f(m_locWaterWave,      m_waterWaveStrength);
    glUniform1f(m_locWaterClarity,   m_waterClarity);
    glUniform3fv(m_locWaterDeepColor, 1, m_waterDeepColor);
    glUniform1f(m_locWaterFoamWidth, m_waterFoamWidth);
    glUniform1f(m_locWaterReflect,   m_waterReflectivity);
    m_waterPassActive = true;
}

void RendererGL::EndWaterPass() {
    if (!m_waterPassActive) return;
    glUseProgram(m_shaderProgram);
    glUniform1i(m_locWaterMode, 0);
    m_waterPassActive = false;
}

void RendererGL::SetSunDirection(float x, float y, float z) {
    // SOURCEPORT: caller passes the unnormalized sun direction (e.g. -SunShadowK, 1, -SunShadowK).
    // Normalise and store; BeginWorldShadowPass reads m_sunDirWorld each frame.
    float len = sqrtf(x*x + y*y + z*z);
    if (len < 1e-6f) return;
    m_sunDirWorld[0] = x / len;
    m_sunDirWorld[1] = y / len;
    m_sunDirWorld[2] = z / len;
    // SOURCEPORT: upload world-space sun direction for PBR lighting in basic.frag.
    // Called each frame before geometry rendering; m_shaderProgram may or may not be
    // current — bind explicitly to guarantee the upload lands in the right program.
    glUseProgram(m_shaderProgram);
    glUniform3fv(m_locSunDirWorld, 1, m_sunDirWorld);
}

void RendererGL::BeginWorldShadowPass(int cascade) {
    m_currentCascade = cascade;

    // ── Compute cascade ortho half-extent ─────────────────────────────────────
    float range = m_shadowRange * CASCADE_RANGE_FRACS[cascade];
    m_cascadeRanges[cascade] = range;

    // ── Compute light-space matrix (same basis for all cascades) ──────────────
    float lx = m_sunDirWorld[0], ly = m_sunDirWorld[1], lz = m_sunDirWorld[2];
    float len = sqrtf(lx*lx + ly*ly + lz*lz);
    lx /= len; ly /= len; lz /= len;

    const float dist = 10000.0f;
    float cx = m_cameraWorldPos[0], cy = m_cameraWorldPos[1], cz = m_cameraWorldPos[2];
    float lpx = cx + lx*dist, lpy = cy + ly*dist, lpz = cz + lz*dist;

    float fwd[3] = {-lx, -ly, -lz};
    float up[3]  = {0.f, 1.f, 0.f};
    float d = fabsf(fwd[0]*up[0] + fwd[1]*up[1] + fwd[2]*up[2]);
    if (d > 0.95f) { up[0] = 1.f; up[1] = 0.f; up[2] = 0.f; }

    float rt[3] = { fwd[1]*up[2]-fwd[2]*up[1],
                    fwd[2]*up[0]-fwd[0]*up[2],
                    fwd[0]*up[1]-fwd[1]*up[0] };
    float rlen = sqrtf(rt[0]*rt[0]+rt[1]*rt[1]+rt[2]*rt[2]);
    rt[0]/=rlen; rt[1]/=rlen; rt[2]/=rlen;
    up[0] = rt[1]*fwd[2]-rt[2]*fwd[1];
    up[1] = rt[2]*fwd[0]-rt[0]*fwd[2];
    up[2] = rt[0]*fwd[1]-rt[1]*fwd[0];

    // SOURCEPORT: snap to texel grid for this cascade (prevents shadow crawl).
    float texelSize = 2.0f * range / (float)CASCADE_SHADOW_SIZE;
    float projCamX  = rt[0]*cx + rt[1]*cy + rt[2]*cz;
    float projCamY  = up[0]*cx + up[1]*cy + up[2]*cz;
    projCamX = floorf(projCamX / texelSize) * texelSize;
    projCamY = floorf(projCamY / texelSize) * texelSize;
    float tx = -projCamX;
    float ty = -projCamY;
    float tz = -(fwd[0]*lpx + fwd[1]*lpy + fwd[2]*lpz);

    float view[16] = {
        rt[0], up[0], fwd[0], 0.f,
        rt[1], up[1], fwd[1], 0.f,
        rt[2], up[2], fwd[2], 0.f,
        tx,    ty,    tz,     1.f
    };

    const float near_z = 1.0f, far_z = dist + range * 0.75f;
    float proj[16] = {
        1.f/range, 0.f,       0.f,                       0.f,
        0.f,       1.f/range, 0.f,                       0.f,
        0.f,       0.f,       2.f/(far_z-near_z),        0.f,
        0.f,       0.f,      -(far_z+near_z)/(far_z-near_z), 1.f
    };

    float* mat = m_cascadeLightMatrix[cascade];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            mat[i*4+j] = 0.f;
            for (int k = 0; k < 4; ++k)
                mat[i*4+j] += proj[k*4+j] * view[i*4+k];
        }

    // SOURCEPORT: per-cascade NDC bias — scale with texel size so all cascades share
    // the same ~1.875-texel world-space offset (matching the original single-pass tuning
    // of 60 GU at 32 GU/texel).  Without scaling, C0 (4 GU/texel) would get 15 texels
    // of bias, creating a large shadow-free zone near the player and making close shadows
    // appear lighter than distant ones.  Cap at 60 GU so the far cascade matches original.
    float worldBias = std::min(texelSize * 1.875f, 60.0f);
    m_cascadeBiasNDC[cascade] = worldBias / (far_z - near_z);

    // ── Bind this cascade's FBO and set up GL state ───────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_cascadeFBO[cascade]);
    glViewport(0, 0, CASCADE_SHADOW_SIZE, CASCADE_SHADOW_SIZE);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.f, 4.f);

    m_shadowPassActive     = true;
    m_shadowGeomSuppressed = false;

    glUseProgram(m_depthShaderProgram);
    glUniformMatrix4fv(m_depthLocLightSpace, 1, GL_FALSE, mat);
    glUniform1f(m_depthLocVideoCX,  m_unifVideoCX);
    glUniform1f(m_depthLocVideoCY,  m_unifVideoCY);
    glUniform1f(m_depthLocCameraW,  m_unifCameraW);
    glUniform1f(m_depthLocCameraH,  m_unifCameraH);
    glUniform3fv(m_depthLocCameraPos,  1,         m_cameraWorldPos);
    glUniformMatrix3fv(m_depthLocCamToWorld, 1, GL_FALSE, m_camToWorld);
    glUniform1i(m_depthLocTexture, 0);
}

void RendererGL::EndWorldShadowPass(int cascade) {
    FlushWorldSpaceShadow();  // flush world-space batch for this cascade

    if (cascade < NUM_SHADOW_CASCADES - 1) {
        // SOURCEPORT: more cascades follow — keep shadow-pass GL state active.
        // BeginWorldShadowPass(cascade+1) will bind the next FBO and clear it.
        return;
    }

    // ── Last cascade: restore GL state ───────────────────────────────────────
    m_shadowPassActive = false;
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
    glClearDepth(0.0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glUseProgram(m_shaderProgram);

    // Bind shadow depth array to unit 4.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_cascadeDepthTex);
    glActiveTexture(GL_TEXTURE0);

    // Upload shadow uniforms to main shader.
    glUniform1i(m_locShadowMapArray, 4);
    glUniform1i(m_locWorldShadow, 1);
    m_shadowsRendered = true;

    // Upload all 3 light matrices in one call (mat4 array).
    float flatMat[NUM_SHADOW_CASCADES * 16];
    for (int c = 0; c < NUM_SHADOW_CASCADES; ++c)
        std::memcpy(flatMat + c*16, m_cascadeLightMatrix[c], 16*sizeof(float));
    glUniformMatrix4fv(m_locLightSpaceArr, NUM_SHADOW_CASCADES, GL_FALSE, flatMat);

    glUniform1f(m_locShadowStrength, m_shadowStrength);

    // Upload per-cascade biases and split distances.
    float biasArr[3] = { m_cascadeBiasNDC[0], m_cascadeBiasNDC[1], m_cascadeBiasNDC[2] };
    glUniform3fv(m_locCascadeBias, 1, biasArr);
    float splits[3] = { m_cascadeRanges[0], m_cascadeRanges[1], m_cascadeRanges[2] };
    glUniform3fv(m_locCascadeSplits, 1, splits);
}

// SOURCEPORT: Post-process overlay — bloom implementation.
// Proven approach (Steps 1-3 confirmed):
//   - fullscreen quad renders correctly
//   - glCopyTexImage2D reads backbuffer into texture
//   - shaders can read and modify that texture
// Bloom pipeline: copy scene → extract brights → blur H → blur V → composite additive
void RendererGL::RunPostOverlay() {
    // ── Shared vertex shader ──────────────────────────────────────────────────
    static const char* VS = R"(
#version 330 core
layout(location=0) in vec2 aPos;
out vec2 vTexCoord;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); vTexCoord = aPos * 0.5 + 0.5; }
)";

    // ── Bloom pass shaders ────────────────────────────────────────────────────
    // Pass 1: extract pixels above brightness threshold
    static const char* FS_THRESHOLD = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTex;
uniform float uThreshold;
void main() {
    vec3 c = texture(uTex, vTexCoord).rgb;
    float brightness = dot(c, vec3(0.299, 0.587, 0.114));
    float soft = clamp((brightness - uThreshold) / max(uThreshold * 0.5, 0.001), 0.0, 1.0);
    FragColor = vec4(c * soft, 1.0);
}
)";

    // Pass 2: 9-tap Gaussian blur (used for both H and V, direction via uniform)
    static const char* FS_BLUR = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;   // (texelW,0) for H, (0,texelH) for V
void main() {
    // Gaussian weights for 9 taps (sigma≈2)
    float w[5] = float[](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);
    vec3 acc = texture(uTex, vTexCoord).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        acc += texture(uTex, vTexCoord + uDir * float(i)).rgb * w[i];
        acc += texture(uTex, vTexCoord - uDir * float(i)).rgb * w[i];
    }
    FragColor = vec4(acc, 1.0);
}
)";

    // ── God ray pass shaders ──────────────────────────────────────────────────
    // SOURCEPORT: screen-space crepuscular rays (Mitchell radial blur technique).
    // Mask pass: sky pixels near the sun become the light source. The engine uses
    // reversed depth (clear=0, sky plane writes sz≈0.0001, geometry writes -16/cam_z
    // which is ≥ ~0.001 even at maximum view range), so depth < 0.0004 ⟺ sky.
    static const char* FS_GODRAY_MASK = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec2  uSunPos;    // sun position in UV space
uniform vec3  uSunColor;
uniform float uAspect;    // width/height for circular falloff
void main() {
    float d = texture(uDepth, vTexCoord).r;
    // SOURCEPORT: hairline terrain cracks read as sky depth; without this fill the
    // radial blur smears each crack row into a long fading streak whenever the sun
    // sits low — same selective crack test as the height fog pass.
    if (d < 0.0004) {
        vec2 tx = 1.0 / vec2(textureSize(uDepth, 0));
        float dl = texture(uDepth, vTexCoord - vec2(tx.x, 0.0)).r;
        float dr = texture(uDepth, vTexCoord + vec2(tx.x, 0.0)).r;
        float du = texture(uDepth, vTexCoord + vec2(0.0, tx.y)).r;
        float dd = texture(uDepth, vTexCoord - vec2(0.0, tx.y)).r;
        if (max(min(dl, dr), min(du, dd)) > 0.0004) d = 1.0;  // crack ⟹ solid, not sky
    }
    float sky = (d < 0.0004) ? 1.0 : 0.0;
    vec2 dv = (vTexCoord - uSunPos) * vec2(uAspect, 1.0);
    float falloff = max(1.0 - dot(dv, dv) * 2.0, 0.0);
    FragColor = vec4(uSunColor * (sky * falloff * falloff), 1.0);
}
)";

    // Radial blur pass: march each pixel toward the sun accumulating masked light.
    static const char* FS_GODRAY_BLUR = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2  uSunPos;
uniform float uDensity;   // 0..1 fraction of the pixel→sun distance to march
uniform float uDecay;     // per-sample falloff
void main() {
    const int SAMPLES = 48;
    vec2 delta = (vTexCoord - uSunPos) * (uDensity / float(SAMPLES));
    vec2 p = vTexCoord;
    float illum = 1.0;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < SAMPLES; ++i) {
        p -= delta;
        acc += texture(uTex, p).rgb * illum;
        illum *= uDecay;
    }
    FragColor = vec4(acc * (2.0 / float(SAMPLES)), 1.0);
}
)";

    // ── SSAO shaders ──────────────────────────────────────────────────────────
    // SOURCEPORT: Alchemy-style screen-space ambient occlusion, depth-only (no
    // G-buffer): view-space position reconstructed from the captured depth, normal
    // from position derivatives, 12-sample golden-angle spiral disk with a
    // per-pixel random rotation. World-space radius keeps the AO scale consistent
    // at any distance. Sky pixels return 1.0 (unoccluded).
    static const char* FS_SSAO = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec2  uScreenSize;   // full-res pixels
uniform float uVideoCX;
uniform float uVideoCY;
uniform float uCameraW;
uniform float uCameraH;
uniform float uRadius;       // world-space sample radius (GU)
uniform float uIntensity;    // occlusion gain

vec3 viewPos(vec2 uv) {
    // SOURCEPORT: deterministic full-res texel selection. This half-res pass's
    // pixel centers land exactly on full-res texel boundaries, where NEAREST
    // rounding (and a naive floor(uv*size) snap) flips on float noise — per-row
    // it made dark horizontal lines on receding ground, per-column it striped
    // vertical silhouettes. Deriving the half-res cell index first (floor at
    // i+0.5, never boundary-ambiguous) and mapping to a fixed texel of the 2×2
    // block makes ray and depth agree on the same texel, deterministically.
    vec2 fs = vec2(textureSize(uDepth, 0));
    uv = (floor(uv * fs * 0.5) * 2.0 + 0.5) / fs;
    float d = texture(uDepth, uv).r;
    float z = 16.0 / max(d, 1e-6);
    vec2 px = vec2(uv.x * uScreenSize.x, (1.0 - uv.y) * uScreenSize.y);
    return vec3((px.x - uVideoCX) * z / uCameraW,
                (uVideoCY - px.y) * z / uCameraH,
                -z);
}

// SOURCEPORT: interleaved gradient noise (Jimenez 2014). The classic
// fract(sin(dot))·43758 hash degenerates into structured horizontal bands at
// large pixel coords (GPU sin precision), which correlated the kernel rotation
// along screen rows and surfaced as camera-locked dash lines in the AO.
float hash(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

void main() {
    float d0 = texture(uDepth, vTexCoord).r;
    if (d0 < 0.0004) { FragColor = vec4(1.0); return; }   // sky: unoccluded
    vec3 P = viewPos(vTexCoord);
    // SOURCEPORT: robust normal reconstruction — dFdx/dFdy straddle depth
    // discontinuities (hairline terrain cracks at tile rows), giving garbage
    // normals for the pixels alongside each crack and rows of false-occlusion
    // dashes that track the camera. Instead, build each tangent from whichever
    // side has the smaller depth step — always the same surface, never across
    // a crack or silhouette edge. Tap stride = one HALF-res texel (2 full-res):
    // depth reads quantize to 2×2 blocks, so a 1-texel step could land in the
    // same block (zero tangent → NaN normal).
    vec2 fts = 2.0 / uScreenSize;
    vec3 Pr = viewPos(vTexCoord + vec2(fts.x, 0.0));
    vec3 Pl = viewPos(vTexCoord - vec2(fts.x, 0.0));
    vec3 Pu = viewPos(vTexCoord + vec2(0.0, fts.y));
    vec3 Pd = viewPos(vTexCoord - vec2(0.0, fts.y));
    vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddx, ddy));
    if (dot(N, -P) < 0.0) N = -N;        // ensure normal faces the camera
    float z = -P.z;
    // World radius → screen pixels at this depth, clamped to bound texture-cache
    // cost up close and keep at least a couple of pixels far away.
    float rpx = clamp(uRadius * uCameraW / z, 2.0, 80.0);
    vec2 ruv = vec2(rpx / uScreenSize.x, rpx * (uCameraH / uCameraW) / uScreenSize.y);
    float angle = hash(gl_FragCoord.xy) * 6.2831853;
    const int KERNEL = 12;
    float occ = 0.0;
    for (int i = 0; i < KERNEL; i++) {
        float a = angle + float(i) * 2.3999632;   // golden angle
        float rr = (float(i) + 0.7) / float(KERNEL);
        vec2 suv = vTexCoord + vec2(cos(a), sin(a)) * ruv * rr;
        vec3 S = viewPos(suv);
        vec3 v = S - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        // Positive occlusion when the sample sits above the surface plane (minus a
        // depth-proportional bias against self-occlusion), with squared-distance
        // falloff so distant geometry contributes nothing — built-in range check.
        occ += max(0.0, vn - 0.02 * z) / (vv + uRadius * uRadius * 0.01);
    }
    float ao = 1.0 - clamp(occ * uIntensity * uRadius / float(KERNEL), 0.0, 1.0);
    FragColor = vec4(vec3(ao), 1.0);
}
)";

    // SOURCEPORT: depth-aware bilateral 5×5 blur — smooths SSAO sampling noise
    // without bleeding occlusion across depth edges (no halos around silhouettes).
    // Gradient-aware: on ground receding from the camera, depth between VERTICAL
    // neighbours legitimately changes by hundreds of GU per texel; a naive
    // |sd - cd| test rejects them, degenerating the blur to horizontal-only and
    // smearing AO noise into camera-locked horizontal dashes. Instead, predict
    // each tap's depth from the local gradient and weight by the deviation from
    // that prediction — continuous slopes blur fully, discontinuities still reject.
    static const char* FS_SSAO_BLUR = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uAO;       // half-res raw AO
uniform sampler2D uDepth;    // full-res depth
uniform vec2 uTexel;         // half-res texel size
float lz(vec2 uv) {
    // SOURCEPORT: same deterministic texel selection as the AO pass — half-res
    // pixel centers sit on full-res texel boundaries where rounding flips on
    // float noise; pick a fixed texel of each 2×2 block via the half-res cell.
    vec2 fs = vec2(textureSize(uDepth, 0));
    uv = (floor(uv * fs * 0.5) * 2.0 + 0.5) / fs;
    return 16.0 / max(texture(uDepth, uv).r, 1e-6);
}
void main() {
    float cd = lz(vTexCoord);
    // Local depth gradient per texel, clamped: a discontinuity corrupts the
    // gradient estimate, but real surface slope rarely exceeds 25% of depth/texel.
    float gx = (lz(vTexCoord + vec2(uTexel.x, 0.0)) - lz(vTexCoord - vec2(uTexel.x, 0.0))) * 0.5;
    float gy = (lz(vTexCoord + vec2(0.0, uTexel.y)) - lz(vTexCoord - vec2(0.0, uTexel.y))) * 0.5;
    float gcap = cd * 0.25;
    vec2 grad = clamp(vec2(gx, gy), vec2(-gcap), vec2(gcap));
    float acc = 0.0, wsum = 0.0;
    for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
        vec2 suv = vTexCoord + vec2(float(dx), float(dy)) * uTexel;
        float sd = lz(suv);
        float expected = cd + grad.x * float(dx) + grad.y * float(dy);
        float w = exp(-float(dx*dx + dy*dy) * 0.12)
                * exp(-abs(sd - expected) / (cd * 0.03 + 16.0));
        acc += texture(uAO, suv).r * w;
        wsum += w;
    }
    FragColor = vec4(vec3(acc / max(wsum, 1e-5)), 1.0);
}
)";

    // ── Volumetric height fog shader ──────────────────────────────────────────
    // SOURCEPORT: depth-aware exponential height fog with sun forward scattering.
    // World position is reconstructed per pixel from the captured depth using the
    // same camera convention the shadow depth shader inverts (sz = -16/cam_z,
    // screen → camera via VideoCX/CY + CameraW/H, camera → world via uCamToWorld).
    // Fog density: rho(y) = density * exp(-(y - anchor) * falloff); the integral
    // along the view ray has the closed form used below (Quilez-style analytic fog).
    static const char* FS_HEIGHTFOG = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform vec2  uScreenSize;
uniform float uVideoCX;
uniform float uVideoCY;
uniform float uCameraW;
uniform float uCameraH;
uniform mat3  uCamToWorld;
uniform float uCameraY;
uniform vec3  uSunDir;
uniform float uDensity;    // extinction per GU at anchor height
uniform float uFalloff;    // height falloff per GU
uniform float uAnchorY;    // fog reference height (lowest terrain, GU)
uniform float uSkyDist;    // ray length used for sky pixels (GU)
uniform vec3  uFogColor;
uniform vec3  uSunColor;
uniform float uSunPower;
uniform sampler2D uAO;       // half-res blurred SSAO (1.0 = unoccluded)
uniform float uAOStrength;   // 0 = SSAO inactive this frame
uniform float uAODebug;      // 1 = visualize the raw AO buffer
void main() {
    vec3 scene = texture(uScene, vTexCoord).rgb;
    // SOURCEPORT: AO darkens the scene BEFORE fog is mixed in — occlusion is a
    // surface property; the fog scattering above it stays unoccluded.
    if (uAOStrength > 0.0) {
        float ao = texture(uAO, vTexCoord).r;
        // SOURCEPORT: ssao_debug pack param — show the blurred AO buffer directly
        // so sampling artifacts can be inspected without scene texture masking.
        if (uAODebug > 0.5) { FragColor = vec4(vec3(ao), 1.0); return; }
        scene *= mix(1.0, ao, uAOStrength);
    }
    float d = texture(uDepth, vTexCoord).r;
    // SOURCEPORT: seal hairline terrain cracks. T-junctions at tile/LOD seams leave
    // 1px gaps where the sky depth shows through between two ground rows; fogging
    // those pixels at sky distance draws faint dashes that track the camera. If a
    // sky-depth pixel has solid geometry on BOTH sides of an axis it is a crack,
    // not real sky — fill with the farther neighbour. True sky is unaffected: at
    // the horizon the pixels alongside are sky too, so no pair qualifies.
    if (d < 0.0004) {
        vec2 tx = 1.0 / uScreenSize;
        float dl = texture(uDepth, vTexCoord - vec2(tx.x, 0.0)).r;
        float dr = texture(uDepth, vTexCoord + vec2(tx.x, 0.0)).r;
        float du = texture(uDepth, vTexCoord + vec2(0.0, tx.y)).r;
        float dd = texture(uDepth, vTexCoord - vec2(0.0, tx.y)).r;
        float fill = max(min(dl, dr), min(du, dd));
        if (fill > 0.0004) d = fill;
    }
    // Reversed depth: sky plane writes ~0.0001 → treat as a long ray; geometry: cam_z = -16/d
    float dist = (d < 0.0004) ? uSkyDist : 16.0 / d;
    vec2 px = vec2(vTexCoord.x * uScreenSize.x, (1.0 - vTexCoord.y) * uScreenSize.y);
    vec3 camVec = vec3((px.x - uVideoCX) * dist / uCameraW,
                       (uVideoCY - px.y) * dist / uCameraH,
                       -dist);
    vec3 worldVec = uCamToWorld * camVec;
    float t = length(worldVec);
    vec3 rd = worldVec / t;
    // Analytic integral of exponential height density along the ray.
    float by = uFalloff * rd.y;
    float integ = (abs(by) > 1e-7) ? (1.0 - exp(clamp(-t * by, -60.0, 60.0))) / by : t;
    float optical = uDensity * exp(-(uCameraY - uAnchorY) * uFalloff) * integ;
    float f = 1.0 - exp(-max(optical, 0.0));
    // Mie-style forward scattering: fog glows warm when looking toward the sun.
    float sunAmt = pow(max(dot(rd, uSunDir), 0.0), uSunPower);
    vec3 fogCol = mix(uFogColor, uSunColor, sunAmt);
    FragColor = vec4(mix(scene, fogCol, f), 1.0);
}
)";

    // Pass 3: composite — bloom + tone mapping in one draw call
    static const char* FS_COMPOSITE = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uGodRays;
uniform float uGodRayIntensity;  // 0 = god rays inactive this frame
uniform float uAODebug;          // 1 = pass the scene (AO visualization) through untouched
uniform float uIntensity;
uniform int   uToneMap;      // 0=off, 1=ACES, 2=Reinhard
uniform float uExposure;     // pre-exposure multiplier (1.0 = neutral)
uniform float uCGEnabled;    // 0=off, 1=on
uniform float uSaturation;   // 1=neutral, >1 vivid, <1 desaturated
uniform float uContrast;     // 1=neutral, >1 punchy, <1 flat
uniform vec3  uLift;         // shadow color offset  (0,0,0 = neutral)
uniform vec3  uGain;         // highlight color scale (1,1,1 = neutral)
uniform float uSharpen;      // 0=off, >0 = unsharp mask strength

// ACES filmic approximation (Hill/Narkowicz).
// On LDR input [0,1]: lifts shadows, compresses highlights, adds mid contrast.
vec3 ACESFilmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x*(a*x+b)) / (x*(c*x+d)+e), 0.0, 1.0);
}

// Reinhard: simple luminance-preserving compression.
vec3 Reinhard(vec3 x) {
    float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
    return x / (1.0 + luma);
}

void main() {
    vec3 scene = texture(uScene, vTexCoord).rgb;
    // SOURCEPORT: AO debug — the scene pass already wrote the AO buffer as the
    // scene; bypass sharpen/bloom/god rays/tonemap so the view is truly raw AO.
    if (uAODebug > 0.5) { FragColor = vec4(scene, 1.0); return; }
    // SOURCEPORT: unsharp mask sharpen — applied to scene before bloom/tonemapping.
    // Samples 4-tap neighborhood average, adds the high-frequency difference back.
    // textureSize avoids needing a resolution uniform.
    if (uSharpen > 0.0) {
        vec2 ts = 1.0 / vec2(textureSize(uScene, 0));
        vec3 blur = (texture(uScene, vTexCoord + vec2( ts.x, 0)).rgb +
                     texture(uScene, vTexCoord + vec2(-ts.x, 0)).rgb +
                     texture(uScene, vTexCoord + vec2(0,  ts.y)).rgb +
                     texture(uScene, vTexCoord + vec2(0, -ts.y)).rgb) * 0.25;
        vec3 hf = scene - blur;
        // SOURCEPORT: coring — fade the unsharp mask out for sub-perceptual deltas
        // (≲2/255). Without it, faint derivative kinks in smooth fog gradients along
        // terrain vertex rows get etched into visible camera-tracking dash lines;
        // real texture and silhouette edges are far above this threshold.
        float amp = max(max(abs(hf.r), abs(hf.g)), abs(hf.b));
        float gate = smoothstep(0.004, 0.012, amp);
        scene = clamp(scene + hf * (uSharpen * gate), 0.0, 1.0);
    }
    vec3 bloom = texture(uBloom, vTexCoord).rgb;
    // SOURCEPORT: additive bloom — brights contribute a soft glow additively.
    // (1-scene) weighting prevents already-saturated pixels from over-clipping.
    // Applied before tone mapping so ACES/Reinhard compresses the bloom highlights.
    vec3 result = scene + bloom * uIntensity * (1.0 - scene);
    // SOURCEPORT: god rays added before exposure/tone mapping so ACES compresses the
    // shaft highlights instead of letting them clip to white.
    if (uGodRayIntensity > 0.0) {
        result += texture(uGodRays, vTexCoord).rgb * uGodRayIntensity;
    }
    // SOURCEPORT: tone mapping applied after bloom composite.
    result *= uExposure;
    if      (uToneMap == 1) result = ACESFilmic(result);
    else if (uToneMap == 2) result = Reinhard(result);
    // SOURCEPORT: color grading — contrast, saturation, lift/gain.
    // Applied after tone mapping so it operates on the final tonal range.
    if (uCGEnabled > 0.5) {
        // Contrast: S-curve pivot around 0.5
        result = clamp((result - 0.5) * uContrast + 0.5, 0.0, 1.0);
        // Saturation: lerp toward luma
        float luma = dot(result, vec3(0.2126, 0.7152, 0.0722));
        result = clamp(mix(vec3(luma), result, uSaturation), 0.0, 1.0);
        // Lift/gain: per-channel shadow offset + highlight scale
        result = clamp(result * uGain + uLift, 0.0, 1.0);
    }
    // SOURCEPORT: ±0.5 LSB dither — breaks up 8-bit banding on smooth gradients
    // (fog, sky, bloom) at the final quantization to the backbuffer. Interleaved
    // gradient noise: the sin-dot hash bands at large pixel coords.
    float dn = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
    result += (dn - 0.5) / 255.0;
    FragColor = vec4(result, 1.0);
}
)";

    // ── One-time initialisation ───────────────────────────────────────────────
    static GLuint s_vao = 0, s_vbo = 0;
    static GLuint s_progThreshold = 0, s_progBlur = 0, s_progComposite = 0;
    static GLuint s_progGRMask = 0, s_progGRBlur = 0;
    static GLuint s_progHeightFog = 0;
    static GLuint s_progSSAO = 0, s_progSSAOBlur = 0;
    static GLuint s_sceneTex = 0;
    static GLuint s_depthTex = 0;          // SOURCEPORT: scene depth capture for depth-aware effects
    static GLuint s_fboA = 0, s_texA = 0;  // half-res ping
    static GLuint s_fboB = 0, s_texB = 0;  // half-res pong (second pass uses same pair)
    static GLuint s_fboC = 0, s_texC = 0;  // half-res god ray mask
    static GLuint s_fboD = 0, s_texD = 0;  // half-res god ray blur result
    static GLuint s_fboE = 0, s_texE = 0;  // half-res raw SSAO
    static GLuint s_fboF = 0, s_texF = 0;  // half-res blurred SSAO
    static GLuint s_fogFBO = 0, s_fogTex = 0;  // full-res fogged scene
    static int    s_bloomW = 0, s_bloomH = 0;
    static bool   s_ready = false;
    // SOURCEPORT: cached uniform locations for all three post-process programs.
    // Avoids ~16 glGetUniformLocation driver hash-map lookups per frame.
    static GLint s_locThrTex = -1,    s_locThrThreshold = -1;
    static GLint s_locBlurTex = -1,   s_locBlurDir = -1;
    static GLint s_locCmpScene = -1,  s_locCmpBloom = -1,    s_locCmpIntensity = -1;
    static GLint s_locCmpToneMap = -1,s_locCmpExposure = -1, s_locCmpCGEnabled = -1;
    static GLint s_locCmpSat = -1,    s_locCmpContrast = -1;
    static GLint s_locCmpLift = -1,   s_locCmpGain = -1,     s_locCmpSharpen = -1;
    static GLint s_locCmpGodRays = -1, s_locCmpGRIntensity = -1, s_locCmpAODebug = -1;
    static GLint s_locGRMaskDepth = -1, s_locGRMaskSunPos = -1, s_locGRMaskColor = -1, s_locGRMaskAspect = -1;
    static GLint s_locGRBlurTex = -1,   s_locGRBlurSunPos = -1, s_locGRBlurDensity = -1, s_locGRBlurDecay = -1;
    static GLint s_locHFScene = -1, s_locHFDepth = -1, s_locHFScreenSize = -1;
    static GLint s_locHFVCX = -1, s_locHFVCY = -1, s_locHFCW = -1, s_locHFCH = -1;
    static GLint s_locHFC2W = -1, s_locHFCamY = -1, s_locHFSunDir = -1;
    static GLint s_locHFDensity = -1, s_locHFFalloff = -1, s_locHFAnchorY = -1, s_locHFSkyDist = -1;
    static GLint s_locHFColor = -1, s_locHFSunColor = -1, s_locHFSunPower = -1;
    static GLint s_locHFAO = -1, s_locHFAOStrength = -1, s_locHFAODebug = -1;
    static GLint s_locAODepth = -1, s_locAOScreenSize = -1, s_locAOVCX = -1, s_locAOVCY = -1;
    static GLint s_locAOCW = -1, s_locAOCH = -1, s_locAORadius = -1, s_locAOIntensity = -1;
    static GLint s_locAOBlurAO = -1, s_locAOBlurDepth = -1, s_locAOBlurTexel = -1;
    // SOURCEPORT: number of H+V blur iterations; 2 gives a ~5-texel effective kernel
    // at half-res (≈10 screen pixels), producing a soft glow visible at typical game distances.
    static constexpr int BLUR_ITERATIONS = 2;

    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
            fprintf(stderr, "[Bloom] Shader error: %s\n", log);
            glDeleteShader(s); return 0;
        }
        return s;
    };
    auto link = [&](GLuint vs, GLuint fs) -> GLuint {
        GLuint p = glCreateProgram();
        glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
        GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(p); p = 0; }
        glDeleteShader(vs); glDeleteShader(fs);
        return p;
    };

    if (!s_ready) {
        // Fullscreen quad VAO
        float verts[] = { -1,-1,  1,-1,  -1,1,  1,1 };
        glGenVertexArrays(1, &s_vao); glGenBuffers(1, &s_vbo);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);

        // Compile programs
        s_progThreshold = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_THRESHOLD));
        s_progBlur       = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_BLUR));
        s_progComposite  = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_COMPOSITE));
        s_progGRMask     = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_GODRAY_MASK));
        s_progGRBlur     = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_GODRAY_BLUR));
        s_progHeightFog  = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_HEIGHTFOG));
        s_progSSAO       = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_SSAO));
        s_progSSAOBlur   = link(compile(GL_VERTEX_SHADER, VS), compile(GL_FRAGMENT_SHADER, FS_SSAO_BLUR));

        // Scene copy texture (full-res)
        glGenTextures(1, &s_sceneTex);
        glBindTexture(GL_TEXTURE_2D, s_sceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // SOURCEPORT: depth copy texture (full-res) — captured from the backbuffer each
        // frame god rays are active. NEAREST filtering: depth values must not be blended.
        glGenTextures(1, &s_depthTex);
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        s_ready = s_progThreshold && s_progBlur && s_progComposite && s_progGRMask && s_progGRBlur
               && s_progHeightFog && s_progSSAO && s_progSSAOBlur;
        if (s_ready) {
            s_locThrTex       = glGetUniformLocation(s_progThreshold, "uTex");
            s_locThrThreshold = glGetUniformLocation(s_progThreshold, "uThreshold");
            s_locBlurTex      = glGetUniformLocation(s_progBlur,      "uTex");
            s_locBlurDir      = glGetUniformLocation(s_progBlur,      "uDir");
            s_locCmpScene     = glGetUniformLocation(s_progComposite,  "uScene");
            s_locCmpBloom     = glGetUniformLocation(s_progComposite,  "uBloom");
            s_locCmpIntensity = glGetUniformLocation(s_progComposite,  "uIntensity");
            s_locCmpToneMap   = glGetUniformLocation(s_progComposite,  "uToneMap");
            s_locCmpExposure  = glGetUniformLocation(s_progComposite,  "uExposure");
            s_locCmpCGEnabled = glGetUniformLocation(s_progComposite,  "uCGEnabled");
            s_locCmpSat       = glGetUniformLocation(s_progComposite,  "uSaturation");
            s_locCmpContrast  = glGetUniformLocation(s_progComposite,  "uContrast");
            s_locCmpLift      = glGetUniformLocation(s_progComposite,  "uLift");
            s_locCmpGain      = glGetUniformLocation(s_progComposite,  "uGain");
            s_locCmpSharpen   = glGetUniformLocation(s_progComposite,  "uSharpen");
            s_locCmpGodRays     = glGetUniformLocation(s_progComposite, "uGodRays");
            s_locCmpGRIntensity = glGetUniformLocation(s_progComposite, "uGodRayIntensity");
            s_locCmpAODebug     = glGetUniformLocation(s_progComposite, "uAODebug");
            s_locGRMaskDepth  = glGetUniformLocation(s_progGRMask, "uDepth");
            s_locGRMaskSunPos = glGetUniformLocation(s_progGRMask, "uSunPos");
            s_locGRMaskColor  = glGetUniformLocation(s_progGRMask, "uSunColor");
            s_locGRMaskAspect = glGetUniformLocation(s_progGRMask, "uAspect");
            s_locGRBlurTex     = glGetUniformLocation(s_progGRBlur, "uTex");
            s_locGRBlurSunPos  = glGetUniformLocation(s_progGRBlur, "uSunPos");
            s_locGRBlurDensity = glGetUniformLocation(s_progGRBlur, "uDensity");
            s_locGRBlurDecay   = glGetUniformLocation(s_progGRBlur, "uDecay");
            s_locHFScene      = glGetUniformLocation(s_progHeightFog, "uScene");
            s_locHFDepth      = glGetUniformLocation(s_progHeightFog, "uDepth");
            s_locHFScreenSize = glGetUniformLocation(s_progHeightFog, "uScreenSize");
            s_locHFVCX        = glGetUniformLocation(s_progHeightFog, "uVideoCX");
            s_locHFVCY        = glGetUniformLocation(s_progHeightFog, "uVideoCY");
            s_locHFCW         = glGetUniformLocation(s_progHeightFog, "uCameraW");
            s_locHFCH         = glGetUniformLocation(s_progHeightFog, "uCameraH");
            s_locHFC2W        = glGetUniformLocation(s_progHeightFog, "uCamToWorld");
            s_locHFCamY       = glGetUniformLocation(s_progHeightFog, "uCameraY");
            s_locHFSunDir     = glGetUniformLocation(s_progHeightFog, "uSunDir");
            s_locHFDensity    = glGetUniformLocation(s_progHeightFog, "uDensity");
            s_locHFFalloff    = glGetUniformLocation(s_progHeightFog, "uFalloff");
            s_locHFAnchorY    = glGetUniformLocation(s_progHeightFog, "uAnchorY");
            s_locHFSkyDist    = glGetUniformLocation(s_progHeightFog, "uSkyDist");
            s_locHFColor      = glGetUniformLocation(s_progHeightFog, "uFogColor");
            s_locHFSunColor   = glGetUniformLocation(s_progHeightFog, "uSunColor");
            s_locHFSunPower   = glGetUniformLocation(s_progHeightFog, "uSunPower");
            s_locHFAO         = glGetUniformLocation(s_progHeightFog, "uAO");
            s_locHFAOStrength = glGetUniformLocation(s_progHeightFog, "uAOStrength");
            s_locHFAODebug    = glGetUniformLocation(s_progHeightFog, "uAODebug");
            s_locAODepth      = glGetUniformLocation(s_progSSAO, "uDepth");
            s_locAOScreenSize = glGetUniformLocation(s_progSSAO, "uScreenSize");
            s_locAOVCX        = glGetUniformLocation(s_progSSAO, "uVideoCX");
            s_locAOVCY        = glGetUniformLocation(s_progSSAO, "uVideoCY");
            s_locAOCW         = glGetUniformLocation(s_progSSAO, "uCameraW");
            s_locAOCH         = glGetUniformLocation(s_progSSAO, "uCameraH");
            s_locAORadius     = glGetUniformLocation(s_progSSAO, "uRadius");
            s_locAOIntensity  = glGetUniformLocation(s_progSSAO, "uIntensity");
            s_locAOBlurAO     = glGetUniformLocation(s_progSSAOBlur, "uAO");
            s_locAOBlurDepth  = glGetUniformLocation(s_progSSAOBlur, "uDepth");
            s_locAOBlurTexel  = glGetUniformLocation(s_progSSAOBlur, "uTexel");
        }
        fprintf(stdout, "[Bloom] Init %s\n", s_ready ? "OK" : "FAILED");
    }

    if (!s_ready) return;

    // Recreate half-res FBOs if resolution changed
    int bw = m_width / 2, bh = m_height / 2;
    if (bw != s_bloomW || bh != s_bloomH) {
        s_bloomW = bw; s_bloomH = bh;

        auto makeFBO = [](GLuint& fbo, GLuint& tex, int w, int h, GLenum internalFmt) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (tex) glDeleteTextures(1, &tex);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        };
        makeFBO(s_fboA, s_texA, bw, bh, GL_RGB16F);
        makeFBO(s_fboB, s_texB, bw, bh, GL_RGB16F);
        makeFBO(s_fboC, s_texC, bw, bh, GL_RGB16F);
        makeFBO(s_fboD, s_texD, bw, bh, GL_RGB16F);
        // SOURCEPORT: single-channel targets for SSAO (raw + bilateral-blurred).
        // R16F not R8: 8-bit AO gradients band visibly once multiplied into the scene.
        makeFBO(s_fboE, s_texE, bw, bh, GL_R16F);
        makeFBO(s_fboF, s_texF, bw, bh, GL_R16F);
        // SOURCEPORT: full-res target for the scene pass (SSAO apply + height fog) —
        // replaces s_sceneTex as the source for bloom extraction and the composite.
        // RGB16F not RGB8: the slow fog gradient quantizes to visible 1/255 contour
        // bands at 8 bits, which the composite sharpen then etches into distinct
        // lines that track the camera (iso-distance contours).
        makeFBO(s_fogFBO, s_fogTex, m_width, m_height, GL_RGB16F);
        fprintf(stdout, "[Bloom] FBOs resized to %dx%d\n", bw, bh);
    }

    // ── Save GL state ─────────────────────────────────────────────────────────
    GLboolean depthTest;  glGetBooleanv(GL_DEPTH_TEST,     &depthTest);
    GLboolean depthWrite; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    GLboolean blend;      glGetBooleanv(GL_BLEND,          &blend);
    GLint blendSrc, blendDst, prevFBO, prevVAO;
    glGetIntegerv(GL_BLEND_SRC_RGB,  &blendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB,  &blendDst);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glBindVertexArray(s_vao);

    // ── Step A: copy backbuffer → sceneTex ───────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_sceneTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, m_width, m_height, 0);

    // ── Step A2: project sun to screen + shared depth capture ────────────────
    // SOURCEPORT: god ray intensity is 0 whenever the sun is behind the camera or
    // far off-screen; the composite pass then skips the god-ray texture entirely.
    float grIntensity = 0.0f;
    float sunU = 0.5f, sunV = 0.5f;
    if (m_godRaysEnabled && m_godRayIntensity > 0.0f) {
        // World→camera rotation R is the transpose of m_camToWorld (which stores
        // R^T column-major), so R[i][j] = m_camToWorld[i*3+j].
        const float* rm = m_camToWorld;
        const float* sd = m_sunDirWorld;
        float sunCamX = rm[0]*sd[0] + rm[1]*sd[1] + rm[2]*sd[2];
        float sunCamY = rm[3]*sd[0] + rm[4]*sd[1] + rm[5]*sd[2];
        float sunCamZ = rm[6]*sd[0] + rm[7]*sd[1] + rm[8]*sd[2];
        float facing  = -sunCamZ;   // camera looks down -z; >0 ⟹ sun in front
        if (facing > 0.05f) {
            // Same projection the depth shader inverts for world-pos reconstruction.
            float screenX = m_unifVideoCX + sunCamX * m_unifCameraW / facing;
            float screenY = m_unifVideoCY - sunCamY * m_unifCameraH / facing;
            sunU = screenX / (float)m_width;
            sunV = 1.0f - screenY / (float)m_height; // captured texture row 0 = screen bottom
            // Fade rays out as the sun moves past the screen edge so they never pop.
            float offU = fmaxf(0.0f, fmaxf(-sunU, sunU - 1.0f));
            float offV = fmaxf(0.0f, fmaxf(-sunV, sunV - 1.0f));
            float edgeFade = fmaxf(0.0f, 1.0f - fmaxf(offU, offV) * 2.0f);
            grIntensity = m_godRayIntensity * edgeFade;
        }
    }
    bool fogActive = m_heightFogEnabled && m_heightFogDensity > 0.0f;
    bool aoActive  = m_ssaoEnabled && m_ssaoStrength > 0.0f;
    if (fogActive || aoActive || grIntensity > 0.0f) {
        // SOURCEPORT: capture scene depth while the default framebuffer is still
        // bound — shared by SSAO, height fog and god ray passes.
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 0, 0, m_width, m_height, 0);
    }

    // ── Step A3a: SSAO (half-res: depth → raw AO → bilateral blur) ───────────
    if (aoActive) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboE);
        glViewport(0, 0, s_bloomW, s_bloomH);
        glUseProgram(s_progSSAO);
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glUniform1i(s_locAODepth, 0);
        glUniform2f(s_locAOScreenSize, (float)m_width, (float)m_height);
        glUniform1f(s_locAOVCX, m_unifVideoCX);
        glUniform1f(s_locAOVCY, m_unifVideoCY);
        glUniform1f(s_locAOCW,  m_unifCameraW);
        glUniform1f(s_locAOCH,  m_unifCameraH);
        glUniform1f(s_locAORadius,    m_ssaoRadius);
        glUniform1f(s_locAOIntensity, m_ssaoIntensity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Depth-aware blur: raw AO (unit 0) + depth (unit 1) → s_texF
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboF);
        glUseProgram(s_progSSAOBlur);
        glBindTexture(GL_TEXTURE_2D, s_texE);
        glUniform1i(s_locAOBlurAO, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glUniform1i(s_locAOBlurDepth, 1);
        glUniform2f(s_locAOBlurTexel, 1.0f / s_bloomW, 1.0f / s_bloomH);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glActiveTexture(GL_TEXTURE0);
    }

    // ── Step A3b: scene pass — SSAO apply + height fog (full-res → fogTex) ───
    // SOURCEPORT: AO multiplies the scene first, then fog mixes over it; the fogged
    // result replaces s_sceneTex for all downstream passes so bloom extraction and
    // tone mapping operate on the final scene. With fog disabled the pass runs with
    // density 0 (pure AO apply); with SSAO disabled uAOStrength=0 skips the AO read.
    GLuint sceneSrc = s_sceneTex;
    if (fogActive || aoActive) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fogFBO);
        glViewport(0, 0, m_width, m_height);
        glUseProgram(s_progHeightFog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_sceneTex);
        glUniform1i(s_locHFScene, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glUniform1i(s_locHFDepth, 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_texF);
        glUniform1i(s_locHFAO, 2);
        glUniform1f(s_locHFAOStrength, aoActive ? m_ssaoStrength : 0.0f);
        glUniform1f(s_locHFAODebug, (aoActive && m_ssaoDebug) ? 1.0f : 0.0f);
        glUniform2f(s_locHFScreenSize, (float)m_width, (float)m_height);
        glUniform1f(s_locHFVCX, m_unifVideoCX);
        glUniform1f(s_locHFVCY, m_unifVideoCY);
        glUniform1f(s_locHFCW,  m_unifCameraW);
        glUniform1f(s_locHFCH,  m_unifCameraH);
        glUniformMatrix3fv(s_locHFC2W, 1, GL_FALSE, m_camToWorld);
        glUniform1f(s_locHFCamY, m_cameraWorldPos[1]);
        glUniform3fv(s_locHFSunDir, 1, m_sunDirWorld);
        glUniform1f(s_locHFDensity, fogActive ? m_heightFogDensity : 0.0f);
        glUniform1f(s_locHFFalloff, m_heightFogFalloff);
        glUniform1f(s_locHFAnchorY, m_fogAnchorY);
        glUniform1f(s_locHFSkyDist, m_fogViewRange * 2.0f);
        glUniform3fv(s_locHFColor,    1, m_heightFogColor);
        glUniform3fv(s_locHFSunColor, 1, m_heightFogSunColor);
        glUniform1f(s_locHFSunPower, m_heightFogSunPower);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glActiveTexture(GL_TEXTURE0);
        sceneSrc = s_fogTex;
    }

    // ── Step A4: god ray GPU passes (mask + radial blur) ─────────────────────
    if (grIntensity > 0.0f) {
        // Mask pass: sky pixels near sun → light source (half-res fboC)
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboC);
        glViewport(0, 0, s_bloomW, s_bloomH);
        glUseProgram(s_progGRMask);
        glBindTexture(GL_TEXTURE_2D, s_depthTex);
        glUniform1i(s_locGRMaskDepth, 0);
        glUniform2f(s_locGRMaskSunPos, sunU, sunV);
        glUniform3f(s_locGRMaskColor, m_godRayColor[0], m_godRayColor[1], m_godRayColor[2]);
        glUniform1f(s_locGRMaskAspect, (float)m_width / (float)m_height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Radial blur pass: march toward sun (fboC → fboD)
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboD);
        glUseProgram(s_progGRBlur);
        glBindTexture(GL_TEXTURE_2D, s_texC);
        glUniform1i(s_locGRBlurTex, 0);
        glUniform2f(s_locGRBlurSunPos, sunU, sunV);
        glUniform1f(s_locGRBlurDensity, m_godRayDensity);
        glUniform1f(s_locGRBlurDecay, m_godRayDecay);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // ── Steps B-D: bloom extraction + multi-pass blur (skipped if only tone mapping active) ──
    // SOURCEPORT: 2 iterations of separable H+V Gaussian blur at half-resolution.
    // Each iteration doubles the effective kernel radius; 2 passes ≈ 10-screen-pixel glow.
    // Result always lands in s_texA (ping), ready for the composite step.
    if (m_postOverlayEnabled) {
        // Step B: bright-pass extract → s_fboA
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboA);
        glViewport(0, 0, s_bloomW, s_bloomH);
        glUseProgram(s_progThreshold);
        glBindTexture(GL_TEXTURE_2D, sceneSrc);
        glUniform1i(s_locThrTex,       0);
        glUniform1f(s_locThrThreshold, m_bloomThreshold);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Steps C-D: BLUR_ITERATIONS × (H blur ping→pong, V blur pong→ping)
        glUseProgram(s_progBlur);
        glUniform1i(s_locBlurTex, 0);
        for (int i = 0; i < BLUR_ITERATIONS; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_fboB);
            glBindTexture(GL_TEXTURE_2D, s_texA);
            glUniform2f(s_locBlurDir, 1.0f / s_bloomW, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glBindFramebuffer(GL_FRAMEBUFFER, s_fboA);
            glBindTexture(GL_TEXTURE_2D, s_texB);
            glUniform2f(s_locBlurDir, 0.0f, 1.0f / s_bloomH);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    // ── Step E: composite scene + bloom → default framebuffer ────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glUseProgram(s_progComposite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneSrc);
    glUniform1i(s_locCmpScene,    0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texA);
    glUniform1i(s_locCmpBloom,    1);
    // SOURCEPORT: god rays on unit 2 — s_texD may hold stale content when the sun was
    // off-screen this frame, but grIntensity=0 makes the composite shader skip it.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_texD);
    glUniform1i(s_locCmpGodRays,     2);
    glUniform1f(s_locCmpGRIntensity, grIntensity);
    glUniform1f(s_locCmpAODebug, (aoActive && m_ssaoDebug) ? 1.0f : 0.0f);
    glUniform1f(s_locCmpIntensity, m_postOverlayEnabled ? m_bloomIntensity : 0.0f);
    glUniform1i(s_locCmpToneMap,  m_toneMappingMode);
    glUniform1f(s_locCmpExposure, m_exposure);
    glUniform1f(s_locCmpCGEnabled, m_cgEnabled ? 1.0f : 0.0f);
    glUniform1f(s_locCmpSat,      m_cgSaturation);
    glUniform1f(s_locCmpContrast, m_cgContrast);
    glUniform3f(s_locCmpLift,     m_cgLift[0],  m_cgLift[1],  m_cgLift[2]);
    glUniform3f(s_locCmpGain,     m_cgGain[0],  m_cgGain[1],  m_cgGain[2]);
    glUniform1f(s_locCmpSharpen,  m_sharpenStrength);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ── Restore GL state ──────────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glBindVertexArray(prevVAO);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(m_shaderProgram);
    if (depthTest)  glEnable(GL_DEPTH_TEST);  else glDisable(GL_DEPTH_TEST);
    glDepthMask(depthWrite);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(blendSrc, blendDst);
    glViewport(0, 0, m_width, m_height);
}


