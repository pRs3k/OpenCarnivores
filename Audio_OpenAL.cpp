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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include "hunt.h"

static void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

// ─── Constants ────────────────────────────────────────────────────────────────
#define MAX_CHANNELS  16
#define SAMPLE_RATE   22050
#define MAX_RADIUS    6000.f
#define MIN_RADIUS    512.f
// OAmbient crossfade rate: ~3s matches Audio_SDL.cpp's FADE_SPEED=4 @ 1024-sample callback.
#define FADE_RATE_PER_SEC  (256.f / 3.f)

// ─── State ────────────────────────────────────────────────────────────────────
static ALCdevice*  g_device  = nullptr;
static ALCcontext* g_context = nullptr;

// One OpenAL source per one-shot voice slot.
static ALuint g_srcVoice[MAX_CHANNELS] = {0};

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

    // Pre-generate voice sources.
    alGenSources(MAX_CHANNELS, g_srcVoice);
    for (int i = 0; i < MAX_CHANNELS; i++) {
        alSourcef(g_srcVoice[i], AL_REFERENCE_DISTANCE, MIN_RADIUS);
        alSourcef(g_srcVoice[i], AL_MAX_DISTANCE,       MAX_RADIUS);
        alSourcef(g_srcVoice[i], AL_ROLLOFF_FACTOR,     1.f);
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

    g_lastTick = SDL_GetTicks();
    Log("OpenAL audio initialized.\n");
    return true;
}

void SDL_Audio_Shutdown()
{
    if (!g_context) return;

    alSourceStopv(MAX_CHANNELS, g_srcVoice);
    alDeleteSources(MAX_CHANNELS, g_srcVoice);
    std::memset(g_srcVoice, 0, sizeof(g_srcVoice));

    for (int a = 0; a < 2; a++) {
        if (g_amb[a].src) { alSourceStop(g_amb[a].src); alDeleteSources(1, &g_amb[a].src); }
        g_amb[a] = OAmbient{};
    }

    for (auto& kv : g_bufCache) alDeleteBuffers(1, &kv.second);
    g_bufCache.clear();

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
        alSourcef(g_srcVoice[i], AL_GAIN, std::max(0, std::min(256, vol)) / 256.f);
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
