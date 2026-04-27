// Audio_DLL.cpp — SOURCEPORT: replaces the original runtime-DLL dispatch.
// The original code loaded a_soft.dll / a_ds3d.dll / a_a3d.dll / a_eax.dll at
// runtime and called through function pointers.  In the source port we link
// Audio_SDL.cpp directly and call it here instead.

#include <windows.h>
#include "hunt.h"

// ─── Forward declarations from Audio_SDL.cpp ─────────────────────────────────
bool SDL_Audio_Init();
void SDL_Audio_Shutdown();
void SDL_Audio_Stop();
void SDL_Audio_SetCameraPos(float cx, float cy, float cz, float alpha, float beta);
void SDL_Audio_Update();
void SDL_Audio_AddVoice3dv(int length, short* data, float x, float y, float z, int vol);
void SDL_Audio_SetAmbient(int length, short* data, int vol);
void SDL_Audio_SetAmbient3d(int length, short* data, float x, float y, float z);

static bool g_audioAvailable = false;
static bool g_noSnd = false;   // set only by explicit -nosnd flag, not by save file

// Called from ProcessCommandLine when -nosnd is passed.
void Audio_SetNoSnd() { g_noSnd = true; }

// ─── Public API ───────────────────────────────────────────────────────────────

void InitAudioSystem(HWND /*hw*/, HANDLE /*hlog*/, int /*driver*/)
{
    // SOURCEPORT: legacy driver index (DLL selector) is ignored — SDL audio is always used.
    // Only the explicit -nosnd flag (g_noSnd) disables audio; OptSound=-1 from old saves does not.
    Audio_Shutdown();

    if (g_noSnd) {
        PrintLog("Audio disabled by user (-nosnd).\n");
        return;
    }

    g_audioAvailable = SDL_Audio_Init();
}

void Audio_Shutdown()
{
    if (!g_audioAvailable) return;
    SDL_Audio_Shutdown();
    g_audioAvailable = false;
}

void AudioStop()
{
    if (!g_audioAvailable) return;
    SDL_Audio_Stop();
}

void Audio_Restore()
{
    // SDL audio device does not get lost on focus change — no-op.
}

void AudioSetCameraPos(float cx, float cy, float cz, float alpha, float beta)
{
    if (!g_audioAvailable) return;
    SDL_Audio_SetCameraPos(cx, cy, cz, alpha, beta);
}

// Per-frame audio tick (ambient crossfades, etc.) — must be called every
// frame regardless of whether the game loop, menus, or a cutscene is
// driving the display, so fades advance in all game states.
void AudioUpdate()
{
    if (!g_audioAvailable) return;
    SDL_Audio_Update();
}

void Audio_SetEnvironment(int /*env*/, float /*f*/)
{
    // EAX reverb not implemented in SDL backend.
}

void Audio_UploadGeometry()
{
    UploadGeometry();
    // Geometry-based reverb not implemented in SDL backend.
}

void SetAmbient(int length, short* lpdata, int vol)
{
    if (!g_audioAvailable) return;
    SDL_Audio_SetAmbient(length, lpdata, vol);
}

void SetAmbient3d(int length, short* lpdata, float cx, float cy, float cz)
{
    if (!g_audioAvailable) return;
    SDL_Audio_SetAmbient3d(length, lpdata, cx, cy, cz);
}

void AddVoice3dv(int length, short* lpdata, float cx, float cy, float cz, int vol)
{
    if (!g_audioAvailable) return;
    SDL_Audio_AddVoice3dv(length, lpdata, cx, cy, cz, vol);
}

void AddVoice3d(int length, short* lpdata, float cx, float cy, float cz)
{
    AddVoice3dv(length, lpdata, cx, cy, cz, 256);
}

void AddVoicev(int length, short* lpdata, int vol)
{
    AddVoice3dv(length, lpdata, 0.f, 0.f, 0.f, vol);
}

void AddVoice(int length, short* lpdata)
{
    AddVoice3dv(length, lpdata, 0.f, 0.f, 0.f, 256);
}
