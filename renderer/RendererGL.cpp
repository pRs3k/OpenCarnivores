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
uniform vec3      uSunDirView;     // normalized; camera-ish space approximation

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

bool RendererGL::Init(void* windowHandle, int width, int height) {
    // SOURCEPORT: Phase 4 — read display options from game globals
    extern int OptDisplayMode, OptVSync;

    FILE* dbg = fopen("C:\\Users\\User\\Documents\\claude_code\\OpenCarnivores\\debug_renderer.txt", "a");
    if (dbg) {
        fprintf(dbg, "[OpenGL Renderer] Initializing RendererGL backend (GL 4.1 Core)\n");
        fflush(dbg);
        fclose(dbg);
    }

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

    // SOURCEPORT: Create world shadow map FBO (depth-only, standard GL_LESS convention).
    {
        glGenFramebuffers(1, &m_worldShadowFBO);
        glGenTextures(1, &m_worldShadowDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_worldShadowDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                     WORLD_SHADOW_SIZE, WORLD_SHADOW_SIZE,
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.f, 1.f, 1.f, 1.f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        glBindFramebuffer(GL_FRAMEBUFFER, m_worldShadowFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, m_worldShadowDepthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[RendererGL] World shadow FBO incomplete (0x%x)\n", status);
        else
            fprintf(stdout, "[RendererGL] World shadow FBO created (%dx%d)\n",
                    WORLD_SHADOW_SIZE, WORLD_SHADOW_SIZE);

        // Default shadow uniforms in main shader.
        glUseProgram(m_shaderProgram);
        glUniform1i(glGetUniformLocation(m_shaderProgram, "uShadowMap"), 4);
        glUniform1i(glGetUniformLocation(m_shaderProgram, "uWorldShadow"), 0);
        glUniform1f(glGetUniformLocation(m_shaderProgram, "uShadowStrength"), m_shadowStrength);
        glUniform1i(glGetUniformLocation(m_shaderProgram, "uWorldShadow"), 0);
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
    m_locSunDirView       = glGetUniformLocation(m_shaderProgram, "uSunDirView");
    m_locDebugMode        = glGetUniformLocation(m_shaderProgram, "uDebugMode");

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
    // Hardcoded screen-space sun (upper-right, toward viewer). Will make this
    // data-driven once world→view basis is plumbed.
    glUniform3f(m_locSunDirView, 0.4f, -0.5f, 0.8f);

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
        // SOURCEPORT: log depth shader uniform locations so dead-code elimination
        // of uLightSpace / uCamToWorld (loc==-1) is immediately visible in stderr.
        fprintf(stderr, "[Shadow] depth prog=%u uLightSpace=%d uCamToWorld=%d uCameraPos=%d\n",
            depthProg,
            glGetUniformLocation(depthProg, "uLightSpace"),
            glGetUniformLocation(depthProg, "uCamToWorld"),
            glGetUniformLocation(depthProg, "uCameraPos"));
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
            m_wsDepthProgram = wsProg;
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
                          (void*)offsetof(RenderVertex, sz));
    // location 2: vec4 color — D3D stores as ARGB (0xAARRGGBB), in memory [BB,GG,RR,AA].
    // Use GL_BGRA so OpenGL remaps bytes to (R=RR, G=GG, B=BB, A=AA) correctly.
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, color));
    // location 3: vec4 specular — same ARGB layout
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, specular));
    // location 4: vec2 texcoord (tu, tv)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                          (void*)offsetof(RenderVertex, tu));
    glBindVertexArray(0);

    // SOURCEPORT: world-space shadow VAO/VBO (layout: vec3 pos + vec2 uv)
    glGenVertexArrays(1, &m_wsVAO);
    glGenBuffers(1, &m_wsVBO);
    glBindVertexArray(m_wsVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_wsVBO);
    glBufferData(GL_ARRAY_BUFFER, MAX_WS_SHADOW_VERTS * sizeof(WSShadowVert), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WSShadowVert), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WSShadowVert), (void*)(3*sizeof(float)));
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

    // SOURCEPORT: world shadow FBO cleanup
    if (m_worldShadowDepthTex) { glDeleteTextures(1, &m_worldShadowDepthTex); m_worldShadowDepthTex = 0; }
    if (m_worldShadowFBO)      { glDeleteFramebuffers(1, &m_worldShadowFBO);  m_worldShadowFBO = 0; }

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
    glUniform1i(glGetUniformLocation(m_shaderProgram, "uWorldShadow"), 0);

    // Route scene into the post-processing FBO only when an effect is active and
    // we are in flatscreen mode. VR uses its own per-eye FBOs via XR::BeginEye().
    // When no effects are enabled we render directly to screen (no FBO overhead).
    extern bool g_enableBloom, g_enableToneMapping;
    bool needsCapture = (g_enableBloom || g_enableToneMapping)
                        && m_postProcessingPipeline
                        && m_postProcessingPipeline->IsInitialized()
                        && !XR::StereoActive();
    if (needsCapture) {
        m_postProcessingPipeline->BeginCapture();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
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
    // SOURCEPORT: run post-overlay if any post-process effect is active
    if (m_postOverlayEnabled || m_toneMappingMode != 0 || m_cgEnabled) {
        RunPostOverlay();
    }
    SDL_GL_SwapWindow(m_window);
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

void RendererGL::UnlockAndDrawGeometry(int vertexCount, bool colorKey) {
    if (vertexCount <= 0) return;

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
    // SOURCEPORT: during the shadow pass the game calls SetRenderStates(false,...)
    // for transparent/additive geometry (water, env-map overlays), which would
    // disable depth writes and leave the shadow FBO empty.  Ignore all state
    // changes during the depth-only pass except keeping glDepthMask on.
    if (m_shadowPassActive) {
        glDepthMask(GL_TRUE);
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
    glUniform1i(m_locFogEnabled, enabled ? 1 : 0);
}

void RendererGL::SetFogColor(uint32_t color) {
    m_fogColor[0] = ((color >> 16) & 0xFF) / 255.0f;
    m_fogColor[1] = ((color >> 8)  & 0xFF) / 255.0f;
    m_fogColor[2] = ((color)       & 0xFF) / 255.0f;
    m_fogColor[3] = 1.0f;
    glUniform4fv(m_locFogColor, 1, m_fogColor);
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
    glUniform1i(m_locAlphaTest, enabled ? 1 : 0);
}

GLuint RendererGL::GetWhiteTexture() {
    return m_whiteTexture;
}

void RendererGL::SetHUDMode(bool /*enabled*/) {
    // SOURCEPORT: no-op — fproc1 alpha is now always 1.0 in the shader (no HUD mode needed)
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
    const Materials::Material* m = static_cast<const Materials::Material*>(materialPtr);
    // Only enable PBR when a normal map exists — without it the tangent-frame
    // path contributes no perturbation and specular would be flat lit.
    if (!m || m->normalTex == 0) {
        if (m_pbrActive) {
            glUniform1i(m_locPBR, 0);
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
    glUniform1i(m_locPBR, 1);
    m_pbrActive = true;
}

void RendererGL::BindCustomMaterial(const void* materialPtr) {
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
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

// SOURCEPORT: Render text using GDI into a temp DIB, then upload as GL texture.
// Uses fnt_Small for all menu and HUD text. Performance is acceptable for
// non-realtime menu use; in-game calls are infrequent.
// SOURCEPORT: shared scaled font — built once per WinH change, used by DrawText and MeasureText.
// fnt_Small is 14px/5px wide designed for 600px height; scale both dimensions with WinH.
static HFONT  s_scaledFont  = NULL;
static int    s_scaledWinH  = 0;

static HFONT GetScaledFont() {
    extern HFONT fnt_Small;
    if (s_scaledWinH != WinH) {
        if (s_scaledFont) { DeleteObject(s_scaledFont); s_scaledFont = NULL; }
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
static HFONT  s_bigFont    = NULL;
static int    s_bigWinH    = 0;

static HFONT GetBigFont() {
    if (s_bigWinH != WinH) {
        if (s_bigFont) { DeleteObject(s_bigFont); s_bigFont = NULL; }
        int fh = std::max(36, 36 * WinH / 600);
        int fw = std::max(14, 14 * WinH / 600);
        s_bigFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, NULL);
        s_bigWinH = WinH;
    }
    return s_bigFont ? s_bigFont : GetScaledFont();
}

// SOURCEPORT: menu font — fnt_Midd style: weight=550 (semibold), 16px/7px at 600p, scaled by WinH/600.
static HFONT  s_menuFont   = NULL;
static int    s_menuWinH   = 0;

static HFONT GetMenuFont() {
    if (s_menuWinH != WinH) {
        if (s_menuFont) { DeleteObject(s_menuFont); s_menuFont = NULL; }
        int fh = std::max(16, 16 * WinH / 600);
        int fw = std::max(7,   7 * WinH / 600);
        // SOURCEPORT: NULL face name → system default sans-serif (matches original fnt_Midd style).
        // Weight=700 (Bold) to match the medium-bold appearance of the original menu text.
        s_menuFont = CreateFontA(fh, fw, 0, 0, 700, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, NULL);
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
    HDC hdc = CreateCompatibleDC(NULL);
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
    HBITMAP hbmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &dibBits, NULL, 0);
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
    glUniform1i(m_locFogEnabled, m_fogEnabled ? 1 : 0);
}

int RendererGL::MeasureText(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetScaledFont();
    HDC hdc = CreateCompatibleDC(NULL);
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
    HDC hdc = CreateCompatibleDC(NULL);
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
    DrawTextWithFont(x, y, text, color, GetScaledFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextMed(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    DrawTextWithFont(x, y, text, color, GetMenuFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

void RendererGL::DrawTextBig(int x, int y, const char* text, uint32_t color) {
    if (!text || !text[0]) return;
    DrawTextWithFont(x, y, text, color, GetBigFont(),
        m_bitmapTexture, m_vao, m_vbo,
        m_zBufferEnabled, m_locAlphaTest, m_locFogEnabled, m_fogEnabled);
}

int RendererGL::MeasureTextBig(const char* text) {
    if (!text || !text[0]) return 0;
    HFONT font = GetBigFont();
    HDC hdc = CreateCompatibleDC(NULL);
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
    float a = ((argbColor >> 24) & 0xFF) / 255.0f;
    float r = ((argbColor >> 16) & 0xFF) / 255.0f;
    float g = ((argbColor >> 8)  & 0xFF) / 255.0f;
    float b = ((argbColor)       & 0xFF) / 255.0f;

    uint32_t vertColor =
        (uint32_t)(a * 255) << 24 |
        (uint32_t)(b * 255) << 16 |  // Note: GL reads as RGBA bytes, but our
        (uint32_t)(g * 255) << 8  |  // vertex attrib is BGRA from the D3D convention
        (uint32_t)(r * 255);

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
    GLint loc;
    // SOURCEPORT: log uniform locations once to stderr so we can detect dead-code
    // elimination of uCameraPos/uCamToWorld by the GLSL driver (loc==-1 means eliminated).
    static bool s_loggedLocs = false;
    if (!s_loggedLocs) {
        s_loggedLocs = true;
        fprintf(stderr, "[Shadow] prog=%u uVideoCX=%d uCameraW=%d uCameraPos=%d uCamToWorld=%d\n",
            m_shaderProgram,
            glGetUniformLocation(m_shaderProgram, "uVideoCX"),
            glGetUniformLocation(m_shaderProgram, "uCameraW"),
            glGetUniformLocation(m_shaderProgram, "uCameraPos"),
            glGetUniformLocation(m_shaderProgram, "uCamToWorld"));
    }
    loc = glGetUniformLocation(m_shaderProgram, "uVideoCX"); glUniform1f(loc, vcx);
    loc = glGetUniformLocation(m_shaderProgram, "uVideoCY"); glUniform1f(loc, vcy);
    loc = glGetUniformLocation(m_shaderProgram, "uCameraW"); glUniform1f(loc, cw);
    loc = glGetUniformLocation(m_shaderProgram, "uCameraH"); glUniform1f(loc, ch);
    loc = glGetUniformLocation(m_shaderProgram, "uCameraPos");
    glUniform3fv(loc, 1, m_cameraWorldPos);
    loc = glGetUniformLocation(m_shaderProgram, "uCamToWorld");
    glUniformMatrix3fv(loc, 1, GL_FALSE, m_camToWorld);
}

void RendererGL::FlushWorldSpaceShadow() {
    if (m_wsShadowCount == 0 || !m_wsDepthProgram || !m_wsVAO) return;
    glUseProgram(m_wsDepthProgram);
    glUniformMatrix4fv(glGetUniformLocation(m_wsDepthProgram, "uLightSpace"),
                       1, GL_FALSE, m_worldLightMatrix);
    glUniform1i(glGetUniformLocation(m_wsDepthProgram, "uAlphaTest"), m_wsAlphaTest ? 1 : 0);
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
    m_wsAlphaTest = alphaTest;
    m_wsShadowBuffer[m_wsShadowCount++] = {x0, y0, z0, u0, v0};
    m_wsShadowBuffer[m_wsShadowCount++] = {x1, y1, z1, u1, v1};
    m_wsShadowBuffer[m_wsShadowCount++] = {x2, y2, z2, u2, v2};
}

void RendererGL::SetSunDirection(float x, float y, float z) {
    // SOURCEPORT: caller passes the unnormalized sun direction (e.g. -SunShadowK, 1, -SunShadowK).
    // Normalise and store; BeginWorldShadowPass reads m_sunDirWorld each frame.
    float len = sqrtf(x*x + y*y + z*z);
    if (len < 1e-6f) return;
    m_sunDirWorld[0] = x / len;
    m_sunDirWorld[1] = y / len;
    m_sunDirWorld[2] = z / len;
}

void RendererGL::BeginWorldShadowPass() {
    // ── Compute light-space matrix ────────────────────────────────────────────
    // Sun direction points FROM scene TOWARD sun (unit vector).
    float lx = m_sunDirWorld[0], ly = m_sunDirWorld[1], lz = m_sunDirWorld[2];
    float len = sqrtf(lx*lx + ly*ly + lz*lz);
    lx /= len; ly /= len; lz /= len;

    // SOURCEPORT: place the light IN the sun direction (above the scene).
    // Light looks toward scene (-sunDir), so Z_view = dist for objects at camera
    // height, with smaller Z_view = closer to sun = smaller stored depth.
    const float dist = 10000.0f;
    float cx = m_cameraWorldPos[0], cy = m_cameraWorldPos[1], cz = m_cameraWorldPos[2];
    float lpx = cx + lx*dist, lpy = cy + ly*dist, lpz = cz + lz*dist; // light ON sun side

    // Build orthonormal basis.  fwd = -sunDir: light looks TOWARD the scene.
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

    float tx = -(rt[0]*lpx + rt[1]*lpy + rt[2]*lpz);
    float ty = -(up[0]*lpx + up[1]*lpy + up[2]*lpz);
    float tz = -(fwd[0]*lpx + fwd[1]*lpy + fwd[2]*lpz);

    // View matrix (column-major GL).
    float view[16] = {
        rt[0], up[0], fwd[0], 0.f,
        rt[1], up[1], fwd[1], 0.f,
        rt[2], up[2], fwd[2], 0.f,
        tx,    ty,    tz,     1.f
    };

    // SOURCEPORT: left-handed ortho — Z_view is positive for objects in front
    // (Z_view = dist for camera pos). +2/(far-near) maps [near,far]→NDC[-1,+1].
    // The previous -2/(far-near) mapped Z_view≈10000 to NDC≈-2, clipping everything.
    //
    // SOURCEPORT: far_z sizing analysis.
    // The fragment shader gate (proj.z < 1.0) excludes any fragment whose
    // z_light > far_z, cutting off shadows short.
    // For terrain at world distance R from the camera, the maximum z_light
    // (along the light's forward axis) is:
    //   z_light_max = dist + R * |fwd_horiz| = dist + R * 0.577   (for K=0.5 sun)
    // So far_z must satisfy: far_z > dist + range * 0.577.
    // Use 0.75 (0.577 + 30% margin) to cover all view directions and K values.
    const float range  = m_shadowRange;
    const float near_z = 1.0f, far_z = dist + range * 0.75f;
    float proj[16] = {
        1.f/range, 0.f,       0.f,                       0.f,
        0.f,       1.f/range, 0.f,                       0.f,
        0.f,       0.f,       2.f/(far_z-near_z),        0.f,  // +2, not -2
        0.f,       0.f,      -(far_z+near_z)/(far_z-near_z), 1.f
    };

    // SOURCEPORT: lightSpace = proj * view.
    // Column-major GL convention: M_cm[col=i, row=j] encodes M_math[j][i].
    // Correct formula for C = A*B: C_cm[i*4+j] = Σk A_cm[k*4+j] * B_cm[i*4+k].
    // (The previous index order computed view*proj — reversed, scrambling all depths.)
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            m_worldLightMatrix[i*4+j] = 0.f;
            for (int k = 0; k < 4; ++k)
                m_worldLightMatrix[i*4+j] += proj[k*4+j] * view[i*4+k];
        }

    // ── Set up depth FBO and GL state ─────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_worldShadowFBO);
    glViewport(0, 0, WORLD_SHADOW_SIZE, WORLD_SHADOW_SIZE);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    // SOURCEPORT: polygon offset prevents terrain self-shadow acne in the depth pass.
    // For orthographic projection the world-space effect of the slope factor is
    // independent of far_z (the far-z scaling cancels in the depth slope formula),
    // so static values are correct here.
    glPolygonOffset(2.f, 4.f);
    // SOURCEPORT: store NDC bias = one shadow-texel width in depth space.
    // Texel size in GU = 2*range / WORLD_SHADOW_SIZE; divided by (far_z - near_z)
    // gives a bias that exactly equals one texel of world-space depth error.
    // This scales automatically with both range and far_z across all view distances.
    m_shadowBiasNDC = (2.f * range / (float)WORLD_SHADOW_SIZE) / (far_z - near_z);

    m_shadowPassActive = true; // SOURCEPORT: guards UnlockAndDraw* to re-assert depth program
    // ── Upload uniforms to depth shader ───────────────────────────────────────
    glUseProgram(m_depthShaderProgram);
    GLint loc;
    loc = glGetUniformLocation(m_depthShaderProgram, "uLightSpace");
    glUniformMatrix4fv(loc, 1, GL_FALSE, m_worldLightMatrix);
    loc = glGetUniformLocation(m_depthShaderProgram, "uVideoCX"); glUniform1f(loc, m_unifVideoCX);
    loc = glGetUniformLocation(m_depthShaderProgram, "uVideoCY"); glUniform1f(loc, m_unifVideoCY);
    loc = glGetUniformLocation(m_depthShaderProgram, "uCameraW"); glUniform1f(loc, m_unifCameraW);
    loc = glGetUniformLocation(m_depthShaderProgram, "uCameraH"); glUniform1f(loc, m_unifCameraH);
    loc = glGetUniformLocation(m_depthShaderProgram, "uCameraPos");
    glUniform3fv(loc, 1, m_cameraWorldPos);
    loc = glGetUniformLocation(m_depthShaderProgram, "uCamToWorld");
    glUniformMatrix3fv(loc, 1, GL_FALSE, m_camToWorld);
    loc = glGetUniformLocation(m_depthShaderProgram, "uTexture"); glUniform1i(loc, 0);
}

void RendererGL::EndWorldShadowPass() {
    FlushWorldSpaceShadow();  // SOURCEPORT: flush world-space batch before ending pass
    m_shadowPassActive = false;
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);     // SOURCEPORT: restore depth write — game may have called
                              // SetRenderStates(false,...) during the shadow DrawScene.
    glDepthFunc(GL_GEQUAL);   // Restore reversed depth for main scene
    glClearDepth(0.0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glUseProgram(m_shaderProgram);

    // Bind shadow depth texture to unit 4.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_worldShadowDepthTex);
    glActiveTexture(GL_TEXTURE0);

    // Upload shadow uniforms to main shader.
    GLint loc;
    loc = glGetUniformLocation(m_shaderProgram, "uShadowMap");
    glUniform1i(loc, 4);
    loc = glGetUniformLocation(m_shaderProgram, "uWorldShadow");
    glUniform1i(loc, 1);
    loc = glGetUniformLocation(m_shaderProgram, "uLightSpace");
    glUniformMatrix4fv(loc, 1, GL_FALSE, m_worldLightMatrix);
    loc = glGetUniformLocation(m_shaderProgram, "uShadowStrength");
    glUniform1f(loc, m_shadowStrength);
    loc = glGetUniformLocation(m_shaderProgram, "uShadowBias");
    glUniform1f(loc, m_shadowBiasNDC);
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

    // Pass 3: composite — bloom + tone mapping in one draw call
    static const char* FS_COMPOSITE = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uBloom;
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
    // SOURCEPORT: unsharp mask sharpen — applied to scene before bloom/tonemapping.
    // Samples 4-tap neighborhood average, adds the high-frequency difference back.
    // textureSize avoids needing a resolution uniform.
    if (uSharpen > 0.0) {
        vec2 ts = 1.0 / vec2(textureSize(uScene, 0));
        vec3 blur = (texture(uScene, vTexCoord + vec2( ts.x, 0)).rgb +
                     texture(uScene, vTexCoord + vec2(-ts.x, 0)).rgb +
                     texture(uScene, vTexCoord + vec2(0,  ts.y)).rgb +
                     texture(uScene, vTexCoord + vec2(0, -ts.y)).rgb) * 0.25;
        scene = clamp(scene + (scene - blur) * uSharpen, 0.0, 1.0);
    }
    vec3 bloom = texture(uBloom, vTexCoord).rgb;
    // SOURCEPORT: hue-preserving bloom — drive glow from bloom luminance
    // scaled by scene color so highlights glow in their own hue.
    float bloomLuma = dot(bloom, vec3(0.299, 0.587, 0.114)) * uIntensity;
    vec3 result = 1.0 - (1.0 - scene) * (1.0 - scene * bloomLuma);
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
    FragColor = vec4(result, 1.0);
}
)";

    // ── One-time initialisation ───────────────────────────────────────────────
    static GLuint s_vao = 0, s_vbo = 0;
    static GLuint s_progThreshold = 0, s_progBlur = 0, s_progComposite = 0;
    static GLuint s_sceneTex = 0;
    static GLuint s_fboA = 0, s_texA = 0;  // half-res ping
    static GLuint s_fboB = 0, s_texB = 0;  // half-res pong
    static int    s_bloomW = 0, s_bloomH = 0;
    static bool   s_ready = false;

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

        // Scene copy texture (full-res)
        glGenTextures(1, &s_sceneTex);
        glBindTexture(GL_TEXTURE_2D, s_sceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        s_ready = s_progThreshold && s_progBlur && s_progComposite;
        fprintf(stdout, "[Bloom] Init %s\n", s_ready ? "OK" : "FAILED");
    }

    if (!s_ready) return;

    // Recreate half-res FBOs if resolution changed
    int bw = m_width / 2, bh = m_height / 2;
    if (bw != s_bloomW || bh != s_bloomH) {
        s_bloomW = bw; s_bloomH = bh;

        auto makeFBO = [](GLuint& fbo, GLuint& tex, int w, int h) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (tex) glDeleteTextures(1, &tex);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        };
        makeFBO(s_fboA, s_texA, bw, bh);
        makeFBO(s_fboB, s_texB, bw, bh);
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

    // ── Steps B-D: bloom extraction + blur (skipped when only tone mapping is active) ──
    if (m_postOverlayEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fboA);
        glViewport(0, 0, s_bloomW, s_bloomH);
        glUseProgram(s_progThreshold);
        glUniform1i(glGetUniformLocation(s_progThreshold, "uTex"), 0);
        glUniform1f(glGetUniformLocation(s_progThreshold, "uThreshold"), m_bloomThreshold);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, s_fboB);
        glUseProgram(s_progBlur);
        glBindTexture(GL_TEXTURE_2D, s_texA);
        glUniform1i(glGetUniformLocation(s_progBlur, "uTex"), 0);
        glUniform2f(glGetUniformLocation(s_progBlur, "uDir"), 1.0f / s_bloomW, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, s_fboA);
        glBindTexture(GL_TEXTURE_2D, s_texB);
        glUniform2f(glGetUniformLocation(s_progBlur, "uDir"), 0.0f, 1.0f / s_bloomH);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // ── Step E: composite scene + bloom → default framebuffer ────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glUseProgram(s_progComposite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_sceneTex);
    glUniform1i(glGetUniformLocation(s_progComposite, "uScene"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texA);
    glUniform1i(glGetUniformLocation(s_progComposite, "uBloom"), 1);
    glUniform1f(glGetUniformLocation(s_progComposite, "uIntensity"), m_postOverlayEnabled ? m_bloomIntensity : 0.0f);
    glUniform1i(glGetUniformLocation(s_progComposite, "uToneMap"),    m_toneMappingMode);
    glUniform1f(glGetUniformLocation(s_progComposite, "uExposure"),   m_exposure);
    glUniform1f(glGetUniformLocation(s_progComposite, "uCGEnabled"),  m_cgEnabled ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(s_progComposite, "uSaturation"), m_cgSaturation);
    glUniform1f(glGetUniformLocation(s_progComposite, "uContrast"),   m_cgContrast);
    glUniform3f(glGetUniformLocation(s_progComposite, "uLift"),       m_cgLift[0],  m_cgLift[1],  m_cgLift[2]);
    glUniform3f(glGetUniformLocation(s_progComposite, "uGain"),       m_cgGain[0],  m_cgGain[1],  m_cgGain[2]);
    glUniform1f(glGetUniformLocation(s_progComposite, "uSharpen"),    m_sharpenStrength);
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


