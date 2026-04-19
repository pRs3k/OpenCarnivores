// Audio_OpenAL.cpp — SOURCEPORT: OpenAL Soft backend replacing Audio_SDL.cpp.
// Provides the same SDL_Audio_* API surface consumed by Audio_DLL.cpp so no
// game-side changes are required. Uses OpenAL's native 3D positional audio
// (alSource3f + AL_POSITION + distance model) instead of a hand-rolled pan calc.
// Format: 22050 Hz mono 16-bit source buffers — same raw PCM as the game's
// existing .wav assets, no conversion needed.

#define NOMINMAX
#include <SDL.h>
#include <AL/al.h>
#include <AL/alc.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/efx.h>
#include <AL/efx-presets.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include "hunt.h"

// SOURCEPORT: terrain raycast occlusion uses the game's GetLandH() to sample
// ground height between listener and source. CameraX/Y/Z are world-space
// floats updated every frame by Game.cpp before SetCameraPos fires.
extern float GetLandH(float x, float z);
extern float CameraX, CameraY, CameraZ;

static void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

// ─── Constants ────────────────────────────────────────────────────────────────
#define MAX_CHANNELS  16
#define SAMPLE_RATE   22050
// SOURCEPORT: world scale is ~256 units/meter — 6000 was ~23m and caused sounds
// to go silent after walking for a couple seconds. Bumped audible radius to
// ~14000 (~55m) and reference distance to 768 so gain curve stays near 1.0
// for a longer "natural" range before rolling off.
#define MAX_RADIUS    14000.f
#define MIN_RADIUS    768.f
// OAmbient crossfade rate: ~3s matches Audio_SDL.cpp's FADE_SPEED=4 @ 1024-sample callback.
#define FADE_RATE_PER_SEC  (256.f / 3.f)

// ─── State ────────────────────────────────────────────────────────────────────
static ALCdevice*  g_device  = nullptr;
static ALCcontext* g_context = nullptr;

// One OpenAL source per one-shot voice slot.
static ALuint g_srcVoice[MAX_CHANNELS] = {0};
// Matching low-pass filter per voice for terrain occlusion.
static ALuint g_filterVoice[MAX_CHANNELS] = {0};

// OAmbient: two slots for crossfade. Each has its own source + buffer.
struct OAmbient {
    ALuint src       = 0;
    ALuint buf       = 0;
    float  volume    = 0.f;   // 0..1
    float  target    = 0.f;   // 0..1
    bool   active    = false;
};
static OAmbient g_amb[2];

// Cache OpenAL buffers by source PCM pointer so we don't re-upload per-play.
// The game keeps sfx data alive for the whole session, so pointer identity is safe.
struct BufKey { short* data; int len; };
struct BufKeyHash {
    size_t operator()(const BufKey& k) const noexcept {
        return std::hash<void*>()(k.data) ^ (size_t)k.len;
    }
};
struct BufKeyEq {
    bool operator()(const BufKey& a, const BufKey& b) const noexcept {
        return a.data == b.data && a.len == b.len;
    }
};
static std::unordered_map<BufKey, ALuint, BufKeyHash, BufKeyEq> g_bufCache;

static Uint32 g_lastTick = 0;

// ─── EFX reverb zones ────────────────────────────────────────────────────────
// One aux effect slot attached to every spatial voice; the EAX-reverb effect
// inside it is swapped between presets as the player's environment changes
// (surface → FOREST, swim → UNDERWATER, high altitude above the tree canopy
// → MOUNTAINS). Dry weapon/UI sounds bypass the send so muzzle reports and
// HUD beeps stay clean.
//
// Zero-cost graceful fallback: if ALC_EXT_EFX is unavailable or the driver
// rejects the slot/effect gens, g_reverbSlot stays 0 and every voice path
// skips the AL_AUXILIARY_SEND_FILTER call — identical to pre-EFX behaviour.
static ALuint g_reverbEffect = 0;
static ALuint g_reverbSlot   = 0;
static int    g_currentPreset = -1;   // cached so we don't re-upload on every frame

