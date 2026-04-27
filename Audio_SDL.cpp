// Audio_SDL.cpp — SOURCEPORT: SDL2 software mixer replacing the original a_soft.dll
// Implements the same API surface as the original DirectSound DLL backend.
// 22050 Hz stereo 16-bit output; up to 16 concurrent one-shot channels + 2 ambient slots.

#define NOMINMAX       // prevent windows.h min/max macros from clobbering std::
#include <SDL.h>
#include <cmath>
#include <algorithm>
#include <string.h>
#include "hunt.h"

// PrintLog takes LPSTR (non-const) — wrap with a cast helper
static void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

// ─── Constants ────────────────────────────────────────────────────────────────
#define MAX_CHANNELS  16
#define SAMPLE_RATE   22050
#define CB_SAMPLES    1024        // samples per audio callback (~46ms)
#define MAX_RADIUS    6000        // world units — silent beyond this
#define MIN_RADIUS    512         // world units — full volume within this
// Ambient fade: FADE_SPEED volume units per callback call.
// 256 / 3 / (22050/1024) ≈ 3-second crossfade.
#define FADE_SPEED    4

// ─── Internal channel structs ─────────────────────────────────────────────────
struct SChannel {
    int    status;   // 0=free, 1=playing
    int    iLength;  // byte count of source PCM
    int    iPos;     // current byte position
    short* lpData;   // pointer to raw 16-bit mono samples
    float  x, y, z; // world position (0,0,0 = non-spatial / centered)
    int    volume;   // 0–256
};

struct SAmbient {
    short* lpData;
    int    iLength, iPos;
    int    volume, avolume;  // current and target (for fade-in/out)
};

// ─── State (accessed from both main and audio threads; protected by device lock) ──
static SDL_AudioDeviceID g_dev = 0;
static SChannel          g_ch[MAX_CHANNELS];
static SAmbient          g_amb[2];   // [0] = active (fading in), [1] = previous (fading out)

// Camera position and orientation — written main thread, read in callback.
// Floats are written atomically on x86/x64 so no explicit lock needed for these.
static volatile float g_camX, g_camY, g_camZ;
static volatile float g_ca, g_sa;   // cos/sin of camera yaw (CameraAlpha)

// ─── Spatialization ───────────────────────────────────────────────────────────
// Computes L/R volume (0–256 each) for a world-space point.
// Camera-right vector in world XZ = (ca, sa).
// Matching the original Audio.cpp range: left ∈ [32,256], right ∈ [32,256].
static void CalcPan(float sx, float sy, float sz, int volume,
                    int& volL, int& volR)
{
    if (sx == 0.f && sz == 0.f) {
        // Non-spatial sound: centered at full volume
        volL = volR = volume;
        return;
    }

    float dx   = sx - g_camX;
    float dz   = sz - g_camZ;
    float dist = sqrtf(dx * dx + dz * dz);

    if (dist >= (float)MAX_RADIUS) { volL = volR = 0; return; }

    float atten = (dist > (float)MIN_RADIUS)
                ? 1.f - (dist - (float)MIN_RADIUS) / (float)(MAX_RADIUS - MIN_RADIUS)
                : 1.f;
    int vol = (int)((float)volume * atten);

    // pan ∈ [-1 = full left, +1 = full right]
    float pan = (dist > 0.f) ? (dx * g_ca + dz * g_sa) / dist : 0.f;
    pan = std::max(-1.f, std::min(1.f, pan));

    // centre = 0.5625 (= 144/256), half-spread = 0.4375 (= 112/256)
    volL = (int)((float)vol * (0.5625f - pan * 0.4375f));
    volR = (int)((float)vol * (0.5625f + pan * 0.4375f));
    volL = std::max(0, std::min(vol, volL));
    volR = std::max(0, std::min(vol, volR));
}