enum ReverbZone {
    ZONE_FOREST     = 0,   // default — dense vegetation, medium tail
    ZONE_MOUNTAINS  = 1,   // open high-altitude, long sparse reflections
    ZONE_UNDERWATER = 2,   // heavily low-passed, long tail
};

static void ApplyReverbPreset(ReverbZone zone)
{
    if (!g_reverbEffect || g_currentPreset == (int)zone) return;

    EFXEAXREVERBPROPERTIES p;
    switch (zone) {
        case ZONE_UNDERWATER: { EFXEAXREVERBPROPERTIES u = EFX_REVERB_PRESET_UNDERWATER; p = u; } break;
        case ZONE_MOUNTAINS:  { EFXEAXREVERBPROPERTIES m = EFX_REVERB_PRESET_MOUNTAINS;  p = m; } break;
        case ZONE_FOREST:
        default:              { EFXEAXREVERBPROPERTIES f = EFX_REVERB_PRESET_FOREST;     p = f; } break;
    }

    // Upload the EAX-reverb property block. Only the EAXReverb effect reads
    // all of these; if the driver falls back to plain AL_EFFECT_REVERB the
    // non-core parameters silently no-op, which is fine.
    alEffectf (g_reverbEffect, AL_EAXREVERB_DENSITY,             p.flDensity);
    alEffectf (g_reverbEffect, AL_EAXREVERB_DIFFUSION,           p.flDiffusion);
    alEffectf (g_reverbEffect, AL_EAXREVERB_GAIN,                p.flGain);
    alEffectf (g_reverbEffect, AL_EAXREVERB_GAINHF,              p.flGainHF);
    alEffectf (g_reverbEffect, AL_EAXREVERB_GAINLF,              p.flGainLF);
    alEffectf (g_reverbEffect, AL_EAXREVERB_DECAY_TIME,          p.flDecayTime);
    alEffectf (g_reverbEffect, AL_EAXREVERB_DECAY_HFRATIO,       p.flDecayHFRatio);
    alEffectf (g_reverbEffect, AL_EAXREVERB_DECAY_LFRATIO,       p.flDecayLFRatio);
    alEffectf (g_reverbEffect, AL_EAXREVERB_REFLECTIONS_GAIN,    p.flReflectionsGain);
    alEffectf (g_reverbEffect, AL_EAXREVERB_REFLECTIONS_DELAY,   p.flReflectionsDelay);
    alEffectfv(g_reverbEffect, AL_EAXREVERB_REFLECTIONS_PAN,     p.flReflectionsPan);
    alEffectf (g_reverbEffect, AL_EAXREVERB_LATE_REVERB_GAIN,    p.flLateReverbGain);
    alEffectf (g_reverbEffect, AL_EAXREVERB_LATE_REVERB_DELAY,   p.flLateReverbDelay);
    alEffectfv(g_reverbEffect, AL_EAXREVERB_LATE_REVERB_PAN,     p.flLateReverbPan);
    alEffectf (g_reverbEffect, AL_EAXREVERB_ECHO_TIME,           p.flEchoTime);
    alEffectf (g_reverbEffect, AL_EAXREVERB_ECHO_DEPTH,          p.flEchoDepth);
    alEffectf (g_reverbEffect, AL_EAXREVERB_MODULATION_TIME,     p.flModulationTime);
    alEffectf (g_reverbEffect, AL_EAXREVERB_MODULATION_DEPTH,    p.flModulationDepth);
    alEffectf (g_reverbEffect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF,
                                                                 p.flAirAbsorptionGainHF);
    alEffectf (g_reverbEffect, AL_EAXREVERB_HFREFERENCE,         p.flHFReference);
    alEffectf (g_reverbEffect, AL_EAXREVERB_LFREFERENCE,         p.flLFReference);
    alEffectf (g_reverbEffect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, p.flRoomRolloffFactor);
    alEffecti (g_reverbEffect, AL_EAXREVERB_DECAY_HFLIMIT,       p.iDecayHFLimit);

    // Rebind the updated effect into the active aux slot so the changes
    // take effect; without this second call the slot keeps the previous
    // snapshot of the effect's properties.
    alAuxiliaryEffectSloti(g_reverbSlot, AL_EFFECTSLOT_EFFECT, (ALint)g_reverbEffect);

    g_currentPreset = (int)zone;
}

// Pick the zone based on the player's current environment. UNDERWATER is the
// one hard signal the engine already tracks; MOUNTAINS is inferred from the
// player being well above their local ground (ridgeline / high plateau).
static ReverbZone SelectZoneForPlayer()
{
    extern int UNDERWATER;
    if (UNDERWATER) return ZONE_UNDERWATER;
    // Player Y above terrain by > ~6m (256 units/m) and no canopy overhead
    // → treat as high-altitude open. Sampling a single GetLandH under the
    // player is cheap and matches where the listener actually is.
    float gnd = GetLandH(CameraX, CameraZ);
    if (CameraY - gnd > 1500.f) return ZONE_MOUNTAINS;
    return ZONE_FOREST;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static ALuint GetOrCreateBuffer(int length, short* data)
{
    if (!data || length <= 0) return 0;
    BufKey key{data, length};
    auto it = g_bufCache.find(key);
    if (it != g_bufCache.end()) return it->second;

    ALuint buf = 0;
    alGenBuffers(1, &buf);
    if (!buf) return 0;
    alBufferData(buf, AL_FORMAT_MONO16, data, length, SAMPLE_RATE);
    g_bufCache[key] = buf;
    return buf;
}

// ─── Public API (same symbols as Audio_SDL.cpp) ───────────────────────────────

bool SDL_Audio_Init()
{
    if (g_context) return true;

    g_device = alcOpenDevice(nullptr);
    if (!g_device) { Log("alcOpenDevice failed\n"); return false; }

    g_context = alcCreateContext(g_device, nullptr);
    if (!g_context || !alcMakeContextCurrent(g_context)) {
        Log("alcCreateContext/MakeCurrent failed\n");
        if (g_context) { alcDestroyContext(g_context); g_context = nullptr; }
        alcCloseDevice(g_device); g_device = nullptr;
        return false;
    }

    // Distance attenuation: linear falloff between MIN_RADIUS and MAX_RADIUS.
    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);

    // Pre-generate voice sources + matching occlusion filters.
    alGenSources(MAX_CHANNELS, g_srcVoice);
    alGenFilters(MAX_CHANNELS, g_filterVoice);
    for (int i = 0; i < MAX_CHANNELS; i++) {
        alSourcef(g_srcVoice[i], AL_REFERENCE_DISTANCE, MIN_RADIUS);
        alSourcef(g_srcVoice[i], AL_MAX_DISTANCE,       MAX_RADIUS);
        // SOURCEPORT: rolloff was 1.0 → linear falloff hit 50% only at ~7k units,
        // which made distant dino roars/footfalls sound uncomfortably close. With
        // 1.75 the curve reaches ~50% near 4.5k units and zero by ~8.3k, so near-
        // field voices are unchanged while far-away ones correctly fade into the
        // ambient bed. Full formula (AL_LINEAR_DISTANCE_CLAMPED):
        //     gain = 1 - ROLLOFF * (dist - REF) / (MAX - REF), clamped [0,1].
        alSourcef(g_srcVoice[i], AL_ROLLOFF_FACTOR,     1.75f);
        alFilteri(g_filterVoice[i], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(g_filterVoice[i], AL_LOWPASS_GAIN,   1.f);
        alFilterf(g_filterVoice[i], AL_LOWPASS_GAINHF, 1.f);
    }

    // OAmbient sources: non-positional (SOURCE_RELATIVE), looping.
    for (int a = 0; a < 2; a++) {
        alGenSources(1, &g_amb[a].src);
        alSourcei(g_amb[a].src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(g_amb[a].src, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcei(g_amb[a].src, AL_LOOPING, AL_TRUE);
        alSourcef(g_amb[a].src, AL_GAIN, 0.f);
    }

    alListener3f(AL_POSITION, 0.f, 0.f, 0.f);
    ALfloat orient[6] = {0.f, 0.f, -1.f,  0.f, 1.f, 0.f};
    alListenerfv(AL_ORIENTATION, orient);

    // EFX reverb setup — optional; if the device/driver lacks ALC_EXT_EFX we
    // leave g_reverbSlot=0 and every downstream path no-ops the send wiring.
    if (alcIsExtensionPresent(g_device, "ALC_EXT_EFX")) {
        alGenEffects(1, &g_reverbEffect);
        alEffecti(g_reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
        if (alGetError() != AL_NO_ERROR) {
            // Driver doesn't support EAX reverb — fall back to plain reverb
            // which still gives us a meaningful (if less parameterized) tail.
            alEffecti(g_reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        }
        alGenAuxiliaryEffectSlots(1, &g_reverbSlot);
        if (alGetError() == AL_NO_ERROR && g_reverbSlot && g_reverbEffect) {
            ApplyReverbPreset(ZONE_FOREST);
            Log("OpenAL EFX reverb zones active.\n");
        } else {
            if (g_reverbEffect) { alDeleteEffects(1, &g_reverbEffect); g_reverbEffect = 0; }
            if (g_reverbSlot)   { alDeleteAuxiliaryEffectSlots(1, &g_reverbSlot); g_reverbSlot = 0; }
        }
    }

    g_lastTick = SDL_GetTicks();
    Log("OpenAL audio initialized.\n");
    return true;
}

void SDL_Audio_Shutdown()
{
    if (!g_context) return;

    alSourceStopv(MAX_CHANNELS, g_srcVoice);
    alDeleteSources(MAX_CHANNELS, g_srcVoice);
    alDeleteFilters(MAX_CHANNELS, g_filterVoice);
    std::memset(g_srcVoice,    0, sizeof(g_srcVoice));
    std::memset(g_filterVoice, 0, sizeof(g_filterVoice));

    for (int a = 0; a < 2; a++) {
        if (g_amb[a].src) { alSourceStop(g_amb[a].src); alDeleteSources(1, &g_amb[a].src); }
        g_amb[a] = OAmbient{};
    }

    for (auto& kv : g_bufCache) alDeleteBuffers(1, &kv.second);
    g_bufCache.clear();

    if (g_reverbSlot)   { alDeleteAuxiliaryEffectSlots(1, &g_reverbSlot); g_reverbSlot = 0; }
    if (g_reverbEffect) { alDeleteEffects(1, &g_reverbEffect);            g_reverbEffect = 0; }
    g_currentPreset = -1;

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(g_context); g_context = nullptr;
    alcCloseDevice(g_device);     g_device  = nullptr;
}

void SDL_Audio_Stop()
{
    if (!g_context) return;
    alSourceStopv(MAX_CHANNELS, g_srcVoice);
    for (int a = 0; a < 2; a++) {
        alSourceStop(g_amb[a].src);
        alSourcef(g_amb[a].src, AL_GAIN, 0.f);
        g_amb[a].volume = g_amb[a].target = 0.f;
        g_amb[a].active = false;
    }
}

void SDL_Audio_SetCameraPos(float cx, float cy, float cz, float alpha, float /*beta*/)
{
    if (!g_context) return;

    alListener3f(AL_POSITION, cx, cy, cz);

    // Carnivores world: XZ is the horizontal plane; alpha rotates around Y.
    // Forward in world-XZ: (sin(alpha), -cos(alpha)) matches the original
    // camera-right vector (ca, sa) rotated 90° into forward.
    float ca = std::cos(alpha), sa = std::sin(alpha);
    ALfloat orient[6] = {
        sa, 0.f, -ca,   // forward
        0.f, 1.f, 0.f   // up
    };
    alListenerfv(AL_ORIENTATION, orient);

    // Pick the reverb zone for wherever the listener is now. Internal
    // cache short-circuits if the zone hasn't changed since last frame.
    if (g_reverbSlot) ApplyReverbPreset(SelectZoneForPlayer());

    // Tick ambient crossfades here (called every frame by the game).
    Uint32 now = SDL_GetTicks();
    float dt = (now - g_lastTick) / 1000.f;
    g_lastTick = now;
    if (dt > 0.25f) dt = 0.25f;

    for (int a = 0; a < 2; a++) {
        OAmbient& am = g_amb[a];
        if (!am.active && am.volume == 0.f) continue;
        float step = (FADE_RATE_PER_SEC / 256.f) * dt;
        if (am.volume < am.target)
            am.volume = std::min(am.target, am.volume + step);
        else if (am.volume > am.target) {
            am.volume = std::max(am.target, am.volume - step);
            if (am.volume == 0.f && am.target == 0.f) {
                alSourceStop(am.src);
                am.active = false;
            }
        }
        alSourcef(am.src, AL_GAIN, am.volume);
    }
}

// SOURCEPORT: terrain-raycast occlusion. Samples ground height at N points
// along the listener→source ray; each sample whose terrain rises above the
// straight-line ray counts as a blocked segment. Returns occlusion strength
// in [0..1] where 0 = fully line-of-sight, 1 = completely blocked.
// Caller maps this to AL_GAIN + AL_LOWPASS_GAINHF so blocked sounds are
// quieter AND muffled (the physically-correct dual effect).
static float ComputeTerrainOcclusion(float sx, float sy, float sz)
{
    // Source at the listener or very close: skip the raycast — the ray would
    // degenerate and GetLandH on identical endpoints is wasted work.
    float dx = sx - CameraX, dy = sy - CameraY, dz = sz - CameraZ;
    float dist2 = dx*dx + dz*dz;
    if (dist2 < 16.f * 16.f) return 0.f;

    // 12 samples strike a reasonable balance between accuracy (can resolve a
    // ridge between two low points) and cost (one GetLandH call per sample,
    // called at most 16× per play, typically a few times per second).
    constexpr int N = 12;
    int blocked = 0;
    float worstPenetration = 0.f;
    const float dist = std::sqrt(dist2 + dy*dy);

    for (int i = 1; i < N; ++i) {
        float t   = (float)i / (float)N;
        float wx  = CameraX + dx * t;
        float wy  = CameraY + dy * t;
        float wz  = CameraZ + dz * t;
        float gnd = GetLandH(wx, wz);
        if (gnd > wy) {
            ++blocked;
            float pen = gnd - wy;
            if (pen > worstPenetration) worstPenetration = pen;
        }
    }

    // Two-term occlusion: fraction of ray blocked, weighted by how deeply the
    // ray passes through terrain. A ridge that pokes up a little bit barely
    // attenuates; a hill that fully swallows the ray attenuates strongly.
    float fracBlocked = (float)blocked / (float)(N - 1);
    float depthNorm   = std::min(1.f, worstPenetration / 512.f);
    float occ = fracBlocked * (0.4f + 0.6f * depthNorm);

    // Long-distance grazing occlusion: even a very shallow blockage at >4000
    // units away should muffle. Boost slightly with distance.
    if (dist > 4000.f && occ > 0.f)
        occ = std::min(1.f, occ * (1.f + (dist - 4000.f) / 8000.f));

    return std::min(1.f, occ);
}

void SDL_Audio_AddVoice3dv(int length, short* data, float x, float y, float z, int vol)
{
    if (!g_context || !data || length <= 0) return;

    ALuint buf = GetOrCreateBuffer(length, data);
    if (!buf) return;

    // Find a free source (AL_STOPPED or AL_INITIAL).
    for (int i = 0; i < MAX_CHANNELS; i++) {
        ALint state = 0;
        alGetSourcei(g_srcVoice[i], AL_SOURCE_STATE, &state);
        if (state == AL_PLAYING || state == AL_PAUSED) continue;

        bool spatial = !(x == 0.f && y == 0.f && z == 0.f);
        alSourcei(g_srcVoice[i], AL_SOURCE_RELATIVE, spatial ? AL_FALSE : AL_TRUE);
        alSource3f(g_srcVoice[i], AL_POSITION, x, y, z);
        alSourcei(g_srcVoice[i], AL_LOOPING, AL_FALSE);

        float gain = std::max(0, std::min(256, vol)) / 256.f;

        // SOURCEPORT: apply terrain occlusion to spatial voices only. Listener-
        // relative sounds (weapon fire, UI beeps) are always "in the head" and
        // never occluded. Occlusion attenuates volume to 15% minimum and rolls
        // off high frequencies to 5% so blocked roars sound low and distant.
        if (spatial) {
            float occ = ComputeTerrainOcclusion(x, y, z);
            float gainMul   = 1.f - 0.85f * occ;
            float gainHFMul = 1.f - 0.95f * occ;
            alFilterf(g_filterVoice[i], AL_LOWPASS_GAIN,   gainMul);
            alFilterf(g_filterVoice[i], AL_LOWPASS_GAINHF, gainHFMul);
            alSourcei(g_srcVoice[i], AL_DIRECT_FILTER, (ALint)g_filterVoice[i]);
            // Route this voice through the reverb aux slot so environmental
            // tails paint the tone of whatever zone the player is in.
            if (g_reverbSlot) {
                alSource3i(g_srcVoice[i], AL_AUXILIARY_SEND_FILTER,
                           (ALint)g_reverbSlot, 0, AL_FILTER_NULL);
            }
        } else {
            // Reset any prior filter for this slot so non-spatial sounds play dry.
            alSourcei(g_srcVoice[i], AL_DIRECT_FILTER, AL_FILTER_NULL);
            // Dry send too — UI/weapon sfx shouldn't get the world's reverb tail.
            if (g_reverbSlot) {
                alSource3i(g_srcVoice[i], AL_AUXILIARY_SEND_FILTER,
                           AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
            }
        }

        alSourcef(g_srcVoice[i], AL_GAIN, gain);
        alSourcei(g_srcVoice[i], AL_BUFFER, (ALint)buf);
        alSourcePlay(g_srcVoice[i]);
        return;
    }
}

void SDL_Audio_SetAmbient(int length, short* data, int vol)
{
    if (!g_context) return;

    // Shift current ambient (slot 0) to slot 1 and stop it immediately —
    // the fade tick lives in SetCameraPos which isn't called in menus, so a
    // crossfade would leave the outgoing ambient playing forever on menu transitions.
    std::swap(g_amb[0], g_amb[1]);
    if (g_amb[1].src) {
        alSourceStop(g_amb[1].src);
        alSourcef(g_amb[1].src, AL_GAIN, 0.f);
    }
    g_amb[1].target = 0.f;
    g_amb[1].volume = 0.f;
    g_amb[1].active = false;

    // Start new ambient in slot 0.
    OAmbient& a = g_amb[0];
    a = OAmbient{};
    // Regen a source (swap moved the old one into slot 1).
    alGenSources(1, &a.src);
    alSourcei(a.src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(a.src, AL_POSITION, 0.f, 0.f, 0.f);
    alSourcei(a.src, AL_LOOPING, AL_TRUE);

    if (data && length > 0) {
        a.buf    = GetOrCreateBuffer(length, data);
        a.target = std::max(0, std::min(256, vol)) / 256.f;
        // SOURCEPORT: snap to target immediately — fade tick lives in SetCameraPos
        // which isn't called in menus, so a fade-in would leave menu ambients silent.
        a.volume = a.target;
        a.active = true;
        alSourcef(a.src, AL_GAIN, a.volume);
        alSourcei(a.src, AL_BUFFER, (ALint)a.buf);
        alSourcePlay(a.src);
    }
}

void SDL_Audio_SetAmbient3d(int length, short* data, float /*x*/, float /*y*/, float /*z*/)
{
    // Ship hum etc. — treat as listener-relative at moderate volume.
    SDL_Audio_SetAmbient(length, data, 192);
}