// ─── Audio callback (SDL audio thread) ───────────────────────────────────────
static void SDLCALL AudioCB(void* /*userdata*/, Uint8* stream, int len)
{
    const int nSamples = len / 4;   // stereo 16-bit: 4 bytes per sample pair

    // 32-bit accumulation buffers (prevent overflow when many channels sum)
    static int L[CB_SAMPLES * 2], R[CB_SAMPLES * 2];
    int n = std::min(nSamples, (int)(CB_SAMPLES * 2));

    memset(L, 0, n * sizeof(int));
    memset(R, 0, n * sizeof(int));

    // ── One-shot channels ──
    for (int c = 0; c < MAX_CHANNELS; c++) {
        SChannel& ch = g_ch[c];
        if (!ch.status) continue;

        int volL, volR;
        CalcPan(ch.x, ch.y, ch.z, ch.volume, volL, volR);

        int pos   = ch.iPos / 2;
        int total = ch.iLength / 2;
        for (int i = 0; i < n; i++) {
            if (pos >= total) { ch.status = 0; break; }
            int s = ch.lpData[pos++];
            L[i] += (s * volL) >> 8;
            R[i] += (s * volR) >> 8;
        }
        ch.iPos = pos * 2;
    }

    // ── Ambient channels (looping with crossfade) ──
    for (int a = 0; a < 2; a++) {
        SAmbient& am = g_amb[a];
        if (!am.lpData) continue;

        // Fade toward target volume BEFORE mixing so a newly-set ambient
        // (volume=0, avolume>0) increments on the very first callback.
        if (am.volume < am.avolume)
            am.volume = std::min(am.avolume, am.volume + FADE_SPEED);
        else if (am.volume > am.avolume) {
            am.volume = std::max(am.avolume, am.volume - FADE_SPEED);
            if (am.volume == 0) { am.lpData = nullptr; continue; }
        }

        if (am.volume == 0) continue;   // still fading in from silence — skip mix

        int total = am.iLength / 2;
        int pos   = am.iPos / 2;
        int vol   = am.volume;
        for (int i = 0; i < n; i++) {
            if (pos >= total) pos = 0;   // loop
            int s = am.lpData[pos++];
            L[i] += (s * vol) >> 8;
            R[i] += (s * vol) >> 8;
        }
        am.iPos = pos * 2;
    }

    // ── Clamp and write stereo output ──
    short* out = (short*)stream;
    for (int i = 0; i < n; i++) {
        out[i * 2]     = (short)std::max(-32768, std::min(32767, L[i]));
        out[i * 2 + 1] = (short)std::max(-32768, std::min(32767, R[i]));
    }
    if (n < nSamples) memset(out + n * 2, 0, (nSamples - n) * 4);
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool SDL_Audio_Init()
{
    if (g_dev) return true;

    SDL_AudioSpec want{}, got{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = CB_SAMPLES;
    want.callback = AudioCB;
    want.userdata = nullptr;

    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!g_dev) {
        Log("SDL_OpenAudioDevice failed: ");
        Log(SDL_GetError());
        Log("\n");
        return false;
    }

    memset(g_ch,  0, sizeof(g_ch));
    memset(g_amb, 0, sizeof(g_amb));
    g_camX = g_camY = g_camZ = 0.f;
    g_ca = 1.f; g_sa = 0.f;

    SDL_PauseAudioDevice(g_dev, 0);  // begin playback
    Log("SDL audio initialized (22050 Hz stereo 16-bit, 1024-sample buffer).\n");
    return true;
}

void SDL_Audio_Shutdown()
{
    if (!g_dev) return;
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

void SDL_Audio_Stop()
{
    if (!g_dev) return;
    SDL_LockAudioDevice(g_dev);
    for (auto& c : g_ch)  c.status = 0;
    for (auto& a : g_amb) { a.lpData = nullptr; a.volume = a.avolume = 0; }
    SDL_UnlockAudioDevice(g_dev);
}

void SDL_Audio_SetCameraPos(float cx, float cy, float cz, float alpha, float /*beta*/)
{
    // Written from main thread; floats are written atomically on x86/x64.
    g_camX = cx; g_camY = cy; g_camZ = cz;
    g_ca   = cosf(alpha);
    g_sa   = sinf(alpha);
}

// Legacy SDL mixer does ambient crossfading inside its audio callback thread,
// so the public Update() hook is a no-op here. Kept for API parity with the
// OpenAL backend.
void SDL_Audio_Update() {}

void SDL_Audio_AddVoice3dv(int length, short* data, float x, float y, float z, int vol)
{
    if (!g_dev || !data || length <= 0) return;
    SDL_LockAudioDevice(g_dev);
    for (int c = 0; c < MAX_CHANNELS; c++) {
        if (!g_ch[c].status) {
            g_ch[c].iLength = length;
            g_ch[c].iPos    = 0;
            g_ch[c].lpData  = data;
            g_ch[c].x = x; g_ch[c].y = y; g_ch[c].z = z;
            g_ch[c].volume  = std::max(0, std::min(256, vol));
            g_ch[c].status  = 1;
            break;
        }
    }
    SDL_UnlockAudioDevice(g_dev);
}

void SDL_Audio_SetAmbient(int length, short* data, int vol)
{
    if (!g_dev) return;
    SDL_LockAudioDevice(g_dev);
    // Shift current ambient to slot 1 for fade-out
    g_amb[1]         = g_amb[0];
    g_amb[1].avolume = 0;

    // Start new ambient in slot 0 — fade in
    g_amb[0].lpData  = (data && length > 0) ? data : nullptr;
    g_amb[0].iLength = length;
    g_amb[0].iPos    = 0;
    g_amb[0].volume  = 0;
    g_amb[0].avolume = (data && length > 0) ? std::max(0, std::min(256, vol)) : 0;
    SDL_UnlockAudioDevice(g_dev);
}

void SDL_Audio_SetAmbient3d(int length, short* data, float /*x*/, float /*y*/, float /*z*/)
{
    // Treat 3D ambient (ship hum etc.) as centered — simple and avoids needing
    // per-sample 3D attenuation in the looping path.
    SDL_Audio_SetAmbient(length, data, 192);
}
